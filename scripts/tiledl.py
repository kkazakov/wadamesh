#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Download 256x256 PNG XYZ tiles for WadaMesh's offline map.

Writes the layout the firmware reads from a microSD card:

    <card>/maps/osm/<z>/<x>/<y>.png      (default; Map options -> Tiles from SD)

or, with --layout tiles, the equivalent /tiles/<z>/<x>/<y>.png tree that the
Transfer app produces. Tiles already on the card are skipped, so an interrupted
run is resumed by running the same command again.

The tile URL template must be an XYZ source whose licence and service terms
allow bulk/offline download:

    scripts/tiledl.py --out /media/$USER/SDCARD \
        --bbox 50.00,14.30,50.15,14.60 --zoom 10-16 \
        --url 'https://tiles.example.org/{z}/{x}/{y}.png' \
        --contact you@example.org

Placeholders: {z} {x} {y}, and optionally {s} for a subdomain from
--subdomains. This script refuses the OSM community tile servers
(tile.openstreetmap.org and the *.openstreetmap.* fronts): their policy
forbids offline bulk use. Use a self-hosted server or a paid/permitted
provider. Always keep the provider's attribution with the pack.

Downloaded tiles are validated as PNG and rejected above the firmware's
256 KiB per-tile limit; a tile that fails validation is deleted, not kept.
"""
import argparse
import concurrent.futures
import math
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

MAX_TILE_BYTES = 256 * 1024          # firmware rejects anything larger
TILE_SIZE = 256
PNG_SIG = b"\x89PNG\r\n\x1a\n"
LAT_LIMIT = 85.05112878              # Web Mercator cut-off
ZOOM_MIN, ZOOM_MAX = 3, 19           # the Transfer app / loader accept 3..19
DEFAULT_MAX_TILES = 25_000           # guard against a typo turning into 1M files

# Bulk-downloading from these is prohibited by the OSM tile usage policy.
BLOCKED_HOSTS = ("tile.openstreetmap.org", "tile.openstreetmap.de",
                 "tile.openstreetmap.fr", "tile.openstreetmap.bzh",
                 "tile.openstreetmap.se")
BLOCKED_SUFFIXES = (".openstreetmap.org",)


def fail(msg):
    sys.exit(f"tiledl: {msg}")


def parse_bbox(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("expected south,west,north,east")
    try:
        south, west, north, east = (float(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("expected four numbers")
    if not (-90 <= south < north <= 90):
        raise argparse.ArgumentTypeError("need south < north within -90..90")
    if not (-180 <= west < east <= 180):
        raise argparse.ArgumentTypeError(
            "need west < east within -180..180 (antimeridian crossing is not supported)")
    return south, west, north, east


def parse_zoom(text):
    lo, _, hi = text.partition("-")
    try:
        zmin, zmax = int(lo), int(hi or lo)
    except ValueError:
        raise argparse.ArgumentTypeError("expected Z or MIN-MAX, e.g. 10-16")
    if not (ZOOM_MIN <= zmin <= zmax <= ZOOM_MAX):
        raise argparse.ArgumentTypeError(
            f"zoom must be within {ZOOM_MIN}..{ZOOM_MAX} with MIN <= MAX")
    return zmin, zmax


def parse_center(text):
    parts = text.split(",")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("expected lat,lon")
    try:
        lat, lon = (float(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("expected two numbers")
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        raise argparse.ArgumentTypeError("lat must be -90..90, lon -180..180")
    return lat, lon


def lon_to_x(lon, z):
    return int(math.floor((lon + 180.0) / 360.0 * (1 << z)))


def lat_to_y(lat, z):
    lat = max(-LAT_LIMIT, min(LAT_LIMIT, lat))
    merc = math.asinh(math.tan(math.radians(lat)))
    return int(math.floor((1.0 - merc / math.pi) / 2.0 * (1 << z)))


def tile_range(bbox, z):
    south, west, north, east = bbox
    span = 1 << z
    def clamp(v):
        return min(max(v, 0), span - 1)
    x0, x1 = clamp(lon_to_x(west, z)), clamp(lon_to_x(east, z))
    y0, y1 = clamp(lat_to_y(north, z)), clamp(lat_to_y(south, z))
    return x0, y0, x1, y1


def bbox_from_center(lat, lon, radius_km):
    """Square bbox around a point; enough for a "download my town" run."""
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * max(math.cos(math.radians(lat)), 1e-6))
    return (max(lat - dlat, -90.0), max(lon - dlon, -180.0),
            min(lat + dlat, 90.0), min(lon + dlon, 180.0))


def check_source(url):
    if not any(p in url for p in ("{z}", "{x}", "{y}")):
        fail("--url needs {z}, {x} and {y} placeholders")
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https"):
        fail("--url must be http:// or https://")
    host = (parts.hostname or "").lower()
    if not host:
        fail("--url has no host")
    blocked = host in BLOCKED_HOSTS or host.endswith(BLOCKED_SUFFIXES) or \
        host.endswith(tuple("." + h for h in BLOCKED_HOSTS))
    if blocked:
        fail(f"refusing {host}: the OSM community tile servers forbid bulk/offline "
             "download (https://operations.osmfoundation.org/policies/tiles/).\n"
             "Use a self-hosted server or a provider whose terms permit offline packs.")
    return host


def png_size(data):
    if len(data) < 33 or not data.startswith(PNG_SIG) or data[12:16] != b"IHDR":
        return None
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def fmt_seconds(sec):
    if sec < 90:
        return f"{sec:.0f}s"
    if sec < 5400:
        return f"{sec / 60:.0f}m"
    return f"{sec / 3600:.1f}h"


def fmt_count(n):
    return f"{n:,}"


class Downloader:
    def __init__(self, args, root):
        self.args = args
        self.root = root
        self.lock = threading.Lock()
        self.next_slot = 0.0
        self.counts = {"ok": 0, "skip": 0, "missing": 0, "fail": 0}
        self.failures = []
        self.warned_size = False
        self.start = time.monotonic()
        self.last_draw = 0.0
        self.ua = "wadamesh-tiledl/1.0"
        if args.contact:
            self.ua += f" ({args.contact})"

    def tile_path(self, z, x, y):
        return os.path.join(self.root, self.args.layout.replace("-", os.sep),
                            str(z), str(x), f"{y}.png")

    def tile_url(self, z, x, y):
        sub = ""
        if "{s}" in self.args.url:
            subs = self.args.subdomains
            sub = subs[(x + y) % len(subs)]
        u = self.args.url
        for key, val in (("{z}", z), ("{x}", x), ("{y}", y), ("{s}", sub)):
            u = u.replace(key, str(val))
        return u

    def pace(self):
        if not self.args.rate:
            return
        with self.lock:
            now = time.monotonic()
            wait = self.next_slot - now
            if wait > 0:
                time.sleep(wait)
            self.next_slot = max(now, self.next_slot) + 1.0 / self.args.rate

    def fetch(self, url):
        """Return tile bytes, None on 404, raise on a torn connection."""
        delay = 1.0
        last = None
        for attempt in range(self.args.retries + 1):
            if attempt:
                time.sleep(delay)
                delay = min(delay * 2, 30.0)
            try:
                req = urllib.request.Request(url, headers={"User-Agent": self.ua})
                with urllib.request.urlopen(req, timeout=self.args.timeout) as resp:
                    return resp.read(MAX_TILE_BYTES + 1)
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    return None
                if e.code not in (408, 425, 429) and e.code < 500:
                    raise
                last = e
                retry_after = e.headers.get("Retry-After") if e.headers else None
                if retry_after and retry_after.isdigit():
                    delay = max(delay, float(retry_after))
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last = e
        raise last

    def one(self, task):
        z, x, y = task
        path = self.tile_path(z, x, y)
        if not self.args.overwrite and os.path.exists(path) and os.path.getsize(path):
            return "skip", None
        self.pace()
        try:
            data = self.fetch(self.tile_url(z, x, y))
        except Exception as e:  # noqa: BLE001 - reported per tile, run continues
            return "fail", f"{z}/{x}/{y}: {e}"
        if data is None:
            return "missing", None
        dims = png_size(data)
        if dims is None:
            return "fail", f"{z}/{x}/{y}: not a PNG"
        if len(data) > MAX_TILE_BYTES:
            return "fail", f"{z}/{x}/{y}: {len(data)} bytes > 256 KiB firmware limit"
        if dims != (TILE_SIZE, TILE_SIZE) and not self.warned_size:
            self.warned_size = True
            print(f"\ntiledl: warning: {z}/{x}/{y} is {dims[0]}x{dims[1]}, "
                  "expected 256x256; the map assumes 256 px tiles", file=sys.stderr)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        return "ok", None

    def progress(self, done, total, force=False):
        if self.args.quiet:
            return
        now = time.monotonic()
        if not force and now - self.last_draw < 0.2:
            return
        self.last_draw = now
        rate = done / max(now - self.start, 1e-9)
        eta = (total - done) / rate if rate > 0 else 0
        pct = 100.0 * done / total
        print(f"\r{done}/{total} {pct:5.1f}%  {rate:5.1f}/s  ETA {fmt_seconds(eta)}   ",
              end="", file=sys.stderr, flush=True)

    def record(self, status, failure=None):
        with self.lock:
            self.counts[status] += 1
            if failure and len(self.failures) < 200:
                self.failures.append(failure)


def plan(bbox, zoom):
    zmin, zmax = zoom
    rows = []
    for z in range(zmin, zmax + 1):
        x0, y0, x1, y1 = tile_range(bbox, z)
        rows.append((z, x0, y0, x1, y1, (x1 - x0 + 1) * (y1 - y0 + 1)))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Download 256x256 PNG XYZ tiles for WadaMesh's offline map.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Tile source terms must permit offline/bulk download; the OSM "
               "community tile servers are refused by this script.")
    ap.add_argument("--out", required=True,
                    help="microSD card root (or any directory to build a pack in)")
    ap.add_argument("--url", required=True,
                    help="XYZ template, e.g. https://tiles.example.org/{z}/{x}/{y}.png")
    ap.add_argument("--zoom", required=True, type=parse_zoom,
                    help="zoom level or inclusive range, e.g. 10 or 9-16")
    ap.add_argument("--bbox", type=parse_bbox,
                    help="south,west,north,east in degrees (WGS84)")
    ap.add_argument("--center", type=parse_center, metavar="LAT,LON",
                    help="center of a square area (use with --radius-km)")
    ap.add_argument("--radius-km", type=float, default=5.0,
                    help="half-size of the --center area (default 5)")
    ap.add_argument("--layout", choices=("maps-osm", "tiles"), default="maps-osm",
                    help="maps-osm -> <out>/maps/osm/... (firmware's first choice); "
                         "tiles -> <out>/tiles/... (default maps-osm)")
    ap.add_argument("--subdomains", default="abc",
                    help="comma-separated values for a {s} placeholder (default abc)")
    ap.add_argument("--workers", type=int, default=4,
                    help="concurrent requests (default 4; lower it if the provider asks)")
    ap.add_argument("--rate", type=float, default=0.0,
                    help="global requests/second cap, 0 = unlimited (default)")
    ap.add_argument("--retries", type=int, default=3,
                    help="retries per tile for timeouts and 5xx/429 (default 3)")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="per-request timeout in seconds (default 20)")
    ap.add_argument("--contact", default="",
                    help="contact appended to the User-Agent, as most policies ask")
    ap.add_argument("--max-tiles", type=int, default=DEFAULT_MAX_TILES,
                    help=f"abort if the area needs more than this (default {DEFAULT_MAX_TILES}; 0 disables)")
    ap.add_argument("--overwrite", action="store_true",
                    help="re-download tiles already on the card")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan and tile counts, download nothing")
    ap.add_argument("--quiet", action="store_true", help="no progress line")
    args = ap.parse_args(argv)

    if bool(args.bbox) == bool(args.center):
        fail("give exactly one of --bbox or --center")
    if args.center and args.radius_km <= 0:
        fail("--radius-km must be positive")
    if not (1 <= len(args.subdomains.split(",")) <= 8) or \
            not all(0 < len(s) < 32 for s in args.subdomains.split(",")):
        fail("--subdomains needs 1..8 short values")
    args.subdomains = args.subdomains.split(",")
    if args.workers < 1:
        fail("--workers must be at least 1")

    bbox = args.bbox or bbox_from_center(*args.center, args.radius_km)
    check_source(args.url)

    rows = plan(bbox, args.zoom)
    total = sum(r[5] for r in rows)
    if args.max_tiles and total > args.max_tiles:
        fail(f"{fmt_count(total)} tiles exceeds --max-tiles {fmt_count(args.max_tiles)}; "
             "narrow the area/zooms or raise the limit")

    print(f"bbox {bbox[0]:.5f},{bbox[1]:.5f} .. {bbox[2]:.5f},{bbox[3]:.5f}   "
          f"zoom {args.zoom[0]}-{args.zoom[1]}   {fmt_count(total)} tiles")
    for z, x0, y0, x1, y1, n in rows:
        print(f"  z{z:<2} x {x0}..{x1}  y {y0}..{y1}  {fmt_count(n)} tiles")
    print(f"target {os.path.abspath(os.path.join(args.out, args.layout.replace('-', os.sep)))}")
    if args.dry_run:
        return 0

    dl = Downloader(args, args.out)
    tasks = ((z, x, y) for z, x0, y0, x1, y1, _ in rows
             for y in range(y0, y1 + 1) for x in range(x0, x1 + 1))

    done = 0
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
            pending = set()
            it = iter(tasks)

            def fill():
                while len(pending) < args.workers * 4:
                    try:
                        pending.add(ex.submit(dl.one, next(it)))
                    except StopIteration:
                        return

            fill()
            while pending:
                finished, pending = concurrent.futures.wait(
                    pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for fut in finished:
                    status, failure = fut.result()
                    dl.record(status, failure)
                    done += 1
                dl.progress(done, total)
                fill()
    except KeyboardInterrupt:
        print("\ntiledl: interrupted; tiles already written are kept, "
              "re-run the same command to resume", file=sys.stderr)
        c = dl.counts
        print(f"tiledl: {c['ok']} downloaded, {c['skip']} present, "
              f"{c['missing']} missing, {c['fail']} failed", file=sys.stderr)
        return 130

    dl.progress(done, total, force=True)
    print(file=sys.stderr)
    c = dl.counts
    print(f"tiledl: {fmt_count(c['ok'])} downloaded, {fmt_count(c['skip'])} already present, "
          f"{fmt_count(c['missing'])} missing (404), {fmt_count(c['fail'])} failed")
    if dl.failures:
        print("first failures:", file=sys.stderr)
        for line in dl.failures[:20]:
            print(f"  {line}", file=sys.stderr)
        if len(dl.failures) > 20:
            print(f"  ... and {len(dl.failures) - 20} more", file=sys.stderr)
    return 1 if c["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
