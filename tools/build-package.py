#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate existing build outputs and prepare a self-contained development package.

Run the Android build/tests/lint and Xbox build first. This script does not install on devices.
It refuses to overwrite an existing versioned package directory.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.1.2"
NAME = f"Remote360-{VERSION}"


def command(args: list[str], env: dict[str, str] | None = None) -> str:
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True)
    text = result.stdout + result.stderr
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(args)}\n{text}")
    return text.strip()


def main() -> None:
    destination = ROOT / "dist" / NAME
    archive = ROOT / "dist" / f"{NAME}.zip"
    if destination.exists() or archive.exists():
        raise FileExistsError(f"Package already exists: {destination}; preserve it or change VERSION.")

    tc = ROOT / "tooling/openxechain/sysroot"
    apk = ROOT / "android/app/build/outputs/apk/debug/app-debug.apk"
    xbox = ROOT / "xbox360/out"
    lint = ROOT / "android/app/build/reports/lint-results-debug.xml"
    lint_root = ET.parse(lint).getroot()
    lint_errors = [i for i in lint_root.findall("issue") if i.get("severity") in {"Error", "Fatal"}]
    if lint_errors:
        raise RuntimeError("Android lint still reports errors: " + str([i.get("id") for i in lint_errors]))
    lint_warnings = [i for i in lint_root.findall("issue") if i.get("severity") == "Warning"]

    suites = []
    test_files = sorted((ROOT / "android/app/build/test-results/testDebugUnitTest").glob("TEST-*.xml"))
    if not test_files:
        raise RuntimeError("Android unit-test results are missing.")
    for path in test_files:
        suite = ET.parse(path).getroot()
        data = {key: suite.get(key) for key in ("name", "tests", "failures", "errors", "skipped")}
        if int(data["failures"] or 0) or int(data["errors"] or 0):
            raise RuntimeError(f"Failing Android test suite: {path}")
        suites.append(data)

    spec = importlib.util.spec_from_file_location("remote360_host_tests", ROOT / "tests/test_protocol.py")
    assert spec and spec.loader
    host_tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host_tests)
    passed_python = []
    for name in sorted(dir(host_tests)):
        if name.startswith("test_"):
            getattr(host_tests, name)()
            passed_python.append(name)

    xbox_checks = [command([
        sys.executable, str(ROOT / "xbox360/check_xex.py"), "--no-fp", str(tc / "bin/llvm-objdump"),
        str(xbox / "remote360.exe"), str(xbox / "remote360.xex"),
    ]), command([
        sys.executable, str(ROOT / "xbox360/check_xex.py"),
        str(xbox / "remote360_loader.exe"), str(xbox / "remote360_loader.xex"),
    ])]

    env = dict(os.environ)
    env["JAVA_HOME"] = str(ROOT / "tooling/jdk17/home")
    apk_signature = command([
        str(ROOT / "tooling/android-sdk/build-tools/35.0.0/apksigner"),
        "verify", "--verbose", str(apk),
    ], env)
    with zipfile.ZipFile(apk) as z:
        if z.testzip() is not None:
            raise RuntimeError("APK ZIP integrity check failed.")

    report = {
        "version": VERSION,
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "android_unit_tests": suites,
        "python_tests_passed": passed_python,
        "android_lint_errors": 0,
        "android_lint_warning_count": len(lint_warnings),
        "android_lint_warning_ids": sorted({i.get("id") for i in lint_warnings}),
        "xbox_static_checks": xbox_checks,
        "apk_signature_verification": apk_signature,
        "hardware_tested": False,
        "device_installation_performed": False,
        "audio_implemented": False,
        "video_target": "320x180 RGB565, 20 fps target, not measured",
        "limitations": [
            "Host Android tests use framework stubs, not a physical controller or Activity.",
            "No measured gameplay, video correctness, vibration, latency or crash behavior.",
            "LAN-only, unauthenticated protocol. Kernel 17559-specific Xbox implementation.",
        ],
    }

    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".remote360-package-", dir=archive.parent) as temp:
        staging = Path(temp) / NAME
        staging.mkdir()

        def copy(source: Path, relative: str | Path) -> None:
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

        copy(apk, f"Android/Remote360-{VERSION}-debug.apk")
        for filename in ("remote360.xex", "remote360_loader.xex"):
            copy(xbox / filename, "Xbox360/Apps/Remote360/" + filename)
        for filename in ("LEIA-ME.txt", "LICENSE", "THIRD_PARTY.md"):
            copy(ROOT / filename, filename)
        copy(ROOT / "protocol/PROTOCOL.md", "PROTOCOL.md")
        for path in test_files:
            copy(path, "validation/" + path.name)
        copy(lint, "validation/android-lint.xml")
        for filename in ("remote360.map", "remote360_loader.map", "symbols.txt"):
            copy(xbox / filename, "validation/" + filename)

        source_paths = []
        for directory in ("core", "protocol", "tests", "tools", "android/app/src"):
            source_paths.extend(p for p in (ROOT / directory).rglob("*")
                                if p.is_file() and "__pycache__" not in p.parts)
        source_paths.extend(p for p in (ROOT / "xbox360").iterdir() if p.is_file())
        source_paths.extend(ROOT / name for name in (
            "README.md", "BUILD.md", "LEIA-ME.txt", "LICENSE", "THIRD_PARTY.md",
            "android/build.gradle", "android/settings.gradle", "android/gradle.properties",
            "android/app/build.gradle", "tooling/build-openxechain-macos.sh",
            "tooling/synthxex_dll_patch.py",
        ))
        for path in sorted(set(source_paths)):
            copy(path, Path("Source") / path.relative_to(ROOT))

        (staging / "VALIDACAO.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        entries = []
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                entries.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(staging).as_posix()}")
        (staging / "SHA256SUMS.txt").write_text("\n".join(entries) + "\n")
        temp_archive = Path(temp) / archive.name
        with zipfile.ZipFile(temp_archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(staging.rglob("*")):
                if path.is_file():
                    z.write(path, Path(NAME) / path.relative_to(staging))
        with zipfile.ZipFile(temp_archive) as z:
            if z.testzip() is not None:
                raise RuntimeError("Release archive integrity check failed.")
            for entry in entries:
                digest, relative = entry.split("  ", 1)
                if hashlib.sha256(z.read(f"{NAME}/{relative}")).hexdigest() != digest:
                    raise RuntimeError(f"Release checksum mismatch: {relative}")
        shutil.move(str(staging), destination)
        shutil.move(str(temp_archive), archive)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{digest}  {archive.name}\n")
    print(json.dumps({
        "package": str(archive), "bytes": archive.stat().st_size, "sha256": digest,
        "java_tests": sum(int(s["tests"]) for s in suites),
        "python_tests": len(passed_python), "lint_errors": 0,
        "lint_warnings": len(lint_warnings), "hardware_tested": False,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ET.ParseError, zipfile.BadZipFile, AssertionError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
