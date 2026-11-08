#!/usr/bin/env python3
"""Build a Zigbee OTA file for SmartDoorBell and update the local index."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

OTA_FILE_MAGIC = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_STACK_VERSION = 2
OTA_IMAGE_TAG = 0x0000
OTA_FIELD_CONTROL_HW_VERSIONS = 1 << 2
OTA_MANDATORY_HEADER_SIZE = 56
OTA_HEADER_STRING_SIZE = 32
OTA_IMAGE_ELEMENT_HEADER_SIZE = 6

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIRMWARE = ROOT / ".pio" / "build" / "esp32-c6-supermini" / "firmware.bin"
DEFAULT_OUTPUT = ROOT / "zigbee2mqtt" / "ota" / "DIY-SmartDoorBell.zigbee"
DEFAULT_INDEX = ROOT / "zigbee2mqtt" / "ota" / "index.json"
DEFAULT_CONFIG = ROOT / "include" / "config.h"
CONFIG_CONSTANTS = (
    "kFirmwareVersion",
    "kOtaManufacturer",
    "kOtaImageType",
    "kOtaHardwareVersion",
)


def parse_int(value: str) -> int:
    return int(value, 0)


def read_config(config_path: Path) -> dict[str, int]:
    try:
        source = config_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read {config_path}: {error}") from error

    values = {}
    for name in CONFIG_CONSTANTS:
        match = re.search(rf"\b{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", source)
        if match is None:
            raise ValueError(f"{name} not found in {config_path}")
        values[name] = int(match.group(1), 0)
    return values


def build_ota(firmware: bytes, args: argparse.Namespace) -> tuple[bytes, int]:
    header_length = OTA_MANDATORY_HEADER_SIZE + 4
    image_size = header_length + OTA_IMAGE_ELEMENT_HEADER_SIZE + len(firmware)
    header_string = args.header_string.encode("ascii")[:OTA_HEADER_STRING_SIZE]
    header_string = header_string.ljust(OTA_HEADER_STRING_SIZE, b"\x00")

    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_FILE_MAGIC,
        OTA_HEADER_VERSION,
        header_length,
        OTA_FIELD_CONTROL_HW_VERSIONS,
        args.manufacturer,
        args.image_type,
        args.version,
        OTA_STACK_VERSION,
        header_string,
        image_size,
    )
    header += struct.pack("<HH", args.hardware_version, args.hardware_version)

    element = struct.pack("<HI", OTA_IMAGE_TAG, len(firmware))
    return header + element + firmware, image_size


def update_index(index_path: Path, args: argparse.Namespace, output: Path,
                 image_size: int, digest: str) -> None:
    if index_path.exists():
        with index_path.open("r", encoding="utf-8") as handle:
            entries = json.load(handle)
        if not isinstance(entries, list):
            raise ValueError(f"{index_path} must contain a JSON array")
    else:
        entries = []

    entries = [
        entry for entry in entries
        if not (
            entry.get("manufacturerCode") == args.manufacturer
            and entry.get("imageType") == args.image_type
        )
    ]
    entries.append(
        {
            "fileName": output.name,
            "fileVersion": args.version,
            "fileSize": image_size,
            "url": args.url or f"ota/{output.name}",
            "imageType": args.image_type,
            "manufacturerCode": args.manufacturer,
            "sha512": digest,
            "otaHeaderString": args.header_string[:OTA_HEADER_STRING_SIZE],
        }
    )

    index_path.parent.mkdir(parents=True, exist_ok=True)
    with index_path.open("w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=2)
        handle.write("\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--firmware", type=Path, default=DEFAULT_FIRMWARE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--version", type=parse_int, default=None)
    parser.add_argument("--manufacturer", type=parse_int, default=None)
    parser.add_argument("--image-type", type=parse_int, default=None)
    parser.add_argument("--hardware-version", type=parse_int, default=None)
    parser.add_argument("--header-string", default="SmartDoorBell")
    url_group = parser.add_mutually_exclusive_group()
    url_group.add_argument("--url", default=None,
                           help="exact URL stored in index.json")
    url_group.add_argument("--base-url", default=None,
                           help="base URL; appends the generated file name")
    parser.add_argument("--max-size", type=parse_int, default=0x140000)
    args = parser.parse_args()

    try:
        config_values = read_config(args.config)
    except ValueError as error:
        sys.exit(str(error))

    if args.version is None:
        args.version = config_values["kFirmwareVersion"]
    if args.manufacturer is None:
        args.manufacturer = config_values["kOtaManufacturer"]
    if args.image_type is None:
        args.image_type = config_values["kOtaImageType"]
    if args.hardware_version is None:
        args.hardware_version = config_values["kOtaHardwareVersion"]
    if args.url is None:
        if args.base_url is not None:
            args.url = f"{args.base_url.rstrip('/')}/{args.output.name}"
        else:
            args.url = f"ota/{args.output.name}"

    if not args.firmware.is_file():
        sys.exit(f"Firmware not found: {args.firmware} (run pio run first)")
    firmware = args.firmware.read_bytes()
    if len(firmware) == 0:
        sys.exit("Firmware image is empty")
    if len(firmware) > args.max_size:
        sys.exit(f"Firmware {len(firmware)} bytes exceeds {args.max_size} bytes")

    image, image_size = build_ota(firmware, args)
    digest = hashlib.sha512(image).hexdigest()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    update_index(args.index, args, args.output, image_size, digest)

    print(f"Wrote {args.output} ({image_size} bytes)")
    print(f"Version 0x{args.version:08x}, manufacturer 0x{args.manufacturer:04x}, "
          f"image type 0x{args.image_type:04x}")
    print(f"SHA-512 {digest}")
    print(f"Updated {args.index}")


if __name__ == "__main__":
    main()
