#!/usr/bin/env python3
"""Package firmware images for release (run after `pixi run build`).

For each device env this writes to dist/<version>/:
  meshmash-<version>-<env>.bin         application only: OTA, and
                                              Launcher's WebUI install
  meshmash-<version>-<env>-full.bin    bootloader + partitions + app,
                                              flashed at 0x0: M5Burner, web
                                              flashers, esptool; and
                                              Launcher from the SD card
  SHA256SUMS

    pixi run package                  # both envs
    python -I scripts/package.py cardputer_adv
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENVS = ["cardputer_adv", "cardputer_adv_usb", "tdeck", "tdeck_usb"]


def version():
    # The same label the device shows: scripts/version.py.
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts", "version.py")], capture_output=True,
                       text=True, check=True)
    return r.stdout.strip()


def metadata(env):
    r = subprocess.run(["pio", "project", "metadata", "-e", env, "--json-output"], cwd=ROOT,
                       capture_output=True, text=True, check=True)
    # Output can start with tool-install chatter: the JSON is the last line.
    return json.loads(r.stdout.strip().splitlines()[-1])[env]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def main():
    envs = sys.argv[1:] or ENVS
    ver = version()
    out = os.path.join(ROOT, "dist", ver)
    os.makedirs(out, exist_ok=True)
    made = []
    for env in envs:
        md = metadata(env)
        app = os.path.join(os.path.dirname(md["prog_path"]), "firmware.bin")
        if not os.path.exists(app):
            sys.exit(f"{env}: {os.path.relpath(app, ROOT)} missing; run `pixi run build` first")
        base = f"meshmash-{ver}-{env}"
        app_out = os.path.join(out, base + ".bin")
        shutil.copyfile(app, app_out)
        full_out = os.path.join(out, base + "-full.bin")
        extra = md["extra"]
        parts = []
        for img in extra["flash_images"]:
            parts += [img["offset"], img["path"]]
        parts += [extra["application_offset"], app]
        subprocess.run(["pio", "pkg", "exec", "-p", "tool-esptoolpy", "--", "esptool.py", "--chip", "esp32s3",
                        "merge_bin", "-o", full_out, "--flash_mode", "keep", "--flash_freq", "keep",
                        "--flash_size", "keep"] + parts,
                       cwd=ROOT, check=True, capture_output=True)
        made += [app_out, full_out]
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        for p in sorted(made):
            f.write(f"{sha256(p)}  {os.path.basename(p)}\n")
    for p in made:
        print(f"{os.path.getsize(p) / 1024:8.1f} KB  {os.path.relpath(p, ROOT)}")


if __name__ == "__main__":
    main()
