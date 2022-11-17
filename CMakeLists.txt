#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.7.2)

include(settings.cmake)

project(sel4KanataBench C CXX ASM)

find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)


# Set the roottask CNode size to be large enough for DTB, timer caps, etc
# Note that this works for the platforms that we have at the moment, and may
# need to be increased in the future
set(KernelRootCNodeSizeBits 13 CACHE INTERNAL "")


if(NOT Sel4benchAllowSettingsOverride)
    if(KernelArchARM)
        if(KernelPlatformOMAP3 OR KernelPlatformAM335X)
            set(KernelDangerousCodeInjection ON CACHE BOOL "" FORCE)
        else()
            set(KernelArmExportPMUUser ON CACHE BOOL "" FORCE)
        endif()
    else()
        set(KernelArmExportPMUUser OFF CACHE BOOL "" FORCE)
    endif()
    if(KernelArchX86)
        set(KernelExportPMCUser ON CACHE BOOL "" FORCE)
        set(KernelX86DangerousMSR ON CACHE BOOL "" FORCE)
#        set(AllowUnstableOverhead ON CACHE BOOL "" FORCE)
        set(KernelX86MicroArch "haswell" CACHE STRING "" FORCE)
        set(KernelXSaveFeatureSet 7 CACHE STRING "" FORCE)
        set(KernelXSaveSize 832 CACHE STRING "" FORCE)
    endif()
    set(KernelRootCNodeSizeBits 13 CACHE STRING "" FORCE)
#    set(KernelTimeSlice 500 CACHE STRING "" FORCE)
#    set(KernelTimerTickMS 1000 CACHE STRING "" FORCE)
    set(KernelVerificationBuild OFF CACHE BOOL "" FORCE)
endif()

sel4_import_kernel()

# ??? what is it for ?
if((NOT Sel4testAllowSettingsOverride) AND (KernelArchARM OR KernelArchRiscV))
    # Elfloader settings that correspond to how Data61 sets its boards up.
    ApplyData61ElfLoaderSettings(${KernelPlatform} ${KernelSel4Arch})
endif()
elfloader_import_project()


set(configure_string "")
add_config_library(sel4KanataBench "${configure_string}")

find_package(musllibc REQUIRED)
find_package(util_libs REQUIRED)
find_package(seL4_libs REQUIRED)
find_package(sel4_projects_libs REQUIRED)

musllibc_setup_build_environment_with_sel4runtime()
sel4_import_libsel4()
util_libs_import_libraries()
sel4_libs_import_libraries()

#set(LibNanopb ON CACHE BOOL "" FORCE)
#set(UserLinkerGCSections OFF CACHE BOOL "" FORCE)
sel4_projects_libs_import_libraries()

file(
    GLOB
        deps
        main.c
#        src/tests/*.c
#        src/tests/*.S
#        src/arch/${arch}/*.c
#        src/arch/${KernelArch}/tests/*.c
#        src/*.cxx
#        src/tests/*.cxx
#        src/arch/${KernelArch}/tests/*.S
)
add_executable(sel4KanataBench EXCLUDE_FROM_ALL ${deps})

set(_inc_folder_KernelSel4Arch "${KernelSel4Arch}")


#message (${KernelArch})

target_include_directories(
    sel4KanataBench
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sel4runtime/gen_config/"
)

#target_include_directories(
#    sel4KanataBench
#    PUBLIC "include/" "libsel4bench/arch_include/${KernelArch}/" "arch_include/${KernelArch}/" "sel4_arch_include/${KernelSel4Arch}/"
#)


target_link_libraries(
    sel4KanataBench
    PUBLIC
        sel4_autoconf
        muslc
        sel4
        sel4runtime
        sel4allocman
        sel4vka
        sel4utils
        sel4sync
        sel4muslcsys
        sel4bench
)

# Set this image as the rootserver
include(rootserver)
DeclareRootserver(sel4KanataBench)
