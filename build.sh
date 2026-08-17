#!/usr/bin/env bash
#
# RC Cars — PS Vita: convert the game's resources and build the .vpk.
#
# Put the game's own files in game_data/ at the root of this tree, then run:
#
#     ./build.sh
#
# game_data/ holds six things, copied from an installed copy of RC Cars:
#
#     game_data/RCCars.pack     the texture and sound archive
#     game_data/RCCarsDB/       the .sb scene databases (tracks, cars, props)
#     game_data/Tracks/         the soundtrack MP3s
#     game_data/Autoexec.gm     the playlist that orders them
#     game_data/GameIcon.ico    the bubble icon
#     game_data/header.jpg      the LiveArea art — OPTIONAL; without it the
#                               LiveArea is derived from the icon instead
#
# --import points that at an install and links it up for you:
#
#     ./build.sh --import "/mnt/c/Games/RC Cars"
#
# Nothing under game_data/ is ever written to, and none of it is in this
# repository — it is the game, and you need your own copy.
#
# The stages are, in order:
#
#   1. unpack   RCCars.pack -> textures, sounds, settings          (unpack_tiox.py)
#   2. lightmap each .sb's embedded lightmap images                (sb2obj.py)
#   3. tracks   ten .vsc scenes and their .col collision grids     (pack_vsc/pack_col)
#   4. cars     three rigged cars with shadow, env-map and effects (pack_vsc.py)
#   5. props    the 13 knockable props, one file for all tracks    (pack_props.py)
#   6. sound    the sound bank and the music                       (pack_snd.py)
#   7. tables   tracks.h, the menu font, the bubble and LiveArea   (gen_*.py)
#   8. ai       ai_data.h and ten .aip opponent paths       (gen_ai_data/pack_ai)
#   9. check    vsc_check.py over every packed scene
#  10. build    cmake + make -> build/rccars_viewer.vpk
#
# Every stage is skipped when its output is already newer than its input, so a
# re-run after editing one .c file goes straight to stage 9. --force redoes
# everything; -s/--stage runs a subset.
#
# The conversion commands and their flags come from BUILD.md; that file explains
# why each one is what it is. Anything surprising here is cross-referenced to it.

set -euo pipefail

# ---------------------------------------------------------------- defaults ---

VITA="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

GAME="${RCCARS_GAME:-$VITA/game_data}"
RE="${RCCARS_RE:-$VITA/../rccars_re}"
WORK="${RCCARS_WORK:-$VITA/.cache}"       # holds extracted/ and lightmaps/
VITASDK="${VITASDK:-/usr/local/vitasdk}"

JOBS="$(nproc 2>/dev/null || echo 4)"
FORCE=0
STAGES="unpack lightmap tracks cars props sound tables ai check build"
WANT=""
NO_MUSIC=0
CLEAN=0
BUILD_VITAGL=0
IMPORT=""
SND_RATE=22050

# What game_data/ must contain, and where each one is used.
NEEDED=(RCCars.pack      # unpack_tiox.py: textures, wavs, settings
        RCCarsDB         # pack_vsc/pack_col/pack_props: the .sb scenes
        Tracks           # pack_snd.py: the soundtrack
        Autoexec.gm      # pack_snd.py: the playlist that orders it
        GameIcon.ico)    # gen_sce_sys.py: the bubble

# Optional. header.jpg is the LiveArea's wallpaper and gate; it ships with the
# Steam release but not with every install. Without it the LiveArea is derived
# from the icon instead, which is plainer but valid — the vpk needs both images
# to exist whatever they contain.
OPTIONAL=(header.jpg)

TRACKS=(beach_1 beach_2 beach_3 beach_4
        country_1 country_2 country_3 country_4
        urban_1 urban_2)

usage() {
    sed -n '3,27p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    cat <<EOF

Options:
  --import DIR    populate game_data/ from an installed copy of the game,
                  by symlink where possible, then carry on
  --game DIR      use this instead of game_data/                  [$GAME]
  --re DIR        the rccars_re tools                             [$RE]
  --work DIR      where intermediates are unpacked to             [$WORK]
  --vitasdk DIR   VitaSDK prefix                                  [$VITASDK]
  -j, --jobs N    parallel make jobs                              [$JOBS]
  -s, --stage S   run only these stages (comma separated, or repeated).
                  One of: $STAGES
  -t, --track T   restrict stages 2-3 to these tracks (comma separated)
  -f, --force     redo every stage even if its output looks current
  --no-music      skip the 60 MB of MP3s (the game is silent between races)
  --rate HZ       sound bank rate, 22050 or 44100                 [$SND_RATE]
  --build-vitagl  clone and install the custom vitaGL this port needs
  --clean         remove build/ before configuring
  -h, --help      this text
EOF
}

# ------------------------------------------------------------------- output ---

if [ -t 1 ]; then
    B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; D=$'\033[2m'; N=$'\033[0m'
else
    B=""; G=""; Y=""; R=""; D=""; N=""
fi

step()  { printf '%s==>%s %s%s%s\n' "$G" "$N" "$B" "$*" "$N"; }
info()  { printf '    %s\n' "$*"; }
skip()  { printf '    %sskip%s %s\n' "$D" "$N" "$*"; }
warn()  { printf '%swarning:%s %s\n' "$Y" "$N" "$*" >&2; }
die()   { printf '%serror:%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

run()   { printf '    %s$ %s%s\n' "$D" "$*" "$N"; "$@"; }

# True when $1 exists and is newer than every other argument.
current() {
    local out=$1; shift
    [ "$FORCE" = 0 ] || return 1
    [ -e "$out" ] || return 1
    local dep
    for dep in "$@"; do
        [ -e "$dep" ] || continue
        [ "$out" -nt "$dep" ] || return 1
    done
    return 0
}

wanted() {
    [ -z "$WANT" ] && return 0
    case ",$WANT," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------- arguments ---

while [ $# -gt 0 ]; do
    case "$1" in
        --import)       IMPORT=$2; shift 2 ;;
        --game)         GAME=$2; shift 2 ;;
        --re)           RE=$2; shift 2 ;;
        --work)         WORK=$2; shift 2 ;;
        --vitasdk)      VITASDK=$2; shift 2 ;;
        -j|--jobs)      JOBS=$2; shift 2 ;;
        -s|--stage)     WANT="${WANT:+$WANT,}$2"; shift 2 ;;
        -t|--track)     IFS=, read -r -a TRACKS <<<"$2"; shift 2 ;;
        -f|--force)     FORCE=1; shift ;;
        --no-music)     NO_MUSIC=1; shift ;;
        --rate)         SND_RATE=$2; shift 2 ;;
        --build-vitagl) BUILD_VITAGL=1; shift ;;
        --clean)        CLEAN=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "unknown option: $1 (--help for usage)" ;;
    esac
done

GAME="${GAME%/}"
RE="$(cd "$RE" 2>/dev/null && pwd)" || die "--re: no such directory"
mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"

DB="$GAME/RCCarsDB"
EXTRACTED="$WORK/extracted"
OBJ="$WORK/lightmaps"
EMB="$OBJ/embedded"
ASSETS="$VITA/assets"

for s in ${WANT//,/ }; do
    case " $STAGES " in *" $s "*) ;; *) die "unknown stage: $s" ;; esac
done

# ------------------------------------------------------------ game_data/ ---
# Linked rather than copied: RCCarsDB and Tracks are 330 MB between them and
# nothing here ever writes to either. A filesystem that cannot symlink (a
# Windows share mounted without them) falls back to a copy.

if [ -n "$IMPORT" ]; then
    step "Importing game files"
    IMPORT="${IMPORT%/}"
    [ -d "$IMPORT" ] || die "--import: no such directory: $IMPORT"
    mkdir -p "$GAME"
    for f in "${NEEDED[@]}" "${OPTIONAL[@]}"; do
        if [ -e "$GAME/$f" ]; then
            skip "$f"
        elif [ ! -e "$IMPORT/$f" ]; then
            case " ${OPTIONAL[*]} " in
                *" $f "*) skip "$f (not in $IMPORT, optional)"; continue ;;
                *) die "$IMPORT/$f is missing — is that an RC Cars install?" ;;
            esac
        elif ln -s "$IMPORT/$f" "$GAME/$f" 2>/dev/null; then
            info "linked  $f"
        else
            info "copying $f"
            cp -r "$IMPORT/$f" "$GAME/$f"
        fi
    done
fi

# ---------------------------------------------------------------- preflight ---

step "Preflight"

command -v python3 >/dev/null || die "python3 not found"
python3 -c 'import PIL' 2>/dev/null || die "Pillow not installed (pip install pillow)"
command -v cmake >/dev/null || die "cmake not found"

[ -f "$RE/pack_vsc.py" ] || die "no pack_vsc.py in '$RE' — point --re at the tools"

missing=()
for f in "${NEEDED[@]}"; do
    [ -e "$GAME/$f" ] || missing+=("$f")
done
if [ ${#missing[@]} -gt 0 ]; then
    printf '%serror:%s %s is incomplete. Missing:\n' "$R" "$N" "$GAME" >&2
    printf '    %s\n' "${missing[@]}" >&2
    cat >&2 <<EOF

Copy them out of an installed copy of RC Cars:

    mkdir -p "$GAME"
    cp -r "/path/to/RC Cars"/{$(IFS=,; echo "${missing[*]}")} "$GAME/"

or let the script link them for you:

    $0 --import "/path/to/RC Cars"
EOF
    exit 1
fi

export VITASDK
export PATH="$VITASDK/bin:$PATH"          # its own statement: see BUILD.md, the
                                          # one-liner expands VITASDK before it
                                          # is assigned and PATH becomes "/bin:"

if wanted build; then
    command -v arm-vita-eabi-gcc >/dev/null \
        || die "arm-vita-eabi-gcc not on PATH — is VITASDK ($VITASDK) right?"

    GL="$VITASDK/arm-vita-eabi/lib/libvitaGL.a"
    if [ "$BUILD_VITAGL" = 1 ]; then
        step "Building vitaGL (custom)"
        tmp="$(mktemp -d)"
        run git clone --depth 1 https://github.com/Rinnegatamante/vitaGL "$tmp/vitaGL"
        # NO_SPLASHSCREEN is not optional: stock vitaGL creates a SECOND GXM
        # context for its splash screen, Vita3K supports one, and vglInit then
        # dies dereferencing null at 0x78. The three speedhacks are worth about
        # half the frame rate on hardware — BUILD.md says why each is safe here.
        make -C "$tmp/vitaGL" NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 \
             INDICES_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 -j"$JOBS"
        make -C "$tmp/vitaGL" NO_SPLASHSCREEN=1 DRAW_SPEEDHACK=2 \
             INDICES_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 install
        rm -rf "$tmp"
    elif [ ! -f "$GL" ]; then
        die "libvitaGL.a not installed — run with --build-vitagl, or see README"
    else
        # Heuristic, not proof: with NO_SPLASHSCREEN=1 the splash translation
        # unit keeps no reference to init_gxm_context. If yours does, the app
        # will die in vglInit on Vita3K.
        tmp="$(mktemp -d)"
        if (cd "$tmp" && ar x "$GL" splashscreen.o 2>/dev/null) &&
           "$VITASDK/bin/arm-vita-eabi-nm" "$tmp/splashscreen.o" 2>/dev/null |
               grep -q 'U .*init_gxm_context'; then
            warn "libvitaGL.a looks like a STOCK build (splash screen compiled in)."
            warn "vglInit will fail on Vita3K. Rebuild with --build-vitagl."
        fi
        rm -rf "$tmp"
    fi
fi

for f in "${OPTIONAL[@]}"; do
    [ -e "$GAME/$f" ] || info "optional  $f not present"
done

info "game      $GAME"
info "tools     $RE"
info "work      $WORK"
info "vitasdk   $VITASDK"
[ -n "$WANT" ] && info "stages    ${WANT//,/ }"
info "tracks    ${TRACKS[*]}"

mkdir -p "$ASSETS"

# ------------------------------------------------------- 1. unpack the pack ---
# 1,875 files: the three texture quality sets, the wavs, Settings, Splines.

if wanted unpack; then
    step "Unpacking RCCars.pack"
    if current "$EXTRACTED/Textures.1" "$GAME/RCCars.pack"; then
        skip "$EXTRACTED already current"
    else
        mkdir -p "$EXTRACTED"
        run python3 "$RE/unpack_tiox.py" extract "$GAME/RCCars.pack" "$EXTRACTED"
    fi
    [ -d "$EXTRACTED/Sound" ] || warn "no Sound/ under $EXTRACTED — stage 'sound' will fail"
fi

# ---------------------------------------------------------- 2. the lightmaps ---
# Lightmaps are embedded IN each .sb rather than in the pack, so they only come
# out through sb2obj.py, which writes them as .csi into one shared embedded/
# directory. pack_vsc.py globs that directory, which is why every scene that
# needs one must have been run through here first.

if wanted lightmap; then
    step "Extracting embedded lightmaps"
    mkdir -p "$EMB"
    for sb in "${TRACKS[@]}" Car stone; do
        src="$DB/$sb.sb"
        [ -f "$src" ] || { warn "$src missing, skipped"; continue; }
        stamp="$OBJ/.$sb.done"   # not in $EMB: pack_vsc.py globs that directory
        if current "$stamp" "$src"; then
            skip "$sb"
        else
            run python3 "$RE/sb2obj.py" "$src" "$OBJ/$sb.obj" --pngdir "$WORK/rccars_png"
            touch "$stamp"
        fi
    done
fi

# ------------------------------------------------- 3. the ten track scenes ---
# --markers exports the checkpoint and wave nodes and tags the water surfaces
# (VSC5). pack_col writes COL3: the grid, a material byte per triangle, and the
# water surface height per cell. Water is deliberately NOT in the collision
# triangles — the car fords the river — but its height is what makes the water
# drag fire, so repack all ten after touching SURF_RE or anything about water.

if wanted tracks; then
    step "Packing tracks"
    for t in "${TRACKS[@]}"; do
        src="$DB/$t.sb"
        [ -f "$src" ] || { warn "$src missing, skipped"; continue; }

        if current "$ASSETS/$t.vsc" "$src" "$RE/pack_vsc.py"; then
            skip "$t.vsc"
        else
            run python3 "$RE/pack_vsc.py" "$src" "$ASSETS/$t.vsc" --markers \
                --csidir "$EXTRACTED" --embdir "$EMB"
        fi

        if current "$ASSETS/$t.col" "$src" "$RE/pack_col.py"; then
            skip "$t.col"
        else
            run python3 "$RE/pack_col.py" "$src" "$ASSETS/$t.col"
        fi
    done
fi

# ------------------------------------------------------------ 4. the cars ---
# All three live in one Car.sb; --subtree pulls one out. --rig keeps the wheels
# articulable (VSC4), --shadow-tex bakes the top-down silhouette the projected
# shadow uses, --envmap packs normals for the body's env-mapped parts (VSC7).
#
# Only the cars get --envmap: no track has an ENVIR_CAR_BODY node, so a track
# would pack VSC7 with an empty field.
#
# DO NOT add --shadow-radius ShadowSize. That is the shadow SOURCE's radius, not
# the texture's extent; the Buggy's 0.29 is shorter than its own half-length and
# the silhouette runs off the texture, smearing the edge over the receiver.
#
# --extra-tex carries what the app binds by NAME at runtime and no mesh
# references: the three upgrade tyre levels the car is not currently wearing,
# `dust` (shared by both particle systems) and the four tyre-mark sprites.
# The per-car lists differ because each car ships wearing a different level.
#
# THE SKINS ARE THE SAME KIND OF THING, and they are the engine's own feature.
# RCCars.exe builds its body texture names with sprintf: FUN_0049fc80 validates
# 0 <= car < 3 and 0 <= skin < 4, formats "car_askin%i%i" and "car_bskin%i%i"
# from (car+1, skin+1), and FUN_0050bf90 hands the two results to FUN_005352a0,
# which walks the model and re-points every texture whose name STARTS WITH
# `car_askin` / `car_bskin`. Car.sb ships wearing skin 1 (car_askin11,
# car_askin21 + car_bskin21, car_askin31 + car_bskin31), so the other three of
# each are asked for here -- exactly the reason the tyre levels are.
#
# ONLY CAR 1 HAS ONE PAGE. FUN_0049fc80's switch loads `b` for cars 2 and 3 and
# writes NULL for car 1, and the shipped art agrees: there is no car_bskin1<n>
# in any of the three texture sets. Two independent statements of the same fact,
# which is why this list is not symmetric.
#
# COST: a skin is 512x512 RGB565 with a full chain, 699 KB in the file and the
# same on the GPU at quality High (175 KB at Medium, 44 KB at Low). Three extra
# skins is +2.1 MB on the Overkill and +4.2 MB on the other two, so a car .vsc
# roughly doubles -- still small against a 35 MB track, but every RESIDENT car
# scene pays it, and load_ai keeps up to three more. Dropping the skins from a
# car's list here is all it takes to get that back: carparts_bind then reports
# one skin, the menu row pins itself at 1/1, and no code changes.

FX=dust,t_halfdry_tire2_1,t_halfdry_tire2_2,t_halfdry_tire2_3,t_halfdry_tire2_4

SKIN1=car_askin12,car_askin13,car_askin14
SKIN2=car_askin22,car_askin23,car_askin24,car_bskin22,car_bskin23,car_bskin24
SKIN3=car_askin32,car_askin33,car_askin34,car_bskin32,car_bskin33,car_bskin34

pack_car() {                      # pack_car <n> <subtree> <extra-tex>
    local out="$ASSETS/car$1.vsc"
    if current "$out" "$DB/Car.sb" "$RE/pack_vsc.py"; then
        skip "car$1.vsc ($2)"; return
    fi
    run python3 "$RE/pack_vsc.py" "$DB/Car.sb" "$out" --subtree "$2" --rig \
        --shadow-tex 256 --envmap --extra-tex "$3,$FX" \
        --csidir "$EXTRACTED" --embdir "$EMB"
}

if wanted cars; then
    step "Packing cars"
    [ -f "$DB/Car.sb" ] || die "$DB/Car.sb missing"
    pack_car 1 Car1 tire3_1,tire3_2,tire3_4,$SKIN1      # Overkill
    pack_car 2 Car2 tire2_1,tire2_2,tire2_4,$SKIN2      # Buggy
    pack_car 3 Car3 tire3_2,tire3_3,tire3_4,$SKIN3      # Hummer
fi

# ----------------------------------------------------------- 5. the props ---
# The 13 knockable models, ONE file shared by all ten tracks.
#
# --extra-tex carries the game's own ON-SCREEN MESSAGE artwork, and this is the
# file it belongs in. RCCars.exe loads six message textures by name at
# 0x004af195 and FUN_004b11e0 draws one of eleven message SLOTS over them, each
# slot carrying a texture index (0x56d2d0), a size as a (w, h) screen fraction
# pair (0x56d278) and a UV rect (0x56d328). Two of the six are wanted here:
#
#   msg_hits      256x256, an atlas of TWO messages -- slot 3 is !HIT! in its
#                 top half, slot 4 GREAT !HIT! in its bottom. hud.c.
#   msg_321_s_f   512x256, an atlas of FIVE -- slot 5 "3", 6 "2", 7 "1",
#                 9 "!!Go!!", 8 "FiNiSH", read off the UV rects and confirmed by
#                 cropping the PNG on them. NOTE the order: the fourth word is
#                 GO, not START, and it is slot NINE while FINISH is slot eight.
#                 countdown.c.
#
# Neither is referenced by any mesh anywhere; both are bound by NAME at runtime
# through scene_tex.
#
# In props.vsc rather than in the ten tracks because props.vsc is the app's one
# LOAD-ONCE scene: main.c builds it before the first track and never reloads it,
# so both are resident for the whole session, cost 0.34 and 0.70 MB ONCE rather
# than ten times each, and neither binding is ever renewed on a track change.
# msg_hits belongs with the props on the merits as well -- it is the thing that
# says you hit one. Texture quality divides both by four a step.

if wanted props; then
    step "Packing props"
    if current "$ASSETS/props.vsc" "$DB/stone.sb" "$RE/pack_props.py"; then
        skip "props.vsc"
    else
        run python3 "$RE/pack_props.py" "$ASSETS/props.vsc" \
            --src "$DB/stone.sb" --texroot "$EXTRACTED" \
            --extra-tex msg_hits,msg_321_s_f
    fi
fi

# ----------------------------------------------------------- 6. the sound ---
# Reads the game's own wavs, its snd.dat volumes and Autoexec.gm's playlist:
# 118 sounds at 22050 Hz mono, plus the 18 MP3s copied verbatim and streamed
# by minimp3 at runtime. --rate 44100 doubles the bank.

if wanted sound; then
    step "Packing sound"
    snd_args=(--out "$ASSETS" --sound-dir "$EXTRACTED/Sound" --game-dir "$GAME"
              --rate "$SND_RATE")
    [ "$NO_MUSIC" = 1 ] && snd_args+=(--no-music)

    if current "$ASSETS/sound.sbk" "$EXTRACTED/Sound" "$RE/pack_snd.py"; then
        skip "sound.sbk"
    else
        run python3 "$RE/pack_snd.py" "${snd_args[@]}"
    fi
fi

# -------------------------------------------------- 7. generated tables/art ---
# tracks.h is the race start of each track, read from its own Players/Player
# instance, PLUS the ambient bed read from its MOD_SNDCHANNEL node — so it is
# regenerated after the sound stage, not before, and --assets lets it check that
# the bed it names actually made it into the bank.
#
# font.h and the sce_sys art are checked in, and font.h needs a TTF that a plain
# Linux box may not have, so both are only generated when missing or forced.

if wanted tables; then
    step "Generating tables and app art"

    if current "$VITA/tracks.h" "$DB" "$RE/gen_tracks.py"; then
        skip "tracks.h"
    else
        run python3 "$RE/gen_tracks.py" "$DB" "$VITA/tracks.h" --assets "$ASSETS"
    fi

    if [ -f "$VITA/font.h" ] && [ "$FORCE" = 0 ]; then
        skip "font.h (checked in)"
    else
        run python3 "$RE/gen_font.py" "$VITA/font.h" --size 18
    fi

    # gen_sce_sys.py has its paths baked in, so drive it as a module rather
    # than sed-ing the source. icon0 is GameIcon.ico's 256x256 frame; bg and
    # startup are header.jpg. All three MUST be 8-bit indexed — see BUILD.md.
    if current "$VITA/sce_sys/icon0.png" "$GAME/GameIcon.ico" "$RE/gen_sce_sys.py"; then
        skip "sce_sys art"
    else
        [ -f "$GAME/header.jpg" ] || info "no header.jpg — LiveArea from the icon"
        info "$D\$ python3 $RE/gen_sce_sys.py  (SRC=$GAME DST=$VITA/sce_sys)$N"
        mkdir -p "$VITA/sce_sys/livearea/contents"
        python3 - "$RE" "$GAME" "$VITA/sce_sys" <<'PY'
import os, sys, tempfile, shutil
from PIL import Image

re_dir, src, dst = sys.argv[1:4]
sys.path.insert(0, re_dir)
import gen_sce_sys as g

tmp = None
if not os.path.exists(os.path.join(src, "header.jpg")):
    # gen_sce_sys builds the wallpaper and the gate from header.jpg and has no
    # path that omits them -- and the vpk needs both files to exist regardless.
    # Rather than fork its logic, stand in a header made from the icon at the
    # real header's own 460x215 and let the generator run unmodified: it will
    # blur that into the wallpaper's fill exactly as it does the real one.
    tmp = tempfile.mkdtemp()
    shutil.copy(os.path.join(src, "GameIcon.ico"), tmp)
    ico = Image.open(os.path.join(src, "GameIcon.ico"))
    ico.size = (256, 256)
    ico = ico.convert("RGBA")
    flat = Image.new("RGB", ico.size, (0, 0, 0))
    flat.paste(ico, (0, 0), ico)
    stand_in = Image.new("RGB", (460, 215), (0, 0, 0))
    side = 215
    stand_in.paste(flat.resize((side, side), Image.LANCZOS), ((460 - side) // 2, 0))
    stand_in.save(os.path.join(tmp, "header.jpg"), quality=95)
    src = tmp

g.SRC, g.DST = src, dst
try:
    g.main()
finally:
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
PY
    fi
fi

# -------------------------------------------------------- 8. the opponents ---
# The AI's roster and the recorded laps it replays. AFTER the tracks, because
# pack_ai.py PROVES the level-name mapping by probing every path against the
# packed .col grid it belongs to -- 0% of samples off-grid against the right one,
# 52-99% against any other -- and refuses to write anything that fails. It also
# measures the per-car body-frame lift there, and the measurement needs all ten
# grids however few tracks are being packed. AFTER `tables` too, because it
# reports each path's offset from the track's own race start out of tracks.h.
# See "The AI opponents" in CLAUDE.md.

if wanted ai; then
    step "Packing the AI opponents"
    if current "$VITA/ai_data.h" "$EXTRACTED/Scripts" "$RE/gen_ai_data.py"; then
        skip "ai_data.h"
    else
        run python3 "$RE/gen_ai_data.py" --root "$EXTRACTED" -o "$VITA/ai_data.h"
    fi
    if current "$ASSETS/beach_1.aip" "$EXTRACTED/CarProfiles" "$RE/pack_ai.py"; then
        skip "ten .aip files"
    else
        run python3 "$RE/pack_ai.py" --profiles "$EXTRACTED/CarProfiles" \
            --data "$VITA/ai_data.h" --tracks "$VITA/tracks.h" \
            --col "$ASSETS" --out "$ASSETS"
    fi
fi

# ------------------------------------------------------------- 9. the check ---

if wanted check; then
    step "Checking packed scenes"
    shopt -s nullglob
    vscs=()
    for v in "$ASSETS"/*.vsc; do
        # props.vsc is VSC8, a different container with its own layout, and
        # vsc_check.py's scene reader walks straight off the end of it. Every
        # other .vsc here is a scene. (BUILD.md's `vsc_check.py assets/*.vsc`
        # predates the props file.)
        [ "$(basename "$v")" = props.vsc ] || vscs+=("$v")
    done
    shopt -u nullglob
    [ ${#vscs[@]} -gt 0 ] || die "no .vsc in $ASSETS — run the packing stages first"
    run python3 "$RE/vsc_check.py" "${vscs[@]}"
fi

# ------------------------------------------------------------ 10. the build ---

if wanted build; then
    step "Building"
    [ "$CLEAN" = 1 ] && rm -rf "$VITA/build"
    mkdir -p "$VITA/build"

    if current "$VITA/build/Makefile" "$VITA/CMakeLists.txt"; then
        skip "cmake configure"
    else
        (cd "$VITA/build" &&
         run cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" ..)
    fi

    (cd "$VITA/build" && run make -j"$JOBS")

    VPK="$VITA/build/rccars_viewer.vpk"
    [ -f "$VPK" ] || die "make finished but $VPK is missing"

    # The vpk is what VitaShell promotes, so verify the art INSIDE it: a stale
    # build directory would ship the old truecolour icons from a fixed tree.
    step "Verifying the package"
    run python3 "$RE/gen_sce_sys.py" --check-vpk "$VPK"

    step "Done"
    info "$(du -h "$VPK" | cut -f1)  $VPK"
    info "copy it to the Vita and promote it with VitaShell, or drop it into"
    info "Vita3K (which also needs libshacccg.suprx at ur0:data/external/)"
fi
