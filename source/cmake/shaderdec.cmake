# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# shaderdec (tools/shaderdec/shaderdec.c) generates the Maxwell opcode enum and decoder from
# src/nx/gpu/shader/maxwell_opcodes.def. It runs during the build, so a cross build also builds it for the machine
# doing the build, with that machine's C compiler (SNX_HOST_CC), as swipc.cmake does for swipc
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_shaderdec_dir "${CMAKE_BINARY_DIR}/shaderdec-host")
    set(snx_shaderdec_exe "${snx_shaderdec_dir}/bin/shaderdec")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_shaderdec_exe ".exe")
    endif()
    ExternalProject_Add(snx_shaderdec_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/shaderdec"
        BINARY_DIR "${snx_shaderdec_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_shaderdec_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_shaderdec_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_shaderdec_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-shaderdec IMPORTED GLOBAL)
    set_target_properties(snx-shaderdec PROPERTIES IMPORTED_LOCATION "${snx_shaderdec_exe}")
    add_dependencies(snx-shaderdec snx_shaderdec_host)
else()
    add_executable(snx-shaderdec "${PROJECT_SOURCE_DIR}/tools/shaderdec/shaderdec.c")
    set_target_properties(snx-shaderdec PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON
                                                   C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_options(snx-shaderdec PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_shaderdec(<target>): generates maxwell_opcodes.h (the enum) and maxwell_decode.inc (the decoder) into the build
# folder and gives <target> their include root
function(snx_shaderdec target)
    set(spec "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/maxwell_opcodes.def")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/shaderdec")
    add_custom_command(OUTPUT "${out}/maxwell_opcodes.h" "${out}/maxwell_decode.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-shaderdec --enum "${out}/maxwell_opcodes.h" --decoder "${out}/maxwell_decode.inc" "${spec}"
        DEPENDS snx-shaderdec "${spec}"
        COMMENT "Generating the Maxwell instruction decoder" VERBATIM)
    target_sources(${target} PRIVATE "${out}/maxwell_opcodes.h" "${out}/maxwell_decode.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()
