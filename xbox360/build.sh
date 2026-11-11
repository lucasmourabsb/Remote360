#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")"

TC=${OXC:-/opt/openxechain/sysroot}
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir -p out

# GODStixBg occupies 0x91E00000 on the target console. Keep our image separate.
DLL_LINK="-Wl,/entry:DllMain,/align:0x1000,/base:0x91C00000,/map:out/remote360.map"
CFLAGS_DLL="-O2 -Wall -Wno-unused-parameter -fno-zero-initialized-in-bss -msoft-float"

"$TC/bin/clang" $CFLAGS_DLL -DXA_HOOK_CAPS=1 -DXA_HOOK_SET=1 $DLL_LINK \
    thunks.s remote360.c platform_min.c xinput_360.c detour.c -o out/remote360.exe
"$TC/bin/synthxex-patched" -i out/remote360.exe -o out/remote360.xex -t sysdll >/dev/null

LOADER_FLAGS="-O2 -Wall -Wextra -ffreestanding -msoft-float -fno-zero-initialized-in-bss -nostdlib"
LOADER_LINK="-Wl,/entry:stage_entry,/nodefaultlib:libc.a,/nodefaultlib:libclang_rt.builtins-powerpc.a"
"$TC/bin/clang" $LOADER_FLAGS $LOADER_LINK loader_entry.s loader.c \
    -Wl,/map:out/remote360_loader.map -o out/remote360_loader.exe
"$TC/bin/synthxex" -i out/remote360_loader.exe -o out/remote360_loader.xex -t title >/dev/null

"$TC/bin/clang" $LOADER_FLAGS $LOADER_LINK -DR360_LOADER_PROBE loader_entry.s loader.c \
    -Wl,/map:out/remote360_loader_probe.map -o out/remote360_loader_probe.exe
"$TC/bin/synthxex" -i out/remote360_loader_probe.exe -o out/remote360_loader_probe.xex -t title >/dev/null

python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/remote360.exe out/remote360.xex
python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/remote360_loader.exe out/remote360_loader.xex
python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/remote360_loader_probe.exe out/remote360_loader_probe.xex
grep -E 'g_r360|xa_xi_(on|calls|read_ctx)' out/remote360.map > out/symbols.txt || true
ls -lh out/*.xex

