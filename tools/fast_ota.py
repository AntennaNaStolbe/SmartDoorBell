#!/usr/bin/env python3
"""Start a Zigbee OTA update with faster chunk settings via Home Assistant MQTT."""

from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import urllib.error
import urllib.request

DEFAULT_DEVICE = "0x9888e0fffe71bca8"
DEFAULT_RELEASES_URL = "https://github.com/AntennaNaStolbe/SmartDoorBell/releases"
DEFAULT_ASSET = "AntennaNS-SmartDoorBell.zigbee"
MQTT_TOPIC = "zigbee2mqtt/bridge/request/device/ota_update/update"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--ha-url",
        default=os.environ.get("HA_URL"),
        help="Home Assistant base URL; defaults to HA_URL",
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("HA_TOKEN"),
        help="Home Assistant long-lived token; defaults to HA_TOKEN",
    )
    parser.add_argument(
        "--device",
        default=DEFAULT_DEVICE,
        help="Zigbee IEEE address or friendly name",
    )
    parser.add_argument(
        "--version",
        default="latest",
        help="release tag, or latest",
    )
    parser.add_argument(
        "--firmware-url",
        default=None,
        help="explicit firmware URL; overrides --version",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=64,
        help="OTA image block size in bytes",
    )
    parser.add_argument(
        "--delay",
        type=int,
        default=50,
        help="delay between OTA blocks in milliseconds",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="disable TLS certificate verification",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the MQTT request without publishing",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="start the OTA without an interactive confirmation",
    )
    return parser.parse_args()


def get_firmware_url(args: argparse.Namespace) -> str:
    if args.firmware_url:
        return args.firmware_url
    if args.version == "latest":
        return f"{DEFAULT_RELEASES_URL}/latest/download/{DEFAULT_ASSET}"
    return f"{DEFAULT_RELEASES_URL}/download/{args.version}/{DEFAULT_ASSET}"


def validate(args: argparse.Namespace) -> None:
    if not 10 <= args.chunk_size <= 100:
        sys.exit("--chunk-size must be between 10 and 100")
    if args.delay < 50:
        sys.exit("--delay must be at least 50")
    if not args.dry_run and not args.ha_url:
        sys.exit("--ha-url or HA_URL is required")
    if not args.dry_run and not args.token:
        sys.exit("--token or HA_TOKEN is required")


def publish(args: argparse.Namespace, payload: dict[str, object]) -> None:
    if args.dry_run:
        print(f"Topic: {MQTT_TOPIC}")
        print(json.dumps(payload, indent=2))
        return

    if not args.yes:
        print(f"Device: {args.device}")
        print(f"Firmware: {payload['url']}")
        print(f"Chunk: {args.chunk_size} bytes, delay: {args.delay} ms")
        answer = input("Start OTA? [y/N] ").strip().lower()
        if answer not in {"y", "yes"}:
            sys.exit("Cancelled")

    body = {
        "topic": MQTT_TOPIC,
        "payload": json.dumps(payload, separators=(",", ":")),
        "qos": 1,
    }
    request = urllib.request.Request(
        f"{args.ha_url.rstrip('/')}/api/services/mqtt/publish",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {args.token}",
            "Content-Type": "application/json",
        },
        method="POST",
    )

    context = ssl.create_default_context()
    if args.insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

    try:
        with urllib.request.urlopen(request, context=context, timeout=60) as response:
            response_body = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        sys.exit(f"Home Assistant API error {error.code}: {detail}")
    except urllib.error.URLError as error:
        sys.exit(f"Home Assistant connection failed: {error.reason}")

    print(f"OTA started for {args.device}")
    print(f"Chunk: {args.chunk_size} bytes, delay: {args.delay} ms")
    print(f"Response: {response_body or 'OK'}")


def main() -> None:
    args = parse_args()
    validate(args)
    payload = {
        "id": args.device,
        "url": get_firmware_url(args),
        "default_maximum_data_size": args.chunk_size,
        "image_block_response_delay": args.delay,
    }
    publish(args, payload)


if __name__ == "__main__":
    main()
