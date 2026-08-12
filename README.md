<div align="center">

# RC Cars · PS Vita

**A native PS Vita port of *RC Cars* (Creat Studios, 2003), rebuilt from the original game's data.**

[![platform](https://img.shields.io/badge/platform-PS%20Vita-003791?style=flat-square)](https://vitasdk.org)
[![toolchain](https://img.shields.io/badge/toolchain-VitaSDK%20%2B%20vitaGL-5c2d91?style=flat-square)](https://github.com/Rinnegatamante/vitaGL)
[![language](https://img.shields.io/badge/language-C-00599c?style=flat-square)](#)
[![status](https://img.shields.io/badge/status-playable-2ea043?style=flat-square)](#roadmap)
[![fps](https://img.shields.io/badge/on%20hardware-45--60%20FPS%20%40%20960x544-orange?style=flat-square)](#)

10 tracks · 3 cars · 129 knockable props · the game's own physics, transcribed

Runs on a **real Vita** at 45–60 FPS, full 960×544.

</div>

---

The original engine is Direct3D 8 **fixed-function, with no shaders at all**. That
is what makes this port tractable: vitaGL maps onto it almost one-to-one, so
there is nothing to translate.

The handling is not an approximation either. It is the game's own rigid-body
integrator and contact solve, transcribed from the disassembly and running on
constants recovered from the game's own Settings and Splines data — per-car
acceleration curves, grip, drag, steering lock, suspension rates.

## ⚡ Quick start

You need your own copy of RC Cars. Put its files in `game_data/` and run the
build script:

```bash
mkdir game_data
cp -r "/path/to/RC Cars"/{RCCars.pack,RCCarsDB,Tracks,Autoexec.gm,GameIcon.ico} game_data/
./build.sh
```

Or point it at an install and let it link them itself — 330 MB that does not need
copying:

```bash
./build.sh --import "/path/to/RC Cars"
```

Either way you end up with `build/rccars_viewer.vpk`, ready for VitaShell.

<details>
<summary><b>What goes in <code>game_data/</code></b></summary>

<br>

| file | used for |
|---|---|
| `RCCars.pack` | the textures, the wavs, Settings and Splines |
| `RCCarsDB/` | the `.sb` scene databases — tracks, cars, props, characters |
| `Tracks/` | the soundtrack MP3s |
| `Autoexec.gm` | the playlist that orders them |
| `GameIcon.ico` | the bubble icon |
| `header.jpg` | the LiveArea wallpaper and gate — **optional**, and without it the LiveArea is derived from the icon instead |

Nothing under `game_data/` is written to, and none of it is in this repository.

</details>

<details>
<summary><b>Build script options</b></summary>

<br>

`build.sh` runs nine stages and skips any whose output is already newer than its
input, so the second run goes straight to compiling.

| stage | tool | output |
|---|---|---|
| `unpack` | `unpack_tiox.py` | 1,875 files out of `RCCars.pack` |
| `lightmap` | `sb2obj.py` | the lightmap atlases embedded in each `.sb` |
| `tracks` | `pack_vsc.py`, `pack_col.py` | ten `.vsc` scenes and their `.col` grids |
| `cars` | `pack_vsc.py` | three rigged cars, with shadow and env-map data |
| `props` | `pack_props.py` | the 13 knockable models, one file for all tracks |
| `sound` | `pack_snd.py` | 118 sounds at 22050 Hz, plus 18 MP3s |
| `tables` | `gen_tracks.py`, `gen_font.py`, `gen_sce_sys.py` | `tracks.h`, `font.h`, the app art |
| `check` | `vsc_check.py` | every packed scene — exit 0 is clean |
| `build` | cmake + make | `build/rccars_viewer.vpk` |

```
-s, --stage S     run only these stages          -f, --force       redo everything
-t, --track T     restrict to these tracks       --no-music        skip the 60 MB of MP3s
-j, --jobs N      parallel make jobs             --rate HZ         sound bank rate
--import DIR      populate game_data/            --build-vitagl    install the custom vitaGL
--game DIR        use this instead of game_data/ --clean           wipe build/ first
```

The conversion tools live in `rccars_re/`, alongside this repository; point
`--re` at them if they are somewhere else.

</details>

## 🎮 Controls

| input | action |
|:--|:--|
| **R** / **L** | throttle / brake · reverse |
| **left stick** | steer |
| **right stick** | camera orbit — springs back |
| **✕** | boost |
| **○** | jump; on the roof or a side, rights the car where it stands |
| **START** | menu — track, car, tuning parts, texture quality, quit |
| **□** | toggle shadow, water animation and checkpoint arrows |
| **SELECT** | free-fly camera |

## 🚧 Roadmap

The port drives. What it is missing is the game *around* the driving — and most
of it is already sitting in the data, recovered and unused.

#### AI opponents

- [ ] The race module: grid, laps, wrong-way detection, finish
- [ ] Opponent driving on the transcribed physics, so they share the car's model
- [ ] Racing lines from `Profiles/*.gpf`, the same path format the road cars use
- [ ] Wire up the AI sound families already in the bank — `carAI_*`, `motorAI_*`,
      11 names that `snd.dat` lists and nothing currently plays

#### NPCs and the dynamic layer

- [ ] **59 characters** the artists placed and the port renders none of: Dog ×9,
      Seagull ×7, Truck ×6, Guard ×6, Spider ×6, Crab ×4, Vulture ×4, and more
- [ ] They are keyframed node hierarchies, **not skinned** — a TRS keyframe per
      node per sample, the identical structure `carani.c` already drives for the
      car rig. AIChars has 815 animation nodes over 13 sequences; `people.sb` 1,359 over 19
- [ ] Behaviour is fully specified in `Settings/`: `dog.ini` is a vision cone and
      a chase, `seagull.ini` walks then takes off, `guard.ini` shoots
- [ ] `MOD_INSTANCE` resolution in `pack_vsc.py` — instances name objects in a
      *separate* database, which is why they currently flatten to nothing

#### Game interface

- [ ] A real front end: the original's `Interface.sb` and `RaceTextures.sb` are
      right there in `RCCarsDB/`
- [ ] In-race HUD — position, lap, timer, speedo
- [ ] Results and standings screens
- [ ] Replace the debug menu with something that is not a debug menu

#### Progression

- [ ] Championship structure over the ten tracks
- [ ] Upgrades: the three tyre levels and the turbo sets are already packed per
      car, and `carparts.c` already shows the fitted parts on the model
- [ ] Unlocks — cars, tracks, parts
- [ ] Prize money and a shop

#### Save data

- [ ] Player profile in `ux0:data/`
- [ ] Best laps and ghosts — the install has a `GhostRecords/` folder to learn the format from
- [ ] Settings persistence, so texture colours and camera choices survive a restart
- [ ] Multiple profiles, as the original's `Players/` does

#### Graphics

- [ ] Fix the one batch of 77 with no texture — visible black patches
- [ ] Wall collision; ground queries are all there is today
- [ ] Better shadows than one projected silhouette
- [ ] Richer particles and surface effects on top of the existing dust and tyre marks
- [ ] Push texture quality — the pack ships three full sets and the port pins one
- [ ] MSAA, distance LOD, and a look at what the frame budget will take

<details>
<summary><b>Known limitations today</b></summary>

<br>

- One batch of 77 has no texture, showing as black patches.
- No wall collision — ground queries only.
- `CenterMassOY` is still unrecovered, so part of the body collision proxy is
  fitted rather than read from the game's data.
- RGB565 channel order differs between real hardware and Vita3K. The port carries
  a runtime switch for it ("Texture colours" in the menu), defaulting to the
  hardware-correct packing.
- The app crashes on exit — teardown only.

</details>

## 📁 Layout

```
build.sh              resources -> .vpk, in one command
main.c                frame loop, input, vitaGL renderer
rb.c contact.c        the transcribed integrator and contact solve
collide.c col.c       sphere/terrain collision against the .col grid
rbcar.c               car body: wheels, throttle, jump, respawn
physics.c             arcade placeholder, no longer reachable from anywhere
carani.c              the car rig, driven by the game's own animation proc
cam.c                 chase camera
scene.c               .vsc loader and draw
shadow.c water.c      projected car shadow, animated water
checkpoint.c fx.c     checkpoint arrows, particles and tyre marks
trace.c envmap.c      tyre marks on the ground, body env-mapping
prop.c                the 13 knockable prop models, 129 placements
mix.c audio.c sfx.c   mixer thread, sceAudioOut, positional sound
menu.c ui.c font.h    in-game menu
*_data.h tracks.h     generated tables — regenerate, do not hand-edit
third_party/          minimp3, for the streamed music
mintest/              20-line vitaGL app, to isolate runtime failures
BUILD.md              the working notes: every fix, trap and measurement
```

<details>
<summary><b>Runtime formats</b></summary>

<br>

Two containers, both written by `rccars_re/`. Older revisions still load and
simply leave the newer subsystems dark.

| `.vsc` scenes | adds |
|---|---|
| VSC3 | textures with mip chains, batches with a sky flag |
| VSC4 | the car's rig — parts, plus a part index per batch |
| VSC5 | markers and the shadow radius |
| VSC6 | lightmaps: a second UV per vertex, a lightmap index per batch |
| VSC7 | an env-map class per batch, plus normals for the batches that have one |
| VSC8 | the props file — a different layout from the scenes above |

| `.col` collision | adds |
|---|---|
| COL1 | uniform XZ grid, downward ray |
| COL2 | one material byte per triangle |
| COL3 | the water surface height per cell |

</details>

<details>
<summary><b>Building by hand, and the traps</b></summary>

<br>

`./build.sh` does all of this. What follows is what it runs and why, for when
something goes wrong.

Requires VitaSDK, Python with Pillow, and a **custom vitaGL build**. Stock vitaGL
creates two GXM contexts — one for its splash screen — and Vita3K supports one,
so `vglInit` fails and the app dies dereferencing null at `0x78`
(`./build.sh --build-vitagl` handles this):

```bash
git clone https://github.com/Rinnegatamante/vitaGL && cd vitaGL
make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
     PRIMITIVES_SPEEDHACK=1 -j8
make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
     PRIMITIVES_SPEEDHACK=1 install
```

The three speedhack flags are worth roughly half the frame rate; BUILD.md
explains why each is safe for this app specifically. Then:

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH        # a separate statement — see below
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ..
make -j8                              # -> rccars_viewer.vpk
```

Packaging notes that cost time once already:

- the vdpm package is `libmathneon`, not `mathneon` — the latter reports
  "Successfully installed" while its tar silently fails and installs nothing;
- `taihen` is required even for plain homebrew, because `SceShaccCgExt` (pulled
  in by vitaGL) references `taiHookRelease`. Link `stdc++` too: those deps are
  C++ and a pure-C link fails on missing `std::__throw_*`;
- `export VITASDK=x PATH=$VITASDK/bin:$PATH` expands `$VITASDK` **before** it
  assigns it, so `PATH` gets `/bin:` and the build dies with
  `arm-vita-eabi-gcc: No such file or directory`. Two statements;
- Vita3K needs `libshacccg.suprx` at `ur0:data/external/`.

`physics.c`, `rb.c`, `contact.c`, `collide.c`, `rbcar.c`, `cam.c` and `carani.c`
are compiled with `-fno-fast-math -ffp-contract=off`. This is not tidiness: the
original runs x87 at PC=53 over float32 state, which ARM reproduces exactly only
if operation order is preserved, and `-ffast-math` permits reassociation.

</details>

## ⚖️ Legal

Source only. This repository contains **no game data, artwork or audio** — you
need your own copy of the game to build anything runnable from this tree. RC Cars
is the property of its respective rights holders.

`third_party/minimp3.h` is CC0; see `third_party/LICENSE.minimp3`.
