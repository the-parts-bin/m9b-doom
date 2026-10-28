# Doom on the m9b SoC

Doom (ozkl's doomgeneric, based on Chocolate Doom) as a program for the SoC in `../core`.
It runs from PSRAM, uses picolibc, and reads the WAD from the SD card
through the core firmware's SD driver and FatFs.

**Status:** it plays, with sound effects and music over HDMI and USB
keyboard and mouse input. The timedemo runs at about 22 fps
(`../core/README.md`, Timing); the measurements under Performance below
were made on the earlier 27 MHz design, where play ran at 12–14 fps.

Run it with `../core/tools/run.py build/doom.bin`, with the SD card
holding `DOOMS/DOOM1.WAD`. Close `tio` first; the script uploads Doom and
then acts as the terminal. Through an external adapter on header J5 the
upload takes about 5 s (921600 baud), through the board's BL702 about 45 s
(see `../core/README.md`, Serial loader).

**Input:** a USB keyboard and mouse, on either of the core's two USB
ports, with vanilla Doom's controls: arrows to move, Ctrl fires, Space
opens doors, Shift runs, Alt strafes; the mouse turns and walks
(left button fires, right strafes, middle walks forward). The terminal's
keys work too: the arrow keys, Enter, Esc, Space (use), `f` or `,` (fire),
and other characters as themselves.

`port/doomgeneric_m9b.c` compares each keyboard report (the keys held)
with the previous one to make Doom's key presses and releases. The mouse
motion comes from the core's USB_MOUSE_POS running sums, read once per
`I_GetEvent` (`M9B_PollMouse`, called from `i_input.c`), so nothing is
lost between tics; with `-DM9B_MOUSE`, `G_Responder` adds up the mouse
events that reach one tic instead of keeping the last.

The normal build prints nothing once running: the frame-rate reports and
the profile are only in the timedemo build (below), since the UART output
stalled the game (the profile's sorting alone took about 0.3 s).

**License:** GPL v2, from Doom's source release by way of doomgeneric
(`doomgeneric/LICENSE`); that's why it's kept apart from `../core`.
`port/` uses `../core/firmware/sd.c`, `diskio.c` and FatFs (BSD-style) as
they are. The game data isn't in the repository (`data/README.md`).

## Sources

| Part | Upstream | Version | Here |
|---|---|---|---|
| The game | [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) (based on [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom), from id Software's source release) | `dcb7a8d` (`doomgeneric/UPSTREAM_COMMIT`) | `doomgeneric/`, the sources the build needs, with changes (Changes to doomgeneric) |
| Music renderer | [Wohlstand/libADLMIDI](https://github.com/Wohlstand/libADLMIDI) | `ef98fab` (`../core/tools/midirender/build.sh`) | used by `tools/musrender.c` on the PC |

## Layout

| Path | Contents |
|---|---|
| `doomgeneric/` | the doomgeneric sources needed for a build, from upstream commit `dcb7a8d` (`doomgeneric/UPSTREAM_COMMIT`), with small changes (below) |
| `port/doomgeneric_m9b.c` | the platform layer: `DG_Init`, `DG_DrawFrame`, `DG_GetKey`, ticks, sleep, `main()` |
| `port/sound_m9b.c` | `DG_sound_module`: sound effects mixed into the 48 kHz audio buffers from the audio interrupts; `DG_music_module`: music streamed from WAV files on the SD card |
| `tools/make_music.sh`, `tools/musrender.c` | render a WAD's music to those WAV files on the PC (libADLMIDI), and optionally copy them to the card through the board |
| `tools/get_data.sh` | takes the WAD from `doom19s.zip` (or the WAD itself) and checks it |
| `tools/profile.py` | maps the timedemo build's PC samples to functions |
| `port/hot.ld` | section ordering: the renderer's inner loops together in the fetch L1 |
| `../core/runtime/` | not here: the runtime all the ports share is in the core: `syscalls.c` (console on the UART, files on the SD card via FatFs), `trap_entry.S` and `trap.c` (interrupts, misaligned loads/stores emulated, PC sampling), `string.c` (`memcpy`/`memmove`/`memset` without misaligned accesses), `m9b.ld` |
| `port/membench.c`, `port/sdtest.c` | startup measurements: memory timings; SD reads checked (`EXTRA=-DM9B_SDTEST`) |
| `Makefile` | builds `build/doom.bin` |

## Building and the SD card

Requires `gcc-riscv64-unknown-elf` and `picolibc-riscv64-unknown-elf`
(Ubuntu packages). Run `make`, which produces `build/doom.elf` and
`build/doom.bin`.

The game data is the shareware Doom 1.9 IWAD, `DOOM1.WAD`;
`tools/get_data.sh SOURCE` takes it from id's `doom19s.zip` (or the WAD
itself) and checks it. On the card it goes to `DOOMS/DOOM1.WAD`, the music
to `DOOMS/MUSIC/` (below) and the program to the root as `DOOM.BIN`.
The SD card build in m9b-system (`../sdcard/make_sdcard.sh`) puts them all
there, from `doom19s.zip` or `DOOM1.WAD` in `data/` (`data/README.md`).

The options that matter:

- `-DCMAP256`: 8-bit palette indices, shown by the video engine's 8 bpp
  line mode.
- `-DM9B_DIRECT` (a small patch in `i_video.c`): `I_FinishUpdate` doesn't
  scale or copy, it just calls `DG_DrawFrame`. `-DDOOMGENERIC_RESX=320
  -DDOOMGENERIC_RESY=200` keeps doomgeneric's own (unused) buffer small.
- **picolibc's `crt0` and `picolibc.ld`,** set up with `--defsym`. `-T` is
  given explicitly, after the `--defsym`s, because `picolibc.ld` tests
  `DEFINED(__stack_size)` as it is read.
- **Arguments:** `main()` passes `-iwad DOOMS/DOOM1.WAD -mb 4`, a 4 MB zone.
  doomgeneric's default of 6 MB doesn't fit.

## Memory

PSRAM, `0x800000`–`0xFFFFFF`:

| Range | Use |
|---|---|
| `0x800000`–`0x8EA21F` | `doom.bin` (code, rodata, `.data` in place; entry at `0x800000`), then `.bss` |
| `0x8EA220`–`0xFEFFFF` | the heap, 7 MB (the 4 MB zone, the three 64 KB display buffers) |
| `0xFF0000`–`0xFFFFFF` | the 64 KB stack |

(`../core/runtime/m9b.ld`, which all the ports link with. While it loads a program,
the core firmware keeps its data and stack in the top 128 KB and the boot
screen at `0xF80000`; both are free once Doom runs.)

## Video

The video engine doubles each line in hardware: the 8 bpp 2× line mode,
with columns 40..679 set by `VIDEO_HWIN`. Doom's 320 × 200 is shown as
640 × 480, each Doom line on 2 or 3 of the 480 line-table entries: DOS's
320 × 200 filled a 4:3 screen with pixels 1.2 times taller than wide, and
this is closer to that than 640 × 400 (the 720 × 480 picture has
non-square pixels itself, so it's not exact).

It's triple buffered, the way DOS Doom used the VGA's pages. Doom draws
straight into one of three display buffers (`I_VideoBuffer`); one is on
screen and one may be waiting for the next vertical blank. At the end of a
frame, `DG_DrawFrame` cleans the buffer from the data cache and queues it.
The vertical-blank interrupt (line 480; the engine fetches each line two
lines ahead, so the 43 lines of blanking leave about 1.3 ms, and the
rewrite takes about 0.15 ms) points the 480 table entries at it and loads
the palette if Doom changed it. Nothing waits: a frame that
wasn't shown yet is simply replaced by the next.

Doom relies on its screen keeping the last frame (the status bar, for one,
is only redrawn where it changes), so the buffer it goes on in is brought
up to date first, by copying only what changed:

- Everything drawn outside the 3D view goes through `V_MarkRect`, which
  DOS Doom used to copy just the changed area to video memory. It records
  each frame's changed columns per row; so does the 3D view window when
  the view was drawn. Per buffer, the rows and columns changed since it
  was last drawn into are copied from the newest frame.
- The part inside the view window is left until `D_Display` starts the
  next frame: if it is about to redraw the whole view (in a level, no
  automap, no wipe, no view size change), it is dropped, otherwise copied.
- `am_map.c` and `f_wipe.c` kept their own copy of the `I_VideoBuffer`
  pointer; they now take it every frame. `R_VideoErase`, which wrote the
  screen without marking it, now marks it.

In the demos that is about 1.2 ms of copying per frame instead of 3.7 ms.
`make TIMEDEMO=1 EXTRA=-DM9B_FULLCOPY` copies the whole screen at every
switch instead (trivially right); its frame checksums (below) match.

## Sound

`port/sound_m9b.c` is doomgeneric's sound module (`-DFEATURE_SOUND
-DM9B_SOUND`). The core plays 48 kHz stereo from two 1 KB halves of 256
frames (5.33 ms) and interrupts when it moves on from one (see
`../core/README.md`, Audio). The interrupt mixes the next 256 frames of the
active channels into that half's buffer in PSRAM, cleans it from the data
cache and writes its pointer register, so latency is 5–11 ms.

- **Samples:** Doom's effects are DMX lumps, 8-bit unsigned 11025 Hz mono,
  loaded from the WAD the first time each plays and kept (`PU_STATIC`).
- **Mixing:** at 12 kHz, each result held for 4 output frames: nearest
  sample in 16.16 fixed point (as DMX did), left and right volumes from
  Doom's separation (Chocolate Doom's formula), the first channel writing
  the buffer and the others adding with clipping. With nothing playing,
  both halves point at a silent buffer and nothing is mixed.
- **Channels** are changed with interrupts masked.

**Cost** (timedemo, `sound x%` in its report, on the 27 MHz design with
the direct-mapped data L1): 1–4% of the CPU, tic 2000 at 149.2 s against
144.8 s without sound. Mixing at 48 kHz took 8–9% (about 55 cycles per
output frame for one channel), and a first version that summed into a
2 KB accumulator slowed the demo by 12%: the data cache pays for
everything the interrupt touches.

## Music

Doom's music is MUS scores played on an OPL2 FM chip. Neither fits here:
an OPL2 core (jtopl2, synthesized alone for this FPGA: 1,337 logic cells, 4
BSRAM) needs more than the design has left, and a software FM synth would
take an estimated 10–15% of the CPU. So the music is rendered once on a PC
and streamed from the SD card.

**Rendering** (`tools/make_music.sh WAD OUTDIR [--upload] [LUMP...]`):
`musrender` plays each `D_*` lump with libADLMIDI (Nuked OPL3 emulation,
one chip, Doom's own GENMIDI bank "DMX (Bobby Prince v2)", DMX's volume
model), then ffmpeg makes 24 kHz 16-bit mono (OPL2 is mono) with about
8 dB of gain. The shareware's 13 tracks are 29 minutes, 81 MB. With
`--upload` they go to `DOOMS/MUSIC/<lump>.WAV` on the card through the
board (`../core/tools/sdput`, about 80 KB/s, so about 17 minutes).

**Playback** (`DG_music_module` in `port/sound_m9b.c`): `I_RegisterSong`
gets only the MUS data, so the module finds its lump name by the cached
lump pointer. `PlaySong` opens the WAV (24 kHz mono 16-bit checked) and a
256 KB ring buffer in PSRAM (5.5 s, enough to cover a level load) is kept
full from `Poll`, once per frame, at most two 16 KB reads per call. The
audio interrupt takes a sample per two output frames, scaled by the music
volume (Doom's 0..15 times 8; the default, 8, plays the render as it is,
higher settings boost it, clipped: it was half that, too quiet next to the
effects), and the effects are added on top. At the end of the file it seeks
back when Doom asked for looping. A song without a file is silent.

## Performance

`make TIMEDEMO=1` builds `build-td/doom.bin`, which runs `-timedemo demo1`
and prints, every 250 game tics, the time since the first frame, a
checksum of the frame and the file reads so far; every 5 s the frame rate
and where the time goes; every 30 s a PC-sampling profile
(`tools/profile.py build-td/doom.elf <log>`); and the memory timings
(`port/membench.c`) at startup. All of that is `M9B_PROFILE`, which only
the timedemo build sets. A
timedemo draws one frame per tic, so runs are comparable, and the
checksums must match between builds (checkpoints are by tic, not frame:
the screen wipe before the demo takes a varying number of frames).
`make BASELINE=1` leaves out the renderer changes; `EXTRA=-D...` with
`B=<dir>` is for experiments.

Time to frame 2000 of demo1 (27 MHz design; these runs counted frames,
not tics):

| Build | s | fps |
|---|---|---|
| baseline (flip by busy-waiting for line 440, RANGECHECK) | 181.0 | 11.0 |
| + `R_DrawColumn`/`R_DrawSpan`: locals, 4 pixels per iteration | 157.2 | 12.7 |
| + page flip in the vblank interrupt | 144.5 | 13.8 |
| + RANGECHECK off | 141.3 | 14.2 |
| + drawing straight into the display buffers (above) | 138.0 | 14.5 |

In normal play (the attract-mode demos) that is 12–14 fps, about 12.8 on
average (9.5 before these changes): at that rate each frame also runs 2–3
game tics.

**Memory layout mattered by a few percent** with the direct-mapped data
L1 the core had at first. Reading the WAD with `read()` instead of stdio
(below) made the demo about 3% slower (tic 2000: 144.7 s against 140.3 s,
the same frames), though it reads almost nothing during play: the code
that doesn't matter moved by 100 bytes, and some memory moved with it.
Placing the display buffers at different offsets from a 16 KB boundary
alone changed the time by up to 2%: a column of the screen touches 200 of
the cache's 256 lines, so what else is hot (colormaps, textures) and where
it lands decided the conflict misses. The data L1 is 2-way now
(`../core/README.md`), which was worth 14% on the timedemo.

**Loading:** the level (about 1.8 MB of lumps) now loads its data in 1.7 s
(4.6 s before):

| WAD reads | Level load (reads) |
|---|---|
| stdio `fread`, one CMD17 per sector, SPI_DATA a byte at a time | 4.58 s |
| `read()` (straight into Doom's buffer), CMD18 for runs of sectors | 3.56 s |
| + SPI_STREAM, 32 bits per transfer (core bitstream) | 1.73 s |

**Where the time goes now** (profile): `R_DrawColumn` 26%, `R_DrawSpan`
14%, `R_RenderSegLoop` 4%.

**Things that didn't help:** packing 4 span pixels into one 32-bit store
(145.3 s, slower). Byte stores take 2–3 cycles even on a hit (membench),
but that isn't what limits the span loop.

**Limits:** the CPU runs at 36 MHz with little timing margin left (see
`../core/README.md`, Timing), so there is no easy clock headroom.

## Changes to doomgeneric

- `doomgeneric.c`: `DG_ScreenBuffer` is allocated as `RESX * RESY * sizeof(pixel_t)` instead of `* 4`.
- `i_video.c`: `I_FinishUpdate` returns right after `DG_DrawFrame()` with `M9B_DIRECT`.
- `r_draw.c`: with `M9B_FASTDRAW` (on unless `BASELINE=1`), `R_DrawColumn` and `R_DrawSpan` keep the source and colormap in locals (the byte stores could alias the globals, so GCC reloaded them for every pixel) and do 4 pixels per iteration, loads first.
- `d_main.c`: with `M9B_PROFILE` (timedemo build), times the game tics and `D_Display` for the frame-rate report.
- `doomdef.h`: `RANGECHECK` is off.
- For drawing straight into the display buffers (`M9B_DIRECT`, see Video): `v_video.c` (`V_MarkRect` reports to the platform layer), `r_draw.c` (`R_VideoErase` marks what it erases), `d_main.c` (`D_Display` tells the platform layer whether it will redraw the view, and when a frame ends normally), `am_map.c` and `f_wipe.c` (take `I_VideoBuffer` every frame).
- `w_file_stdc.c`: with `M9B_RAWWAD`, `open`/`lseek`/`read` instead of stdio.
- `i_sound.c`: doesn't include `SDL_mixer.h` with `M9B_SOUND`.
- With `M9B_MOUSE`: `i_input.c` (`I_GetEvent` calls `M9B_PollMouse` first) and `g_game.c` (`G_Responder` adds up mouse events instead of keeping the last).
- `m_config.c`: with `M9B_SAVEDIR`, the saves in a directory on the card next to the WAD (FatFs can't take `.` as a path component).
- `m_menu.c`: with `M9B_NOQUIT`, no "Quit Game" in the main menu (there is nothing to quit to).
- `r_main.c`, `r_draw.c`: with `M9B_MISSPROF` (measurement builds, with a `PERF_COUNTERS` bitstream), the L1 refills, write-backs and bus cycles per rendering phase.

## Next steps

1. **More speed:** a larger fetch cache, or low detail.
