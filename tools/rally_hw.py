#!/usr/bin/env python3
"""Standalone PicOS hardware driver for the rally M1 spike.
Does the full deploy sequence over /dev/ttyACM0 without the MCP server:
  exit app -> zip+push -> relaunch -> keypresses -> tail logs -> screenshot
Usage: rally_hw.py [push|launch|keys|shot|log] [--zip PATH]
  --zip PATH  push this prebuilt zip verbatim (with push/all), instead of
              re-deriving the bundle from RALLY_APP_DIR via bundle.py"""
import base64, io, os, sys, time, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle  # bundle contents are defined once, in tools/bundle.py

PORT = "/dev/ttyACM0"
APP_DIR = os.environ.get("RALLY_APP_DIR",
                         os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REMOTE = "/apps/rally"

import serial  # pyserial


def detect_port():
    for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"):
        if os.path.exists(p):
            return p
    raise RuntimeError("no ttyACM device found")


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


class Dev:
    def __init__(self, port=None):
        self.ser = serial.Serial(port or detect_port(), 115200, timeout=0.2)
        time.sleep(0.3)

    def cmd(self, text, wait=0.4):
        self.ser.write(text.encode() + b"\n")
        self.ser.flush()
        time.sleep(wait)

    def drain(self, seconds=2.0, prefix=""):
        end = time.time() + seconds
        out = []
        while time.time() < end:
            line = self.ser.readline()
            if line:
                try:
                    out.append(prefix + line.decode(errors="replace").rstrip())
                except Exception:
                    pass
        return out

    def put_file(self, data: bytes, remote: str):
        self.cmd(f"putb64 {remote} {len(data)}")
        # wait for Ready B64
        end = time.time() + 5
        while time.time() < end:
            line = self.ser.readline()
            if b"Ready B64" in line:
                break
            if b"Error" in line or b"Failed" in line:
                raise RuntimeError(f"putb64 rejected: {line}")
        b64 = base64.b64encode(data)
        final = None
        for off in range(0, len(b64), 512):
            self.ser.write(b64[off:off + 512] + b"\n")
            self.ser.flush()
            end = time.time() + 15
            while time.time() < end:
                line = self.ser.readline()
                if b"ACK " in line or b"File received" in line:
                    if b"File received" in line:
                        final = line
                    break
        if final is None:
            end = time.time() + 15
            while time.time() < end:
                line = self.ser.readline()
                if b"File received" in line:
                    final = line
                    break
        if final is None:
            raise RuntimeError("no File received marker")
        dev_fnv = int(final.split(b"fnv1a=")[1].split()[0], 16)
        if dev_fnv != fnv1a(data):
            raise RuntimeError("fnv1a mismatch")
        print(f"  uploaded {len(data)}B -> {remote} (fnv ok)")

    def app_running(self):
        self.cmd("status", wait=0.5)
        for line in self.drain(1.5):
            if "Status:" in line:
                return "app=launcher" not in line
        return False

    def push_app(self, zip_path=None):
        # Same bundle the release workflow publishes, so what is tested on
        # device is what players download. The previous hand-rolled filter
        # shipped no assets at all.
        #
        # zip_path pushes an existing archive verbatim, e.g. a zip already
        # published by the release workflow, skipping re-derivation entirely
        # so what gets pushed is provably byte-for-byte what was built.
        if zip_path is not None:
            with open(zip_path, "rb") as f:
                data = f.read()
        else:
            try:
                data = bundle.build_zip_bytes(APP_DIR)
            except bundle.BundleError as e:
                raise RuntimeError(f"cannot build app bundle: {e}")
        print(f"  zip: {len(data)}B, {len(zipfile.ZipFile(io.BytesIO(data)).namelist())} files")
        self.put_file(data, "/data/tmp/push_app.zip")
        self.cmd("unzip /data/tmp/push_app.zip " + REMOTE, wait=1.0)
        for line in self.drain(2.0):
            if "UNZIP" in line or "Unzipped" in line or "Error" in line:
                print(" ", line)
        self.cmd("rm /data/tmp/push_app.zip", wait=0.2)

    def screenshot(self, path):
        self.ser.write(b"screenshot64\n")
        self.ser.flush()
        raw = bytearray()
        w = h = 320
        end = time.time() + 60
        while time.time() < end:
            line = self.ser.readline()
            if not line:
                continue
            if line.startswith(b"~"):
                raw += base64.b64decode(line[1:])
            elif b"SCRN64 " in line:
                for tok in line.split():
                    if tok.startswith(b"w="):
                        w = int(tok[2:])
                    elif tok.startswith(b"h="):
                        h = int(tok[2:])
            elif b"SCRN64_END" in line:
                fnv = int(line.split(b"fnv1a=")[1].split()[0], 16)
                if fnv != fnv1a(bytes(raw)):
                    raise RuntimeError("shot fnv mismatch")
                break
        else:
            raise TimeoutError("screenshot timeout")
        from PIL import Image
        img = Image.frombytes("RGB", (w, h), bytes(w * h * 3))
        px = img.load()
        for i in range(w * h):
            v = (raw[i * 2] << 8) | raw[i * 2 + 1]
            r = (v >> 11) & 31
            g = (v >> 5) & 63
            b = v & 31
            px[i % w, i // w] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
        img.save(path)
        print(f"  saved {path} ({w}x{h})")


VERBS = ("all", "push", "launch", "keys", "put", "shot", "log")


def parse_argv(argv):
    """Split argv into (verb, zip_path, rest).

    Pure and side-effect free so it can be exercised without a device.
    Raises SystemExit with a usable message on any misuse, rather than
    letting a bad invocation fall through to a confusing IndexError.
    """
    rest = list(argv)
    zip_path = None
    if "--zip" in rest:
        i = rest.index("--zip")
        if i + 1 >= len(rest):
            raise SystemExit("rally_hw: --zip needs a path, e.g. --zip dist/app.zip")
        zip_path = rest[i + 1]
        if zip_path.startswith("-"):
            raise SystemExit(f"rally_hw: --zip needs a path, got the flag {zip_path!r}")
        del rest[i:i + 2]

    what = rest[0] if rest else "all"
    if what not in VERBS:
        raise SystemExit(
            f"rally_hw: unknown command {what!r}; expected one of {', '.join(VERBS)}")

    if zip_path is not None:
        if what not in ("all", "push"):
            raise SystemExit(f"rally_hw: --zip applies to push or all, not {what}")
        if not os.path.isfile(zip_path):
            raise SystemExit(f"rally_hw: no such zip: {zip_path}")

    return what, zip_path, rest


def main():
    what, zip_path, argv = parse_argv(sys.argv[1:])
    dev = Dev()
    if what in ("all", "push"):
        if dev.app_running():
            print("app running — exit..."); dev.cmd("exit", wait=1.5)
            if dev.app_running():
                raise RuntimeError("app would not exit")
        else:
            print("at launcher, no exit needed")
        print("push..."); dev.push_app(zip_path=zip_path)
        print("launch..."); dev.cmd("launch rally", wait=1.5)
        print("\n".join(dev.drain(2.0)))
    if what in ("all", "keys"):
        # idle_dim swallows the first key after the screen dims (wake key).
        # Send a neutral char first to wake, wait, then the real key.
        dev.cmd("keypress z", wait=0.4)
        time.sleep(0.6)
        for k in argv[1:] or ["f4"]:
            dev.cmd(f"keypress {k}", wait=0.2)
            print(f"key {k}")
    if what == "launch":
        dev.cmd("launch rally", wait=1.5)
        print("\n".join(dev.drain(2.0)))
    if what == "put":
        local, remote = argv[1], argv[2]
        with open(local, "rb") as fh:
            dev.put_file(fh.read(), remote)
        print(f"put {local} -> {remote}")
    if what == "shot":
        dev.screenshot(argv[1] if len(argv) > 1 else "/tmp/rally_hw.png")
    if what == "log":
        print("\n".join(dev.drain(float(argv[1]) if len(argv) > 1 else 5.0)))


if __name__ == "__main__":
    main()
