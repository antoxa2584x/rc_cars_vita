# RC Cars — PS Vita track viewer

First native Vita target for the reverse-engineered assets. Renders a converted
track through vitaGL's fixed-function pipeline, which is the natural fit: the
original engine is Direct3D 8 fixed-function with **no shaders at all**, so
there is nothing to translate.

## Layout

    main.c            vitaGL renderer + flying camera
    CMakeLists.txt    builds eboot.bin and packages the .vpk
    rccars_re/        the RE tools, a submodule -- every packer, generator and
                      host test harness below lives in here
    assets/*.vsc      packed scenes (produced by rccars_re/pack_vsc.py)
assets/menu.vsc   the MAIN MENU's art -- textures only, no geometry
dlg_data.h        the engine's own DIALOG LAYOUTS, out of Settings/dlg*.ini
                  (rccars_re/gen_dlg_data.py)
    assets/props.vsc  the 13 knockable props + the !HIT! banner, VSC8
                      (rccars_re/pack_props.py)
    assets/boot.vsc   THE LOADING SCREEN's four textures and nothing else --
                      Desktop, logoRC, logo1C, logoCR (2 MB, so the screen is up
                      before menu.vsc's 29 MB is read)
    assets/intro.vid  THE LAUNCH MOVIE, H.264 + a frame index + the part table
    assets/intro_N.mp3  each of the movie's three parts' own audio
                      (both from rccars_re/pack_vid.py; see intro.h)
    sce_sys/          bubble icon + LiveArea art (rccars_re/gen_sce_sys.py)
    mintest/          20-line vitaGL app, used to isolate runtime failures

## Toolchain setup (done on this machine)

The RE tools are a submodule, so the clone has to bring them along:

    git clone --recurse-submodules git@github.com:antoxa2584x/rc_cars_vita.git

    # already cloned without them, or the pointer moved:
    git submodule update --init rccars_re

Every `rccars_re/...` path below is that submodule, relative to this repository
root, and every command in this file is run from the root. `build.sh --re DIR`
(or `RCCARS_RE`) still points at a checkout somewhere else.

    git clone https://github.com/vitasdk/vdpm && cd vdpm
    export VITASDK=/usr/local/vitasdk
    ./bootstrap-vitasdk.sh
    ./vdpm vitaGL vitashark SceShaccCgExt libmathneon taihen

Two packaging gotchas worth remembering:

- the package is `libmathneon`, not `mathneon` — the latter reports
  "Successfully installed" while its tar silently fails and installs nothing;
- `taihen` IS required even for a plain homebrew app, because `SceShaccCgExt`
  (pulled in by vitaGL) references `taiHookRelease`. Link `stdc++` too — those
  deps are C++ and a pure-C link fails with missing `std::__throw_*`.

## Build

    export VITASDK=/usr/local/vitasdk PATH=$VITASDK/bin:$PATH
    python3 rccars_re/pack_vsc.py "<...>/RCCarsDB/beach_1.sb" assets/beach_1.vsc
    python3 rccars_re/pack_props.py assets/props.vsc \
        --extra-tex msg_hits          # the 13 knockable props, ONE file for all
                                      # ten tracks -- plus the game's own !HIT!
                                      # banner, which belongs in this file
                                      # because it is the only LOAD-ONCE scene
    mkdir -p build && cd build
    cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ..
    make -j8            # -> rccars_viewer.vpk

Current `beach_1` payload: 74 textures (3.7 MB of pixels), 75 draw batches,
57,636 vertices, 52,358 triangles, 5.1 MB scene file, 2.9 MB vpk.

## App identity: the bubble and the LiveArea

The app is **`RC Cars`** (`VITA_APP_NAME`, which becomes both `TITLE` and
`STITLE` in `param.sfo`), title ID `RCCV00001`. The title ID is deliberately
unchanged — it is the install path, and renaming it orphans anything already on
a device rather than upgrading it.

The art is the game's own, converted by `rccars_re/gen_sce_sys.py`:

| slot | size | format | source |
|------|------|--------|--------|
| `sce_sys/icon0.png` | 128x128 | 8-bit indexed | `GameIcon.ico`, its 256x256 frame |
| `livearea/contents/bg.png` | 840x500 | 8-bit indexed | `header.jpg` |
| `livearea/contents/startup.png` | 280x158 | 8-bit indexed | `header.jpg` |
| `livearea/contents/template.xml` | — | — | style `a1`, background + gate |

### All three are 8-bit INDEXED, and that is an install-time requirement

The first build carrying an icon shipped them as 24-bit truecolour (PNG colour
type 2), which is what Pillow's `save()` writes for an RGB image, and
**VitaShell refused to install the vpk with `0x8010113d`** — package promotion
rejecting a LiveArea asset. The failure is at *install* time, so it presents as
a corrupt or oversized vpk rather than as an art problem, and nothing in the
build says a word. Size, `param.sfo` and archive layout were all already
correct; the colour depth was the only difference.

`gen_sce_sys.py` writes colour type 3 at bit depth 8 now, with an adaptive
palette and Floyd-Steinberg dithering. 256 colours is not much for a
photographic background — `bg.png` has 221,544 unique colours as generated — but
against the truecolour reference the cost measures **PSNR 33.2 dB on `bg`, 32.8
on `startup`, 35.3 on the icon**, and at LiveArea scale the dither is not
visible. The files also come out about 3x smaller.

Two guards, because a stale build directory would look fixed:

    python3 rccars_re/gen_sce_sys.py                    # regenerate, verify on disk
    python3 rccars_re/gen_sce_sys.py --check-vpk build/rccars_viewer.vpk

`verify()` re-reads each IHDR after writing; `check_vpk` asserts the same of the
art *inside* the packaged vpk, which is the file that actually gets promoted.
`save8` deliberately does **not** pass `optimize`: with it Pillow is free to
shrink the palette and drop the bit depth to 4 or 2. It also pads the palette to
a full 256 entries, so even the flat icon stays at 8.

Three things about the sizes are load-bearing:

- **They are exact.** The shell does not scale a mis-sized image, it rejects it,
  and a rejected background leaves the LiveArea blank with no error anywhere.
- **`GameIcon.ico` is multi-frame** — 256, 64, 48, 32, 16 — and PIL hands back
  the *last* frame unless you assign `.size` first. Opened naively the icon is
  the 16x16 one upscaled to 128, which looks like a decoding bug rather than a
  frame-selection one.
- **`header.jpg` is 460x215, i.e. 2.14:1 against the LiveArea's 1.68:1.** A
  cover-crop to 840x500 throws away a third of the width — including the logo,
  which sits on the right — and upscales 2.3x. `gen_sce_sys.py` puts a darkened,
  blurred cover behind a contained copy instead, so the whole header survives at
  1.83x. The gate is 1.77:1 and close enough to crop directly.

Neither slot has an alpha channel; the icon is flattened onto black.

## Status: RUNNING

`beach_1` renders in Vita3K at **60 FPS, 960x544**, textured, with a free
camera. Screenshots: `vita_fixed.png` (overview), `vita_shot.png` (ground level).

Two real bugs had to be fixed to get here.

### 1. vitaGL creates two GXM contexts; Vita3K supports one

Symptom: `vglInit` returns 0, then `sceGxmCreateContext` returns
`SCE_GXM_ERROR_ALREADY_INITIALIZED` and the app dies with a read violation at
`0x78` (vitaGL does not check the return before dereferencing the context).

Reproduced with a 20-line app (`mintest/`), so it was not the renderer or the
scene loader. Cause: vitaGL calls `init_gxm_context()` twice --
`VGL_CONTEXT_MAIN` in `vgl.c`, and `VGL_CONTEXT_SPLASHSCREEN` in
`splashscreen.c`. The second `sceGxmCreateContext` fails on Vita3K.

Fix: rebuild vitaGL from source with the splash screen compiled out.

    git clone https://github.com/Rinnegatamante/vitaGL
    cd vitaGL
    make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
         PRIMITIVES_SPEEDHACK=1 -j8
    make NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 INDICES_SPEEDHACK=1 \
         PRIMITIVES_SPEEDHACK=1 install

**The three speedhack flags are not decoration** -- see "The library build is
half the frame rate" below for what each one is for and why each is safe for THIS
app specifically. `NO_SPLASHSCREEN=1` on its own builds a library that works and
hands back a large part of the draw time this port measured on hardware.

Set `PATH` in its own statement: `export VITASDK=x PATH=$VITASDK/bin:$PATH`
expands `$VITASDK` before it assigns it, so `PATH` gets `/bin:` and the build dies
with `arm-vita-eabi-gcc: No such file or directory`.

Wrong turns worth not repeating: `HAVE_SHARK=0` does not exist (only
`HAVE_SHARK_LOG`); patching `sceGxmVshInitialize` to `sceGxmInitialize` does not
help; switching Vita3K's backend between Vulkan and OpenGL does not help.

### 1b. The library build is worth half the frame rate

Stock vitaGL, for every vertex attribute NOT backed by a VBO, scans the whole
index buffer for its highest index (`ffp.c:1472`) and then memcpy's
`top_idx * stride` bytes into a fresh GPU-mapped temp buffer (`ffp.c:1587`) --
per attribute, per draw call, per frame. On a lightmapped batch that is three
attributes at a 28-byte stride, so **84 bytes copied per visible vertex per
frame**, ~5.2 MB a frame on beach_1. Measured on hardware at 0.67 us/tri, 69% of
the frame. Full working in SCENE VERTEX BUFFERS in `scene.h`.

| flag | what it removes | why it is safe here |
|---|---|---|
| `DRAW_SPEEDHACK=2` | the scan and the copy, for draws over 32 KB | every buffer the port hands GL is on the newlib heap, the one region vitaGL maps for the GPU (`mem_utils.c:535`) |
| `INDICES_SPEEDHACK=1` | index-buffer handling | costs instanced draws and 32-bit indices; this port draws `GL_TRIANGLES` with `GL_UNSIGNED_SHORT` and nothing else |
| `PRIMITIVES_SPEEDHACK=1` | per-draw primitive fixups | glitches `GL_LINES`/`GL_POINTS`; this port draws only `GL_TRIANGLES` and `GL_TRIANGLE_FAN` |

`DRAW_SPEEDHACK=1` (every draw, not only the large ones) is deliberately NOT
used. It buys little now that `scene.c` keeps the static geometry in buffers --
what is left on client pointers is small -- and it removes the size guard below.

**THE BSS RULE THIS CREATES.** Once the copy is gone GXM reads the app's own
pointer, and only the newlib heap is mapped: an array in BSS lives in the app's
image, which the GPU cannot see. `fx.c` (344 KB at a full emitter), `trace.c`
(48 KB) and `envmap.c` all used `static vtx_t v[N]` and all three cross the 32 KB
line, so all three now `malloc` on first draw. **Anything new that hands GL a
pointer has to do the same.**

Note `vglInitExtended` still *returns* 0 on Vita3K even when initialisation
succeeds -- do not treat its return value as fatal.

### 2. pack_vsc.py collapsed vertices across meshes

The de-index key was `(vertex_index, uv_index)`, but a batch merges every mesh
sharing a texture and those indices are mesh-local -- so vertex 5 of one mesh
aliased onto vertex 5 of another. On screen: shredded silver geometry. Fixed by
keying on `(mesh_id, vertex_index, uv_index)`. Vertex count for `beach_1` went
42,406 -> 57,636 with the triangle count unchanged, which is the tell.

## Reading the log on a real Vita

`sceClibPrintf` writes the debug channel, which Vita3K shows and **retail hardware
does not**. Everything this port diagnoses with — the frame breakdown, the
per-wheel contact dump, the packing counts, the audio residency — was therefore
invisible on the only machine whose numbers matter.

`rlog.c` writes both. After a run, pull:

    ux0:data/rccars/rccars.log

`remove()`d and recreated at each launch -- `fopen(..., "w")` alone does NOT
reliably truncate here, and a log that is half this run and half the last one has
already cost one wrong diagnosis: 4,400 lines of which the first 2,728 were the
current session and the rest, after one torn line, an older and longer one, which
read as "the characters stopped loading half way through the session" because the
stale tail had no `.chr` load lines in it. **If a log ever looks like it changes
behaviour part way down, check for a torn line first.** Flushed after every line
(so a crash keeps the line that says why), and capped at 4 MB. Nothing may call `rlog` per frame — it costs a
memory-card write per line, and the existing callers are per second or per load.
Off the Vita it is stdout, so the host harnesses link `rlog.c` unchanged.

**TRIANGLE dumps an inventory** — every character near the eye with its model, variant,
position, distance, drawn/culled and the GL texture id each of its slots resolves to
(`!` = nothing bound, so the model draws white; `?` = bound but no image ever uploaded,
so the sampler is undefined), then every prop within 30 m and every opponent. Park
facing the thing on screen, press TRIANGLE, read the log: it is the only way to put a
NAME to geometry in a screenshot, and three rounds of screenshots is what it cost to
learn that.

Two lines are worth going straight to:

    [rccars] frame N us = sim N + draw N + swap N (N% waiting on the GPU)
    [rccars] draw: N batches N tris (N keyed), culled N batches N tris (N% of the track)

Read together they separate the two ways a spot can be slow. If `tris` jumps and
`culled` drops, the camera can genuinely see more geometry there and the cost is
submission. If neither moves but `swap` climbs, it is fill — overdraw — and the
geometry is a red herring.

## libshacccg.suprx

Vita3K needs Sony's runtime shader compiler at
`<Vita3K data>/ur0/data/external/libshacccg.suprx`; vitaGL calls into it during
init. It ships on retail Vita firmware, so this is an emulator-only setup step.
Installed on this machine.

## Mipmaps and channel order (fixed)

### RGB565 channel order: RED in the high bits, and Vita3K disagrees

This section used to say the opposite, on the strength of a `..._U5U6U5_BGR` seen
in vitaGL's source. That constant is a **transfer** format on the cube-map path.
The path this port actually takes -- `glTexImage2D(GL_RGB, GL_RGB,
GL_UNSIGNED_SHORT_5_6_5)` -- hits `fast_store`, memcpy's the pixels verbatim, and
selects `SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB` (`textures.c:797`). The SDK spells that
same enum `SCE_GXM_COLOR_FORMAT_R5G6B5` (`gxm.h:343`): **red in the high bits**.

So `pack_vsc.py` packs `((r>>3)<<11) | ((g>>2)<<5) | (b>>3)`, plain 565.

The swap is nearly invisible on grey rock and green foliage (grey is unaffected;
green sits in the untouched middle field) and unmistakable on strongly red/blue
content: sand turns blue and the sea turns gold. That is what identified it both
times -- first in Vita3K with the packing red-high, and then on real hardware with
it blue-high, which is the direction that was actually wrong.

**Vita3K reads `U5U6U5_RGB` the other way round from the hardware.** Correct assets
therefore look R/B-swapped in the emulator. The port carries a switch for it --
the menu's "Texture colours" row, `scene_set_tex_swap_rb` -- defaulting to the
hardware-correct order. On a real Vita, leave it alone.

Migrating assets packed the old way does **not** need a repack: the change is a
permutation of bytes already in the file.

    python3 rccars_re/fix565.py --check assets/*.vsc     # report, change nothing
    python3 rccars_re/fix565.py assets/*.vsc             # rewrite in place

It decides which order a file is in by decoding the **source .csi** and comparing,
not by a statistic over the pixels -- a warmth heuristic got `car1.vsc` wrong,
because the Overkill's paint is genuinely blue. `vsc_check.py` asserts the order
permanently.

### Mipmaps

`.csi` files already carry full mip chains, so nothing needs generating at
runtime -- `pack_vsc.py` now stores every level and the app uploads each with
`glTexImage2D(..., lvl, ...)` and switches to `GL_LINEAR_MIPMAP_LINEAR`
(staying on `GL_LINEAR` for single-level textures, so the chain is never
incomplete).

Scene file format bumped to **VSC2** for the extra `mip count` field.
`beach_1`: 74 textures, **573 mip levels**, 5.0 MB of pixels, 6.4 MB scene,
3.6 MB vpk. Still 60 FPS.

## Driving build

The viewer now loads a track **and** a car, with a chase camera.

    python3 rccars_re/pack_vsc.py "<...>/RCCarsDB/beach_1.sb" assets/beach_1.vsc
    python3 rccars_re/pack_vsc.py "<...>/RCCarsDB/Car.sb" assets/car1.vsc --subtree Car1 --rig

`pack_vsc.py --subtree NAME` pulls one model out of a shared database.
`Car.sb` holds three cars (folders `Overkill`, `Buggy`, `Hummer`), each with a
top-detail model plus LOD1/2/3 and an antenna: `Car1`/`Car2`/`Car3` are the
high-detail ones, ~3,800 triangles and 8 textures each.

Car geometry is already in metres -- Car1 measures 0.24 x 0.35 x 0.49, correct
for an RC car against a 107 x 228 unit track. No scaling needed. Forward is +Z
(the longest axis).

## Full asset build -- ten tracks, three cars, the menu

    DB="/mnt/c/Games/RC Cars/RCCarsDB"
    RE=rccars_re

    # every track: geometry, then the collision grid.
    #
    # RUN build.sh INSTEAD OF THIS unless you have a reason not to. The lines
    # below are here to be read, and they are the ones build.sh runs -- but three
    # of the flags are not optional and every one of them fails SILENTLY:
    #
    #   --extra-tex trackmap_<n>  the minimap's painted art, referenced by no
    #                             mesh and bound by name at runtime. Without it
    #                             the scene packs, loads and drives, and the
    #                             minimap is simply blank. `n` is the ENGINE's
    #                             level number, not the port's -- it comes from
    #                             `gen_hud_data.py --print-maps`, and vis_test
    #                             part 12 is what catches a wrong one
    #   --csidir                  the unpacked textures
    #   --embdir                  the per-track EMBEDDED lightmap atlases, whose
    #                             names (lightmap___rds_00..) are the SAME on
    #                             every track. Point this at the wrong directory
    #                             and a track is lit with another track's
    #                             lightmaps, or with none
    for t in beach_1 beach_2 beach_3 beach_4 \
             country_1 country_2 country_3 country_4 urban_1 urban_2; do
        n=$(python3 $RE/gen_hud_data.py --print-maps | awk -v t=$t '$1==t{print $2}')
        python3 $RE/pack_vsc.py "$DB/$t.sb" assets/$t.vsc --markers \
            --extra-tex "trackmap_$n" \
            --csidir .cache/extracted --embdir .cache/lightmaps/embedded
        python3 $RE/pack_col.py "$DB/$t.sb" assets/$t.col \
            --settings .cache/extracted/Settings
    done

    # THE FRONT END's art, out of Interface.sb -- which carries NO GEOMETRY: it is
    # the engine's own manifest of what the interface is made of, in named
    # folders ("Dialog Textures", "Shot textures", "Track preview", "Control
    # sounds"). Packing it gives a scene of 0 batches and 30 textures, which is
    # exactly what mainmenu.c wants. --imgdir is for the player portrait: the
    # FacesSys images are TARGA, not .csi, so the .csi index cannot see them.
    #
    # Smash20/Smash26 are deliberately NOT on this list. They live in
    # game_data/Language/, outside the pack, and ride in props.vsc instead;
    # main.c falls back to that scene for both.
    IF=Desktop,Podl_LeftTop,Podl_RightTop,Podl_LeftBottom,Podl_RightBottom
    IF=$IF,HeaderSkin,ButtonsTextures,Button_race,Button_back,logoRC_Main
    IF=$IF,Face1,enumarrows
    for i in 1 2 3 4 5 6 7 8 9; do IF=$IF,ButtonPodl_right_$i; done
    for t in beach1 beach2 beach3 beach4 country1 country2 country3 country4 \
             urban1 urban2; do IF=$IF,shot_${t}_0; done
    python3 $RE/pack_vsc.py "$DB/Interface.sb" assets/menu.vsc \
        --extra-tex "$IF" --csidir .cache/extracted \
        --embdir .cache/lightmaps/embedded --imgdir .cache/extracted/FacesSys

    # the three cars. --shadow-tex bakes the top-down silhouette the projected
    # shadow uses and fits its radius to the car; --envmap classifies the body's
    # env-mapped parts and packs their vertex normals for the glance (writes
    # VSC7); --extra-tex packs the upgrade tyre levels and the three alternate
    # SKINS, neither of which any mesh references, plus the two effect sprite
    # sets -- `dust`, which both particle systems share, and the four
    # `t_halfdry_tire2_<n>` tyre marks.
    #
    # The skin lists are not symmetric: car 1 has one atlas page and the other
    # two have two, which is FUN_0049fc80's own switch and also the shipped art
    # (no car_bskin1<n> exists). See "The paint" in CLAUDE.md. Dropping the skins
    # from a car's list is the supported way to get the ~4 MB back -- carparts.c
    # then reports one skin and the menu's Skin row pins itself at 1/1.
    FX=dust,t_halfdry_tire2_1,t_halfdry_tire2_2,t_halfdry_tire2_3,t_halfdry_tire2_4
    S1=car_askin12,car_askin13,car_askin14
    S2=car_askin22,car_askin23,car_askin24,car_bskin22,car_bskin23,car_bskin24
    S3=car_askin32,car_askin33,car_askin34,car_bskin32,car_bskin33,car_bskin34
    python3 $RE/pack_vsc.py "$DB/Car.sb" assets/car1.vsc --subtree Car1 --rig \
        --shadow-tex 256 --envmap --extra-tex tire3_1,tire3_2,tire3_4,$S1,$FX
    python3 $RE/pack_vsc.py "$DB/Car.sb" assets/car2.vsc --subtree Car2 --rig \
        --shadow-tex 256 --envmap --extra-tex tire2_1,tire2_2,tire2_4,$S2,$FX
    python3 $RE/pack_vsc.py "$DB/Car.sb" assets/car3.vsc --subtree Car3 --rig \
        --shadow-tex 256 --envmap --extra-tex tire3_2,tire3_3,tire3_4,$S3,$FX

    # Only the CARS get --envmap. No track has an ENVIR_CAR_BODY or *_GRE<n> node,
    # so a track would pack VSC7 with nothing in the new field; tracks stay VSC6.
    # The per-car cost of --envmap is small because the normals are written ONLY
    # for the classified batches: 9 of the Overkill's 28, 1,500-odd vertices.

    # DO NOT pass --shadow-radius ShadowSize here. ShadowSize is the shadow
    # SOURCE's radius, not the texture's extent, and the Buggy's 0.29 is
    # shorter than its own 0.298 half-length -- the silhouette runs off the
    # texture and CLAMP_TO_EDGE smears that edge over the whole receiver.
    # The default fits the radius to the car and writes it into the .vsc.

    # the sound bank and the music. Reads the game's own Sound/ wavs, its
    # snd.dat volumes and Autoexec.gm's playlist: 118 sounds, 33 MB at 22050 Hz
    # mono, plus the 18 MP3s copied verbatim (60 MB) and streamed by minimp3.
    # --rate 44100 doubles the bank; --no-music skips the copy.
    python3 $RE/pack_snd.py --out assets

    # THE LAUNCH SEQUENCE's assets. Reads the SHIPPED CONFIGURATION -- the part
    # ranges come out of Scripts/intro.ini, not out of this line -- and needs
    # ffmpeg with libx264 and libmp3lame on the PATH.
    #
    # The Vita has no MPEG-1 decoder, so Tracks/Intro.dat's three parts are
    # transcoded to H.264 once, here. Two of the encoder settings are not
    # preferences: `-bf 0' so decode order is display order and one access unit
    # gives one picture, and `repeat-headers=1' so SPS/PPS ride in every IDR --
    # parts 2 and 3 begin mid-stream and a skip means the decoder has never seen
    # the sequence header when it gets there. An IDR is forced on each part's
    # first frame. pack_vid.py checks that last one and refuses to write a file
    # where it did not happen.
    #
    # TWO OF THESE ARGUMENTS ARE THIS PORT'S CHOICES, not the game's, and they are
    # made HERE rather than in the app because the runtime's job is to play every
    # part its table names -- which is what the engine does with intro.ini's.
    #
    # --parts 2,3 DROPS THE 1C LOGO: ten seconds of a publisher's logo every
    # launch. The packer then TRIMS -- only the frames the kept parts span are
    # written and the table is rebased onto them, so the dropped part's 2.5 MB
    # comes back. introtest DERIVES which of intro.ini's parts each shipped one
    # is, by matching lengths, so the ranges are still held against the shipped
    # configuration.
    #
    # --crop 640:368:0:56 REMOVES THE FILM'S BAKED-IN LETTERBOX. The source is a
    # 640x368 picture inside a 640x480 frame: 56 rows of STUDIO black -- Y = 16,
    # not 0 -- top and bottom, which is why `cropdetect' at its default threshold
    # of 24 reports crop=640:480:0:0 and sees nothing at all. Measured by taking
    # the per-row MAXIMUM of the luma plane over seven frames across the film and
    # counting rows at or below 18. Both 640 and 368 are multiples of 16, which
    # sceAvcdec requires and the packer checks. 640x368 is 1.739:1 against the
    # screen's 1.765, so it fills the screen at a 1.5% stretch; the Creat Studios
    # logo's content is rows 104..367 and survives with room either side.
    #
    # ~18 MB of video and 1.5 MB of audio. --width scales the picture down (the
    # frame is converted from NV12 and uploaded every tick); --crf and --maxrate
    # trade size for quality. DELETING assets/intro.vid is how this port spells
    # Config.gm's `AutoRunIntro' 0 -- the launch then goes straight to the
    # loading screen, which is what the engine does too.
    python3 $RE/pack_vid.py .cache/extracted/Scripts "$GAME/Tracks" assets \
        --parts 2,3 --crop 640:368:0:56

    # and the loading screen the movies hand over to: FOUR textures, its own
    # file. Not a corner of menu.vsc, because the whole point of the screen is
    # to be up BEFORE menu.vsc is read -- which is what the engine does as well
    # (FUN_004a25c0 case 0x13 loads these four by name, on demand, rather than
    # out of the interface manifest). main.c releases it after the last load.
    python3 $RE/pack_vsc.py "$DB/Interface.sb" assets/boot.vsc \
        --extra-tex Desktop,logoRC,logo1C,logoCR \
        --csidir .cache/extracted --embdir .cache/lightmaps/embedded

    # the track table the menu and the spawn read, and the menu font.
    # gen_tracks.py also reads each track's ambient sound bed out of its own
    # MOD_SNDCHANNEL node, so regenerate tracks.h whenever that changes.
    python3 $RE/gen_tracks.py "$DB" tracks.h --assets assets
    python3 $RE/gen_font.py font.h --size 18

    python3 $RE/vsc_check.py assets/*.vsc      # exit 0 = clean

`pack_col.py` writes **COL5**: the grid, plus one material byte per triangle and
a per-track default (what the surface sounds key on), the **water surface height
per cell** (COL3), the ENGINE's own surface class per triangle (COL4, what the
tyre marks key their strength on) and how bright the level's own LIGHTMAP is on
each triangle (COL5, what darkens the car under a bridge). Older grids still
load -- COL1 reports the default material everywhere, COL1 or COL2 report no
water anywhere, a pre-COL4 grid reports surface class 0 and a pre-COL5 grid has
no opinion about the lightmap -- so an un-repacked track runs; it just always
sounds like one surface, its rivers do not slow the car, its marks are one flat
strength and its car never goes dark. Repack all ten after touching `SURF_RE`,
anything about water, `eng_surface_class` or the lightmap bake.

**Every version so far APPENDS a trailing array and leaves the prefix -- header,
tris, cell_start, tri_idx -- byte-identical, and readers should depend on that
rather than on a list of magics.** `pack_ai.py` has its own small `.col` reader
for ground heights and whitelisted COL1-COL4; COL5 then broke `./build.sh` in a
module with no interest in lightmaps. It now takes any `COL`*n* and validates the
parse instead. **Three things read this format -- `col.c`, `pack_col.py` and
`pack_ai.py` -- so a version bump means grepping for all three.**

Six of the ten tracks have water (the four beaches, country_1, country_3);
country_2, country_4, urban_1 and urban_2 have none. The water is deliberately
NOT in the collision triangles -- the car fords the river rather than driving on
it -- and the height grid is what lets `carSurfaceDrag`'s water term fire at all.
See the WATER note at the top of `pack_col.py`, and `rccars_re/wetcheck.c`, which
checks the packed grids and prints every race start's depth.

**The `sea` cells are stored where the sea is DRAWN**, not where the artists
authored it: `pack_col.py` adds the track's own `WLOD_<track>_more1` `offset`
(and the `magnet` ramp, which is zero on every shipped track) to them, exactly as
`water.c` does when it displaces the surface. So `pack_col.py` reads
`Settings/` -- `--settings`, defaulting to the same directory `gen_vis_data.py`
uses, and through the same `wsurf_values()` so the two cannot be typed
differently. **Regenerate `vis_data.h` and repack all ten together**; a grid
packed against one `offset` and a header carrying another puts the waterline the
car feels somewhere the waterline it can see is not, which is a car that wades
through dry sand. `vis_test` part 3 checks the two shipped files against each
other on all ten tracks.

Host harnesses, none of which needs the Vita toolchain. **`rm` the binary first**
-- two of these lines had gone stale and did not link at all, and a failed link
leaves the previous binary sitting there to answer for it:

    rm -f rb_test vis_test carparts_test menuframe menu_test settings_test ui_test meshalign \
          rockroll allstarts track wetcheck proptest chartest ceiling audio_test \
          colprof flipped antheight aitest chrfloat curb hudshot wideline \
          mainmenu_test menushot results_test finishshot introtest player_test \
          garage_test net_test

    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rb_test.c rb.c contact.c collide.c rbcar.c carani.c cam.c \
        -lm -o rb_test                                             # physics + rig
    gcc -I. -Itestgl -O2 vis_test.c scene.c shadow.c water.c checkpoint.c \
        col.c carani.c rb.c contact.c collide.c antenna.c envmap.c trace.c fx.c \
        sun.c ai.c rbcar.c rlog.c carlight.c -lm -o vis_test        # rendering
                            # ai.c is on this line for part 14, the OPPONENTS'
                            # dust and smoke. The two fields fx reads off a car
                            # and the replay does not record -- the wheel contact
                            # points and the throttle -- are ai.c's, and a
                            # hand-built ai_car would only assert that
                            # ai_fake_contacts agrees with a copy of itself. So
                            # that fixture drives a REAL recorded lap on a REAL
                            # .col grid and reads the particles back through
                            # testgl, the same recorder the rest of the file uses.
    gcc -I. -Itestgl -O2 rccars_re/carparts_test.c carparts.c scene.c \
        carani.c rb.c contact.c collide.c rlog.c carlight.c \
        -o carparts_test -lm                           # upgrade parts
    gcc -I. -Itestgl -O2 rccars_re/menuframe.c scene.c carparts.c \
        antenna.c carani.c rb.c contact.c collide.c rlog.c carlight.c \
        -o menuframe -lm             # the MAIN MENU's car viewport framing
                            # AND IT IS WHAT SIZES THE CAR. MENU_CAR_MARGIN is
                            # 0.84 because this is the tightest value all three
                            # cars still frame at: 0.80 clips the Buggy sideways
                            # and 0.78 clips all three. Keep MARGIN here in step
                            # with main.c's and re-run after changing it.
                            # antenna.c is on this line because the framing has
                            # to leave the WHIP out of what it aims at, and
                            # antenna.c is the file that knows which rig part
                            # that is. carparts.c because the menu draws one
                            # exhaust, not four, and a box round three that never
                            # draw is not the picture's box.
                            # `./menuframe old' prints the two rules this
                            # replaced beside the current one -- the check has to
                            # be shown to fail.
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/proptest.c prop.c col.c rb.c rbcar.c contact.c collide.c \
        carani.c rlog.c -lm -o proptest                # the knockable props
    gcc -I. -Itestgl -O2 rccars_re/chartest.c char.c scene.c col.c \
        carani.c rb.c contact.c collide.c rbcar.c rlog.c carlight.c \
        -lm -o chartest
                            # the tracks' people, animals and road cars. scene.c
                            # is on this line for scene_read_texture, which the
                            # .chr texture table goes through so that quality,
                            # the 565 byte order and the mip rule cannot drift
                            # between the two formats. rbcar.c is on it for part
                            # 12, which drives a REAL car at a character to check
                            # that the character stops it. The GL stub lives in
                            # chartest.c and keeps a REAL MATRIX STACK -- a
                            # character is placed entirely through glTranslatef /
                            # glRotatef / glScalef, and a stub that discarded
                            # them could see the vertices and not where they went.
    gcc -I. -O2 rccars_re/carlight_test.c carlight.c contact.c rb.c \
        collide.c -lm -o carlight_test
                            # the CAR'S OWN LIGHT: the direction, light_car.bspl,
                            # the level's chase off the lightmap under the wheels,
                            # the two intensities, the LOD fade, and what reaches
                            # the RENDERER as a vertex colour. No GL at all --
                            # carlight.c is pure arithmetic, which is the whole
                            # reason the shading is on the CPU (carlight.h).
                            # contact.c/rb.c/collide.c are on
                            # the line for ONE symbol, rb_curve_eval: the curve is
                            # checked against the port's OTHER transcription of
                            # 0x0040f830 rather than against itself. 28 checks;
                            # 3 of 3 mutants die. See docs/render-car.md.
    gcc -I. -O2 rccars_re/menu_test.c menu.c contact.c rb.c collide.c \
        -lm -o menu_test    # the menu. The model is on this line because the
                            # booster row quotes rb_boost_capacity -- the menu
                            # names the tank size the upgrade buys.
    gcc -I. -O2 rccars_re/settings_test.c settings.c menu.c rlog.c \
        contact.c rb.c collide.c -lm -o settings_test
                            # the SETTINGS FILE -- ux0:data/rccars/settings.txt.
                            # menu.c is on the line for menu_init, which is where
                            # every default the file may omit comes from, and
                            # rlog.c because settings.c says what it read; the
                            # model tags along behind menu.c as it does for
                            # menu_test. Runs on files in the current directory
                            # through settings_set_path and removes them again.
                            # 64 checks; 21 of 23 mutants die, and the two that
                            # live are named in the file's own header.
    gcc -I. -O2 rccars_re/player_test.c player.c rlog.c -lm -o player_test
                            # THE PLAYER PROFILE and the game's own `.scp` format.
                            # Its first part is the one that matters and it needs
                            # a retail install:
                            #
                            #   ./player_test --game "/mnt/c/Games/RC Cars"
                            #
                            # It parses every profile in that install's Players/
                            # into player_t, writes it back and compares BYTE FOR
                            # BYTE -- 152,994 of them, the 128 KB portrait
                            # included. Without --game that part says SKIPPED
                            # rather than passing on nothing. The rest is the
                            # blank profile, the rank ladder rung by rung, create
                            # / select / remove with its refusals, the portraits
                            # through assets/faces.bin, and what a malformed file
                            # costs. 81 checks; 13 of 13 mutants die.
    gcc -I. -Itestgl -O2 rccars_re/ui_test.c ui.c hud.c countdown.c \
        race_ui.c dirarrow.c msg.c -lm -o ui_test
                            # menu drawing, the !HIT! banner, the 3-2-1-GO race
                            # start, the IN-RACE HUD -- the minimap, the place
                            # badge, the two clocks and the two gauges -- and the
                            # DIRECTION ARROW with its WRONG WAY banner. All five
                            # of those files -- six with msg.c -- are on this line
                            # because they draw THROUGH ui.c, so the same recorder reads back what
                            # really went on screen -- which CELL of which atlas,
                            # at the recovered size, in the recovered band, at the
                            # recovered angle, over the recovered sweep.
                            # 414 checks; 38 of 38 mutants die on part 17 and
                            # 27 of 27 on part 18.
    gcc -I. -Itestgl -O2 rccars_re/hudshot.c rccars_re/glrec.c ui.c \
        race_ui.c sfont.c dirarrow.c msg.c -lm -o hudshot
                            # NOT a test: it PRINTS the HUD's triangles, and
                            # rccars_re/hudshot.py composites them over the game's
                            # real .csi art into a 960x544 PNG. ui_test asserts
                            # where every quad went and cannot say whether the
                            # result looks like a HUD, which is the standing rule
                            # about verifying visually. Its most useful picture is
                            # the minimap: the car goes at the race start out of
                            # tracks.h (the .sb files) and the transform comes out
                            # of the exe, so an arrow landing ON the painted
                            # ribbon is two independent sources agreeing. It also
                            # draws the DIRECTION ARROW, which is the one element
                            # whose shape is not a rectangle -- a perspective
                            # projection of two chevrons cut into a grid of quads,
                            # and "does that look like an arrow pointing left" is
                            # not a question ui_test can answer.
                            #
                            #   ./hudshot 4 | python3 rccars_re/hudshot.py \
                            #       /tmp/hud.png --track 4
                            #
                            # AND read the picture as a pointer, not a verdict:
                            # rccars_re/mapcheck.py is what MEASURES the minimap,
                            # by holding the .vsc's own cp_N markers against the
                            # ribbon as painted in trackmap_<n>. Needs no build:
                            #
                            #   python3 rccars_re/mapcheck.py
    gcc -I. -Itestgl -O2 rccars_re/cpground.c scene.c checkpoint.c col.c \
        carani.c rb.c contact.c collide.c rbcar.c rlog.c carlight.c \
        rccars_re/glstub_host.c -lm -o cpground
                            # a probe: what the checkpoint GROUND probe finds at
                            # each candidate ceiling. col_ground_at returns the
                            # HIGHEST surface under its ceiling, so on a track
                            # with tunnels the ceiling decides floor or roof --
                            # nine of the fifty checkpoints used to resolve to a
                            # roof, putting the respawn and the marker on top of
                            # it. This is what sized CP_GROUND_CEIL.
    gcc -I. -Itestgl -O2 rccars_re/placechk.c scene.c checkpoint.c col.c \
        ai.c rb.c rbcar.c contact.c collide.c carani.c rlog.c carlight.c \
        rccars_re/glstub_host.c -lm -o placechk
                            # also a probe: the PLACING's two inputs on every real
                            # track -- monotonic AND moving. The second half is
                            # what caught three wrong fixes, since a frozen
                            # progress measure never goes backwards.
    gcc -I. -Itestgl -O2 -Wall rccars_re/progchk.c scene.c checkpoint.c col.c \
        ai.c rb.c rbcar.c contact.c collide.c carani.c rlog.c carlight.c \
        rccars_re/glstub_host.c -lm -o progchk
                            # and the third question placechk cannot ask: how far
                            # OUT is it? A measure can be monotonic, cover exactly
                            # a lap, and still be 100 m wrong at every instant in
                            # between -- which is what shipped. Drives the player
                            # along a recorded lap, where the truth is known
                            # exactly, scores BOTH sides of the placing against
                            # the road, and runs the RULE IT REPLACED beside it on
                            # the same frames as its own control. Exits non-zero.
    gcc -I. -Itestgl -O2 -Wall rccars_re/wideline.c scene.c checkpoint.c col.c \
        ai.c rb.c rbcar.c contact.c collide.c carani.c rlog.c carlight.c \
        rccars_re/glstub_host.c -lm -o wideline
                            # and the question all three above are blind to: the
                            # player is NOT on the recorded line. They all drive
                            # the player along a recording, which is the one lap
                            # on which an ODOMETER and the road are the same
                            # number -- so none of them could see the player's
                            # progress running ahead of the player by 10 to 23 m
                            # the moment the line got wide. Drives a lap three
                            # metres wide of the racing line and asks the
                            # placing's two rulers about the SAME CAR, with the
                            # odometer (still the fallback) run on the same
                            # frames as its control. Exits non-zero.
    gcc -I. -Itestgl -O2 -Wall rccars_re/mainmenu_test.c mainmenu.c touch.c \
        ui.c sfont.c records.c rlog.c player.c garage.c net.c ime.c \
        -lm -o mainmenu_test
                            # THE MAIN MENU's input and its HIT BOXES -- the half
                            # a picture cannot answer. The focus ring skipping the
                            # five unbuilt rows, a drag off a button cancelling it,
                            # a tap on a photograph walking the carousel, and the
                            # touch panel's own units (it reports 1920x1088 over a
                            # 960x544 screen and holds nothing on the release
                            # frame). The layout is checked as PROPERTIES -- the
                            # 45 px pitch with one 65 px gap, no two rows on one
                            # line, every row reaching the right edge -- not
                            # against a copy of its own table, and the QUICK
                            # RACE page's four enums walked a full lap each.
                            #
                            # AND THE THREE SIBLING PAGES: the navigation column,
                            # the per-page focus ring (every live stop visited
                            # once, no dead one landed on, a full lap coming
                            # home), dlgMAPINFO's WRAPPING shot list and every
                            # thumbnail of it being touchable, dlgSTAT's sort
                            # enum, and CIRCLE stepping back one page rather than
                            # out of the front end. Plus str_data.h's track order
                            # held against tracks.h through the engine's own
                            # internal names, and the record book behind
                            # `Track stats' -- the merge, the sort with `n/a'
                            # last, and a round trip through its own file.
                            # Exits non-zero.
    gcc -I. -Itestgl -O2 -Wall rccars_re/menushot.c rccars_re/glrec.c \
        mainmenu.c touch.c ui.c sfont.c records.c rlog.c player.c garage.c \
        net.c ime.c -lm -o menushot
                            # and hudshot's twin for the menu: it PRINTS the
                            # front end's triangles and rccars_re/hudshot.py
                            # composites them over the real .csi and .tga art.
                            # "Does it look like the game's own menu" is the only
                            # question that matters about a reproduction of a
                            # screenshot, and no assertion answers it. It drives
                            # the input too, so the picture can be of a state
                            # rather than of the idle screen:
                            #
                            #   ./menushot 800 600 | python3 rccars_re/hudshot.py \
                            #       menu.png            # beside the original
                            #   ./menushot 960 544 rrdd | ...   # two right, two down
                            #   ./menushot 960 544 t900,180 | ...  # a touch
                            #   ./menushot 800 600 m | ...      # Map and info
                            #   ./menushot 800 600 srr | ...    # Track stats,
                            #                                   # sorted by 5 laps
                            #   ./menushot 800 600 p | ...      # Select player
                            #   ./menushot 800 600 n | ...      # ...its name modal
                            #   ./menushot 800 600 y | ...      # ...Remove player?
                            #   ./menushot 800 600 e | ...      # ...the refusal
                            #   ./menushot 800 600 g | ...      # THE GARAGE
                            #   ./menushot 800 600 B | ...      # ...its booster
                            #   ./menushot 800 600 E | ...      # ...engine
                            #   ./menushot 800 600 T | ...      # ...and tyre page
                            #   ./menushot 800 600 Brr | ...    # ...picker two up
                            #   ./menushot 800 600 M | ...      # MULTIPLAYER
                            #   ./menushot 800 600 L | ...      # ...the LOBBY,
                            #                                   # and I / C / R
                            #
                            # `L', `I', `C' and `R' OPEN A REAL SOCKET and host a
                            # real game with a real second peer injected over the
                            # loopback -- the roster is what those pages are, and
                            # an empty table is a picture of nothing.
                            #
                            # `g', `B', `E' and `T' seed a roster too -- every
                            # figure on those two screens comes out of a profile
                            # and with none the four money bars all deny.
                            #
                            # `p', `n', `y' and `e' seed a roster of three
                            # profiles in menushot.tmp/ off fixed arithmetic, for
                            # the same reason `s' seeds the record book.
                            #
                            # `s' seeds the record book off fixed arithmetic, so
                            # the stats table has rows and the same run twice
                            # gives the same picture.
                            #
                            # 800x600 is the size to compare at: it is the frame
                            # every constant in mainmenu.c was measured in.
    gcc -I. -O2 -Wall rccars_re/net_test.c net.c rlog.c -lm -o net_test
                            # MULTIPLAYER, over two REAL sockets: it forks, the
                            # child hosts, the parent joins, and every datagram
                            # between them is a real one through net.c's POSIX
                            # backend. Discovery, the join, the roster, who owns
                            # the settings, the restrictions, the start, a car
                            # state crossing and being dead-reckoned, a kick, a
                            # bye, a peer that goes silent and a peer whose build
                            # is not ours -- and part 11, a remote car drawn
                            # BETWEEN two states: the midpoint of every field,
                            # the quaternion's hemisphere, the handover into
                            # dead reckoning, and a reordered datagram refused.
                            # 108 checks, 13 of 14 mutants dead (the survivor is
                            # shown equivalent in the file) plus 8 of 8 on the
                            # interpolation, exits non-zero. It binds UDP 3658
                            # and 3659 on the loopback; nothing leaves the
                            # machine.
    gcc -I. -O2 -Wall rccars_re/garage_test.c garage.c player.c rlog.c \
        -lm -o garage_test
                            # THE SHOP behind dlgSETCAR and dlgSETDETAIL: what a
                            # part costs, what it fetches back, which of the
                            # three levels may be bought or sold, and that
                            # nobody can sell their way out of a garage. The
                            # four figures the game's own screenshots pin --
                            # $100 of cash, $900 to sell the Overkill, $50 for
                            # its first booster, $65 for its first engine -- are
                            # asserted against championship.ini's tables rather
                            # than against a second copy of the numbers, and so
                            # is the one that looks like a typo and is not: the
                            # tyres are charged the RESONATOR's column, because
                            # that is what the retail exe does. 53 checks,
                            # 10 of 10 mutants dead, exits non-zero.
    gcc -I. -Itestgl -O2 -Wall rccars_re/results_test.c results.c touch.c \
        ui.c sfont.c -lm -o results_test
                            # THE FINISH SCREEN's ordering and its hit boxes.
                            # A finisher always beats one that did not, whatever
                            # the clocks say; finishers sort by time and the rest
                            # by how far behind they were; the gap is against the
                            # WINNER and only where both crossed. It also holds
                            # dlgFINISH.ini's own numbers to what the code
                            # assumes of them -- that column 0 is SQUARE (47.8 x
                            # 47.2), which is what says it is the portrait's and
                            # not Player's, and that six rows fit between the two
                            # rules. Exits non-zero.
    gcc -I. -Itestgl -O2 -Wall rccars_re/finishshot.c rccars_re/glrec.c \
        results.c touch.c ui.c sfont.c -lm -o finishshot
                            # and its picture, menushot's sibling:
                            #   ./finishshot 960 544 | python3 rccars_re/hudshot.py f.png
                            # The fixture is four racers with one still out on
                            # track when the flag fell, which is the row worth a
                            # picture -- `---' for a time it never set and a
                            # distance for its gap. It is what caught the column
                            # indexing being one out.
    gcc -I. -Itestgl -O2 -Wall rccars_re/introtest.c intro.c avc.c audio.c \
        mix.c ui.c touch.c rlog.c -lm -o introtest
                            # THE LAUNCH SEQUENCE. No H.264 decoder on the host,
                            # so NO CHECK IN IT IS ABOUT A PICTURE -- it says so
                            # out loud, and asserts that avc_open really did
                            # fail, so nobody reads it as if the pictures had
                            # been checked. What it does check is the half that
                            # can be quietly wrong:
                            #
                            # the shipped .vid's part table re-derived from the
                            # shipped Scripts/intro.ini and the header's own
                            # frame rate -- not from a copy of pack_vid.py's
                            # answer, which would only prove the file is itself;
                            # every part's first frame being an IDR, which is
                            # what a skip lands on; the frame index tiling the
                            # file exactly; intro_frame_at walked over all 2,616
                            # frames; the part sequence against FUN_004a3030,
                            # including that ONE press skips ONE logo and not the
                            # sequence (the thing a port is most tempted to get
                            # wrong, since "ESC skips the intro" is what it looks
                            # like from outside); seven kinds of lying header
                            # being refused; and the loading screen's layout
                            # against the recovered rects at three resolutions --
                            # all three logos SQUARE, which is why the port does
                            # not simply multiply the fractions by 960x544.
                            #
                            # AND EACH PART'S AUDIO BEING AT LEAST AS LONG AS ITS
                            # VIDEO, which is the one check here that guards a
                            # HANG rather than a wrong picture: the part ends when
                            # its frames are spent AND its clock has passed its
                            # length, and the clock is the audio. Exits non-zero.
                            # Shown to fail on six mutations: a part start moved
                            # off its IDR, an mp3 truncated, an mp3 deleted, the
                            # logos stretched instead of boxed, one skip made to
                            # end the whole sequence, and a shipped part's LENGTH
                            # shifted by 30 frames -- which is what the intro.ini
                            # derivation catches, and the reason that check
                            # matches on length rather than on frame number.
    gcc -I. -O2 rccars_re/audio_test.c mix.c audio.c sfx.c col.c \
        rb.c contact.c collide.c -lm -o audio_test                 # sound
    gcc -I. -O2 -fno-fast-math -ffp-contract=off rccars_re/curb.c \
        col.c rb.c rbcar.c contact.c collide.c carani.c \
        -lm -o curb                    # driving AT a low obstacle / a kerb
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/wetcheck.c col.c rb.c contact.c collide.c rbcar.c \
        -lm -o wetcheck                         # water, against the real grids
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/meshalign.c rb.c rbcar.c contact.c collide.c carani.c \
        -lm -o meshalign                        # drawn car vs physics car
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/rockroll.c rb.c rbcar.c contact.c collide.c \
        -lm -o rockroll                         # heave/pitch/roll, and inverted
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/allstarts.c col.c rb.c rbcar.c contact.c collide.c \
        carani.c -lm -o allstarts        # all ten REAL starts on the REAL .col
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/track.c col.c rb.c rbcar.c contact.c collide.c carani.c \
        cam.c -lm -o track                 # one hand-picked spawn, with tracing
    gcc -I. -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/ceiling.c col.c rb.c rbcar.c contact.c collide.c \
        carani.c -lm -o ceiling         # low overhead space: driving under roofs
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off -DCOL_PROFILE \
        rccars_re/colprof.c col.c rb.c rbcar.c contact.c collide.c \
        carani.c prop.c rlog.c -lm -o colprof   # where the SIM time goes
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/flipped.c scene.c carani.c col.c rb.c rbcar.c contact.c \
        collide.c rlog.c carlight.c rccars_re/glstub_host.c \
        -lm -o flipped                  # a car on its ROOF, on the real grids
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/aitest.c ai.c col.c rb.c rbcar.c contact.c collide.c \
        carani.c scene.c rlog.c carlight.c rccars_re/glstub_host.c \
        -lm -o aitest        # the AI opponents, on the real .aip and .col files
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/antheight.c scene.c antenna.c carani.c col.c rb.c rbcar.c \
        contact.c collide.c rlog.c carlight.c rccars_re/glstub_host.c \
        -lm -o antheight             # where the proxy is vs where the car is
    gcc -I. -Itestgl -O2 -fno-fast-math -ffp-contract=off \
        rccars_re/chrfloat.c char.c scene.c col.c carani.c rb.c contact.c \
        collide.c rbcar.c rlog.c carlight.c rccars_re/glstub_host.c \
        -lm -o chrfloat        # where the characters and their paths actually
                               # ARE, against the real grids. A probe, not a
                               # test -- it prints, it does not assert, and
                               # chartest part 13 is the assertions. glstub_host
                               # carries char.c's glTranslatef/glRotatef/
                               # glScalef for it, so nothing here may check a
                               # draw.

`flipped` and `antheight` link `rccars_re/glstub_host.c`, a no-op GL, because
they want `scene.c` for the geometry it loads and have no renderer. That is NOT
`testgl/`, which is vis_test's *recording* stub -- nothing in either harness may
assert on a drawing call, because a no-op stub cannot fail one. `antheight`'s own
header carried a build line that omitted the stub and had stopped linking when
`scene.c` gained its VBO calls; it is here now so that cannot happen quietly
again.

`chrfloat` and `antheight` are the two probes: they print numbers and assert nothing, so
read them, do not run them for a pass. Everything either one found is asserted in
`chartest` or `proptest`.

`allstarts` is the one to run after any change to the physics. It parks the car at
each track's own race start for 15 s and then drives it for 2 s, on that track's
real collision grid, and it is the only harness that can tell you a settled car is
*actually* settled on terrain rather than on a synthetic plane. Its driving window
is deliberately short: nothing steers, and the port has no wall collision, so a
longer run measures the crash instead of the suspension.

Note `track.c` still uses the OLD hand-picked beach_1 spawn, which is a 23° pier
edge the car rolls off. That is expected there; use `allstarts` for the real
starts. Its trajectory is chaotic by construction, so it is not a diff target --
a change that leaves `allstarts` byte-identical can still move `track` metres.

`ceiling` is the other half of the world. Everything else here drives on top of
geometry; `ceiling` drives UNDER it, which is the case `rb_coll_list` and
`rb_body_depenetrate` used to get exactly backwards. Run it after touching either
of those, or `col_sphere`, or anything to do with `rb_world_hit.normal`.

`flipped` is the third one: the car itself upside down. Every other harness here
drives a car the right way up, so the whole of the bodywork ABOVE the wheels was
a surface nothing ever tested standing on -- and it turned out the collision
proxy had nothing there at all, so an inverted shell sank into the terrain up to
its wheel arches on all ten tracks. It measures the DRAWN mesh out of
`car<n>.vsc` against the surface under it, deliberately not the proxy: the proxy
is what the fix changes, so asserting on it would be the change marking its own
work. Run it after touching the body spheres, `gen_rb_data.py`'s cdt block, or
`rb_body_depenetrate`.

Note that a car resting on its roof does not go perfectly still, and that is
expected rather than a regression: with no `carSubstepContact` bisection the body
ends each substep slightly inside the surface, gets lifted back out, and falls
about 1.5 mm before the gate fires again -- a buzz of ~0.17 m/s in place, just
above the 0.18 m/s the rest clamp needs to put the car to sleep. It stays put
(29 mm in 5 s against a 422 mm car). Checks around inverted cars should bind to
displacement or to a named axis, not to instantaneous speed; `rb_test` has both
forms and says so at each.

`colprof` is the sim's counterpart to the `sim / draw / swap` log line, and the
only harness that measures cost rather than behaviour. It drives the real car on a
real `.col` at the hardware log's own worst positions, then sweeps **every**
drivable cell in the grid, reporting microseconds per 1/60 tick alongside the
query counts that explain them (`-DCOL_PROFILE` turns those counters on; they are
compiled out of the app). It also carries the five checks that keep the broad
phase honest -- an over-eager reject is a hole in the world, not a slow frame, so
they compare `col_sphere` against an independent barycentric-lattice sampler, and
bound the reject ratio at 2% against the 0.9-1.2% every shipped grid measures.
Run it after touching `col.c`; it exits nonzero on a failure, and it takes a path
as well as a track name, so a trial `pack_col.py --cell` repack can be measured
without disturbing `assets/`.

Three of those lines were wrong and it cost real bugs:

- `rb_test` was missing `rbcar.c carani.c cam.c`, and `vis_test` `antenna.c`.
- worse, `rb_test`'s rig loader demanded the scene magic be exactly `"VSC4"`,
  and the cars have been packed as VSC5 and then VSC6 since markers and
  lightmaps landed -- so **every rig check silently skipped**, printing its
  "not VSC4" note into a wall of passing output. The rig had a real bug the
  whole time (the four left-hand springs aimed up into the body). It now
  accepts VSC4-and-up and treats a load failure as a FAILED check.
- `carparts_test` stopped LINKING when the lightmap pass added
  `glActiveTexture`/`glTexEnvi`/`glClientActiveTexture` to `scene.c`, because it
  supplies its own stubs and had none for those.

`carparts_test`, `meshalign`, `rockroll` and now `vis_test` run against the real
packed cars, so they must be run from `rccars_vita/` with `assets/` populated.
`carparts_test` is the one that would catch a repack that dropped `--extra-tex`.

`vis_test` joined that list over the tyre marks' width, and stayed on it when
that width stopped being measured off a mesh: the mark is now the engine's own
constant, and the only way to see that its INKED band matches the tyre is to
measure the tyre — off the real packed wheel meshes. It loads all three cars,
hence the `rbcar.c` on its link line (it needs a real `rb_car` to bind a rig to
a mesh). A missing car is a FAILED check, not a skip.

`vsc_check.py` is the other half, and it is what catches a repack that dropped
`--envmap`: it now reads VSC7 and asserts a car has env-mapped batches, that one
of them is the painted shell, and that every packed normal is unit length. Same
shape of assertion as the lightmap one, for the same reason -- the classes are
resolved by NAME MATCHING at pack time, so a rename in `Car.sb` leaves a car that
loads and draws perfectly and simply has no shine on it.

It asserts the same thing for the SKINS: all four on every `car_?skin<car>` page
the file carries, and the number of pages matching the CAR DIGIT in the names --
one for car 1, two for the others. The second clause is the one that catches a car
packed with *another* car's skins, which loads, looks right at skin 1 and repaints
into a different car's colours at skin 2.

    python3 rccars_re/vsc_check.py assets/car1.vsc assets/car2.vsc \
        assets/car3.vsc assets/beach_1.vsc

`meshalign` is the one to run after any change to `pack_vsc.py`, `rb_data.h` or
the car draw path: it puts each car on a flat plane, settles it, and checks that
the wheels you can SEE are where the physics has them. It exists because they
were not -- the mesh was drawn 57 mm (Overkill), 50-72 mm (Buggy) and 71 mm
(Hummer) above the surface the physics was standing on.

`vis_test` compiles the three visual modules against `testgl/`, a RECORDING
stand-in for vitaGL, and reads back the vertices they submit -- so the shadow's
UV orientation, the checkpoint billboard's placement and the water's damping are
checked as numbers rather than by eye. `testgl/` is compile-only and is not in
CMakeLists; it cannot affect the Vita build.

About 85 MB of assets, a 36 MB vpk.

`gen_tracks.py` reads each track's own `Players/Player` instance -- the race
start position, heading and starting car -- so the app spawns where the game
does. `--assets` makes it list only tracks that are actually packed, so the menu
can never offer one that will not load.

`gen_font.py` bakes a monospace face into `font.h` for the menu. The game has no
reusable glyph atlas: `Faces/` and `FacesSys/` are avatar portraits and
`Interface.sb` carries only dialog skins.

Pass `--rig` for all three cars. `CAR_RIG` is the union of the game's own three
node tables (`0x00572fcc`, `0x005731b8`, `0x005733f0`), so one flat list covers
all three procs: Car1 packs 22 parts, the Buggy 30 (four wheels, twelve wishbone
nodes, eight spring halves) and the Hummer 29 (six wheels, two knuckles, three
axles, twelve spring halves). `carani_bind` resolves by name, stores -1 for
anything missing, and picks the proc from what it found. **Repack all three after
touching `CAR_RIG`** -- a rig node that is not a part flattens into the body batch
and then simply never moves, which is what kept the Buggy's suspension and
steering frozen and the Hummer's middle axle welded to the body.

Splitting a batch per part costs draw calls: the Buggy goes 17 -> 36 batches and
the Hummer 23 -> 34, on 3,631 and 3,890 triangles. Car1 has been at 28 for as long
as it has had a rig.

### Handling is a PLACEHOLDER

`drive()` in `main.c` is a simple arcade model written only to make the car
controllable enough to inspect the track. It is **not** the game's handling,
which lives in the physics code that has not been reverse engineered. There is
no terrain collision either -- the car drives on a fixed Y plane.

### Sky

Meshes whose TEXTURE matches `--sky` (default `^sky`) are flagged in the scene
file and drawn first, camera-locked with `glDepthMask(GL_FALSE)`, so the dome
never clips the world or occludes anything. Format bumped to **VSC3** for the
batch flags field.

The default used to be the mesh-NAME regex `^pCylinder`, and that was wrong on 8
of the 10 tracks in both directions -- 1,868 triangles of ordinary geometry drawn
camera-locked (beach_1's column shafts, beach_3's round walls near the start, the
`kooler` barrels on three tracks) and five domes not flagged at all, which also
left `envmap_init` with no sky for the car's glance. `vsc_check.py` now asserts
exactly one sky batch and that it is as wide as the level. Reasoning in full at
`pack_vsc.py`'s `SKY_TEX_RE`.

### Surface transitions -- VSC9

Where two ground surfaces meet, the artists authored a **blend band**: the face
carries a second base texture (`BAS2`) and an alpha mask (`ALP1`) alongside the
usual `BAS1` and `LM_1`, and the engine lerps the two. 3,608 faces over nine
tracks; country_1 authors none and still packs as VSC6. Recovered from the
engine's own layer-type map (`FUN_0046fc20`) and documented in full in
`docs/render-world.md`, "The surface transition is a SECOND BASE TEXTURE and a
MASK".

    python3 rccars_re/pack_vsc.py <...>/beach_1.sb assets/beach_1.vsc --markers ...
    python3 rccars_re/pack_vsc.py ... --no-blend      # the mutant, for vsc_check

Format bumped to **VSC9**: two more texture indices per batch and, on a blend
batch only, a four-float side array per vertex (the second base texture's UVs and
the mask's) sitting between the normals and the indices -- the same arrangement as
the env-map normals, for the same reason.

- **INSTALL THE ASSETS AND THE BINARY TOGETHER.** An eboot older than this
  rejects a VSC9 file at the magic (the loader accepted `VSC3`..`VSC8`), so a
  half-installed test is a track that will not load at all rather than one that
  looks unchanged
- `scene_draw_blend` draws them in three passes, because a lit blend has three
  inputs and vitaGL's fixed-function path compiles two texture coordinate sets
  unless built with `HAVE_HIGH_FFP_TEXUNITS`. The arithmetic, and why the obvious
  two-pass version is wrong, is in the comment on that function
- The build's check stage now passes `--sb "$DB"` to `vsc_check.py`: it walks each
  source `.sb` with its own reader and asserts the number of authored transition
  faces equals the packed blend triangle count. It is the only invariant here that
  can notice the PACKER declining to pack something, which is the failure this
  feature had for the whole life of the port

### Alpha testing

Foliage and signage are alpha-keyed quads; without `GL_ALPHA_TEST` they drew as
opaque rectangles with the cut-out showing black. `glAlphaFunc(GL_GREATER, 0.5)`
fixes it and costs nothing for opaque geometry -- RGB565 has no alpha channel
and the RGBA decoder fills alpha with 255, so those texels always pass.

Screenshots: `vita_alpha.png` (chase camera, current), `vita_drive.png`
(before alpha test), `vita_mips.png` (overview).

## Terrain collision

    python3 rccars_re/pack_col.py "<...>/RCCarsDB/beach_1.sb" assets/beach_1.col

The engine has its own collision data in the `MOD_COLLISION` node -- 4.5 MB, a
bbox pair, a 128,289-entry index array and a 2.6 MB byte blob (probably a BSP or
grid). That format is NOT reversed. Since the visible geometry is the surface
the car drives on, `pack_col.py` derives collision from it instead.

A uniform XZ grid keeps queries cheap: a downward ray only tests the triangles
registered in one cell (~64 here), not all 37,000. At runtime `ground_at()` does
a barycentric point-in-triangle test in XZ, interpolates Y, and returns the
highest surface not above a ceiling value -- the ceiling stops the car snapping
onto cliffs and bridges overhead.

The car samples the surface at its centre plus the four axle corners, so body
pitch and roll follow the slope.

`beach_1`: 37,085 triangles, 28x61 grid, 57,586 refs, 1.5 MB.

### Water is excluded from collision

`--exclude-tex` (default `^sea$|^sky`) drops those faces. Without it the car
drives on the ocean. 15,051 faces are dropped for `beach_1`.

### Choosing a spawn: a trap worth recording

Searching for "flattest ground near sea level" picks **the ocean** -- it is by
far the flattest surface at that height, and `sea` is the single largest-area
texture on the map (21,709 units2). The first attempt spawned the car floating
on open water.

Nor is "sand" enough: `rubbish_sand_wet` is 52 faces all at exactly y = -1.38,
i.e. seabed under the water.

Spawns must be selected by **texture plus height**. The current spawn is a large
level face using `floor` at y = 1.2 -- the pier decking, comfortably above the
waterline. Useful ground textures on `beach_1`: `sand_wet` (y -1.99..0.17),
`sand_halfdry` (-0.35..0.52), `path_1` (-0.17..3.75), `floor` (-0.14..2.92).

### Verification

The runtime ground query was checked against an independent Python
implementation reading the same `.col`: offline ground at the old spawn was
-0.223, and the app reported `car_y = -0.2` (= -0.223 + 0.06 ride height at one
decimal). Collision is confirmed numerically, not just visually.

## Node transforms (car was rendering in pieces)

`sb2obj.collect` originally took each mesh's vertices verbatim. That is fine for
track meshes -- they carry identity transforms -- but a car is a hierarchical
rig whose parts are positioned by their node transforms, so it rendered as a
scattered pile.

### Chunk 0x540B is not a 3x3 matrix

It is nine floats making **three vec3s: translation, scale, rotation in
DEGREES**. The giveaway is that the middle triple is exactly (1,1,1) on
unrotated nodes. Printing it as a matrix with `%7.3f` also runs the columns
together -- `-0.001180.000` is `-0.001`, `180.000`, `0.000`, i.e. a half turn
about Y, which is how a mirrored wheel is placed.

`0x5438` holds the same three vectors in a different order (translation,
rotation, scale).

### Euler order is XYZ

Determined by measurement, not assumption. `0x8216` is a bounding sphere
(radius + centre); composing the whole Car1 hierarchy and comparing the
resulting wheel origins against those centres gives:

| order | rms error |
|-------|-----------|
| **xyz** | **0.011** |
| zyx   | 0.011 |
| all others | 0.261 |

Composition is `world = parent ∘ local` with `local(v) = t + R*S*v`, so
`M = M_parent * R * S` and `T = T_parent + M_parent * t`.

**Verified:** Car1's bounds went from a lopsided x[-0.14, 0.09] to a symmetric
x[-0.19, 0.19] z[-0.26, 0.25], with wheels at +/-0.15 in both axes. The track
bbox is bit-identical before and after, confirming track nodes are identity and
nothing regressed.

Note that summing translations alone is NOT enough -- a parent's rotation
rotates its children's offsets, which is exactly how the mirrored wheels get
their +/-X placement.

## Car orientation

The model faces **+Z** (wheels at z=+0.147 front, -0.147 rear) but travel is
along -Z at heading 0, so the body needs `glRotatef(car_h + 180, 0,1,0)`. It was
driving backwards.

That half turn also mirrors the model's local X and Z, so the terrain-derived
pitch and roll must be **negated** when applied, or the car leans the wrong way
on slopes.

The current spawn is a 23-degree slope at (1.09, 1.27, -31.01) precisely so the
body angles are visible immediately.

## Physics build

`physics.c` / `physics.h` / `physics_data.h`, generated by
`rccars_re/gen_physics_header.py`.

**The constants and curves are the game's own.** Per-car acceleration curves,
top speeds, grip, drag, steering lock, suspension rates -- all recovered from
the game's Settings and Splines data and converted with the loader scale
constants (see PHYSICS.md).

**The model around them is not.** RC Cars' integrator (`FUN_005074D0`) is still
untranscribed, so this is a conventional bicycle model with load transfer and a
spring/damper suspension. Right ballpark, not lap-for-lap.

Built with `PHYSICS_C_FLAGS` (`-O2 -fno-fast-math -ffp-contract=off`) attached
to `physics.c` only, per the precision findings.

### Verified against the recovered curves

A host harness (`harness.c`, compiles `physics.c` natively against a flat-ground
stub) runs each car at full throttle from rest:

| car | reaches | curve hits zero at |
|-----|---------|--------------------|
| Overkill | 24.8 m/s | 25 |
| Buggy | 27.0 m/s | 30 (clamped by speedBaseMax) |
| Hummer | 19.3 m/s | 20 |

Ranking Buggy > Overkill > Hummer, matching the three cars' character.

### Bug found by that harness: double-counted drag

The first version applied quadratic air drag on top of the acceleration curve
and pinned Overkill at **9.5 m/s** against its real 27.

`splAccelBase` is a NET curve -- it already falls to zero at roughly
speedBaseMax, so the losses at full throttle are inside it. Resistance now
scales with `1 - throttle`, so it dominates when coasting and does not fight the
curve. Worth remembering for anyone else using these curves.

### Respawn

Water is deliberately excluded from collision, so driving off the pier drops the
car into a hole. It now respawns when it falls 30 m below the spawn height
instead of falling forever.

### Telemetry

`sceClibPrintf` mangles formats with several floats (see the printf note above),
so the per-second line prints **integers only** -- speed and position in cm.

### Cost

Frame rate moved from a solid 60 to roughly 45-55. The step does five ground
queries per frame (body centre plus four axle corners), each walking one
collision cell. Worth batching if it needs to come back.

## Known issues remaining

- **One batch of 77 has no texture** (974 triangles) -- black patches.
- The handling model is not the game's integrator; see PHYSICS.md step 3.
- No wall collision -- only downward ground queries.
- The app crashes on exit (teardown only).

## The car rig: VSC4

The wheels used to be baked into the body transform, so they neither steered nor
turned. They are now a rig, driven by the game's own animation proc
(`carAniProc1` 0x00504820, transcribed in `carani.c`).

The obstacle was the scene format. `pack_vsc.py` flattens the whole hierarchy and
merges every mesh sharing a texture into one batch, which is exactly what you want
for a track and exactly wrong for anything articulated. `--rig` keeps a named set
of nodes -- the game's own list, from the pointer table at `0x00572fcc` --
addressable afterwards, and writes **VSC4**:

    u32 part count             after the batch count
    parts, before the batches:
      u16 name length, char[] name
      i32 parent index         -1 for the root, which is always part 0
      float[16] rest           node -> model at rest
      float[16] rest_inv
      float[16] local          rest relative to the parent part
    batches gain a u32 part index

Vertices stay **baked in model space**, which is the point: a part that is not
animated draws byte-identically to before, and an animated one draws under
`rest_inv * animated_world`. So the change is inert until something moves. The
batch key gains the part, since a batch is one draw call under one matrix --
`car1.vsc` goes from 11 batches to 23, which costs nothing at 3,778 triangles.

VSC3 still loads; the track is not repacked.

`Car1` (Overkill) carries the full rig: two axles, two steering knuckles, four
wheels and eight springs, 17 parts. `Car2` and `Car3` do not -- the Buggy and
Hummer are animated by `carAniProc2`/`3` (0x00505780 / 0x005068e0) over a
different node set (the table at `0x005731b8`: `AXLE_FRONT_LEFT_UP/DOWN/WHEEL`,
`Spring<n>_<m>`), which is not transcribed. `carani_bind` finds nothing for the
parts they lack and leaves those at rest, so they still draw.

Matrices are the engine's row-major row-vector layout throughout, which is the
same memory layout `glMultMatrixf` wants -- nothing is transposed anywhere on
this path.

### The 1.05 scale

Every node in `Car1`'s chain carries a uniform 1.05 scale, so `rest_inv` cannot be
a transpose; `pack_vsc.py` inverts the 3x3 properly through its adjugate. Worth
knowing before writing any "optimised" version of it.
