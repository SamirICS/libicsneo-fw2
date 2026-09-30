"""Install the built icsneopy extension module into the active virtualenv.

After this, any script/REPL in this venv can do a plain `import icsneopy`
with no sys.path hacking -- unlike fw2_icsneopy_can.py's built-in fallback,
which only patches sys.path for that one script.

Run again after every `cmake --build ... --target icsneopy` rebuild.
"""

from __future__ import annotations

import shutil
import sys
import sysconfig
from pathlib import Path

BUILD_DIRS = [
    Path(r"C:\fw2b\bindings\python\Release"),
    Path(r"C:\fw2b\bindings\python"),
    Path(__file__).resolve().parents[1] / "libicsneo-fw2" / "build-fw2" / "bindings" / "python" / "Release",
]


def find_built_module() -> Path:
    for build_dir in BUILD_DIRS:
        matches = sorted(build_dir.glob("icsneopy*.pyd")) if build_dir.is_dir() else []
        if matches:
            return matches[0]
    raise SystemExit(
        "no built icsneopy module found. Build it first:\n"
        r"  cmake -S libicsneo-fw2 -B C:\fw2b -DLIBICSNEO_ENABLE_BINDINGS_PYTHON=ON" "\n"
        r"  cmake --build C:\fw2b --config Release --target icsneopy"
    )


def main() -> int:
    module_path = find_built_module()
    site_packages = Path(sysconfig.get_paths()["purelib"])
    package_dir = site_packages / "icsneopy"
    package_dir.mkdir(parents=True, exist_ok=True)

    # Named __init__<ext> so the folder itself becomes the importable package;
    # this is the layout `import icsneopy` actually resolves.
    dest = package_dir / f"__init__{module_path.suffix}"
    shutil.copy2(module_path, dest)
    print(f"installed {module_path.name} -> {dest}")

    stub = module_path.with_suffix(".pyi").parent / "icsneopy.pyi"
    if stub.is_file():
        shutil.copy2(stub, package_dir / "icsneopy.pyi")
        print(f"installed type stub -> {package_dir / 'icsneopy.pyi'}")

    print(f"\ninto: {site_packages}")
    print(f"python: {sys.executable}")
    print("done. `import icsneopy` should now work from any script using this interpreter.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
