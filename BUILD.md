# Building Remote360

## Existing Mac workspace

The existing project-local OpenXeChain installation is already built. Do not repeat the full LLVM
bootstrap to rebuild the two small Xbox targets.

```sh
OXC="$PWD/tooling/openxechain/sysroot" bash xbox360/build.sh

export JAVA_HOME="$PWD/tooling/jdk17/home"
export ANDROID_HOME="$PWD/tooling/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
"$PWD/tooling/gradle/gradle-8.9/bin/gradle" --no-daemon -p android \
  :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

The APK uses Java 17, Gradle 8.9, AGP 8.7.3, compile/target SDK 35 and minimum SDK 26.
For another machine, set its SDK path with ANDROID_HOME or android/local.properties. The latter
contains a local absolute path and is deliberately not included in the source package.

## Xbox toolchain

OpenXeChain Clang 20.1.5 targets ppc32-unknown-xbox360. The installed compiler configuration uses
Newlib, xecorelib and libclang_rt.builtins-powerpc.a. The source revisions used for the original
toolchain build were recorded as:

| Component | Revision |
| --- | --- |
| buildscript | eed1fa65bf9577fd31625764b320a90182ea9ade |
| llvm | 890b83f6c8259a8899e182a5f7d9cf39c64131cc |
| newlib | f929633c27099d404e9cb5b2739a9c7f9b6afccc |
| synthxex | 48d1453a55468aa2f8a211db0b20edd594ef5be3 |
| xecorelib | c65d67e071357acade681f04c46ae9719797f239 |

Sources: https://github.com/OpenXeChain/buildscript and its pinned submodules.
The macOS wrapper is `tooling/build-openxechain-macos.sh`. It now creates xecorelibtmp before
the upstream installer invokes realpath. This particular clean-bootstrap fix has not been
validated by rebuilding the entire toolchain. The successful application builds use the existing
installation, which was repaired during initial setup.

System DLLs require the upstream Bridge project's SynthXEX patch, included at
`tooling/synthxex_dll_patch.py`. Apply it once in the `src` directory of a clean copy of the pinned
SynthXEX checkout, then build SynthXEX with the version define required by that revision:

```sh
python3 /path/to/Remote360/tooling/synthxex_dll_patch.py
```

From the parent directory of that patched `src` directory, compile the C files under `src` and
`include` with `-O2 -Iinclude -DSYNTHXEX_VERSION='"v0.0.5"'`, producing `synthxex-patched` in the
toolchain's bin directory. The current workspace also uses that patched executable as `synthxex`.
The application build expects both names.

The expected linker warning `/align specified without /driver` is also documented by the upstream
Bridge project. Its presence is not a runtime validation. `check_xex.py` separately verifies image
coverage and rejects floating-point instructions in the system DLL. Linker maps now provide the
addresses of `g_r360`, `g_stage` and the XInput diagnostic counters.

The local loader was revised after the failed 0.1.2 console test. It now uses
`loader_entry.s` and a freestanding C body with Newlib excluded. Both loader targets
also pass the no-floating-point check. `remote360_loader_probe.xex` skips plugin
loading and is intended to isolate entry and dashboard hand-off. The tool blocked
the upload-and-launch command before execution, so this diagnostic build has not
run on the console. The failed build is preserved in `backups/failed-0.1.2/`.

## Current fluency build and capture profiler

`bash xbox360/build-video.sh --latency` builds the Xbox module for Remote360 0.1.9.
`--latency-probe` builds the timing experiment with cached validation of the 16 MiB
physical mapping. `--latency-scan` keeps the normal page checks, uses a cheaper BC1
encoder, and reconstructs alternate rows in fluency mode when the client requests 60 FPS.
`--latency-dual` also splits the compression between CPU 3 and CPU 5. It uses the same APK.
Use only one Remote360 module per boot: they share the load address, UDP port and controller hooks.

With the probe loaded and **Priorizar fluidez** active, read the latest completed fast frame
using `python3 tools/watch_xbox.py XBOX_IP latency-probe --seconds 30`. It adds time in
microseconds for the display query, the validation of the surface's memory pages, the pixel
copy, the controller and network service inside capture, encoding and transmission.
Input service can overlap the pixel-copy measurement; the two values should not be added.
These are timings on the console, not TV-to-Thor latency. For scan and dual builds use
the corresponding `latency-scan` or `latency-dual2` argument to the watcher.

## Validation boundaries for the original loader build

Java unit tests exercise input/status encoding, malformed video rejection, exact frame assembly,
duplicates, out-of-order delivery, frame-id wrap and ACK correlation. Android framework methods
are stubbed in these host tests, so they do not verify gamepad events, Activity lifecycle, rendering,
network scheduling or vibration on an Android device. The Python tests independently check the
host input packet and status decoder. The original 0.1.2 console launch failed.
After a physical restart, a fresh read-only check confirmed recovery to Aurora.
The revised loader and probe have only been checked locally.

Build success, static checks and APK signature verification are recorded in the package. Audio,
actual gameplay, loader behavior, image correctness and end-to-end latency remain unverified.
