# Doom's data

Put **one** of these here (it isn't in the repository: id Software's
shareware, freely distributable):

| What | Name here |
|---|---|
| id Software's shareware 1.9 distribution | `doom19s.zip` |
| the IWAD from it | `DOOM1.WAD` (4,196,020 bytes, MD5 `f0cefca49926d00903cf57551d901abe`) |

With `doom19s.zip`, the SD card build (`../sdcard/make_sdcard.sh` in
m9b-system) unpacks `DOOM1.WAD` from it into this directory
(`tools/get_data.sh data/doom19s.zip`, which needs `unzip` and `7z`).
Only the shareware episode works: the port opens `DOOM1.WAD`.

The music is rendered from the WAD (`tools/make_music.sh`); the card build
does that too.
