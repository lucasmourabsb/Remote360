#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")"

TC=${OXC:-/opt/openxechain/sysroot}
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir -p out

name=remote360_video
flags=()
sources=(remote360.c platform_min.c)
case "${1:-}" in
    "") flags=(-DR360_VIDEO_ONLY) ;;
    --fast-monitor)
        name=remote360_monitor
        flags=(-DR360_VIDEO_ONLY -DR360_VIDEO_INTERVAL_MS=50 -DR360_VIDEO_PACKETS_PER_BURST=40)
        ;;
    --controls)
        name=remote360_controls
        flags=(-DXA_HOOK_CAPS=1 -DXA_HOOK_SET=0 -DR360_VIDEO_INTERVAL_MS=50 -DR360_VIDEO_PACKETS_PER_BURST=40)
        sources+=(xinput_360.c detour.c)
        ;;
    --next)
        name=remote360_next
        flags=(-DXA_HOOK_CAPS=1 -DXA_HOOK_SET=1 -DR360_EXPERIMENTAL_MEDIA -DR360_VIDEO_INTERVAL_MS=33 -DR360_VIDEO_PACKETS_PER_BURST=56)
        sources+=(xinput_360.c detour.c)
        ;;
    --settings)
        name=remote360_settings
        flags=(-DXA_HOOK_CAPS=1 -DXA_HOOK_SET=1 -DR360_EXPERIMENTAL_MEDIA -DR360_VIDEO_SETTINGS
               -DR360_VIDEO_INTERVAL_MS=33 -DR360_VIDEO_PACKETS_PER_BURST=56)
        sources+=(xinput_360.c detour.c video_jpeg.c)
        ;;
    --latency|--latency-probe|--latency-scan|--latency-dual|--play|--snapshot-32|--snapshot-check)
        name=remote360_latency
        flags=(-DXA_HOOK_CAPS=1 -DXA_HOOK_SET=1 -DR360_EXPERIMENTAL_MEDIA -DR360_VIDEO_SETTINGS
               -DR360_LOW_LATENCY -DR360_VIDEO_INTERVAL_MS=33 -DR360_VIDEO_PACKETS_PER_BURST=56)
        if [[ "$1" == --latency-probe ]]; then
            name=remote360_latency_probe
            flags+=(-DR360_CAPTURE_PROFILE -DR360_MAP_REUSE)
        elif [[ "$1" == --latency-scan ]]; then
            name=remote360_latency_scan
            flags+=(-DR360_CAPTURE_PROFILE -DR360_FAST_SCAN)
        elif [[ "$1" == --latency-dual ]]; then
            name=remote360_latency_dual2
            flags+=(-DR360_CAPTURE_PROFILE -DR360_FAST_SCAN -DR360_PARALLEL_BC1)
        elif [[ "$1" == --play || "$1" == --snapshot-32 || "$1" == --snapshot-check ]]; then
            name=remote360_play
            flags+=(-DR360_CAPTURE_PROFILE -DR360_FAST_SCAN -DR360_PARALLEL_BC1 -DR360_FUSED_SCAN)
            if [[ "$1" != --play ]]; then
                name=remote360_snapshot32c
                flags+=(-DR360_SCANOUT_SNAPSHOT)
                if [[ "$1" == --snapshot-check ]]; then
                    name=remote360_snapshot_check
                    flags+=(-DR360_SNAPSHOT_LOAD_ONLY)
                fi
            fi
        fi
        sources+=(xinput_360.c detour.c video_jpeg.c)
        ;;
    *) printf '%s\n' 'Usage: build-video.sh [--fast-monitor|--controls|--next|--settings|--latency|--latency-probe|--latency-scan|--latency-dual|--play|--snapshot-32|--snapshot-check]' >&2; exit 2 ;;
esac

"$TC/bin/clang" -O2 -Wall -msoft-float -fno-zero-initialized-in-bss \
    "${flags[@]}" "-Wl,/entry:DllMain,/align:0x1000,/base:0x91C00000,/map:out/$name.map" \
    -x assembler-with-cpp thunks.s -x c "${sources[@]}" -o "out/$name.exe"
"$TC/bin/synthxex-patched" -i "out/$name.exe" -o "out/$name.xex" -t sysdll
python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" "out/$name.exe" "out/$name.xex"
if [[ "$name" == remote360_video || "$name" == remote360_monitor ]] &&
   rg -q 'Xa(ReadState|Caps|SetState)Hook|xa_xinput|xa_trampolines' "out/$name.map"; then
    printf '%s\n' 'Unexpected controller hooks in the video-only module.' >&2
    exit 1
fi
