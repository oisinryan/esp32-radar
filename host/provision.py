#!/usr/bin/env python3
"""Push Wi-Fi credentials to all attached xraywifi boards over USB serial.

Non-interactive:
  XRAY_SSID / XRAY_PASS / XRAY_HOST
  --ssid --password --host
"""

from __future__ import annotations

import argparse
import getpass
import os
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bridge import discover_ports, local_ipv4s  # noqa: E402


def send(port: str, ssid: str, password: str, host: str) -> None:
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"\n")
    ser.write(f"ssid {ssid}\n".encode())
    time.sleep(0.15)
    ser.write(f"pass {password}\n".encode())
    time.sleep(0.15)
    if host:
        ser.write(f"host {host}\n".encode())
        time.sleep(0.15)
    else:
        ser.write(b"host auto\n")
        time.sleep(0.15)
    ser.write(b"join\n")
    print(f"{port}:")
    deadline = time.time() + 8
    while time.time() < deadline:
        chunk = ser.read(2048)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            for line in text.splitlines():
                if line.startswith('{"t":"status"'):
                    continue
                print(line)
    ser.close()


def main() -> None:
    lan = local_ipv4s()
    guessed = next((ip for ip in lan if ip.startswith("10.") or ip.startswith("192.168.")), lan[0] if lan else "")
    p = argparse.ArgumentParser(description="Provision xraywifi boards over USB")
    p.add_argument("--ssid", default=os.environ.get("XRAY_SSID", ""))
    p.add_argument("--password", default=os.environ.get("XRAY_PASS", ""))
    p.add_argument("--host", default=os.environ.get("XRAY_HOST", guessed),
                   help="Mac LAN IP stored on the board (blank = UDP discovery only)")
    p.add_argument("ports", nargs="*", help="serial devices; default = auto-detect")
    args = p.parse_args()

    ports = discover_ports(args.ports or None)
    if not ports:
        print("no ESP32 serial ports found")
        sys.exit(1)
    print("boards:", ", ".join(ports))

    ssid = args.ssid.strip() or input("Wi-Fi SSID: ").strip()
    password = args.password
    if not password:
        password = getpass.getpass("Wi-Fi password: ")
    host = args.host.strip()
    if not args.ssid and sys.stdin.isatty() and guessed:
        typed = input(f"Mac LAN IP [{host or 'auto'}]: ").strip()
        if typed:
            host = typed

    if not ssid:
        print("SSID is required")
        sys.exit(1)

    print(f"joining {ssid!r}  host={host or 'auto (UDP)'}")
    for port in ports:
        send(port, ssid, password, host)
    print("done — leave host/bridge.py running and wait for via wifi on the dashboard")


if __name__ == "__main__":
    main()
