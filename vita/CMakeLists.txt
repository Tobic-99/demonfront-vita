cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(demonfront_vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Demon Front")
set(VITA_TITLEID  "DMFR00001")
set(VITA_VERSION  "01.00")
set(CMAKE_CXX_STANDARD 17)
set(R ${CMAKE_SOURCE_DIR}/..)
set(COMMON_FLAGS "-O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-strict-aliasing")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS}")
set(GEN_WARN -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-value)

file(GLOB ARM_GEN ${R}/gen/arm/*.cpp)
file(GLOB M68K_GEN ${R}/gen/m68k/*.cpp)
add_library(df_gen STATIC ${ARM_GEN} ${M68K_GEN})
target_include_directories(df_gen PUBLIC ${R}/runtime ${R}/gen/arm ${R}/gen/m68k ${R}/host)
target_compile_options(df_gen PRIVATE ${GEN_WARN})
target_compile_definitions(df_gen PRIVATE DF_VITA=1)

add_executable(demonfront ${R}/host/arm_host.cpp ${R}/host/arm_native.cpp ${R}/host/m68k_host.cpp ${R}/host/scheduler.cpp
               ${R}/host/fiber.cpp ${R}/host/trace.cpp ${R}/host/video.cpp ${R}/host/z80.cpp ${R}/host/ics2115.cpp ${R}/host/sound.cpp ${R}/host/romset.cpp
               ${CMAKE_SOURCE_DIR}/vita_main.cpp)
target_include_directories(demonfront PRIVATE ${R}/runtime ${R}/gen/arm ${R}/gen/m68k ${R}/host)
target_compile_definitions(demonfront PRIVATE DF_VITA=1)
target_compile_options(demonfront PRIVATE -include ${CMAKE_SOURCE_DIR}/vita_log.h)
target_link_libraries(demonfront df_gen vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceSysmodule_stub SceKernelDmacMgr_stub SceAppMgr_stub
    SceAppUtil_stub SceCommonDialog_stub SceIofilemgr_stub ScePower_stub SceLibKernel_stub SceKernelThreadMgr_stub
    SceRtc_stub SceAudio_stub SceTouch_stub SceMotion_stub z m stdc++)
vita_create_self(eboot.bin demonfront UNSAFE)
# LiveArea images (sce_sys/icon0.png, sce_sys/livearea/contents/{bg,startup}.png) are packed when present
set(VPK_FILES FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE ${CMAKE_SOURCE_DIR}/shader_cache/v/00000028-0.gxp shader_cache/v/00000028-0.gxp
    FILE ${CMAKE_SOURCE_DIR}/shader_cache/f/000000EF-0000000000000000.gxp shader_cache/f/000000EF-0000000000000000.gxp
    FILE ${CMAKE_SOURCE_DIR}/../THIRD-PARTY-NOTICES.md licenses/THIRD-PARTY-NOTICES.md
    FILE ${CMAKE_SOURCE_DIR}/../LICENSE-MAME.txt licenses/LICENSE-MAME.txt)
foreach(a icon0.png livearea/contents/bg.png livearea/contents/startup.png)
  if(EXISTS ${CMAKE_SOURCE_DIR}/sce_sys/${a})
    list(APPEND VPK_FILES FILE ${CMAKE_SOURCE_DIR}/sce_sys/${a} sce_sys/${a})
  endif()
endforeach()
vita_create_vpk(demonfront.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME} ${VPK_FILES})
