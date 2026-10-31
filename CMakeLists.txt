cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(app0stream C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -fno-builtin-printf")

add_executable(app0stream
  src/main.c
  src/vfs.c
  src/client.c
  src/cache.c
  src/config.c
  src/upcase.c
  src/log.c
  src/port_vita.c
  src/netps_fix.c
)

target_link_libraries(app0stream
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceModulemgrForDriver_stub
  SceIofilemgrForDriver_stub
  SceThreadmgrForDriver_stub
  SceSysmemForDriver_stub
  SceDebugForDriver_stub
  SceSysclibForDriver_stub
  SceNetPsForDriver_stub
  SceKernelSuspendForDriver_stub
  gcc
)

set_target_properties(app0stream PROPERTIES LINK_FLAGS "-nostdlib")

vita_create_self(app0stream.skprx app0stream
  CONFIG exports.yml
  UNSAFE
  STRIPPED
  REL_OPTIMIZE
)
