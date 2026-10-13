"""Builds the browser installer (ESP Web Tools) from the built firmware envs.

    pixi run installer          # after pixi run fw-build; -> out/installer/

One manifest per board. It flashes the bootloader, the partition table,
boot_app0 and the app separately, so the settings and reading positions in NVS
(0x9000-0xDFFF) stay. ESP Web Tools erases the whole chip on a new install
unless the manifest says new_install_prompt_erase: then it asks, with "erase"
unticked. Serve out/installer/ over https or from localhost (Web Serial).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# board name (BOARD_NAME), PlatformIO env, chip family, bootloader offset, label
BOARDS = [
    ("t5-213", "reader", "ESP32", 0x1000, "LilyGo T5-2.13"),
    ("t5-47", "reader_t547", "ESP32-S3", 0x0, 'LilyGo T5-4.7" S3'),
    ("m5papers3", "reader_m5papers3", "ESP32-S3", 0x0, "M5Stack M5PaperS3"),
    ("m5paper", "reader_m5paper", "ESP32", 0x1000, "M5Stack M5Paper"),
]


def boot_app0() -> Path:
    """boot_app0.bin (OTA data: boot from app0) from the Arduino-ESP32 package."""
    for base in (Path.home() / ".platformio" / "packages").glob("framework-arduinoespressif32*"):
        path = base / "tools" / "partitions" / "boot_app0.bin"
        if path.exists():
            return path
    raise FileNotFoundError("boot_app0.bin: build a firmware env first (pixi run fw-build)")


def fw_version() -> str:
    ini = (ROOT / "firmware" / "platformio.ini").read_text()
    return re.search(r'-DFW_VERSION=\\"([^\\"]+)\\"', ini).group(1)


def build(out: Path) -> list[str]:
    version = fw_version()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copy(boot_app0(), out / "boot_app0.bin")
    rows = []
    for board, env, chip, boot_at, label in BOARDS:
        build_dir = ROOT / "firmware" / ".pio" / "build" / env
        d = out / board
        d.mkdir()
        for part in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            shutil.copy(build_dir / part, d / part)
        manifest = {
            "name": f"T5 Reader for {label}",
            "version": version,
            "new_install_prompt_erase": True,  # ask, "erase" unticked: settings stay
            "builds": [{
                "chipFamily": chip,
                "parts": [
                    {"path": f"{board}/bootloader.bin", "offset": boot_at},
                    {"path": f"{board}/partitions.bin", "offset": 0x8000},
                    {"path": "boot_app0.bin", "offset": 0xE000},
                    {"path": f"{board}/firmware.bin", "offset": 0x10000},
                ],
            }],
        }  # fmt: skip
        (out / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=1))
        rows.append(board)
    page = (ROOT / "installer" / "index.html").read_text().replace("{{VERSION}}", version)
    (out / "index.html").write_text(page)
    return rows


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, default=ROOT / "out" / "installer")
    args = p.parse_args(argv)
    boards = build(args.out)
    print(f"{args.out}: installer for {', '.join(boards)} (v{fw_version()})")
    print(f"try it: python -m http.server -d {args.out} 8001, then http://localhost:8001")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
