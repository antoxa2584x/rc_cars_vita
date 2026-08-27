cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(rccars_viewer C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "RC Cars")
set(VITA_TITLEID  "RCCV00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O3 -ffast-math -mtune=cortex-a9 -mfpu=neon")

# Flags for the eventual physics translation units. -ffast-math must NOT apply
# there: the original is x87 running at PC=53 (IEEE double) with float32 state,
# which ARM reproduces exactly ONLY if operation order is preserved.
# -ffast-math permits reassociation and breaks that. It also enables float
# auto-vectorisation onto NEON, whose ARMv7 single-precision path is
# flush-to-zero and not IEEE-conformant. See rccars_re/PHYSICS.md.
#
# -O3, NOT -O2, AND THAT IS NOT A RELAXATION. The rule above is about
# reassociation and vectorisation, and -fno-fast-math -ffp-contract=off forbid
# both at every optimisation level -- -O3 may unroll and inline more, but it may
# not reorder a float expression or fuse a multiply-add. Measured rather than
# argued: with these flags -O3 is BIT-IDENTICAL to -O2 across `allstarts`,
# `wetcheck`, `ceiling` and `curb`, i.e. every numeric column of the ten real
# race starts on the ten real grids. The -O2 here was never justified by the
# comment above it, and it cost 26% (0.280 s against 0.206 s on the host's
# allstarts) in the two hottest modules in the port.
set(PHYSICS_C_FLAGS "-Wall -O3 -fno-fast-math -ffp-contract=off -mtune=cortex-a9")

add_executable(${PROJECT_NAME} main.c physics.c rb.c contact.c collide.c col.c rbcar.c cam.c carani.c ui.c hud.c countdown.c race_ui.c sfont.c dirarrow.c msg.c menu.c mainmenu.c results.c touch.c settings.c antenna.c carparts.c scene.c shadow.c water.c checkpoint.c sun.c fx.c trace.c envmap.c carlight.c
  prop.c char.c ai.c mix.c audio.c sfx.c rlog.c)

separate_arguments(PHYSICS_FLAGS_LIST UNIX_COMMAND "${PHYSICS_C_FLAGS}")
# rb.c is the transcribed original integrator, so the order-preservation rule
# above applies to it above all. physics.c is the placeholder arcade model.
# ai.c is on this line because it writes the rigid-body state directly and its
# cursor walk is metre arithmetic over a 500 m polyline -- the same reasons the
# integrator itself is here.
#
# AND col.c, WHICH WAS THE HOLE IN THE LIST. Every contact point, face normal and
# surface height the modules above integrate is computed in col.c, so building it
# with -ffast-math means the order-preservation argument stops one call short of
# the numbers it is about. It is not hypothetical: with col.c at -O3 -ffast-math
# and everything else on this list, `allstarts`, `wetcheck`, `ceiling` and `curb`
# ALL differ from the same build with col.c order-preserved -- the car ends 4 cm
# higher and 2 cm along after seven seconds at beach_2's kerb (5.355 -> 5.395 m),
# and 28 of allstarts' numeric columns move. Which means every bound in
# docs/ -- the penetration percentages, the roof heights, the curb thresholds --
# was measured on a col.c the Vita does not run.
#
# It is also FREE, which is why there is nothing to trade off: on the host,
# col.c order-preserved at -O3 runs allstarts in 0.206 s against the shipped
# arrangement's 0.205 s, because the -O2 above was costing more than -ffast-math
# was buying. On ARM the two builds differ by 18 divisions turned into
# reciprocal-multiplies (127 vmul / 31 vdiv fast, 79 / 49 safe) and by NOTHING
# else -- in particular col.c gets zero NEON quad-register float ops either way,
# so the auto-vectorisation half of the argument above never applied to it. That
# division count is the one part of this not measured on target.
set_source_files_properties(physics.c rb.c contact.c collide.c col.c rbcar.c cam.c carani.c ai.c
  PROPERTIES COMPILE_OPTIONS "${PHYSICS_FLAGS_LIST}")

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  taihen_stub     # required by SceShaccCgExt, which vitaGL links
  SceKernelDmacMgr_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceCommonDialog_stub
  SceShaccCg_stub
  SceSysmodule_stub
  SceTouch_stub
  ScePower_stub
  SceAudio_stub   # sceAudioOut: the output port audio.c's mixer thread feeds
  stdc++          # SceShaccCgExt/vitashark are C++; a pure-C link misses these
  m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
# Icon and LiveArea art come from the game's own assets, converted by
# rccars_re/gen_sce_sys.py: icon0.png is GameIcon.ico's 256x256 frame, and
# bg.png / startup.png are header.jpg. Regenerate with that script rather than
# editing the PNGs by hand.
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE assets assets
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
