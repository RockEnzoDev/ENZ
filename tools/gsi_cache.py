#!/usr/bin/env python3
"""GSI tile cache builder for the ENZ T-Deck map viewer.

Downloads Geospatial Information Authority of Japan (GSI) map tiles
around a centre point and writes them in the layout the T-Deck
firmware reads from SD:

    <out>/<z>/<x>/<y>.jpg

Run it at home before a trip; the T-Deck itself has no IP Wi-Fi,
so maps are prepared here and carried on the SD card.

Usage:
    python gsi_cache.py --lat 35.681 --lon 139.692 --radius 3 --zooms 14,15,16
    python gsi_cache.py --lat 36.19 --lon 138.72 --radius 5 --style photo

Tiles: (c) Geospatial Information Authority of Japan
(https://maps.gsi.go.jp/development/ichiran.html). Keep the "GSI"
attribution visible when you use the maps. Requires Pillow for the
standard style (its tiles are PNG and are converted to JPG):
    pip install pillow
"""
import argparse
import io
import math
import os
import sys
import time
import urllib.request

URLS = {
    # style: (url template, source extension)
    "std": ("https://cyberjapandata.gsi.go.jp/xyz/std/{z}/{x}/{y}.png", "png"),
    "photo": ("https://cyberjapandata.gsi.go.jp/xyz/seamlessphoto/{z}/{x}/{y}.jpg", "jpg"),
}
UA = {"User-Agent": "ENZ-CoT-gsi-cache/1.0 (personal offline map cache)"}


def tile_xy(lat, lon, z):
    n = 2.0 ** z
    x = (lon + 180.0) / 360.0 * n
    r = math.radians(lat)
    y = (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * n
    return x, y


def fetch(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description="Cache GSI tiles for the T-Deck map viewer")
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--radius", type=float, default=2.0, help="radius in km (default 2)")
    ap.add_argument("--zooms", default="14,15,16", help="comma list (default 14,15,16)")
    ap.add_argument("--style", choices=list(URLS), default="std")
    ap.add_argument("--out", default="maps", help="output folder (default ./maps)")
    args = ap.parse_args()

    zooms = [int(z) for z in args.zooms.split(",") if z.strip()]
    tmpl, ext = URLS[args.style]

    convert = ext != "jpg"
    Image = None
    if convert:
        try:
            from PIL import Image
        except ImportError:
            print("Pillow is required for the 'std' style:  pip install pillow")
            print("(or use --style photo, whose tiles are already JPG)")
            return 1

    # lat/lon half-extents of the requested radius
    dlat = args.radius / 111.32
    dlon = args.radius / (111.32 * max(0.2, math.cos(math.radians(args.lat))))

    total = done = skipped = failed = 0
    nbytes = 0
    for z in zooms:
        x0, y1 = tile_xy(args.lat + dlat, args.lon - dlon, z)
        x1, y0 = tile_xy(args.lat - dlat, args.lon + dlon, z)
        xs = range(int(math.floor(x0)), int(math.floor(x1)) + 1)
        ys = range(int(math.floor(y0)), int(math.floor(y1)) + 1)
        count = len(xs) * len(ys)
        total += count
        print(f"z{z}: {count} tiles  x {xs.start}..{xs.stop - 1}  y {ys.start}..{ys.stop - 1}")
        for x in xs:
            for y in ys:
                dst = os.path.join(args.out, str(z), str(x), f"{y}.jpg")
                if os.path.exists(dst):
                    skipped += 1
                    continue
                url = tmpl.format(z=z, x=x, y=y)
                try:
                    data = fetch(url)
                except Exception as e:
                    print(f"  FAIL {url}: {e}")
                    failed += 1
                    continue
                if convert:
                    im = Image.open(io.BytesIO(data)).convert("RGB")
                    buf = io.BytesIO()
                    im.save(buf, "JPEG", quality=85)
                    data = buf.getvalue()
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(data)
                done += 1
                nbytes += len(data)
                time.sleep(0.15)  # be polite to the tile server
    print(f"done: {done} new, {skipped} already cached, {failed} failed, "
          f"{nbytes / 1048576:.1f} MiB written to {args.out}/")
    print("Copy the folder onto the SD card as /maps. Tiles: (c) GSI Japan.")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
