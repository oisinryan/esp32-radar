#!/usr/bin/env python3
"""Aggregator for xraywifi ESP32 nodes over USB serial and/or Wi-Fi TCP.

Serves the dashboard, a WebSocket, a TCP ingest port, and a UDP discovery beacon
so boards on wall power can find this Mac on the LAN.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import socket
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import serial
import serial.tools.list_ports
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

ROOT = Path(__file__).resolve().parent.parent
DASHBOARD = ROOT / "dashboard"
CONTACT_TTL = 30.0
NODE_TTL = 15.0
BROADCAST_HZ = 8
UDP_PORT = 49421
TCP_PORT = 8082

state_lock = threading.Lock()
nodes: dict[str, dict[str, Any]] = {}
contacts: dict[str, dict[str, Any]] = {}
clients: set = set()
loop: asyncio.AbstractEventLoop | None = None


def norm_addr(addr: str) -> str:
    return addr.strip().upper().replace("-", ":")


def discover_ports(explicit: list[str] | None) -> list[str]:
    if explicit:
        return explicit
    found = []
    for p in serial.tools.list_ports.comports():
        dev = p.device
        desc = f"{p.description} {p.manufacturer} {p.hwid}".lower()
        if "usbmodem" in dev or "usbserial" in dev or "jtag" in desc or "espressif" in desc:
            found.append(dev)
    return sorted(found)


def local_ipv4s() -> list[str]:
    ips: set[str] = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("1.1.1.1", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return [i for i in sorted(ips) if not i.startswith("127.")]


def rfc1918(ip: str) -> bool:
    p = ip.split(".")
    if len(p) != 4:
        return False
    try:
        a, b = int(p[0]), int(p[1])
    except ValueError:
        return False
    return a == 10 or a == 192 and b == 168 or a == 172 and 16 <= b <= 31


def lan_ipv4s() -> list[str]:
    return [i for i in local_ipv4s() if rfc1918(i)]


def upsert_node(nid: str, port: str, patch: dict[str, Any]) -> None:
    now = time.time()
    with state_lock:
        row = nodes.get(nid) or {"id": nid, "port": port, "label": ""}
        row.update(patch)
        row["id"] = nid
        row["port"] = port
        if "via" not in patch:
            row["via"] = "wifi" if str(port).startswith("wifi:") else "usb"
        row["last_seen"] = now
        row["ok"] = True
        nodes[nid] = row


def apply_contacts(nid: str, items: list[dict[str, Any]]) -> None:
    now = time.time()
    with state_lock:
        for c in items:
            addr = norm_addr(str(c.get("addr") or ""))
            if not addr:
                continue
            row = contacts.get(addr) or {
                "addr": addr,
                "name": "",
                "type": "WIFI",
                "open": False,
                "brg": 0,
                "nodes": {},
            }
            name = str(c.get("name") or "").strip()
            if name and name != addr:
                row["name"] = name
            elif not row["name"]:
                row["name"] = name or addr
            row["type"] = str(c.get("type") or row["type"])
            row["open"] = bool(c.get("open", row["open"]))
            row["brg"] = int(c.get("brg") or row["brg"] or 0)
            row["nodes"][nid] = {
                "rssi": int(c.get("rssi", -100)),
                "age": int(c.get("age", 0)),
                "seen": now,
            }
            contacts[addr] = row


def apply_sweep(nid: str, port: str, msg: dict[str, Any]) -> None:
    now = time.time()
    patch = {"last_sweep": now}
    if msg.get("mode"):
        patch["mode"] = msg.get("mode")
    for key in ("ap", "ble", "tracked", "heap"):
        if key in msg:
            patch[key] = msg.get(key, 0)
    upsert_node(nid, port, patch)
    apply_contacts(nid, msg.get("contacts") or [])


def prune() -> None:
    now = time.time()
    with state_lock:
        dead_nodes = [k for k, v in nodes.items() if now - v.get("last_seen", 0) > NODE_TTL]
        for k in dead_nodes:
            nodes[k]["ok"] = False
        drop = []
        for addr, row in contacts.items():
            alive = {
                nid: info
                for nid, info in row["nodes"].items()
                if now - info.get("seen", 0) <= CONTACT_TTL
            }
            if not alive:
                drop.append(addr)
            else:
                row["nodes"] = alive
        for addr in drop:
            contacts.pop(addr, None)


def snapshot() -> dict[str, Any]:
    prune()
    with state_lock:
        node_list = sorted(nodes.values(), key=lambda n: n.get("id") or "")
        labels = "ABC"
        for i, n in enumerate(node_list):
            n["label"] = labels[i] if i < len(labels) else str(i + 1)
        merged = []
        for row in contacts.values():
            rssis = [info["rssi"] for info in row["nodes"].values()]
            ages = [time.time() - info["seen"] for info in row["nodes"].values()]
            merged.append(
                {
                    "addr": row["addr"],
                    "name": row["name"],
                    "type": row["type"],
                    "open": row["open"],
                    "brg": row["brg"],
                    "best": max(rssis) if rssis else -127,
                    "age": min(ages) if ages else 99,
                    "nodes": {
                        nid: {"rssi": info["rssi"], "age": round(time.time() - info["seen"], 1)}
                        for nid, info in row["nodes"].items()
                    },
                }
            )
        merged.sort(key=lambda c: c["best"], reverse=True)
        return {
            "t": "state",
            "ts": time.time(),
            "nodes": node_list,
            "contacts": merged,
        }


def handle_line(port: str, line: str, fallback_id: str) -> None:
    line = line.strip()
    if not line.startswith("{"):
        if line and '"t":' not in line and not line.endswith("}"):
            print(f"[{Path(port).name}] {line}", flush=True)
        return
    if not line.endswith("}"):
        return
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        if len(line) < 400:
            print(f"[{Path(port).name}] bad json: {line}", flush=True)
        else:
            print(f"[{Path(port).name}] bad json ({len(line)} bytes)", flush=True)
        return
    kind = msg.get("t")
    nid = norm_addr(str(msg.get("id") or fallback_id))
    extra: dict[str, Any] = {}
    if msg.get("via"):
        extra["via"] = msg.get("via")
    if msg.get("ip"):
        extra["ip"] = msg.get("ip")
    if kind == "hello":
        extra["heap"] = msg.get("heap", 0)
        extra.setdefault("mode", "BOTH")
        upsert_node(nid, port, extra)
        print(f"[{port}] hello {nid}", flush=True)
    elif kind == "status":
        extra.update(
            {
                "mode": msg.get("mode", "BOTH"),
                "ap": msg.get("ap", 0),
                "ble": msg.get("ble", 0),
                "tracked": msg.get("tracked", 0),
                "heap": msg.get("heap", 0),
            }
        )
        upsert_node(nid, port, extra)
    elif kind == "blip":
        upsert_node(nid, port, extra)
        apply_contacts(nid, [msg])
    elif kind == "sweep":
        apply_sweep(nid, port, msg)
        if extra:
            upsert_node(nid, port, extra)
    else:
        print(f"[{port}] {line[:200]}", flush=True)


def reader_thread(port: str, stop: threading.Event) -> None:
    fallback = Path(port).name
    while not stop.is_set():
        ser = None
        try:
            ser = serial.Serial(port, 115200, timeout=1)
            print(f"opened {port}", flush=True)
            buf = bytearray()
            last_host = 0.0
            while not stop.is_set():
                now = time.time()
                if now - last_host >= 20:
                    ips = lan_ipv4s()
                    if ips:
                        try:
                            ser.write(f"host {ips[0]}\n".encode())
                        except Exception:
                            pass
                    last_host = now
                chunk = ser.read(8192)
                if not chunk:
                    continue
                buf.extend(chunk)
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    try:
                        line = raw.decode("utf-8", errors="replace")
                    except Exception:
                        continue
                    handle_line(port, line, fallback)
        except serial.SerialException as e:
            print(f"{port}: {e}", flush=True)
            time.sleep(1.5)
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass


class DashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DASHBOARD), **kwargs)

    def log_message(self, fmt: str, *args: Any) -> None:
        print("[http] " + (fmt % args), flush=True)

    def do_GET(self) -> None:
        if self.path.split("?", 1)[0] == "/api/state":
            body = json.dumps(snapshot()).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/":
            self.path = "/index.html"
        super().do_GET()


async def ws_handler(ws) -> None:
    clients.add(ws)
    try:
        await ws.send(json.dumps(snapshot()))
        async for _ in ws:
            pass
    except ConnectionClosed:
        pass
    finally:
        clients.discard(ws)


async def broadcaster() -> None:
    delay = 1.0 / BROADCAST_HZ
    while True:
        await asyncio.sleep(delay)
        if not clients:
            continue
        payload = json.dumps(snapshot())
        dead = []
        for ws in list(clients):
            try:
                await ws.send(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            clients.discard(ws)


async def tcp_board(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    ip = peer[0] if peer else "wifi"
    label = f"wifi:{ip}"
    print(f"board connected {label}", flush=True)
    buf = b""
    try:
        while True:
            chunk = await reader.read(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, _, buf = buf.partition(b"\n")
                line = raw.decode("utf-8", errors="replace")
                handle_line(label, line, ip)
    except Exception as e:
        print(f"{label}: {e}", flush=True)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        print(f"board dropped {label}", flush=True)


async def udp_beacon(tcp_port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setblocking(False)
    while True:
        ips = lan_ipv4s()
        if not ips:
            await asyncio.sleep(1)
            continue
        for ip in ips:
            msg = f"XRAYWIFI ip={ip} tcp={tcp_port}\n".encode()
            try:
                sock.sendto(msg, ("255.255.255.255", UDP_PORT))
                parts = ip.split(".")
                if len(parts) == 4:
                    sock.sendto(msg, (".".join(parts[:3] + ["255"]), UDP_PORT))
            except OSError:
                pass
        await asyncio.sleep(1)


async def main_async(args: argparse.Namespace) -> None:
    global loop
    loop = asyncio.get_running_loop()
    ports = [] if args.no_serial else discover_ports(args.ports)
    if ports:
        print("serial:", ", ".join(ports), flush=True)
    else:
        print("no serial ports — Wi-Fi boards still work", flush=True)

    stop = threading.Event()
    for p in ports:
        threading.Thread(target=reader_thread, args=(p, stop), daemon=True).start()

    httpd = ThreadingHTTPServer((args.host, args.port), DashboardHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    lan = lan_ipv4s()
    print(f"dashboard http://{args.host}:{args.port}/", flush=True)
    for ip in lan:
        print(f"  LAN  http://{ip}:{args.port}/", flush=True)
    if not lan:
        print("  (no LAN IPv4 yet — join Wi-Fi on this Mac)", flush=True)

    try:
        async with serve(ws_handler, args.host, args.ws_port):
            print(f"websocket ws://{args.host}:{args.ws_port}/", flush=True)
            tcp = await asyncio.start_server(tcp_board, args.host, args.tcp_port)
            sockets = tcp.sockets or []
            for s in sockets:
                print(f"boards tcp {s.getsockname()[0]}:{s.getsockname()[1]}", flush=True)
            print(f"discovery UDP broadcast port {UDP_PORT}", flush=True)
            print("allow incoming TCP/UDP for Python if macOS Firewall asks", flush=True)
            async with tcp:
                await asyncio.gather(broadcaster(), udp_beacon(args.tcp_port))
    finally:
        stop.set()
        httpd.shutdown()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="xraywifi bridge + dashboard")
    p.add_argument("--host", default="0.0.0.0", help="bind address (default all interfaces)")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--ws-port", type=int, default=8081)
    p.add_argument("--tcp-port", type=int, default=TCP_PORT)
    p.add_argument("--no-serial", action="store_true", help="do not open USB serial ports")
    p.add_argument("ports", nargs="*", help="serial devices; default = auto-detect")
    return p.parse_args()


if __name__ == "__main__":
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        sys.exit(0)
