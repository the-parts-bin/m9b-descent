# Descent on the m9b SoC

Descent 1 as a program for the SoC in `../core`: D1X Rebirth 0.40 (2006),
which is Parallax's Descent 1.5 source release with its assembly rewritten
in C and a platform layer split out, built for the shareware data. It runs
from PSRAM, uses picolibc, and reads the game data from the SD card
through the core firmware's SD driver and FatFs.

**Status:** it runs: pilots, menus, briefings, the levels, with sound
effects and keyboard (and mouse) control, at about 10 fps in the first
level (320 x 200, full cockpit). Music: the HMP songs, pre-rendered (Music).

Start it from the boot screen's browser (`DESCENT.BIN` on the card), or
with `../core/tools/run.py build/descent.bin`. The first start asks for a
pilot name and an input device.

**Input:** USB keyboards and mice on either USB port, and the terminal's
keys (arrows, Enter, Esc, printable characters), with Descent's controls
(configurable in Options, Controls).

**License:** the Parallax license (`d1x/license.txt`): free for
non-commercial use. The shareware data is freely distributable and isn't in
the repository (`tools/get_data.sh` fetches it).

## Sources

| Part | Upstream | Version | Here |
|---|---|---|---|
| The game | [DXX Rebirth](https://github.com/dxx-rebirth/dxx-rebirth)'s D1X Rebirth 0.40 (Parallax's Descent 1.5 source release, its assembly rewritten in C) | `cde0244a0` (March 2006) | `d1x/`, trimmed, with changes (Changes to D1X) |
| Sound slots | D1X's `arch/sdl/digi.c` | the same | `port/m9b_digi.c`, with the m9b mixer in place of SDL's |
| Music renderer | [Wohlstand/libADLMIDI](https://github.com/Wohlstand/libADLMIDI) | `ef98fab` (`../core/tools/midirender/build.sh`) | used by `tools/make_music.sh` on the PC |

## Why D1X and not the Parallax release

Parallax's release (1998) is DOS code for Watcom C and MASM: 66 assembly
files, among them all of the fixed-point maths (`FIX`, `VECMAT`), the whole
3D pipeline (`3D`: 11 files) and most texture mappers. D1X Rebirth 0.40 is
the same code with those rewritten in C, file access through stdio, and the
platform code (video, input, sound, timer) in `arch/`; its earliest version
in the DXX Rebirth repository (commit `cde0244a0`, March 2006) is still
plain C without later dependencies (PhysFS, OpenGL requirements, C++).

## Layout

| Path | Contents |
|---|---|
| `d1x/` | D1X Rebirth 0.40, trimmed to what the build uses (no editor, `ui`, `arch`, makefiles or assembly), with small changes (below) |
| `port/m9b_input.c` | start-up (traps, interrupts), the timer, keyboard (USB and UART) into `main/key.c`, the mouse (D1X's SDL mouse functions), joystick stubs |
| `port/m9b_video.c` | `gr_init`, `gr_set_mode`, `gr_update` and the palette functions; screenshots over the UART |
| `port/m9b_digi.c` | D1X's `arch/sdl/digi.c` (sound slots, sounds linked to objects and positions) with the m9b audio mixer in place of SDL's |
| `port/m9b_tmap.c` | the perspective texture mapper by 16-pixel spans (`-tmap m9b`) |
| `port/m9b_mask.c` | the cockpit mask: the 3D view's scanlines cut to the cockpit's transparent runs (Performance) |
| `port/m9b_memprof.c` | measurement builds only (`EXTRA=-DM9B_MEMPROF LDEXTRA=-Wl,--wrap=memcpy`): `memcpy`'s callers and sizes |
| `../core/firmware/m9b_div.h` | 64-by-32-bit division with the hardware divider, for the fixed-point maths (in the core) |
| `port/m9b_posix.c` | `glob` (pilot and mission lists) over FatFs, `access`, `usleep`, `gettimeofday`, `stdin` |
| `port/key_arch.h` | the `ARCH_KEY_*` codes: USB HID usages |
| `port/joy.h`, `port/mouse.h`, `port/SDL/SDL.h` | headers from D1X's platform code, and a stub for the one SDL call left in `game.c` |
| `tools/get_data.sh` | fetches and extracts the shareware data |
| `tools/make_music.sh`, `hmp2mid.py`, `hogx.py` | render the music to WAV files on a PC (see Music) and optionally copy them to the card |
| `tools/newgame.sh` | loads a build and starts level 1 from the UART (tests) |
| `../core/runtime/syscalls.c`, `string.c`, `trap.c`, `trap_entry.S` | the runtime all the ports share (in the core): files through FatFs, the UART console, misaligned access emulation, the trap handler and PC-sampling profiler |

## Building and the SD card

`make` builds `build/descent.bin` (about 700 KB). `make B=build-prof
EXTRA="-DM9B_PROFILE -DPROF_SIZE=0x00100000u"` reports frames per second,
sound time and emulated misaligned accesses every 5 s and the PC profile
every 15 s (`../core/tools/prof.py ELF LOG`); add `-DM9B_PERF` with a
`PERF_COUNTERS` bitstream for the L1 bus counters.

The game data goes in `data/` (`data/README.md` says what goes there).
The whole card, all the games with their data and music, is built in
m9b-system, which has this repository as a submodule:
`../sdcard/make_sdcard.sh` (see `../sdcard/README.md`) takes the data from
here. The scripts below add or replace files on a card in the board,
through the serial port.

The card needs `DESCENT/descent.hog` and `DESCENT/descent.pig` (the 1.4
shareware: 2,339,773 and 5,092,871 bytes), and `DESCENT.BIN` in the root for
the boot screen's browser. The pilot files and `descent.cfg` are written to
the root. `d1x.ini` in the root can add command-line options.

There's no command line: `main()` (in `main/inferno.c`) uses `-320x200
-menu_gameres -automap_gameres -nocdaudio -nojoystick -tmap m9b`.

## Video

The game draws into its 320 x 200 8 bpp screen; `gr_update()` copies it
into one of three display buffers, cleans it from the data cache and has
the vertical-blank interrupt show it (stretched to all 480 lines, 2 or 3
per line of 320 x 200, the 4:3 DOS's 320 x 200 filled), loading the
palette (6-bit entries plus the gamma setting, as RGB565) at the same
time. Palette fades step once per vertical blank. Other modes are stretched
the same way (columns doubled up to 360 wide).

`../core/tools/m9bctl.py shot OUT.png` takes a screenshot of the running
game over the UART and `m9bctl.py keys ...` types keys.

## Sound

The audio interrupt mixes the next 256 frames of the 48 kHz output at
12 kHz (each result held for 4 frames): the playing slots' 8-bit 11025 Hz
samples, with D1X's pan and volume, and the music on top at 24 kHz.

## Music

Descent's songs are HMP files (HMI's MIDI variant) in `descent.hog`,
played through an OPL2 or General MIDI synthesizer, none of which fits
the CPU. `tools/make_music.sh descent.hog OUTDIR [--upload]` takes them out
(`tools/hogx.py`), converts them to standard MIDI (`tools/hmp2mid.py`: HMI's
delta times, the HMP tempo as the division at 1,000,000 us per quarter
note), renders them with libADLMIDI (`../core/tools/midirender`: Nuked
OPL3, the "HMI (Descent, Asterix)" bank, Descent's own `melodic.bnk` and
`drum.bnk`, HMI's volume model) and makes 24 kHz 16-bit mono at a common
loudness; `--upload` puts them in `DESCENT/MUSIC/` (GAME0.WAV for
game0.hmp, ...). The shareware's 8 songs are 25 minutes, 69 MB.

`digi_play_midi_song` (`port/m9b_digi.c`) opens the song's WAV file and
the main loop (`m9b_digi_poll`, once a frame) keeps a 256 KB ring (5.5 s)
filled from the card, as Doom's music; the audio interrupt adds a sample
per 2 output frames. D1X's music volume (0..128, 64 by default) scales it,
64 playing the render as it is. A song without a file is silent.

## Performance

About 10 fps in the first level, from 5.0 at first:
- **Texture mapping** (two thirds of the time at first): D1X's C mapper
  divides u and v by z for every pixel, 70 cycles or more with this CPU's
  divider. `port/m9b_tmap.c` computes the texture coordinates exactly every
  16 pixels, from one reciprocal of z (a 32-bit divide and two multiplies),
  steps linearly in between with u and v packed in one word (as Doom's span
  drawer), and does four pixels at a time with the loads first.
- **Division:** `fixdiv`, `fixmuldiv` and `fixdivquadlong` (point
  projection: one bit per iteration) now divide 64 by 32 bits with two
  hardware divides (`../core/firmware/m9b_div.h`), and `fixmulaccum` multiplies with
  `mul`/`mulh` instead of 16-bit halves.
- **The cockpit** is a raw 320 x 200 bitmap (not RLE) drawn over the whole
  screen every frame, with colour 255 (the 3D window) transparent: 12% of
  the time even a word at a time. Its opaque runs are now found once per
  bitmap and each frame copies just those (`2d/bitblt.c`), about 3%.
- **What the cockpit covers:** the full cockpit's 3D view is the top 133
  lines, which is exactly where the cockpit's first fully opaque row is,
  but the pillars and the top of the dashboard still cover 22% of the
  view. While the view is rendered, the texture mappers' scanlines are cut
  to the cockpit's transparent runs (`port/m9b_mask.c`, through `SCANLINE`
  in `texmap/ntmap.c` and `tmapflat.c`): 6% less time in the mapper, about
  3% overall once the splitting's own cost is paid.
- **Drawing the cockpit once:** D1X redrew the whole cockpit every frame
  (`update_cockpits(1)`, and `init_gauges()` with it so every gauge
  redrew too). Now the whole cockpit is drawn only when needed (a mode
  change, or `reset_cockpit()` after a menu), and each frame only its rows
  over the 3D view (0..132), where sprites and window clears still draw
  over the pillars; the gauges sit in transparent holes of the cockpit and
  redraw themselves when they change (`main/game.c`). Cockpit and gauges
  went from 8% of the time to 1%.
- **Key polling:** every key query polls, and each poll checked all 256
  keys for repeats (6%); now only held keys are checked, and a key event
  finds its key through a table (`main/key.c`).

Where the time goes now, timed directly (the profile build's phase
timers): `render_frame` about 84% of the time, of which the texture
mapper's scanlines 37% (about 40 cycles per pixel) and the rest of
`draw_tmap` (polygon and scanline set-up) about 20%; the cockpit and
gauges 1%; game logic and `gr_update` the rest.

The vertical-blank PC profile over-counts the texture mapper here (it
showed 65% where the timers measure 37%), so the timers are the better
guide for Descent. The L1 bus counters
show the data L1 busy about 9% of the time and the fetch L1 about 12%
(the code path per frame is much larger than the 8 KB fetch L1).

## Changes to D1X

Marked `M9B`:
- `main/inferno.c`: the built-in options (above).
- `main/game.h`: `Game_double_buffer` 0, so the 3D view is drawn straight
  onto the screen instead of off-screen and copied (about 3%).
- `maths/fixc.c`: the divisions and `fixmulaccum` (Performance).
- `texmap/scanline.c`: `-tmap m9b` selects the span mapper.
- `2d/bitblt.c`: the word-at-a-time masked copy, and the cached opaque
  runs of big masked bitmaps (the cockpit).
- `main/key.c`: the key polling shortcuts.
- `texmap/ntmap.c`, `texmap/tmapflat.c`: scanlines through `SCANLINE`
  (the cockpit mask); `main/game.c`: the mask set around `render_frame`,
  the cockpit drawn whole only when needed, and the phase timers (profile
  builds).
- `2d/scalec.c`: the scaled bitmaps (explosions, powerups) keep color 255
  transparent, as the original's assembly scaler did (D1X's C version drew
  it, as white rectangles).
- `include/strutil.h`, `misc/strutil.c`: picolibc's `strupr`/`strlwr`.

## Next steps

- Music: the end-of-level and end-game songs (not in the shareware).
- Speed: group the hot code (the fetch L1 misses), the texture mapper's
  lighting and texture loads. (Page flipping instead of
  `gr_update`'s copy would save 2%, but menus and the status bar rely on
  the screen keeping the last frame.)
- The pilot files and configuration in `DESCENT/` instead of the root.
