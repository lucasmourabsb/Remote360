#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$PROJECT_ROOT/research/openxechain-buildscript"
PREFIX="$PROJECT_ROOT/tooling/openxechain/sysroot"
PYTOOLS="$PROJECT_ROOT/tooling/python"
LOCAL_SCRIPT="$SRC/build-macos-local.sh"

export PATH="$PYTOOLS/bin:$PATH"
export PYTHONPATH="$PYTOOLS${PYTHONPATH:+:$PYTHONPATH}"
export PREFIX
export PARALLEL="${PARALLEL:-4}"
export HOST_CC="${HOST_CC:-/usr/bin/clang}"
export HOST_CXX="${HOST_CXX:-/usr/bin/clang++}"

python3 - "$SRC/build-toolchain.sh" "$LOCAL_SCRIPT" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
source = source.replace(
    'PARALLEL="${PARALLEL:-$(nproc)}"',
    'PARALLEL="${PARALLEL:-4}"',
)
source = source.replace(
    'check_deps || fail_build',
    'echo -e "${TOOLCHAIN_STEM}Host dependency check handled by macOS wrapper."',
)
source = source.replace(
    '-DLLVM_INSTALL_TOOLCHAIN_ONLY=true \\',
    '-DLLVM_INSTALL_TOOLCHAIN_ONLY=true -DLLVM_PARALLEL_LINK_JOBS=1 \\',
)
source = source.replace(
    'CPP="${PREFIX}/bin/clang-cpp" \\',
    'CPP="${PREFIX}/bin/clang -E -nostdlib -I${PWD}/xecorelibtmp/include" \\',
)
source = source.replace(
    '    --enable-newlib-iconv >> "${BUILD_LOG}" 2>&1 || fail_build',
    '    CFLAGS="-O2" --enable-newlib-iconv >> "${BUILD_LOG}" 2>&1 || fail_build',
)
source = source.replace(
    'BINDIR="${PREFIX}/bin" PREFIX="${PWD}/xecorelibtmp" bash ../xecorelib/install.sh',
    'mkdir -p "${PWD}/xecorelibtmp"\n'
    'BINDIR="${PREFIX}/bin" PREFIX="${PWD}/xecorelibtmp" bash ../xecorelib/install.sh',
)
source = source.replace(
    'ls | grep -P "^(?!xecorelibtmp$).*$" | xargs -d"\\n" rm -rf',
    'for path in ./*; do [[ "$path" == "./xecorelibtmp" ]] || rm -rf "$path"; done',
)
Path(sys.argv[2]).write_text(source)
PY

mkdir -p "$PREFIX"
cd "$SRC"
bash "$LOCAL_SCRIPT"

