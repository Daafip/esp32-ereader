#!/usr/bin/env python3
"""Starts the book server with the add-on's options (/data/options.json), after
logging the address to enter on the reader's page."""

import json
import os
import sys
import urllib.request

PORT = 8484


def supervisor(path: str) -> dict:
    req = urllib.request.Request(
        f"http://supervisor{path}",
        headers={"Authorization": f"Bearer {os.environ.get('SUPERVISOR_TOKEN', '')}"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.load(r)["data"]


def reader_address() -> str | None:
    """http://<Home Assistant's address>:<the port mapped to 8484>."""
    try:
        port = (supervisor("/addons/self/info").get("network") or {}).get(f"{PORT}/tcp")
        if not port:
            return None  # the port is switched off in the add-on's Network settings
        for iface in supervisor("/network/info")["interfaces"]:
            addresses = (iface.get("ipv4") or {}).get("address") or []
            if iface.get("primary") and addresses:
                return f"http://{addresses[0].split('/')[0]}:{port}"
    except Exception as e:  # only a nicety: the server starts anyway
        print(f"(no address from the Supervisor: {e})", flush=True)
    return None


def main() -> None:
    address = reader_address()
    if address:
        print(f"Address for the reader (its page: Settings, Book server): {address}", flush=True)
    os.execv(sys.executable, [
        sys.executable, "-m", "bookprep.bookserver",
        "--options", "/data/options.json",
        "--cache", "/data/cache",
        "--font-cache", "/opt/t5books/fonts",
        "--fonts", "/share/t5books/fonts",
        "--port", str(PORT),
    ])  # fmt: skip


if __name__ == "__main__":
    main()
