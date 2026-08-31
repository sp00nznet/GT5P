cmake_minimum_required(VERSION 3.20)
project(gt5p_recompiled
    VERSION 0.1.0
    DESCRIPTION "Gran Turismo 5 Prologue - Static Recompilation for PC"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# ps3recomp runtime
# =============================================================================
set(PS3RECOMP_DIR "${CMAKE_SOURCE_DIR}/../ps3recomp" CACHE PATH "Path to ps3recomp")

if(NOT EXISTS "${PS3RECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "ps3recomp not found at ${PS3RECOMP_DIR}\n"
        "Clone it from https://github.com/sp00nznet/ps3recomp\n"
        "Or set -DPS3RECOMP_DIR=/path/to/ps3recomp")
endif()

add_subdirectory(${PS3RECOMP_DIR} ps3recomp EXCLUDE_FROM_ALL)

# =============================================================================
# Lifted PPU code
# =============================================================================
set(GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${GENERATED_DIR}/ppu_recomp_*.cpp")
if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR
        "No lifted sources in ${GENERATED_DIR}/.\n"
        "Run the pipeline first -- see the Building section of README.md.")
endif()
list(LENGTH GENERATED_SOURCES N_CHUNKS)
message(STATUS "lifted PPU chunks: ${N_CHUNKS}")

# The runtime library does not bundle the PPU loader/HLE/VFS translation units;
# each title compiles them so it can supply its own function_table and boot
# harness.
set(PS3RECOMP_PPU_SOURCES
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"   # vm + guest dispatch + traps
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"      # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"   # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"       # cellFs VFS
)

# Lifted SPU images. All four of this title's SPURS workloads carry the same
# policy module (the WWS job manager), embedded in EMAIN.ELF rather than loaded
# from a data file -- see src/gen/spu_workloads.c.
option(GT5P_ENABLE_SPU "Compile and register the lifted SPU images" ON)
if(GT5P_ENABLE_SPU)
    file(GLOB GT5P_SPU_LIFTED CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/generated/spu_lifted/*/spu_recomp.c")
    list(LENGTH GT5P_SPU_LIFTED N_SPU)
    message(STATUS "lifted SPU images: ${N_SPU}")
    set(GT5P_SPU_SOURCES "${CMAKE_SOURCE_DIR}/src/gen/spu_workloads.c" ${GT5P_SPU_LIFTED})
    # The lifted sources include "spu_context.h" relative to the runtime; scope
    # the include to these TUs so the huge PPU chunks keep their cached command.
    set_source_files_properties(${GT5P_SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
else()
    set(GT5P_SPU_SOURCES "")
endif()

add_executable(gt5p
    src/main.cpp
    src/gt5p_present.cpp
    src/gt5p_allocwatch.cpp
    src/gt5p_bbcount.cpp
    ${GT5P_SPU_SOURCES}
    src/gen/ppu_hle_nids.cpp    # generated: NID -> ps3recomp lib handler
    ${PS3RECOMP_PPU_SOURCES}
    ${GENERATED_SOURCES}
)

target_include_directories(gt5p PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/compat      # <dirent.h>/<unistd.h> shims for ppu_fs.cpp
    ${CMAKE_SOURCE_DIR}/generated       # ppu_recomp.h
    ${PS3RECOMP_DIR}/include
    ${PS3RECOMP_DIR}/runtime/ppu
    ${PS3RECOMP_DIR}/runtime/memory
    ${PS3RECOMP_DIR}/runtime/syscalls
)

target_link_libraries(gt5p PRIVATE ps3recomp_runtime)

if(WIN32)
    target_link_libraries(gt5p PRIVATE
        ws2_32 xinput ole32 bcrypt
        d3d12 dxgi dxguid d3dcompiler)
else()
    target_link_libraries(gt5p PRIVATE pthread m)
endif()

# =============================================================================
# Compiler settings
# =============================================================================
if(MSVC)
    target_compile_options(gt5p PRIVATE
        /W3
        /MP          # parallel compilation -- the lifted chunks are ~35 MB each
        /bigobj
        /utf-8
        # noise from generated code
        /wd4244 /wd4267 /wd4146 /wd4305 /wd4018)
    target_compile_definitions(gt5p PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(gt5p PRIVATE
        -Wall
        -Wno-unused-variable -Wno-unused-function
        -Wno-sign-compare -Wno-parentheses -Wno-unused-but-set-variable)
endif()

install(TARGETS gt5p RUNTIME DESTINATION bin)
