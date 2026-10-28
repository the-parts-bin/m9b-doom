// Renders a WAD's music (the D_* MUS lumps) to 16-bit stereo WAV files
// with libADLMIDI: Nuked OPL3 emulation, one chip, DMX's volume model and
// Doom's own instrument bank ("DMX (Bobby Prince v2)", the GENMIDI of Doom
// 1.9). make_music.sh turns them into what the board streams.
//
// Build (libADLMIDI built as a static library):
//   cc -O2 musrender.c -I<libADLMIDI>/include <libADLMIDI>/build/libADLMIDI.a -lstdc++ -lm -o musrender
// Usage: musrender DOOM.WAD OUTDIR [RATE]   (writes OUTDIR/D_E1M1.WAV, ...)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adlmidi.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void wr16(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); }

static void wav_header(FILE *f, uint32_t rate, uint32_t frames) {
    uint32_t bytes = frames * 4u;
    fwrite("RIFF", 1, 4, f); wr32(f, 36u + bytes); fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16); wr16(f, 1); wr16(f, 2); wr32(f, rate); wr32(f, rate * 4u); wr16(f, 4); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, bytes);
}

static int find_bank(const char *name) {
    const char *const *names = adl_getBankNames();
    for (int i = 0; i < adl_getBanksCount(); i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s WAD OUTDIR [RATE]\n", argv[0]);
        return 1;
    }
    long rate = argc > 3 ? atol(argv[3]) : 48000;
    FILE *w = fopen(argv[1], "rb");
    if (!w) { perror(argv[1]); return 1; }
    fseek(w, 0, SEEK_END);
    long size = ftell(w);
    fseek(w, 0, SEEK_SET);
    uint8_t *wad = malloc(size);
    if (fread(wad, 1, size, w) != (size_t)size) { perror("read"); return 1; }
    fclose(w);

    int bank = find_bank("DMX (Bobby Prince v2)");
    if (bank < 0) { fprintf(stderr, "Doom bank not found\n"); return 1; }
    uint32_t nlumps = rd32(wad + 4), dir = rd32(wad + 8);
    static short buf[2 * 4096];
    for (uint32_t i = 0; i < nlumps; i++) {
        const uint8_t *e = wad + dir + 16u * i;
        char name[9] = {0};
        memcpy(name, e + 8, 8);
        uint32_t pos = rd32(e), len = rd32(e + 4);
        if (strncmp(name, "D_", 2) != 0 || len < 4 || memcmp(wad + pos, "MUS\x1a", 4) != 0) continue;

        struct ADL_MIDIPlayer *p = adl_init(rate);
        adl_switchEmulator(p, ADLMIDI_EMU_NUKED);
        adl_setNumChips(p, 1);
        adl_setBank(p, bank);
        adl_setVolumeRangeModel(p, ADLMIDI_VolumeModel_DMX);
        adl_setSoftPanEnabled(p, 0);
        adl_setLoopEnabled(p, 0);
        if (adl_openData(p, wad + pos, len) < 0) {
            fprintf(stderr, "%s: %s\n", name, adl_errorInfo(p));
            adl_close(p);
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.WAV", argv[2], name);
        FILE *o = fopen(path, "wb");
        if (!o) { perror(path); return 1; }
        wav_header(o, (uint32_t)rate, 0);
        uint32_t frames = 0;
        int peak = 0;
        while (!adl_atEnd(p)) {
            int n = adl_play(p, 2 * 4096, buf);   // samples (both channels)
            if (n <= 0) break;
            for (int k = 0; k < n; k++) if (abs(buf[k]) > peak) peak = abs(buf[k]);
            fwrite(buf, 2, n, o);
            frames += (uint32_t)n / 2u;
        }
        fseek(o, 0, SEEK_SET);
        wav_header(o, (uint32_t)rate, frames);
        fclose(o);
        printf("%-8s %6.1f s, peak %5d\n", name, frames / (double)rate, peak);
        adl_close(p);
    }
    return 0;
}
