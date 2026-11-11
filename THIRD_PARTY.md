# Upstream work and research

Remote360 is GPL-3.0-or-later. Preserve LICENSE and the original source headers when redistributing.
The package includes the corresponding Remote360 source, tests and build scripts.

| Project | Revision inspected | Use |
| --- | --- | --- |
| Jacckii/Xbox360-Series-Controllers-Native | 7e533d7cff16b92d5271cea645a52d5092070864 | XInput code, compatibility definitions, detours, loader and entry-thunk approach |
| Jacckii/Xbox360-Series-Controllers-Bridge | ad6e4ba66660b72f3fbe268887d0968809a3d049 | OpenXeChain build guidance and SynthXEX system-DLL patch |
| EinTim23/hiddriver360 | 6159866ff0049d379238751d50a19409abf08e9d | Upstream virtual-controller binding and input architecture |
| rovyn-win/XBDM | c6bf8f700c6733150977b347b8b25d53723f3b48 | Research into display information and the screenshot path |
| xecli/xecli | 68749c3d624b1551cce14e1d366e927157311b01 | Research into screenshot pixel decoding and Xenos tiling |
| bitbank2/JPEGENC | f51421bdaba8e098dc82130a17adaae6d61c3377 | JPEG encoding in the configurable Xbox module, Apache-2.0 |

Repository sources:

https://github.com/Jacckii/Xbox360-Series-Controllers-Native
https://github.com/Jacckii/Xbox360-Series-Controllers-Bridge
https://github.com/EinTim23/hiddriver360
https://github.com/rovyn-win/XBDM
https://github.com/xecli/xecli
https://github.com/bitbank2/JPEGENC

The vendored JPEGENC files are in `xbox360/vendor/jpge`. The port uses native-endian Huffman words
and the 32-bit writer. The adapter replicates image edges before passing partial blocks to the
encoder. The original Apache-2.0 license is included alongside the source.

Toolchain: https://github.com/OpenXeChain/buildscript, with LLVM, Newlib, compiler-rt, xecorelib
and SynthXEX. Toolchain revisions and the local bootstrap limitations are recorded in BUILD.md.
The full toolchain and third-party research checkouts are not bundled in the installation ZIP.

Android implementation references:

https://developer.android.com/games/sdk/game-controller/controller-input
https://developer.android.com/guide/components/activities/activity-lifecycle

Remote360 adaptations include UDP transport, Android input, opt-in raw video, watchdog handling,
binding completion reporting, ACK timing, frame validation, HDD/USB loader discovery and packaging.
Upstream projects' reports of working hardware are not evidence that this new combination works.
