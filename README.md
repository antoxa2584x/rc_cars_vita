# RC Cars — PS Vita

A native PS Vita port of **RC Cars** (Creat Studios, 2003, PC), built on top of a
reverse engineering of the original game's data formats and physics.

The original engine is Direct3D 8 **fixed-function with no shaders at all**,
which is what makes the port tractable: vitaGL maps onto it almost one-to-one,
so there is nothing to translate. Handling is not an approximation either — it
is the game's own rigid-body integrator and contact solve, transcribed from the
disassembly, running on the game's own recovered constants and curves.

Status: builds a real `.vpk`, runs at 45–60 FPS at 960x544 in Vita3K. All ten
tracks, all three cars, terrain collision, props, audio and music.

## Controls

| input | action |
|---|---|
| R trigger | throttle |
| L trigger | brake / reverse |
| left stick | steer |
| right stick | camera orbit (springs back) |
| CROSS | boost |
| CIRCLE | jump; on the roof or a side, rights the car |
| START | menu — track, car, tuning parts, texture quality, quit |
| SQUARE | toggle shadow, water animation and checkpoint arrows |
| SELECT | free-fly camera |

## Layout

    build.sh            resources -> .vpk, in one command
    main.c              frame loop, input, vitaGL renderer
    rb.c contact.c      the transcribed integrator and contact solve
    collide.c col.c     sphere/terrain collision against the .col grid
    rbcar.c             car body: wheels, throttle, jump, respawn
    physics.c           arcade placeholder, no longer reachable from anywhere
    carani.c            the car rig, driven by the game's own animation proc
    cam.c               chase camera
    scene.c             .vsc loader and draw
    shadow.c water.c    projected car shadow, animated water
    checkpoint.c fx.c   checkpoint arrows, particles and tyre marks
    trace.c envmap.c    tyre marks on the ground, body env-mapping
    prop.c              the 13 knockable prop models
    mix.c audio.c sfx.c mixer thread, sceAudioOut, positional sound
    menu.c ui.c font.h  in-game menu
    *_data.h tracks.h   generated tables (see below)
    third_party/        minimp3, for the streamed music
    mintest/            20-line vitaGL app, to isolate runtime failures
    CMakeLists.txt      builds eboot.bin and packages the .vpk
    BUILD.md            the working notes: every fix, trap and measurement

## Quick start

You need your own copy of RC Cars. Put its files in `game_data/` at the root of
this tree and run `./build.sh`:

    mkdir game_data
    cp -r "/path/to/RC Cars"/{RCCars.pack,RCCarsDB,Tracks,Autoexec.gm,GameIcon.ico} game_data/
    cp "/path/to/RC Cars"/header.jpg game_data/     # optional, see below
    ./build.sh

or, from an install the script can reach, let it link them itself — 330 MB that
does not need copying:

    ./build.sh --import "/path/to/RC Cars"

That converts every resource and produces `build/rccars_viewer.vpk`. Each of its
nine stages is skipped when its output is already newer than its input, so the
second run goes straight to compiling; `-s` runs one stage, `--force` redoes
everything, `--help` lists the rest.

| `game_data/` | used by |
|---|---|
| `RCCars.pack` | the textures, the wavs, Settings and Splines |
| `RCCarsDB/` | the `.sb` scene databases — tracks, cars, props |
| `Tracks/` | the soundtrack MP3s |
| `Autoexec.gm` | the playlist that orders them |
| `GameIcon.ico` | the bubble icon |
| `header.jpg` | the LiveArea wallpaper and gate — **optional**; without it the LiveArea is derived from the icon |

Nothing under `game_data/` is written to, and none of it is in this repository.

## What the build produces, and what is not tracked

`assets/` and the `sce_sys/` artwork are **generated from your own copy of the
game** and are deliberately untracked — around 480 MB of converted geometry,
collision grids, textures, sounds and music derived from a commercial release.
`build.sh` is the whole pipeline:

| stage | tool | output |
|---|---|---|
| `unpack` | `unpack_tiox.py` | 1,875 files out of `RCCars.pack` |
| `lightmap` | `sb2obj.py` | the lightmap atlases embedded in each `.sb` |
| `tracks` | `pack_vsc.py`, `pack_col.py` | ten `.vsc` scenes and their `.col` grids |
| `cars` | `pack_vsc.py` | three rigged cars, with shadow and env-map data |
| `props` | `pack_props.py` | the 13 knockable props, one file for all tracks |
| `sound` | `pack_snd.py` | 118 sounds at 22050 Hz, plus 18 MP3s |
| `tables` | `gen_tracks.py`, `gen_font.py`, `gen_sce_sys.py` | `tracks.h`, `font.h`, the app art |
| `check` | `vsc_check.py` | every packed scene, exit 0 = clean |
| `build` | cmake + make | `build/rccars_viewer.vpk` |

The tools live in `rccars_re/`, alongside this repository; point `--re` at them
if they are somewhere else. `BUILD.md` explains why each conversion flag is what
it is, and the script cross-references it at the interesting ones.

Several tracked headers are generated too and are marked as such at the top of
the file — `tracks.h`, `font.h`, `rb_data.h`, `physics_data.h`, `prop_data.h`,
`fx_data.h`, `vis_data.h`. Regenerate them rather than editing by hand. The
`rb_*`/`physics_*`/`prop_*`/`fx_*`/`vis_*` ones come from the disassembly rather
than from `game_data/`, so `build.sh` does not touch them.

## Runtime formats

Two container versions, both written by `rccars_re/`. Older revisions still load
and simply leave the newer subsystems dark.

| `.vsc` scenes | adds |
|---|---|
| VSC3 | textures with mip chains, batches with a sky flag |
| VSC4 | the car's rig — parts, plus a part index per batch |
| VSC5 | markers and the shadow radius |
| VSC6 | lightmaps: a second UV per vertex, a lightmap index per batch |
| VSC7 | an env-map class per batch, plus normals for the batches that have one |

| `.col` collision | adds |
|---|---|
| COL1 | uniform XZ grid, downward ray |
| COL2 | one material byte per triangle |
| COL3 | the water surface height per cell |

## Build

`./build.sh` does all of this; what follows is what it runs and why, for when
something goes wrong.

Requires VitaSDK, Python with Pillow, and a **custom vitaGL build**. Stock
vitaGL creates two GXM contexts — one for its splash screen — and Vita3K
supports one, so `vglInit` fails and the app dies dereferencing null at `0x78`
(`./build.sh --build-vitagl` does this for you):

    git clone https://github.com/Rinnegatamante/vitaGL && cd vitaGL
    make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
         PRIMITIVES_SPEEDHACK=1 -j8
    make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
         PRIMITIVES_SPEEDHACK=1 install

The three speedhack flags are worth roughly half the frame rate; BUILD.md
explains why each is safe for this app specifically. Then:

    export VITASDK=/usr/local/vitasdk
    export PATH=$VITASDK/bin:$PATH        # a separate statement — see below
    mkdir -p build && cd build
    cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ..
    make -j8                              # -> rccars_viewer.vpk

Packaging notes that cost time once already:

- the vdpm package is `libmathneon`, not `mathneon` — the latter reports
  "Successfully installed" while its tar silently fails and installs nothing;
- `taihen` is required even for plain homebrew, because `SceShaccCgExt` (pulled
  in by vitaGL) references `taiHookRelease`. Link `stdc++` too: those deps are
  C++ and a pure-C link fails on missing `std::__throw_*`;
- `export VITASDK=x PATH=$VITASDK/bin:$PATH` expands `$VITASDK` *before* it
  assigns it, so `PATH` gets `/bin:` and the build dies with
  `arm-vita-eabi-gcc: No such file or directory`. Two statements;
- Vita3K needs `libshacccg.suprx` at `ur0:data/external/`.

`physics.c`, `rb.c`, `contact.c`, `collide.c`, `rbcar.c`, `cam.c` and `carani.c`
are compiled with `-fno-fast-math -ffp-contract=off`. This is not tidiness: the
original runs x87 at PC=53 over float32 state, which ARM reproduces exactly only
if operation order is preserved, and `-ffast-math` permits reassociation.

## Known limitations

- No wall collision — ground queries only.
- One batch of 77 has no texture, showing as black patches.
- `CenterMassOY` is still unrecovered, so part of the body collision proxy is
  fitted rather than read from the game's data.
- RGB565 channel order differs between real hardware and Vita3K; the port
  carries a runtime switch for it ("Texture colours" on the menu), defaulting to
  the hardware-correct packing.
- The app crashes on exit — teardown only.

## Legal

Source only. This repository contains no game data, artwork or audio. RC Cars is
the property of its respective rights holders; you need your own copy of the
game to build anything runnable from this tree.

`third_party/minimp3.h` is CC0 (see `third_party/LICENSE.minimp3`).
