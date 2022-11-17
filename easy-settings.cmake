#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

# Define our top level settings.  Whilst they have doc strings for readability
# here, they are hidden in the cmake-gui as they cannot be reliably changed
# after the initial configuration.  Enterprising users can still change them if
# they know what they are doing through advanced mode.
#
# Users should initialize a build directory by doing something like:
#
# mkdir build_sabre
# cd build_sabre
#
# Then
#
# ../griddle --PLATFORM=sabre --SIMULATION
# ninja
#
set(FASTPATH ON CACHE BOOL "Turn fastpath on or off")
set(RELEASE ON CACHE BOOL "Performance optimized build")
set(MCS ON CACHE BOOL "MCS kernel")

set(VERIFICATION OFF CACHE BOOL "Only verification friendly kernel features")
set(PLATFORM "x86_64" CACHE STRING "Platform to test")
set(KernelSel4Arch "" CACHE STRING "aarch32, aarch64, arm_hyp, ia32, x86_64, riscv32, riscv64")

