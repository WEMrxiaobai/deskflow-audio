"""Prepare a smaller Windows audio installer stage without changing the full stage.

Qt translations are included only for languages exposed by the app. The MSVC
runtime subset is derived from the PE imports of the staged binaries and then
closed over the selected runtime DLLs. Qt plugins remain available because Qt
loads them dynamically and static PE imports cannot establish that they are unused.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path


IMPORT = re.compile(r"^\s+([^\s]+\.dll)\s*$", re.IGNORECASE | re.MULTILINE)


def imports(dumpbin: Path, binary: Path) -> set[str]:
    result = subprocess.run(
        [str(dumpbin), "/dependents", str(binary)], check=True, capture_output=True, text=True, errors="replace"
    )
    return {name.lower() for name in IMPORT.findall(result.stdout)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--crt", type=Path, required=True)
    parser.add_argument("--dumpbin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-crt", type=Path, required=True)
    args = parser.parse_args()
    stage = args.stage.resolve(strict=True)
    crt = args.crt.resolve(strict=True)
    dumpbin = args.dumpbin.resolve(strict=True)
    output = args.output.resolve()
    output_crt = args.output_crt.resolve()
    if output.exists() or output_crt.exists():
        raise ValueError("Output paths must be new; existing files are never replaced")
    if stage in output.parents or stage in output_crt.parents or output == output_crt:
        raise ValueError("Output paths must be separate from the original stage")
    if "x64" not in [part.lower() for part in output_crt.parts]:
        raise ValueError("Output CRT path must identify the x64 architecture")
    stage_files = list(stage.rglob("*"))
    if any(path.is_symlink() for path in stage_files):
        raise ValueError("The original stage must not contain links")
    binaries = [path for path in stage_files if path.is_file() and path.suffix.lower() in {".exe", ".dll"}]
    if not binaries:
        raise ValueError("No staged binaries found")
    crt_files = {path.name.lower(): path for path in crt.glob("*.dll")}
    if not crt_files:
        raise ValueError("No redistributable runtime DLLs found")
    needed: set[str] = set()
    pending = set().union(*(imports(dumpbin, path) for path in binaries)) & crt_files.keys()
    while pending:
        name = pending.pop()
        if name in needed:
            continue
        needed.add(name)
        pending.update((imports(dumpbin, crt_files[name]) & crt_files.keys()) - needed)
    required = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
    if not required <= needed:
        raise ValueError("Runtime dependency scan did not find the expected MSVC libraries")
    translations = stage / "translations"
    languages = {path.stem.removeprefix("deskflow_") for path in translations.glob("deskflow_*.qm")}
    if "zh_CN" not in languages:
        raise ValueError("The simplified Chinese application translation is missing")
    output.mkdir(parents=True)
    output_crt.mkdir(parents=True)
    excluded = []
    for path in stage_files:
        if not path.is_file():
            continue
        relative = path.relative_to(stage)
        if relative.parent == Path("translations") and path.name.startswith("qt_") and path.suffix == ".qm":
            if path.stem.removeprefix("qt_") not in languages:
                excluded.append(relative.as_posix())
                continue
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    for name in sorted(needed):
        shutil.copy2(crt_files[name], output_crt / crt_files[name].name)
    print(json.dumps({"binaries_scanned": len(binaries), "application_languages": sorted(languages),
                      "runtime_dlls": sorted(needed), "excluded_qt_translations": excluded,
                      "excluded_runtime_dlls": sorted(crt_files.keys() - needed)}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
