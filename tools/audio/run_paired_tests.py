#!/usr/bin/env python3
"""Run the Windows paired-audio tests with isolated, explicit dependencies.

Build deskflow-paired-audio-control-tests and deskflow-paired-audio-group-tests
in Release first. Run this script from any directory; use --help for overrides.
The tests use temporary settings, certificates and OpenSSL configuration.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "desktop-audio")
    parser.add_argument("--qt-dir", type=Path, default=ROOT / "deps" / "qt" / "6.8.3" / "msvc2022_64")
    parser.add_argument("--openssl-dir", type=Path, default=ROOT / "deps" / "openssl-conda-3.5.4" / "Library")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("these paired-audio fixtures require Windows")

    binary_dir = args.build_dir.resolve() / "bin" / "Release"
    qt_dir = args.qt_dir.resolve()
    openssl_dir = args.openssl_dir.resolve()
    openssl = openssl_dir / "bin" / "openssl.exe"
    tests = [binary_dir / "deskflow-paired-audio-control-tests.exe",
             binary_dir / "deskflow-paired-audio-group-tests.exe"]
    required = [*tests, openssl, qt_dir / "bin" / "Qt6Core.dll",
                qt_dir / "plugins" / "platforms" / "qoffscreen.dll",
                qt_dir / "plugins" / "tls" / "qopensslbackend.dll"]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("Required test artifacts are missing:\n" + "\n".join(missing), file=sys.stderr)
        return 2

    # A short path under the build directory avoids the system TEMP ACL and
    # keeps the test executable's portable settings outside the real install.
    work_root = args.build_dir.resolve() / "test-tmp"
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="paired-", dir=work_root) as temporary:
        root = Path(temporary)
        config = root / "openssl.cnf"
        config.write_text("[req]\ndistinguished_name=dn\n[dn]\n", encoding="ascii")
        env = os.environ.copy()
        env.update({"TEMP": str(root), "TMP": str(root), "OPENSSL_CONF": str(config),
                    "QT_QPA_PLATFORM": "offscreen", "QT_PLUGIN_PATH": str(qt_dir / "plugins"),
                    "QT_QPA_PLATFORM_PLUGIN_PATH": str(qt_dir / "plugins" / "platforms"),
                    "QT_FORCE_STDERR_LOGGING": "1"})
        fonts = Path(env.get("WINDIR", r"C:\Windows")) / "Fonts"
        # Offscreen Qt uses FreeType; a directory with one regular TTF avoids
        # scanning system font collections that FreeType cannot open.
        arial = fonts / "arial.ttf"
        if arial.is_file():
            isolated_fonts = root / "fonts"
            isolated_fonts.mkdir()
            shutil.copy2(arial, isolated_fonts / arial.name)
            env["QT_QPA_FONTDIR"] = str(isolated_fonts)
        elif fonts.is_dir():
            env["QT_QPA_FONTDIR"] = str(fonts)
        env["PATH"] = os.pathsep.join([str(qt_dir / "bin"), str(openssl_dir / "bin"),
                                        str(binary_dir), env.get("PATH", "")])
        for test in tests:
            folder = root / test.stem
            folder.mkdir()
            isolated = folder / test.name
            shutil.copy2(test, isolated)
            command = [str(isolated)]
            if "control-tests" in test.name:
                command.append(str(openssl))
            print(f"Running {test.name}", flush=True)
            try:
                result = subprocess.run(command, cwd=folder, env=env, timeout=120, check=False)
            except subprocess.TimeoutExpired:
                print(f"{test.name} timed out after 120 seconds", file=sys.stderr)
                return 1
            if result.returncode:
                print(f"{test.name} failed with exit code {result.returncode}", file=sys.stderr)
                return 1
        print("Both isolated paired-audio tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
