"""Write the manifest.json a release carries next to its firmware image.

The board's updater (src/ota.h) fetches it from the latest release, compares "version" with the
firmware it runs, and downloads "url", checking "size" and "sha256" before it switches slots.
The release workflow (.github/workflows/release.yml) runs this; by hand:

    python scripts/release_manifest.py dist/inkdeck-0.2.0.bin --version 0.2.0 \
        --url https://github.com/OWNER/REPO/releases/download/v0.2.0/inkdeck-0.2.0.bin \
        --out dist/manifest.json

Refuses an image that isn't an ESP app image or doesn't fit an OTA slot.
"""
import argparse
import datetime
import hashlib
import json
import re
import subprocess
import sys

SLOT_BYTES = 0x300000  # app3M_fat9M_16MB.csv (platformio.ini): each of app0 and app1
APP_IMAGE_MAGIC = 0xE9
# As scripts/gen_build_info.py makes them: a tag's 0.2.0 or 0.3.0-rc1, a hand-run build's 0.2.0+5.
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+\d+)?$")


def commit():
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", help="the firmware image (.pio/build/epaper397/firmware.bin)")
    ap.add_argument("--version", required=True, help="the release's version, without the v")
    ap.add_argument("--url", required=True, help="where the board downloads the image")
    ap.add_argument("--build", default=None, help="the commit (default: git's HEAD)")
    ap.add_argument("--out", required=True, help="the manifest to write")
    args = ap.parse_args()

    if not VERSION_RE.match(args.version):
        sys.exit("release_manifest: version %r isn't <major>.<minor>.<patch>" % args.version)
    # The board takes plain http only from a manifest it fetched over http (a test server).
    if not args.url.startswith(("https://", "http://")):
        sys.exit("release_manifest: the url must be https:// (or http:// for a test server)")
    with open(args.image, "rb") as f:
        image = f.read()
    if not image or image[0] != APP_IMAGE_MAGIC:
        sys.exit("release_manifest: %s isn't an ESP app image" % args.image)
    if len(image) > SLOT_BYTES:
        sys.exit("release_manifest: %d bytes won't fit the %d-byte OTA slot" % (len(image), SLOT_BYTES))

    manifest = {
        "version": args.version,
        "build": args.build if args.build is not None else commit(),
        "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "url": args.url,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
