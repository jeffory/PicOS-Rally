# GitHub Actions CI and initial prerelease: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and test PicOS Rally on every push, and publish an installable app bundle as a GitHub prerelease when a `v*` tag is pushed.

**Architecture:** A new `tools/bundle.py` owns the single definition of what an app bundle contains, verifies it against `assets/manifest.json`, and emits a deterministic zip. Both `tools/rally_hw.py` (deploy to hardware) and `.github/workflows/release.yml` (publish) call it, so what you test on device is what players download. `.github/workflows/ci.yml` gates every push on the existing headless test suite plus an ARM cross-build.

**Tech Stack:** Python 3 standard library only (no new dependencies), GNU Make, ARM GNU Toolchain 14.3.rel1, GitHub Actions, `gh` CLI.

**Spec:** `docs/superpowers/specs/2026-07-28-github-actions-release-design.md`

## Global Constraints

- **No new Python dependencies.** `tools/bundle.py` and `tools/test_bundle.py` use the standard library only. `tools/requirements.txt` stays at `pyserial==3.5` and `pillow==12.1.1`.
- **ARM toolchain is pinned.** Version `14.3.rel1`, SHA256 `8f6903f8ceb084d9227b9ef991490413014d991874a1e34074443c2a72b14dbd`. Both workflows must use these exact values.
- **Every GitHub Action pinned to a full commit SHA**, with the version in a trailing comment. Approved pins:
  - `actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1`
  - `actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0`
  - `actions/attest-build-provenance@0f67c3f4856b2e3261c31976d6725780e5e4c373 # v4.1.1`
- **Runner pinned** to `ubuntu-24.04`, never `ubuntu-latest`.
- **Least privilege.** Top-level `permissions: contents: read` in both workflows. Only the release job widens it.
- **Never `pull_request_target`.** Only `pull_request`.
- **Australian English** in all comments and documentation. No em dashes or en dashes in prose.
- **TAP output convention.** Python tests print `ok N - name` / `not ok N - name`, then `1..N`, and exit with the failure count, matching `plat/headless/main.c`.
- The bundle is exactly these 12 arcnames, sorted:
  `app.json`, `assets/car.bin`, `assets/clut.bin`, `assets/hero.bin`, `assets/props.bin`, `assets/tiles_proc.bin`, `assets/tiles_wang_grass_gravel.bin`, `assets/tiles_wang_sand_grass.bin`, `assets/tiles_wang_water_sand.bin`, `main.elf`, `stage01.bin`, `tuning/handling.toml`

---

## Domain Notes

Read these before starting. They explain why the code looks the way it does.

**What a PicOS app dir looks like.** The game reads its data with paths built from a runtime `app_dir`. `app/main.c:116` builds `<app_dir>/assets/<name>`, `app/main.c:180` builds `<app_dir>/tuning/handling.toml`, and `app/main.c:196` builds `<app_dir>/stage01.bin`. The repo root is laid out identically to the on-device app dir, which is why `tools/rally_hw.py:10` sets `APP_DIR` to the repo root. The bundle zip reproduces that layout so it can be unzipped straight onto the device.

**Only 8 asset files are ever loaded.** `load_assets()` at `app/main.c:144` opens `clut.bin`, then one `tiles_<name>.bin` per entry in `TILE_SEC_NAMES`, then `car.bin`, `props.bin`, `hero.bin`. `core/tiles_sections.h:4` currently lists four sections. The three `tiles_path_*.bin` files in `assets/` are dead since M4 replaced the RPG path tiles with the procedural road, and every `.bin.json` sidecar plus `atlas.json` is bake metadata. A glob over `assets/*.bin` would ship files the game never opens.

**Sizes are load-bearing.** `load_bin()` at `app/main.c:122` compares the file size against an expected value and refuses to load on mismatch, which kills the game at startup with "assets missing". `clut.bin` must be exactly 512 bytes.

**`sdk/native/main.elf` is a trap.** The vendored SDK ships its own example binary at that path. Any code that searches for `main.elf` by name rather than resolving the repo-root path will eventually pick up the wrong one.

**The existing push filter is broken.** `push_app()` at `tools/rally_hw.py:101` zips only files named `main.elf`, `app.json`, `handling.toml`, `stage01.bin`. It captures no assets. Deploys work today only because assets were pushed at some earlier point and persist on the SD card. Task 4 fixes this by making it reuse the same definition as the release.

---

## File Structure

| File | Status | Responsibility |
| --- | --- | --- |
| `tools/bundle.py` | Create | Sole definition of bundle contents, verification, deterministic zip |
| `tools/test_bundle.py` | Create | Hermetic TAP tests for `bundle.py` |
| `tools/rally_hw.py` | Modify (line 96 onward) | Reuse `bundle.build_zip_bytes` instead of its own filter |
| `.github/actions/arm-toolchain/action.yml` | Create | Sole definition of the pinned toolchain, used by both workflows |
| `.github/workflows/ci.yml` | Create | Host tests plus ARM cross-build on push and PR |
| `.github/workflows/release.yml` | Create | Tag-triggered package, attest, publish prerelease |
| `.gitignore` | Modify | Add `dist/` and `plat/headless/scene` |
| `app.json` | Modify (line 6) | Version bump `0.1.0` to `0.5.0` |

---

## Task 1: Bundle file list

Build the part of `bundle.py` that decides which files belong in a bundle, and stand up the TAP test harness. No verification and no zipping yet.

**Files:**
- Create: `tools/bundle.py`
- Create: `tools/test_bundle.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `bundle.REPO_ROOT: str` — absolute path to the repo root
  - `bundle.BundleError(Exception)`
  - `bundle.tile_section_names(root: str) -> list[str]`
  - `bundle.bundle_files(root: str) -> list[tuple[str, str]]` — `(absolute_source_path, arcname)`, sorted by arcname

- [ ] **Step 1: Write the failing test**

Create `tools/test_bundle.py`:

```python
#!/usr/bin/env python3
"""TAP tests for tools/bundle.py.

Hermetic: every case builds a throwaway app dir under tempfile, so the real
repo is never mutated. That matters because several cases need a corrupted
asset. It also means these tests need no ARM build, so they can run in the
host-only CI job where main.elf does not exist.
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
    # collect() only checks that main.elf exists; it never parses it.
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tools/test_bundle.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'bundle'`

- [ ] **Step 3: Write minimal implementation**

Create `tools/bundle.py`:

```python
#!/usr/bin/env python3
"""Defines what a PicOS Rally app bundle contains.

Single source of truth, shared by two consumers:
  - .github/workflows/release.yml, which packages the published zip
  - tools/rally_hw.py, which pushes the same set to hardware

The bundle mirrors the on-device app dir that app/main.c expects: assets/
for art (app/main.c:116), tuning/handling.toml (app/main.c:180), and
stage01.bin at the root (app/main.c:196).
"""
import os
import re

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Root-relative bundle members that are not assets.
BASE_FILES = ("main.elf", "app.json", "stage01.bin", "tuning/handling.toml")

# Non-tile assets loaded by load_assets() (app/main.c:145 and :156).
SPRITE_ASSETS = ("clut.bin", "car.bin", "props.bin", "hero.bin")


class BundleError(Exception):
    """Raised when the source tree cannot produce a valid bundle."""


def tile_section_names(root):
    """Read TILE_SEC_NAMES out of core/tiles_sections.h.

    That header is generated by trackbake.py from assets/atlas.json, so
    parsing it means a future stage with a different terrain chain updates
    the bundle automatically rather than silently shipping the wrong tiles.
    """
    path = os.path.join(root, "core", "tiles_sections.h")
    try:
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise BundleError(f"cannot read {path}: {e}")
    m = re.search(r"TILE_SEC_NAMES\[[^\]]*\]\s*=\s*\{(.*?)\}", src, re.S)
    if not m:
        raise BundleError(f"TILE_SEC_NAMES not found in {path}")
    names = re.findall(r'"([^"]+)"', m.group(1))
    if not names:
        raise BundleError(f"TILE_SEC_NAMES is empty in {path}")
    return names


def bundle_files(root):
    """Return [(absolute_source_path, arcname)], sorted by arcname.

    Paths are resolved from `root` explicitly. Nothing is globbed and
    nothing is discovered by walking, so sdk/native/main.elf can never be
    picked up in place of the real one.
    """
    rels = list(BASE_FILES)
    rels += [f"assets/{n}" for n in SPRITE_ASSETS]
    rels += [f"assets/tiles_{n}.bin" for n in tile_section_names(root)]
    return sorted(((os.path.join(root, r), r) for r in rels),
                  key=lambda pair: pair[1])
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tools/test_bundle.py`
Expected: PASS, 4 checks, `# 4/4 checks passed`, exit 0

- [ ] **Step 5: Commit**

```bash
git add tools/bundle.py tools/test_bundle.py
git commit -m "feat(tools): bundle.py file list, parsed from tiles_sections.h

Single definition of what an app bundle contains. Resolves paths
explicitly rather than globbing, so sdk/native/main.elf cannot be
mistaken for the real binary."
```

---

## Task 2: Bundle verification

Add the fail-closed checks. Nothing gets packaged if an asset is corrupt, `clut.bin` is the wrong size, a file is missing, or `app.json` disagrees with the tag.

**Files:**
- Modify: `tools/bundle.py`
- Modify: `tools/test_bundle.py`

**Interfaces:**
- Consumes: `bundle.bundle_files`, `bundle.BundleError` from Task 1.
- Produces:
  - `bundle.CLUT_BYTES: int` (512)
  - `bundle.verify(root: str, files: list[tuple[str, str]]) -> None` — raises `BundleError`
  - `bundle.check_version(root: str, expected: str) -> None` — raises `BundleError`
  - `bundle.collect(root: str = REPO_ROOT) -> list[tuple[str, str]]` — `bundle_files` plus `verify`

- [ ] **Step 1: Write the failing test**

In `tools/test_bundle.py`, replace the whole `def main():` body's first block and add the tampering blocks. The function becomes:

```python
def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = make_fixture(tmp)

        files = bundle.collect(root)
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

        with open(os.path.join(root, "app.json"), encoding="utf-8") as f:
            version = json.load(f)["version"]
        try:
            bundle.check_version(root, version)
            matched = True
        except bundle.BundleError:
            matched = False
        check("matching version passes the gate", matched)
        raises("mismatched version fails the gate",
               lambda: bundle.check_version(root, "9.9.9"))

    # Each tampering case needs its own clean fixture.
    with tempfile.TemporaryDirectory() as tmp:
        root = make_fixture(tmp)
        p = os.path.join(root, "assets", "car.bin")
        with open(p, "rb") as f:
            data = bytearray(f.read())
        data[0] ^= 0xFF
        with open(p, "wb") as f:
            f.write(bytes(data))
        raises("tampered asset fails the manifest sha256 check",
               lambda: bundle.collect(root))

    with tempfile.TemporaryDirectory() as tmp:
        root = make_fixture(tmp)
        with open(os.path.join(root, "assets", "clut.bin"), "ab") as f:
            f.write(b"\x00")
        raises("clut.bin of the wrong size is rejected",
               lambda: bundle.collect(root))

    with tempfile.TemporaryDirectory() as tmp:
        root = make_fixture(tmp)
        os.remove(os.path.join(root, "tuning", "handling.toml"))
        raises("a missing bundle file is a hard failure",
               lambda: bundle.collect(root))

    print(f"1..{_checks}")
    print(f"# {_checks - _failures}/{_checks} checks passed")
    return 1 if _failures else 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tools/test_bundle.py`
Expected: FAIL with `AttributeError: module 'bundle' has no attribute 'collect'`

- [ ] **Step 3: Write minimal implementation**

In `tools/bundle.py`, add `hashlib`, `json` and `sys` to the imports so the block reads:

```python
import hashlib
import json
import os
import re
import sys
```

Add this constant below `REPO_ROOT`:

```python
# app/main.c:145 loads clut.bin with an exact-size check. load_bin() at
# app/main.c:122 refuses a size mismatch, which kills the game at startup.
CLUT_BYTES = 512
```

Append to the end of the file:

```python
def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _manifest_hashes(root):
    """Map asset filename to its expected sha256, from assets/manifest.json."""
    path = os.path.join(root, "assets", "manifest.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except OSError as e:
        raise BundleError(f"cannot read {path}: {e}")
    except json.JSONDecodeError as e:
        raise BundleError(f"{path} is not valid JSON: {e}")
    out = {}
    for entry in manifest.values():
        if isinstance(entry, dict) and "bin" in entry and "bin_sha256" in entry:
            out[entry["bin"]] = entry["bin_sha256"]
    return out


def verify(root, files):
    """Fail closed on anything that would break the app at load time."""
    for src, arc in files:
        if not os.path.isfile(src):
            raise BundleError(f"missing bundle file: {arc}")

    hashes = _manifest_hashes(root)
    for src, arc in files:
        if not arc.startswith("assets/"):
            continue
        name = os.path.basename(arc)
        # clut.bin has no manifest entry (palettize.py does not record one),
        # so its exact length is the only check available.
        if name == "clut.bin":
            size = os.path.getsize(src)
            if size != CLUT_BYTES:
                raise BundleError(
                    f"clut.bin is {size} bytes, expected {CLUT_BYTES}; "
                    "app/main.c:145 rejects a size mismatch")
            continue
        expected = hashes.get(name)
        if expected is None:
            raise BundleError(
                f"{arc} has no bin_sha256 entry in assets/manifest.json")
        actual = _sha256(src)
        if actual != expected:
            raise BundleError(
                f"{arc} sha256 {actual} does not match manifest {expected}")


def check_version(root, expected):
    """Assert app.json's version equals `expected` (the tag minus its v)."""
    path = os.path.join(root, "app.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            app = json.load(f)
    except OSError as e:
        raise BundleError(f"cannot read {path}: {e}")
    except json.JSONDecodeError as e:
        raise BundleError(f"{path} is not valid JSON: {e}")
    actual = app.get("version")
    if actual != expected:
        raise BundleError(
            f"app.json version is {actual!r} but the tag expects {expected!r}; "
            "bump app.json and commit before tagging")


def collect(root=REPO_ROOT):
    """Verified bundle contents. Raises BundleError if the tree is unfit."""
    files = bundle_files(root)
    verify(root, files)
    return files
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tools/test_bundle.py`
Expected: PASS, 9 checks, `# 9/9 checks passed`, exit 0

- [ ] **Step 5: Verify it rejects a real tampered tree**

Run:
```bash
cp assets/car.bin /tmp/car.bin.orig
printf '\xff' | dd of=assets/car.bin bs=1 seek=0 count=1 conv=notrunc status=none
python3 -c "import sys; sys.path.insert(0,'tools'); import bundle; bundle.collect()" ; echo "exit=$?"
cp /tmp/car.bin.orig assets/car.bin
git diff --stat
```
Expected: a `BundleError` traceback mentioning `assets/car.bin sha256`, then `git diff --stat` reports no changes after the restore.

- [ ] **Step 6: Commit**

```bash
git add tools/bundle.py tools/test_bundle.py
git commit -m "feat(tools): fail-closed bundle verification

Checks every asset against its manifest bin_sha256, clut.bin against its
required 512 bytes, and app.json's version against the tag. A corrupt or
mislabelled tree cannot be packaged."
```

---

## Task 3: Deterministic zip and CLI

Make `bundle.py` runnable, producing a byte-identical archive for identical inputs so a published asset can be compared against a local build.

**Files:**
- Modify: `tools/bundle.py`
- Modify: `tools/test_bundle.py`

**Interfaces:**
- Consumes: `bundle.collect`, `bundle.check_version` from Task 2.
- Produces:
  - `bundle.ZIP_TIMESTAMP: tuple` — `(1980, 1, 1, 0, 0, 0)`
  - `bundle.build_zip_bytes(root: str = REPO_ROOT) -> bytes`
  - `bundle.main(argv: list[str] | None = None) -> int`
  - CLI: `python3 tools/bundle.py --out PATH [--expect-version X] [--app-dir DIR]`

- [ ] **Step 1: Write the failing test**

In `tools/test_bundle.py`, insert this block immediately after the `raises("mismatched version fails the gate", ...)` line, still inside the first `with tempfile.TemporaryDirectory()` block:

```python
        first = bundle.build_zip_bytes(root)
        second = bundle.build_zip_bytes(root)
        check("two builds are byte-identical", first == second)
        check("zip is non-empty", len(first) > 0)

        import zipfile as _zf
        import io as _io
        with _zf.ZipFile(_io.BytesIO(first)) as z:
            check("zip entries match the expected set",
                  sorted(z.namelist()) == EXPECTED)
            check("zip timestamps are fixed",
                  all(i.date_time == bundle.ZIP_TIMESTAMP
                      for i in z.infolist()))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tools/test_bundle.py`
Expected: FAIL with `AttributeError: module 'bundle' has no attribute 'build_zip_bytes'`

- [ ] **Step 3: Write minimal implementation**

In `tools/bundle.py`, extend the imports to:

```python
import argparse
import hashlib
import io
import json
import os
import re
import sys
import zipfile
```

Add below `CLUT_BYTES`:

```python
# Fixed DOS timestamp (the zip epoch) so identical inputs give an identical
# archive. Without this, mtimes leak in and every build differs.
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
```

Append to the end of the file:

```python
def build_zip_bytes(root=REPO_ROOT):
    """Deterministic zip: verified contents, sorted, fixed timestamps."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in collect(root):
            info = zipfile.ZipInfo(arc, date_time=ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(src, "rb") as f:
                z.writestr(info, f.read())
    return buf.getvalue()


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Package a PicOS Rally app bundle.")
    p.add_argument("--out", required=True, help="output zip path")
    p.add_argument("--expect-version",
                   help="fail unless app.json's version equals this")
    p.add_argument("--app-dir", default=REPO_ROOT,
                   help="source tree to bundle (default: repo root)")
    args = p.parse_args(argv)

    try:
        if args.expect_version:
            check_version(args.app_dir, args.expect_version)
        files = collect(args.app_dir)
        data = build_zip_bytes(args.app_dir)
    except BundleError as e:
        print(f"bundle: {e}", file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"bundle: {args.out} ({len(data)} bytes, {len(files)} files)")
    for _, arc in files:
        print(f"  {arc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tools/test_bundle.py`
Expected: PASS, 15 checks, `# 15/15 checks passed`, exit 0

> Note: this suite grew from 13 to 15 checks during execution. Review found
> the sorted-order and file-mode determinism pillars were unasserted, so
> checks for both were added. The same round fixed `ZipInfo.create_system`
> leaking the host OS into the archive, and removed a duplicate `collect()`
> call.

- [ ] **Step 5: Exercise the CLI end to end**

The real tree has no `main.elf` unless you have built it. Build first, then bundle:

```bash
make
python3 tools/bundle.py --out dist/test.zip --expect-version 0.1.0
unzip -l dist/test.zip
```
Expected: prints 12 files, and `unzip -l` lists exactly the 12 arcnames from Global Constraints.

Then confirm the version gate bites:
```bash
python3 tools/bundle.py --out dist/test.zip --expect-version 0.5.0 ; echo "exit=$?"
```
Expected: `bundle: app.json version is '0.1.0' but the tag expects '0.5.0'; bump app.json and commit before tagging` on stderr, `exit=1`.

Clean up: `rm -rf dist`

- [ ] **Step 6: Commit**

```bash
git add tools/bundle.py tools/test_bundle.py
git commit -m "feat(tools): deterministic bundle zip and CLI

Fixed timestamps and sorted entries mean identical inputs produce a
byte-identical archive, so a published release asset can be compared
against a local build."
```

---

## Task 4: Reuse the bundle for hardware deploys

Point `rally_hw.py` at the shared definition, fixing its filter that ships no assets at all. Add the new build outputs to `.gitignore`.

**Files:**
- Modify: `tools/rally_hw.py:6` (imports), `tools/rally_hw.py:96-101` (`push_app`)
- Modify: `.gitignore`

**Interfaces:**
- Consumes: `bundle.build_zip_bytes` from Task 3.
- Produces: nothing new.

- [ ] **Step 1: Replace the import block**

In `tools/rally_hw.py`, change line 6 from:

```python
import base64, io, os, sys, time, zipfile
```

to:

```python
import base64, io, os, sys, time, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle  # bundle contents are defined once, in tools/bundle.py
```

- [ ] **Step 2: Replace `push_app`**

Replace the whole method at `tools/rally_hw.py:96-101`, which currently reads:

```python
    def push_app(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            for root, dirs, files in os.walk(APP_DIR):
                dirs[:] = [d for d in dirs if d not in ("src", ".git")]
                for f in files:
                    if f in ("main.elf", "app.json", "handling.toml", "stage01.bin"):
                        p = os.path.join(root, f)
                        z.write(p, os.path.relpath(p, APP_DIR))
        data = buf.getvalue()
```

with:

```python
    def push_app(self):
        # Same bundle the release workflow publishes, so what is tested on
        # device is what players download. The previous hand-rolled filter
        # shipped no assets at all.
        try:
            data = bundle.build_zip_bytes(APP_DIR)
        except bundle.BundleError as e:
            raise RuntimeError(f"cannot build app bundle: {e}")
```

Leave the rest of the method (the `print`, `put_file`, `unzip`, `rm` lines) exactly as it is.

- [ ] **Step 3: Verify the module still loads and packages correctly**

`rally_hw.py` imports `serial` at module scope, so check it compiles and that the shared bundle path works without needing hardware:

```bash
python3 -m py_compile tools/rally_hw.py && echo "compiles OK"
make
python3 -c "
import sys; sys.path.insert(0, 'tools')
import bundle, zipfile, io
data = bundle.build_zip_bytes(bundle.REPO_ROOT)
names = sorted(zipfile.ZipFile(io.BytesIO(data)).namelist())
print(len(names), 'files')
assert any(n.startswith('assets/') for n in names), 'assets missing'
print('\n'.join(names))
"
```
Expected: `compiles OK`, then `12 files` and the 12 arcnames, including the `assets/` entries the old filter dropped.

- [ ] **Step 4: Update `.gitignore`**

Replace the contents of `.gitignore` with:

```
main.elf
rally_headless
dist/
plat/headless/scene
```

- [ ] **Step 5: Confirm the ignores take effect**

Run: `git status --short`
Expected: no untracked `dist/`, `main.elf`, `plat/headless/scene` or `plat/headless/rally_headless` entries.

- [ ] **Step 6: Commit**

```bash
git add tools/rally_hw.py .gitignore
git commit -m "fix(tools): rally_hw pushes the real bundle, not a stale filter

push_app zipped only main.elf, app.json, handling.toml and stage01.bin,
shipping no assets. It now calls bundle.build_zip_bytes, so deploys and
releases package identically. Also ignore dist/ and plat/headless/scene."
```

---

## Task 5: CI workflow

Gate every push and pull request on the headless suite, the bundle tests, and an ARM cross-build.

**Files:**
- Create: `.github/actions/arm-toolchain/action.yml`
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `tools/test_bundle.py` from Task 3.
- Produces: `./.github/actions/arm-toolchain`, a local composite action taking no inputs, which installs the pinned toolchain and puts it on `PATH`. Task 6 uses it unchanged.

- [ ] **Step 1: Write the shared toolchain action**

The toolchain version and checksum must exist in exactly one place, otherwise CI and release can drift onto different compilers. GitHub has no cross-workflow `env` sharing, so a local composite action is the mechanism.

Create `.github/actions/arm-toolchain/action.yml`:

```yaml
name: Install ARM toolchain
description: >
  Install a SHA256-pinned ARM GNU Toolchain and add it to PATH. This file is
  the single source of truth for the toolchain version; both ci.yml and
  release.yml consume it so they cannot drift onto different compilers.

runs:
  using: composite
  steps:
    - name: Resolve pinned version
      shell: bash
      run: |
        set -euo pipefail
        {
          echo "ARM_TOOLCHAIN_VERSION=14.3.rel1"
          echo "ARM_TOOLCHAIN_SHA256=8f6903f8ceb084d9227b9ef991490413014d991874a1e34074443c2a72b14dbd"
        } >> "$GITHUB_ENV"

    - name: Cache ARM toolchain
      id: toolchain-cache
      uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
      with:
        path: ~/toolchain
        key: arm-gnu-toolchain-${{ env.ARM_TOOLCHAIN_SHA256 }}

    - name: Download and verify
      if: steps.toolchain-cache.outputs.cache-hit != 'true'
      shell: bash
      run: |
        set -euo pipefail
        name="arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi"
        url="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/${name}.tar.xz"
        curl -fsSL "$url" -o /tmp/arm-toolchain.tar.xz
        # Fails closed on any mismatch, before anything is extracted.
        echo "${ARM_TOOLCHAIN_SHA256}  /tmp/arm-toolchain.tar.xz" | sha256sum -c -
        mkdir -p "$HOME/toolchain"
        tar -xf /tmp/arm-toolchain.tar.xz -C "$HOME/toolchain" --strip-components=1
        rm /tmp/arm-toolchain.tar.xz

    - name: Add to PATH
      shell: bash
      run: echo "$HOME/toolchain/bin" >> "$GITHUB_PATH"

    - name: Show version
      shell: bash
      run: arm-none-eabi-gcc --version
```

- [ ] **Step 2: Write the workflow**

Create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

# Least privilege. Nothing here needs to write.
permissions:
  contents: read

concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

jobs:
  test:
    name: Host tests
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - name: Build and run the headless suite
        # Must run from plat/headless. The runner opens ../../stage01.bin
        # (plat/headless/main.c:187) and ../../assets/clut.bin (:355) relative
        # to the working directory. From the repo root the stage load fails
        # and 17 of the 51 checks are silently skipped, reporting 34 total.
        # README.md:27 documents this convention.
        working-directory: plat/headless
        run: |
          make
          ./rally_headless

      - name: Run bundle tests
        run: python3 tools/test_bundle.py

  build:
    name: ARM cross-build
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - name: Install ARM toolchain
        uses: ./.github/actions/arm-toolchain

      - name: Build main.elf
        run: make

      - name: Report binary size
        run: arm-none-eabi-size main.elf
```

- [ ] **Step 3: Validate both YAML files parse**

Run:
```bash
python3 -c "
import yaml
for p in ('.github/workflows/ci.yml', '.github/actions/arm-toolchain/action.yml'):
    yaml.safe_load(open(p)); print('valid YAML:', p)
"
```
Expected: both listed as valid. If PyYAML is unavailable, run `pip install --user pyyaml` first, or skip and let GitHub validate on push.

- [ ] **Step 4: Rehearse the CI steps locally**

Run the exact commands the `test` job runs:
```bash
( cd plat/headless && make -s && ./rally_headless ) ; echo "headless exit=$?"
python3 tools/test_bundle.py ; echo "bundle exit=$?"
```
Expected: `# 51/51 checks passed` and `headless exit=0`, then `# 15/15 checks passed` and `bundle exit=0`.

- [ ] **Step 5: Commit and push on a branch**

```bash
git checkout -b ci/github-actions
git add .github/workflows/ci.yml .github/actions/arm-toolchain/action.yml
git commit -m "ci: build and test on push and pull request

Host job runs the headless suite and bundle tests. ARM job cross-builds
main.elf with a SHA256-pinned ARM GNU Toolchain 14.3.rel1, installed by a
local composite action so ci and release cannot drift onto different
compilers. All actions pinned to commit SHAs, read-only token."
git push -u origin ci/github-actions
```

- [ ] **Step 6: Open a PR and confirm both jobs go green**

```bash
gh pr create --fill --title "ci: GitHub Actions build and test"
gh pr checks --watch
```
Expected: `Host tests` and `ARM cross-build` both pass. If the ARM job fails on the checksum, re-verify the pin:
```bash
curl -sSL "https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz.sha256asc"
```
and update `ARM_TOOLCHAIN_SHA256` in `.github/actions/arm-toolchain/action.yml` (the only place it appears) if ARM has republished the artifact.

- [ ] **Step 7: Merge**

```bash
gh pr merge --squash --delete-branch
git checkout main && git pull
```

---

## Task 6: Release workflow and version bump

Publish an attested prerelease when a `v*` tag is pushed, and bump `app.json` so the version gate will accept `v0.5.0`.

**Files:**
- Create: `.github/workflows/release.yml`
- Modify: `app.json:6`

**Interfaces:**
- Consumes: `bundle.py` CLI from Task 3, and `./.github/actions/arm-toolchain` from Task 5.
- Produces: a published GitHub prerelease with `PicOS-Rally-v<version>.zip` and `SHA256SUMS`.

- [ ] **Step 1: Bump the app version**

In `app.json`, change line 6 from:

```json
    "version": "0.1.0",
```

to:

```json
    "version": "0.5.0",
```

- [ ] **Step 2: Confirm the gate now accepts 0.5.0**

```bash
make
python3 tools/bundle.py --out dist/check.zip --expect-version 0.5.0 ; echo "exit=$?"
python3 tools/bundle.py --out dist/check.zip --expect-version 0.1.0 ; echo "exit=$?"
rm -rf dist
```
Expected: first `exit=0`, second `exit=1` with the mismatch message.

- [ ] **Step 3: Write the release workflow**

Create `.github/workflows/release.yml`:

```yaml
name: Release

on:
  push:
    tags: ['v*']

permissions:
  contents: read

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: false

jobs:
  release:
    name: Build and publish prerelease
    runs-on: ubuntu-24.04
    permissions:
      contents: write       # create the release
      id-token: write       # sign the provenance attestation
      attestations: write   # record the provenance attestation
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - name: Install ARM toolchain
        uses: ./.github/actions/arm-toolchain

      - name: Run the headless suite
        # Must run from plat/headless, same reason as ci.yml: the runner opens
        # ../../stage01.bin and ../../assets/clut.bin relative to cwd.
        working-directory: plat/headless
        run: |
          make
          ./rally_headless

      - name: Run bundle tests
        run: python3 tools/test_bundle.py

      - name: Build main.elf
        run: |
          make
          arm-none-eabi-size main.elf

      - name: Derive version from tag
        id: ver
        run: |
          set -euo pipefail
          tag="${GITHUB_REF_NAME}"
          echo "tag=${tag}" >> "$GITHUB_OUTPUT"
          echo "version=${tag#v}" >> "$GITHUB_OUTPUT"

      - name: Package bundle
        run: |
          set -euo pipefail
          python3 tools/bundle.py \
            --out "dist/PicOS-Rally-${{ steps.ver.outputs.tag }}.zip" \
            --expect-version "${{ steps.ver.outputs.version }}"

      - name: Write SHA256SUMS
        run: |
          set -euo pipefail
          cd dist
          sha256sum ./*.zip > SHA256SUMS
          cat SHA256SUMS

      - name: Attest build provenance
        uses: actions/attest-build-provenance@0f67c3f4856b2e3261c31976d6725780e5e4c373 # v4.1.1
        with:
          subject-path: dist/PicOS-Rally-${{ steps.ver.outputs.tag }}.zip

      - name: Write release notes
        run: |
          set -euo pipefail
          cat > dist/NOTES.md <<'EOF'
          Prerelease build of PicOS Rally for the ClockworkPi PicoCalc v2.0,
          running on PicOS. One stage: Cooloola Point, roughly 2.7 km of
          Queensland coastal gravel.

          ## Install

          Unzip onto the SD card so the contents land in the app directory
          (`/apps/rally`), keeping the `assets/` and `tuning/` folders intact.

          ## Verify the download

          ```
          sha256sum -c SHA256SUMS
          gh attestation verify PicOS-Rally-<tag>.zip --repo jeffory/PicOS-Rally
          ```

          ## What is not yet verified

          This is a prerelease. The audio mix has not passed a human ear test
          (engine pitch against speed, gravel roar level, event balance), and
          the M4 visual checks on gravel hue and sand contrast are still
          outstanding. See ROADMAP.md.
          EOF
          cat dist/NOTES.md

      - name: Create prerelease
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -euo pipefail
          gh release create "${{ steps.ver.outputs.tag }}" \
            --prerelease \
            --title "PicOS Rally ${{ steps.ver.outputs.tag }}" \
            --notes-file dist/NOTES.md \
            "dist/PicOS-Rally-${{ steps.ver.outputs.tag }}.zip" \
            dist/SHA256SUMS
```

- [ ] **Step 4: Validate the YAML parses**

Run: `python3 -c "import sys,yaml; yaml.safe_load(open('.github/workflows/release.yml')); print('valid YAML')"`
Expected: `valid YAML`

- [ ] **Step 5: Rehearse the packaging steps locally**

Run exactly what the workflow runs, with the tag values substituted by hand:
```bash
make
python3 tools/bundle.py --out dist/PicOS-Rally-v0.5.0.zip --expect-version 0.5.0
cd dist && sha256sum ./*.zip > SHA256SUMS && cat SHA256SUMS && cd ..
unzip -l dist/PicOS-Rally-v0.5.0.zip
```
Expected: bundle prints 12 files, `SHA256SUMS` has one line, `unzip -l` lists the 12 arcnames. Leave `dist/` in place for Task 7.

- [ ] **Step 6: Commit and merge via PR**

```bash
git checkout -b ci/release-workflow
git add .github/workflows/release.yml app.json
git commit -m "ci: tag-triggered prerelease with signed provenance

Pushing v* builds, verifies app.json against the tag, packages the app
bundle, writes SHA256SUMS, attests build provenance, and publishes a
GitHub prerelease. Bumps app.json to 0.5.0 for the first release."
git push -u origin ci/release-workflow
gh pr create --fill --title "ci: release workflow and 0.5.0 version bump"
gh pr checks --watch
gh pr merge --squash --delete-branch
git checkout main && git pull
```
Expected: both CI jobs green, then merged.

---

## Task 7: Verify on hardware, then cut v0.5.0

The release pipeline is untested until a tag exists, and the bundle contents are unproven until the game runs from them. This task needs the PicoCalc connected.

**Files:** none modified. This is a verification and release procedure.

**Interfaces:**
- Consumes: everything from Tasks 1 to 6.
- Produces: the published `v0.5.0` prerelease.

- [ ] **Step 1: Clear the device app directory, then deploy the locally built bundle**

`rally_hw.py` pushes with `unzip /data/tmp/push_app.zip /apps/rally` and never
clears that directory first. The Step 2 check below only proves the
twelve-file list is complete if the deploy lands on an empty target: if a
file were missing from the bundle but already present in `/apps/rally` from
an earlier push, the game would still load, Step 2 would still pass, and the
release would ship an incomplete bundle that fails for every fresh
installer. Wipe `/apps/rally` on the device before this push. **The Step 2
gate is void without this wipe** — it is not a tidiness step, it is what
makes the check mean anything.

If the PicOS serial shell has no recursive remove, clear `/apps/rally` from
an SD card reader instead (pull the card, delete the directory on the host,
reinsert it), then proceed.

Push the zip built in Task 6 Step 5, verbatim, with the new `--zip` mode, so
what reaches the device is provably the exact bytes that will be published
rather than a re-derivation from source:

```bash
python3 tools/rally_hw.py push --zip dist/PicOS-Rally-v0.5.0.zip
```
Expected: the push reports the zip's exact size and 12 files.

- [ ] **Step 2: Confirm the game runs from bundle contents alone**

```bash
python3 tools/rally_hw.py launch
python3 tools/rally_hw.py log
```
Expected in the log: `RALLY: stage loaded:` with point, note and checkpoint counts, and `RALLY: assets loaded (clut, 4 tile secs, car, props, hero)`. Neither `stage01.bin missing or bad` nor `assets missing (assets/*.bin)` should appear.

Then drive it. Take a screenshot to confirm the render is right:
```bash
python3 tools/rally_hw.py shot /tmp/rally-bundle-check.png
```
Expected: a recognisable Cooloola Point frame, not a red error screen.

**Gate:** do not proceed past this step if the game does not load. A failure here means the 12-file list is incomplete, which is exactly what this check exists to catch. Add the missing file to `BASE_FILES` or `SPRITE_ASSETS` in `tools/bundle.py`, update `EXPECTED` in `tools/test_bundle.py`, and repeat from Task 3 Step 4.

- [ ] **Step 3: Tag and push**

```bash
git checkout main && git pull
git tag v0.5.0
git push origin v0.5.0
gh run watch
```
Expected: the `Release` workflow completes green.

- [ ] **Step 4: Verify the published artifact**

```bash
rm -rf /tmp/rally-release && mkdir -p /tmp/rally-release && cd /tmp/rally-release
gh release download v0.5.0 --repo jeffory/PicOS-Rally
sha256sum -c SHA256SUMS
gh attestation verify PicOS-Rally-v0.5.0.zip --repo jeffory/PicOS-Rally
cd -
```
Expected: `PicOS-Rally-v0.5.0.zip: OK` from `sha256sum`, and a successful provenance verification naming the release workflow.

- [ ] **Step 5: Smoke-test the CI-built binary on hardware**

This is the binary compiled by ARM 14.3.rel1 on the runner, not your local Fedora build, and it has never been executed:
```bash
python3 tools/rally_hw.py push --zip /tmp/rally-release/PicOS-Rally-v0.5.0.zip
python3 tools/rally_hw.py launch
python3 tools/rally_hw.py log
```
Expected: the same two load lines as Step 2, and a drivable stage.

**Gate:** if the CI binary misbehaves where the local one did not, the toolchain difference is the suspect. Do not announce. Investigate before promoting.

- [ ] **Step 6: Record the outcome**

Append a short entry to `NOTES.md` covering: the toolchain version the release was built with, the `arm-none-eabi-size` figures from the release run, and the result of the hardware smoke test. Then:

```bash
git add NOTES.md
git commit -m "docs: record v0.5.0 prerelease build and hardware smoke test"
git push
```

- [ ] **Step 7: Clean up**

```bash
rm -rf dist /tmp/rally-release
git status --short
```
Expected: a clean tree.

---

## Done When

- `ci.yml` runs on every push and PR, with the headless suite (51 checks), bundle tests (15 checks), and an ARM cross-build all passing.
- `tools/bundle.py` is the only place bundle contents are defined, and `rally_hw.py` uses it.
- A corrupt asset, a wrong-sized `clut.bin`, a missing file, or a version mismatch all fail the build closed.
- `v0.5.0` is published as a GitHub prerelease with the bundle zip, `SHA256SUMS`, and a signed provenance attestation.
- The published zip has been downloaded, checksum-verified, attestation-verified, and run on real hardware.
