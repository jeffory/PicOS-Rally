#!/usr/bin/env python3
"""TAP tests for tools/bundle.py.

Each test builds a throwaway app dir under tempfile, so the real repo is
never mutated. This hermetic approach lets tests run without an ARM build or
a pre-built main.elf.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle  # noqa: E402

REPO_ROOT = bundle.REPO_ROOT

# The complete bundle, sorted by arcname. See the spec for why this set and
# not a glob over assets/.
EXPECTED = [
    "app.json",
    "assets/car.bin",
    "assets/clut.bin",
    "assets/hero.bin",
    "assets/props.bin",
    "assets/tiles_proc.bin",
    "assets/tiles_wang_grass_gravel.bin",
    "assets/tiles_wang_sand_grass.bin",
    "assets/tiles_wang_water_sand.bin",
    "main.elf",
    "stage01.bin",
    "tuning/handling.toml",
]

_checks = 0
_failures = 0


def check(name, cond):
    global _checks, _failures
    _checks += 1
    if cond:
        print(f"ok {_checks} - {name}")
    else:
        print(f"not ok {_checks} - {name}")
        _failures += 1


def raises(name, fn):
    """Assert fn() raises BundleError, not some other exception."""
    try:
        fn()
    except bundle.BundleError:
        check(name, True)
        return
    except Exception as e:  # noqa: BLE001
        print(f"# unexpected {type(e).__name__}: {e}")
    check(name, False)


def make_fixture(tmp):
    """A complete, valid app dir copied out of the real tree."""
    for rel in ("app.json", "stage01.bin"):
        shutil.copy2(os.path.join(REPO_ROOT, rel), os.path.join(tmp, rel))
    shutil.copytree(os.path.join(REPO_ROOT, "assets"),
                    os.path.join(tmp, "assets"))
    shutil.copytree(os.path.join(REPO_ROOT, "tuning"),
                    os.path.join(tmp, "tuning"))
    os.makedirs(os.path.join(tmp, "core"))
    shutil.copy2(os.path.join(REPO_ROOT, "core", "tiles_sections.h"),
                 os.path.join(tmp, "core", "tiles_sections.h"))
    # Stub main.elf so the fixture has the binary to resolve; its contents
    # are never read by bundle_files().
    with open(os.path.join(tmp, "main.elf"), "wb") as f:
        f.write(b"\x7fELF stub")
    # The vendored SDK example that a glob would wrongly pick up.
    os.makedirs(os.path.join(tmp, "sdk", "native"))
    with open(os.path.join(tmp, "sdk", "native", "main.elf"), "wb") as f:
        f.write(b"\x7fELF wrong")
    return tmp


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = make_fixture(tmp)

        files = bundle.bundle_files(root)
        arcs = [a for _, a in files]
        check("bundle contains exactly 12 files", len(files) == 12)
        check("bundle arcnames match the expected set", arcs == EXPECTED)
        check("main.elf resolves to the repo root, not sdk/native",
              all(not s.replace(os.sep, "/").endswith("sdk/native/main.elf")
                  for s, _ in files))
        check("tile sections come from tiles_sections.h",
              bundle.tile_section_names(root) == [
                  "wang_water_sand", "wang_sand_grass",
                  "wang_grass_gravel", "proc"])

    print(f"1..{_checks}")
    print(f"# {_checks - _failures}/{_checks} checks passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
