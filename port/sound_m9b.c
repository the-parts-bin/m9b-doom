// Sound effects for the m9b SoC: doomgeneric's DG_sound_module, mixing
// into the video engine's audio buffers (see ../../core/README.md, Audio).
//
// The hardware plays 48 kHz 16-bit stereo from two 1 KB halves (256 frames,
// 5.33 ms each) and raises an interrupt when it moves on from a half. The
// interrupt (trap.c -> sound_irq()) mixes the next 256 frames of every
// active channel into that half's PSRAM buffer, cleans it from the data
// cache and hands it back with AUDIO_PTR_A/B. The first channel writes the
// buffer and the others add to it, so the mixing touches only the 1 KB
// buffer and the samples (the data cache is 16 KB, direct-mapped); with
// nothing playing, the half points at a silent buffer instead.
//
// Sound effects are DMX lumps: 8-bit unsigned mono, 11025 Hz in Doom's
// WADs. They are resampled to 12 kHz by stepping through them in 16.16
// fixed point (nearest sample, as DMX did), with Doom's left/right volumes
// from its separation, and each 12 kHz sample is held for 4 output frames. Channel state is changed with interrupts off.
//
// Music: WAV files streamed from the SD card (see DG_music_module below).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m9b_map.h"

#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile uint32_t*)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile uint32_t*)(M9B_CTRL + 0x10u))   // [0] half A, [1] half B
#define PSRAM_BASE    M9B_PSRAM_BASE
#define CLOCK_CNT     (*(volatile uint32_t*)(M9B_TIMING + 0xCu))   // CLOCK_HZ

#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)

#define OUT_RATE      48000u
#define FRAMES        256u      // per half
#define HOLD          4u        // output frames per mixed sample
#define MIX_RATE      (OUT_RATE / HOLD)
#define NUM_CHANNELS  16        // s_sound.c uses snd_channels (8)

// Unused here, but i_sound.c and m_config.c bind them with FEATURE_SOUND.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static uint32_t audio_buf[2][FRAMES] __attribute__((aligned(256)));
static uint32_t silence[FRAMES] __attribute__((aligned(256)));   // stays zero

typedef struct {
    const uint8_t *data;   // NULL: idle
    uint32_t len;          // samples
    uint32_t pos, step;    // 16.16
    int32_t vol_l, vol_r;  // 0..254
} channel_t;

static channel_t channels[NUM_CHANNELS];
uint32_t sound_cycles, sound_mixes;   // time in sound_irq (clock ticks), halves mixed: for the timedemo report
static boolean use_prefix;
static boolean sound_on;

// A sound's samples, cached in the zone the first time it plays.
typedef struct {
    const uint8_t *data;
    uint32_t len, rate;
} sfx_data_t;

static uint32_t irq_off(void) {
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}

static void irq_restore(uint32_t m) {
    if (m) asm volatile("csrsi mstatus, 8");
}

static void cache_clean(uint32_t addr, uint32_t len) {
    for (uint32_t a = addr & ~63u; a < addr + len; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

static int mix_music(uint32_t *out);   // below, with the rest of the music

// Advances one channel by FRAMES frames (or to its end). The mix runs at
// MIX_RATE (12 kHz) and each result fills HOLD (4) output frames, a
// quarter of the work of mixing at 48 kHz; the effects are 11025 Hz, so
// little is lost. The first channel of a half writes all FRAMES words
// (zeros after its end), the others add to them.
static void mix_channel(channel_t *ch, uint32_t *out, int add) {
    const uint8_t *data = ch->data;
    uint32_t pos = ch->pos, step = ch->step, end = ch->len << 16;
    int32_t vl = ch->vol_l, vr = ch->vol_r;
    uint32_t *o = out, *o_end = out + FRAMES;
    for (; o < o_end && pos < end; o += HOLD, pos += step) {
        int32_t s = (int32_t)data[pos >> 16] - 128;
        int32_t l = (s * vl) >> 1, r = (s * vr) >> 1;   // at most +-16256
        if (add) {
            uint32_t w = o[0];
            l += (int16_t)w;
            r += (int32_t)w >> 16;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        }
        uint32_t w = (uint16_t)l | (uint32_t)r << 16;
        o[0] = w; o[1] = w; o[2] = w; o[3] = w;
    }
    if (!add)
        for (; o < o_end; o++) *o = 0;
    ch->pos = pos;
    if (pos >= end) ch->data = NULL;
}

// Mixes the next FRAMES frames into half `half` and queues it. With nothing
// playing, the half plays the silent buffer instead (nothing to write or
// clean).
static void mix_half(uint32_t half) {
    uint32_t *out = audio_buf[half];
    int any = mix_music(out);
    for (int c = 0; c < NUM_CHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->data) continue;
        mix_channel(ch, out, any);
        any = 1;
    }
    if (!any) {
        AUDIO_PTR[half] = (uint32_t)silence - PSRAM_BASE;
        return;
    }
    cache_clean((uint32_t)out, sizeof(audio_buf[0]));
    AUDIO_PTR[half] = (uint32_t)out - PSRAM_BASE;
    sound_mixes++;
}

// From trap.c, with the IRQ_PENDING bits it has just cleared.
void sound_irq(uint32_t pending) {
    if (!(pending & (IRQ_AUDIO_A | IRQ_AUDIO_B))) return;
    uint32_t t0 = CLOCK_CNT;
    if (pending & IRQ_AUDIO_A) mix_half(0);
    if (pending & IRQ_AUDIO_B) mix_half(1);
    sound_cycles += CLOCK_CNT - t0;
}

static void set_volume(channel_t *ch, int vol, int sep) {
    // As Chocolate Doom: sep 0 (left) .. 254 (right), vol 0..127.
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    ch->vol_l = ((254 - sep) * vol) / 127;
    ch->vol_r = (sep * vol) / 127;
}

static const sfx_data_t *get_sfx(sfxinfo_t *sfx) {
    if (sfx->driver_data) return sfx->driver_data;
    // DMX header: format (3), rate, length; the first and last 16 samples
    // are padding (DMX skipped them).
    int lumpnum = sfx->lumpnum;
    const uint8_t *lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    uint32_t lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8u || lump[0] != 3 || lump[1] != 0) return NULL;
    uint32_t rate = lump[2] | lump[3] << 8;
    uint32_t len = lump[4] | lump[5] << 8 | (uint32_t)lump[6] << 16 | (uint32_t)lump[7] << 24;
    if (len > lumplen - 8u || len <= 48u) return NULL;
    sfx_data_t *d = Z_Malloc(sizeof(*d), PU_STATIC, NULL);
    d->data = lump + 8 + 16;
    d->len = len - 32u;
    d->rate = rate;
    sfx->driver_data = d;
    return d;
}

static boolean snd_init(boolean use_sfx_prefix) {
    use_prefix = use_sfx_prefix;
    memset(channels, 0, sizeof(channels));
    memset(silence, 0, sizeof(silence));
    cache_clean((uint32_t)silence, sizeof(silence));
    uint32_t m = irq_off();
    AUDIO_PTR[0] = (uint32_t)silence - PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)silence - PSRAM_BASE;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
    irq_restore(m);
    sound_on = true;
    return true;
}

static void snd_shutdown(void) {
    if (!sound_on) return;
    uint32_t m = irq_off();
    AUDIO_CTRL = 0u;
    IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
    irq_restore(m);
    sound_on = false;
}

static int snd_get_lump(sfxinfo_t *sfx) {
    char name[16];
    if (sfx->link) sfx = sfx->link;
    if (use_prefix) snprintf(name, sizeof(name), "ds%s", sfx->name);
    else snprintf(name, sizeof(name), "%s", sfx->name);
    return W_GetNumForName(name);
}

static void snd_update(void) {
}

static void snd_update_params(int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    uint32_t m = irq_off();
    set_volume(&channels[channel], vol, sep);
    irq_restore(m);
}

static int snd_start(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;
    const sfx_data_t *d = get_sfx(sfx);
    uint32_t m = irq_off();
    channel_t *ch = &channels[channel];
    ch->data = NULL;
    if (d) {
        ch->len = d->len;
        ch->pos = 0;
        ch->step = (d->rate << 16) / MIX_RATE;   // rate < 65536
        set_volume(ch, vol, sep);
        ch->data = d->data;
    }
    irq_restore(m);
    return d ? channel : -1;
}

static void snd_stop(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channels[channel].data = NULL;   // a single store: no need to mask the interrupt
}

static boolean snd_is_playing(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return channels[channel].data != NULL;
}

static void snd_cache(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;   // loaded when first played
}

static snddevice_t sound_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(*sound_devices),
    snd_init,
    snd_shutdown,
    snd_get_lump,
    snd_update,
    snd_update_params,
    snd_start,
    snd_stop,
    snd_is_playing,
    snd_cache,
};

// -------------------------------------------------------------------------
// Music: streamed from WAV files on the SD card, rendered on a PC from the
// WAD's MUS lumps (tools/make_music.sh): DOOMS/MUSIC/<lump>.WAV, 24 kHz
// 16-bit mono. The main loop (Poll, once per frame) keeps a 256 KB ring
// buffer (5.5 s, enough to cover a level load) filled through FatFs; the
// audio interrupt takes samples from it, each held for 2 output frames,
// and the effects are added on top. A song without a file is silent.
// -------------------------------------------------------------------------
#define MUS_RATE      24000u
#define MUS_RING      (128u * 1024u)             // samples
#define MUS_CHUNK     (8u * 1024u)               // samples per read (16 KB)

static int16_t mus_ring[MUS_RING] __attribute__((aligned(64)));
static volatile uint32_t mus_r, mus_w;           // sample counts: read by the interrupt, written by Poll
static volatile int mus_playing;                 // the interrupt plays the ring
static int mus_fd = -1, mus_loop, mus_eof;
static uint32_t mus_data_start, mus_data_end;    // file offsets of the samples
static int32_t mus_volume = 64;                  // 0..127

// The interrupt's part: FRAMES frames of music into out (first writer).
static int mix_music(uint32_t *out) {
    if (!mus_playing) return 0;
    uint32_t r = mus_r, w = mus_w;
    int32_t vol = mus_volume;
    for (uint32_t i = 0; i < FRAMES; i += 2) {
        int32_t v = 0;
        // Gain vol / 64: Doom's default music volume (8 of 15, 64 here)
        // plays the render as it is (peaks about -2 dB), louder settings
        // boost it, clipped (the effects only reach -6 dB at full volume).
        if (r != w) v = (mus_ring[r++ & (MUS_RING - 1u)] * vol) >> 6;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        uint32_t word = (uint16_t)v | (uint32_t)(uint16_t)v << 16;
        out[i] = word;
        out[i + 1] = word;
    }
    mus_r = r;
    return 1;
}

static void mus_close(void) {
    mus_playing = 0;
    if (mus_fd >= 0) close(mus_fd);
    mus_fd = -1;
}

// Reads the WAV header: returns 0 if it is 24 kHz 16-bit mono PCM, with the
// sample data's position.
static int mus_open_wav(const char *path) {
    mus_fd = open(path, O_RDONLY);
    if (mus_fd < 0) return -1;
    uint8_t h[12];
    if (read(mus_fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    uint32_t pos = 12;
    int fmt_ok = 0;
    for (;;) {   // chunks: "fmt " then "data" (others skipped)
        uint8_t c[16];
        if (lseek(mus_fd, pos, SEEK_SET) != (off_t)pos || read(mus_fd, c, 8) != 8) return -1;
        uint32_t size = c[4] | c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            if (read(mus_fd, c, 16) != 16) return -1;
            uint32_t rate = c[4] | c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
            fmt_ok = c[0] == 1 && c[1] == 0 && c[2] == 1 && rate == MUS_RATE && c[14] == 16;
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt_ok) return -1;
            mus_data_start = pos + 8u;
            mus_data_end = mus_data_start + (size & ~1u);
            return lseek(mus_fd, mus_data_start, SEEK_SET) == (off_t)mus_data_start ? 0 : -1;
        }
        pos += 8u + ((size + 1u) & ~1u);
    }
}

// Tops up the ring buffer with up to max_chunks reads (at most 16 KB each,
// about 14 ms; the music takes 48 KB/s, 4 KB per frame at 12 fps), so a new
// song fills the ring over a few frames instead of stalling one.
static void mus_fill(int max_chunks) {
    if (mus_fd < 0 || mus_eof) return;
    while (max_chunks-- > 0) {
        uint32_t w = mus_w, space = MUS_RING - (w - mus_r);
        uint32_t at = w & (MUS_RING - 1u), n = MUS_RING - at;
        if (n > MUS_CHUNK) n = MUS_CHUNK;
        if (space < n) return;
        off_t pos = lseek(mus_fd, 0, SEEK_CUR);
        uint32_t left = (mus_data_end - (uint32_t)pos) / 2u;
        if (n > left) n = left;
        if (n) {
            ssize_t got = read(mus_fd, &mus_ring[at], n * 2u);
            if (got <= 0) { mus_eof = 1; return; }
            mus_w = w + (uint32_t)got / 2u;
            continue;
        }
        if (!mus_loop || lseek(mus_fd, mus_data_start, SEEK_SET) < 0) { mus_eof = 1; return; }
        max_chunks++;   // the seek read nothing
    }
}

// The lump name of a song's MUS data (the cached lump it came from).
static const char *lump_name_of(const void *data) {
    static char name[9];
    for (unsigned int i = 0; i < numlumps; i++)
        if (lumpinfo[i].cache == data) {
            memcpy(name, lumpinfo[i].name, 8);
            name[8] = 0;
            return name;
        }
    return NULL;
}

static boolean mus_init(void) { return true; }
static void mus_shutdown(void) { mus_close(); }
static void mus_set_volume(int volume) { mus_volume = volume < 0 ? 0 : volume > 127 ? 127 : volume; }
static void mus_pause(void) { mus_playing = 0; }
static void mus_resume(void) { if (mus_fd >= 0) mus_playing = 1; }
static void *mus_register(void *data, int len) { (void)len; return (void *)lump_name_of(data); }
static void mus_unregister(void *handle) { (void)handle; }

static void mus_play(void *handle, boolean looping) {
    mus_close();
    if (!handle) return;
    char path[40];
    snprintf(path, sizeof(path), "DOOMS/MUSIC/%s.WAV", (const char *)handle);
    if (mus_open_wav(path) < 0) {
        printf("music: no %s\n", path);
        mus_close();
        return;
    }
    mus_loop = looping;
    mus_eof = 0;
    mus_r = mus_w = 0;
    mus_fill(2);
    mus_playing = 1;
}

static void mus_stop(void) { mus_close(); }
static boolean mus_is_playing(void) { return mus_playing && !(mus_eof && mus_r == mus_w); }
static void mus_poll(void) { if (mus_playing) mus_fill(2); }

static snddevice_t music_devices[] = {
    SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
    music_devices, sizeof(music_devices) / sizeof(*music_devices),
    mus_init, mus_shutdown, mus_set_volume, mus_pause, mus_resume,
    mus_register, mus_unregister, mus_play, mus_stop, mus_is_playing, mus_poll,
};

void I_InitTimidityConfig(void) {
}
