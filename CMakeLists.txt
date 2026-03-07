cmake_minimum_required(VERSION 3.10)

# PSP toolchain: use psp-cmake wrapper (recommended), or pass toolchain manually.
# psp-cmake sets CMAKE_TOOLCHAIN_FILE automatically from PSPDEV env.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{PSPDEV})
  foreach(_tc
      "$ENV{PSPDEV}/share/pspdev.cmake"
      "$ENV{PSPDEV}/lib/cmake/pspdev.cmake"
      "$ENV{PSPDEV}/cmake/pspdev.cmake")
    if(EXISTS "${_tc}")
      set(CMAKE_TOOLCHAIN_FILE "${_tc}" CACHE PATH "toolchain")
      break()
    endif()
  endforeach()
endif()

project(BatmanDoomPSP C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -O2 -G0 -w -fno-strict-aliasing -DNORMALUNIX -DLINUX -DPSP")

set(DG_DIR "${CMAKE_SOURCE_DIR}/doomgeneric/doomgeneric")

file(GLOB DG_SOURCES "${DG_DIR}/*.c")

set(EXCLUDE_FILES
    "doomgeneric_allegro.c" "doomgeneric_sdl.c" "doomgeneric_sdl2.c"
    "doomgeneric_win.c" "doomgeneric_xlib.c" "doomgeneric_soso.c"
    "doomgeneric_sosox.c" "doomgeneric_txt.c" "doomgeneric_ascii.c"
    "doomgeneric_null.c" "doomgeneric_emscripten.c" "doomgeneric_linuxvt.c"
    "i_allegromusic.c" "i_allegrosound.c" "i_sdlmusic.c" "i_sdlsound.c"
    "i_oplmusic.c" "i_pcsound.c" "i_sound.c" "i_video.c" "i_input.c"
    "i_joystick.c" "i_timer.c" "i_system.c" "i_cdmus.c" "i_endoom.c"
    "i_scale.c" "i_main.c" "icon.c" "gusconf.c"
)
foreach(exc ${EXCLUDE_FILES})
    set(FULL_PATH "${DG_DIR}/${exc}")
    if(EXISTS "${FULL_PATH}")
        list(REMOVE_ITEM DG_SOURCES "${FULL_PATH}")
    endif()
endforeach()

# Remove all other doomgeneric_*.c except doomgeneric.c and doomgeneric_psp.c
foreach(src ${DG_SOURCES})
    get_filename_component(name ${src} NAME)
    if(name MATCHES "^doomgeneric_"
       AND NOT name STREQUAL "doomgeneric.c"
       AND NOT name STREQUAL "doomgeneric_psp.c")
        list(REMOVE_ITEM DG_SOURCES ${src})
    endif()
endforeach()

# Add PSP platform file
set(PSP_SRC "${DG_DIR}/doomgeneric_psp.c")
if(NOT EXISTS "${PSP_SRC}")
    set(PSP_SRC "${CMAKE_SOURCE_DIR}/doomgeneric_psp.c")
endif()
list(FIND DG_SOURCES "${PSP_SRC}" PSP_IDX)
if(PSP_IDX EQUAL -1 AND EXISTS "${PSP_SRC}")
    list(APPEND DG_SOURCES "${PSP_SRC}")
endif()

# Add Nuked OPL3
set(OPL3_SRC "${DG_DIR}/opl3.c")
if(NOT EXISTS "${OPL3_SRC}")
    set(OPL3_SRC "${CMAKE_SOURCE_DIR}/opl3.c")
endif()
if(EXISTS "${OPL3_SRC}")
    list(FIND DG_SOURCES "${OPL3_SRC}" OPL3_IDX)
    if(OPL3_IDX EQUAL -1)
        list(APPEND DG_SOURCES "${OPL3_SRC}")
    endif()
endif()

add_executable(${PROJECT_NAME} ${DG_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE ${DG_DIR})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PSP=1
    FEATURE_SOUND=1
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
    DEH_DATA_PATH="ms0:/PSP/GAME/BATDOOM/"
)

# PSP-specific: disable MIPS assembler issues
set_source_files_properties(${DG_SOURCES} PROPERTIES COMPILE_FLAGS "-mno-gpopt")

target_link_libraries(${PROJECT_NAME}
    pspdisplay
    pspgu
    pspge
    pspctrl
    pspaudio
    psprtc
    psppower
    psputility
    pspuser
    pspkernel
    m
    c
)

# Create EBOOT.PBP
create_pbp_file(
    TARGET      ${PROJECT_NAME}
    ICON_PATH   "${CMAKE_SOURCE_DIR}/assets/ICON0.PNG"
    BACKGROUND_PATH NULL
    PREVIEW_PATH    NULL
    TITLE       "Batman Doom"
)
