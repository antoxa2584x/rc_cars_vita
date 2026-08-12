cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(mintest C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Min Test")
set(VITA_TITLEID  "RCMT00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O3 -ffast-math -mtune=cortex-a9 -mfpu=neon")

add_executable(${PROJECT_NAME} main.c)

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
  stdc++          # SceShaccCgExt/vitashark are C++; a pure-C link misses these
  m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
