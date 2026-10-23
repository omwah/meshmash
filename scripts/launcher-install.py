#!/usr/bin/env python3
#
# Copyright esp-vim authors. Licensed under the Apache License, Version 2.0
# (licenses/Apache-2.0.txt).
#
# Changes for MeshMash: installs this project's packaged app image
# (built and packaged for just the variant, BLE or --usb build) under the name
# meshmash; no SD runtime upload; a reboot that drops the connection
# counts as done.
"""
Install a MeshMash build on a Cardputer or T-Deck running bmorcelli's
Launcher, over Launcher's web interface (WebUI), the way its own page does.

    pixi run launcher-install IP --variant V [--usb] [--replace] [--boot]
    pixi run launcher-install IP --variant V [--usb] --upload [FOLDER]
    python -I scripts/launcher-install.py IP --variant V [BIN] [--name NAME] ...

IP     the device's address, as Launcher's WebUI screen shows it
--variant  the board, required: cardputer or tdeck. Only that board's build
       (BLE, or USB-only with --usb) is compiled and packaged, then installed;
       a device takes one build, so there is no building them all.
BIN    install this image instead of building one (the app .bin, or with
       --upload the merged ...-full.bin); nothing is compiled
--usb  the USB-only build instead (installed as meshmash-usb)
--name the app's name in Launcher's menu (default: meshmash)
--replace  first delete any installed app of that name (or the old name,
       meshcore-adv-ui), so a new build takes
       its place rather than adding a copy. Launcher applies a partition change
       by restarting: start its WebUI again when asked.
--boot restart into the app once it is installed (installing already makes it
       the one Launcher boots).
--upload  instead of installing, copy the merged image (-full.bin) to the SD
       card in the device (to FOLDER, default /), under its own name: install
       it from there with Launcher's SD file browser. Launcher reads the
       partition table in it and creates the spiffs partition the firmware
       saves to; from an app-only image it makes none, and nothing is saved.

The WebUI login is Launcher's default (admin / launcher) unless
$LAUNCHER_USER and $LAUNCHER_PASS say otherwise.

The protocol (Launcher's src/webInterface.cpp and webUi/scripts.js):
  POST /login            username, password -> cookie ESP32SESSION
  GET  /partitions       the table as JSON
  POST /partitions       multipart: action=delete offset=N, then action=apply (restarts)
  POST /OTA              command=0, size, manifest (JSON: the parts to install)
  POST /OTAFILE          the file, multipart field "file1" -> "OK"
  GET  /reboot
  POST /                 folder, file: one file to the SD card (--upload)
Only standard-library Python.
"""

import argparse
import http.cookiejar
import json
import os
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def die(msg):
    print(f"launcher-install: {msg}", file=sys.stderr)
    sys.exit(1)


def say(msg):
    print(f"launcher-install: {msg}")


def image_size(data):
    """Length of the ESP app image at the start of data, as Launcher's page
    measures it (measureEspImageSize): header, segments, checksum, hash."""
    if len(data) < 24 or data[0] != 0xE9 or not 0 < data[1] <= 16:
        die("not an ESP app image (no 0xE9 header): give the app .bin, not a merged one")
    cursor = 24
    for _ in range(data[1]):
        (size,) = struct.unpack_from("<I", data, cursor + 4)
        cursor += 8 + size
        if cursor > len(data):
            die("app image truncated")
    end = (cursor + 15) // 16 * 16 + 1
    if data[23]:
        end += 32                       # SHA-256 appended
    end = (end + 15) // 16 * 16
    return min(end, len(data))


def multipart(fields, files=()):
    """(body, content type) for fields {name: value} and files
    [(field, filename, bytes)]."""
    boundary = uuid.uuid4().hex
    out = []
    for k, v in fields.items():
        out += [f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n".encode(),
                str(v).encode(), b"\r\n"]
    for field, name, data in files:
        out += [f"--{boundary}\r\nContent-Disposition: form-data; name=\"{field}\"; "
                f"filename=\"{name}\"\r\nContent-Type: application/octet-stream\r\n\r\n".encode(),
                data, b"\r\n"]
    out.append(f"--{boundary}--\r\n".encode())
    return b"".join(out), f"multipart/form-data; boundary={boundary}"


class Launcher:
    def __init__(self, ip):
        self.base = f"http://{ip}"
        self.jar = http.cookiejar.CookieJar()
        self.http = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def request(self, method, path, body=None, ctype=None, timeout=30):
        req = urllib.request.Request(self.base + path, data=body, method=method)
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with self.http.open(req, timeout=timeout) as r:
                return r.status, r.read().decode(errors="replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode(errors="replace")
        except (urllib.error.URLError, OSError) as e:
            die(f"{method} {path}: {e}")

    def multipart_form(self, path, fields, timeout=30):
        """POST fields as multipart/form-data, as Launcher's page does (its
        Partition Manager uses FormData; a urlencoded body gets 405)."""
        body, ctype = multipart(fields)
        return self.request("POST", path, body, ctype, timeout)

    def form(self, path, fields, timeout=30):
        body = urllib.parse.urlencode(fields).encode()
        return self.request("POST", path, body, "application/x-www-form-urlencoded", timeout)

    def alive(self):
        try:
            with urllib.request.urlopen(self.base + "/ping", timeout=3) as r:
                return r.status == 200
        except (urllib.error.URLError, OSError):
            return False

    def login(self):
        user = os.environ.get("LAUNCHER_USER", "admin")
        password = os.environ.get("LAUNCHER_PASS", "launcher")
        self.form("/login", {"username": user, "password": password})
        if not any(c.name == "ESP32SESSION" for c in self.jar):
            die("login refused: set $LAUNCHER_USER and $LAUNCHER_PASS to the WebUI's login")

    def apps_named(self, names):
        status, text = self.request("GET", "/partitions")
        if status != 200:
            die(f"GET /partitions: {status} {text}")
        table = json.loads(text)
        return [e for e in table["entries"]
                if e["type"] == 0 and e.get("appName") in names and not e.get("running")]

    def replace(self, name):
        # Also the same build under the project's old name.
        old = self.apps_named({name, name.replace("meshmash", "meshcore-adv-ui")})
        if not old:
            say(f"no installed '{name}' to replace")
            return
        for e in old:
            say(f"deleting '{e['appName']}' ({e['label']} at {e['offset']:#x}, {e['size'] // 1024} KB)")
            status, text = self.multipart_form("/partitions", {"action": "delete", "offset": e["offset"]})
            if status != 200:
                die(f"delete {e['label']}: {status} {text}")
        status, text = self.multipart_form("/partitions", {"action": "apply"})
        if status != 200:
            die(f"apply: {status} {text}")
        say("partition table written; Launcher restarts")
        time.sleep(3)
        say("start the WebUI on the device again (it waits up to 3 minutes)")
        for _ in range(180):
            if self.alive():
                break
            time.sleep(1)
        else:
            die("the WebUI didn't come back; start it and run again without --replace")
        self.jar.clear()
        self.login()

    def reboot(self):
        req = urllib.request.Request(self.base + "/reboot")
        for attempt in (1, 2):
            try:
                with self.http.open(req, timeout=5) as r:
                    if r.status != 401:
                        return
            except urllib.error.HTTPError as e:
                if e.code != 401:
                    return
            except (urllib.error.URLError, OSError):
                return                  # it went down mid-request: restarting
            self.jar.clear()
            self.login()

    def install(self, path, name):
        """Install, logging in again and starting over once if Launcher drops
        the session meanwhile: it keeps one, and a login elsewhere (a browser
        on its WebUI) ends ours."""
        for attempt in (1, 2):
            status, text = self.try_install(path, name)
            if status == 200 and text == "OK":
                say(f"installed as '{name}'")
                return
            if status != 401 or attempt == 2:
                die(f"install: {status} {text}")
            say("Launcher dropped the session (a browser on its WebUI?): logging in again")
            self.jar.clear()
            self.login()

    def try_install(self, path, name):
        data = path.read_bytes()
        size = image_size(data)
        manifest = {
            "sourceName": f"{name}.bin",
            # An empty SPIFFS partition for MeshCore's files (identity,
            # contacts, settings): Launcher's own table has none, and without
            # it nothing is saved. An existing "spiffs" is kept as it is.
            "parts": [{"kind": "app", "label": "app", "subtype": 0, "sourceOffset": 0,
                       "copySize": size, "declaredSize": size},
                      {"kind": "data", "label": "spiffs", "subtype": 0x82, "sourceOffset": 0,
                       "copySize": 0, "declaredSize": SPIFFS_SIZE}],
        }
        body, ctype = multipart({"command": 0, "size": len(data), "manifest": json.dumps(manifest)})
        # Launcher reads and plans the partition table before it answers: give
        # it time (it took over 30 s once, straight after a large SD upload).
        status, text = self.request("POST", "/OTA", body, ctype, timeout=120)
        if status != 200 or text != "OK":
            return status, f"POST /OTA: {text}"
        say(f"sending {path.name}, {len(data)} bytes ...")
        body, ctype = multipart({}, [("file1", f"{name}.bin", data)])
        status, text = self.request("POST", "/OTAFILE", body, ctype, timeout=300)
        return status, text if status == 200 else f"POST /OTAFILE: {text}"

    def upload(self, path, folder, timeout=60):
        """One file to folder on the SD card, tried three times."""
        body, ctype = multipart({"folder": folder}, [("file", path.name, path.read_bytes())])
        for attempt in range(3):
            status, text = self.request("POST", "/", body, ctype, timeout=timeout)
            if status == 200 and text == "OK":
                return
            time.sleep(1 + attempt)
        die(f"{path.name}: {status} {text}")


# The SPIFFS partition asked for: small, the SD card holds the rest. Launcher
# makes an empty one its default size anyway (448 KB with 8 MB of flash or
# more), and so does an SD install of the merged image.
SPIFFS_SIZE = 0x70000

def is_merged(path):
    """A merged image: a partition table at 0x8000."""
    with open(path, "rb") as f:
        f.seek(0x8000)
        return f.read(2) == b"\xaa\x50"


VARIANT_ENVS = {"cardputer": "cardputer_adv", "tdeck": "tdeck"}


def newest_image(env, full=False):
    """The app image (or the merged one) of the newest package in dist/ for env."""
    suffix = "-full" if full else ""
    imgs = sorted((ROOT / "dist").glob(f"*/meshmash-*-{env}{suffix}.bin"), key=lambda p: p.stat().st_mtime)
    if not imgs:
        die(f"no {env} image in dist/: run pixi run package")
    return imgs[-1]


def build_and_package(env):
    """Compile only `env` and write its images to dist/ (what `pixi run build` and
    `package` do, for every env)."""
    for cmd in (["pio", "run", "-e", env], [sys.executable, "-I", str(ROOT / "scripts" / "package.py"), env]):
        say("running " + " ".join(Path(c).name if c == sys.executable else c for c in cmd))
        if subprocess.run(cmd, cwd=ROOT).returncode:
            die(f"{' '.join(cmd[:3])} failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0])
    ap.add_argument("ip")
    ap.add_argument("bin", nargs="?", type=Path)
    ap.add_argument("--variant", choices=VARIANT_ENVS, required=True)
    ap.add_argument("--usb", action="store_true")
    ap.add_argument("--name")
    ap.add_argument("--replace", action="store_true")
    ap.add_argument("--boot", action="store_true")
    ap.add_argument("--upload", nargs="?", const="/", metavar="FOLDER")
    a = ap.parse_args()
    if a.upload is not None and (a.replace or a.boot or a.name):
        die("--upload installs nothing: no --replace, --boot or --name with it")

    env = VARIANT_ENVS[a.variant] + ("_usb" if a.usb else "")
    name = a.name or ("meshmash-usb" if a.usb else "meshmash")
    if not a.bin:
        build_and_package(env)
    path = a.bin or newest_image(env, full=a.upload is not None)
    if not path.is_file():
        die(f"{path}: no such file")
    if a.upload is not None and not is_merged(path):
        die(f"{path.name}: an app-only image; from the SD card Launcher needs the merged -full.bin "
            "(its partition table gives the firmware somewhere to save)")
    if a.upload is None and is_merged(path):
        die(f"{path.name}: that is the merged image for 0x0; give the app image")

    dev = Launcher(a.ip)
    if not dev.alive():
        die(f"no Launcher WebUI at {a.ip} (start it from Launcher's menu)")
    dev.login()
    if a.upload is not None:
        folder = "/" + a.upload.strip("/")
        say(f"sending {path.name}, {path.stat().st_size} bytes ...")
        dev.upload(path, folder, timeout=300)
        say(f"{folder.rstrip('/')}/{path.name} on the SD card: install it from Launcher's SD browser")
        return
    if a.replace:
        dev.replace(name)
    say(f"installing {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    dev.install(path, name)
    if a.boot:
        # The device restarts before answering, so no reply is the usual outcome.
        dev.reboot()
        say("restarting into it")
    else:
        say("it boots next: restart the device, or choose it in Launcher")


if __name__ == "__main__":
    main()
