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

## Assets are not in this repo

`assets/` and the `sce_sys/` artwork are **generated from your own copy of the
game** and are deliberately untracked — around 480 MB of converted geometry,
collision grids, textures, sounds and music derived from a commercial release.

They are produced by the tools in `rccars_re/`, which lives alongside this
repository. In short:

    DB="/path/to/RC Cars/RCCarsDB"
    RE=../rccars_re

    for t in beach_1 beach_2 beach_3 beach_4 country_1 country_2 \
             country_3 country_4 urban_1 urban_2; do
        python3 $RE/pack_vsc.py "$DB/$t.sb" assets/$t.vsc --markers
        python3 $RE/pack_col.py "$DB/$t.sb" assets/$t.col
    done
    # the three cars, with rig, shadow silhouette, env-map classes and effects
    # (see BUILD.md for the full --extra-tex lists)
    python3 $RE/pack_props.py assets/props.vsc
    python3 $RE/pack_snd.py --out assets
    python3 $RE/gen_tracks.py "$DB" tracks.h --assets assets
    python3 $RE/gen_sce_sys.py         # bubble icon and LiveArea art

`BUILD.md` has the exact commands, including the flags that matter.

Several tracked headers are generated too and are marked as such at the top of
the file — `tracks.h`, `font.h`, `rb_data.h`, `physics_data.h`, `prop_data.h`,
`fx_data.h`, `vis_data.h`. Regenerate them rather than editing by hand.

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

Requires VitaSDK and a **custom vitaGL build**. Stock vitaGL creates two GXM
contexts — one for its splash screen — and Vita3K supports one, so `vglInit`
fails and the app dies dereferencing null at `0x78`:

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
