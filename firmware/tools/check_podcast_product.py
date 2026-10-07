#!/usr/bin/env python3
"""Reject podcast app images above the modern installer protocol-4 limit.

This product-size check supplements structural firmware verification. Passing
it does not prove permanent Recovery, phone installation, or data migration.
"""
from pathlib import Path
import csv
import sys

MAX_APP_BYTES = 0x600000
PRODUCT_PARTITIONS = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "phy_init": ("data", "phy", 0xF000, 0x1000),
    "factory": ("app", "factory", 0x10000, 0x650000),
    "store": ("data", "nvs", 0x660000, 0x4000),
    "netcfg": ("data", "nvs", 0x6BC000, 0x4000),
    "recovery": ("app", "test", 0x6C0000, 0x140000),
}


def check_layout(path: Path) -> None:
    parsed = {}
    for row in csv.reader(path.read_text().splitlines()):
        if not row or row[0].lstrip().startswith("#"):
            continue
        fields = [value.strip() for value in row]
        if len(fields) < 5 or fields[0] in parsed:
            raise ValueError("malformed or repeated product partition")
        parsed[fields[0]] = (fields[1], fields[2], int(fields[3], 0), int(fields[4], 0))
    for label, expected in PRODUCT_PARTITIONS.items():
        if parsed.get(label) != expected:
            raise ValueError(f"product partition {label} does not match the verified protocol-4 contract")


def check_app(path: Path) -> int:
    size = path.stat().st_size
    if size <= 0:
        raise ValueError("podcast application image is empty")
    if size > MAX_APP_BYTES:
        raise ValueError(f"podcast app {size} bytes exceeds the protocol-4 limit {MAX_APP_BYTES} bytes; do not publish it as a phone-installable app")
    return size


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("Usage: check_podcast_product.py <application-bin> [partitions.csv]", file=sys.stderr)
        return 2
    try:
        size = check_app(Path(sys.argv[1]))
        if len(sys.argv) == 3:
            check_layout(Path(sys.argv[2]))
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Podcast protocol-4 app size: PASS ({size} / {MAX_APP_BYTES} bytes); physical recovery/install compatibility remains separately verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
