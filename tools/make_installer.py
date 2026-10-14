"""Builds the browser installer (ESP Web Tools) from the built firmware envs.

    pixi run installer          # after pixi run fw-build; -> out/installer/
    python tools/make_installer.py --release DIR
                                # from a release's images instead (CI: page-only update)

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
# board name (BOARD_NAME), PlatformIO env, chip family, bootloader offset, label,
# release file prefix (<prefix>-reader-<version>.bin, tools/fw_dist.sh)
BOARDS = [
    ("t5-213", "reader", "ESP32", 0x1000, "LilyGo T5-2.13", "t5"),
    ("t5-47", "reader_t547", "ESP32-S3", 0x0, 'LilyGo T5-4.7" S3', "t5-47"),
    (
        "m5papers3",
        "reader_m5papers3",
        "ESP32-S3",
        0x0,
        "M5Stack M5PaperS3",
        "m5papers3",
    ),
    ("m5paper", "reader_m5paper", "ESP32", 0x1000, "M5Stack M5Paper", "m5paper"),
]
PARTITIONS_AT, BOOT_APP0_AT, APP_AT = 0x8000, 0xE000, 0x10000


def boot_app0() -> Path:
    """boot_app0.bin (OTA data: boot from app0) from the Arduino-ESP32 package."""
    for base in (Path.home() / ".platformio" / "packages").glob(
        "framework-arduinoespressif32*"
    ):
        path = base / "tools" / "partitions" / "boot_app0.bin"
        if path.exists():
            return path
    raise FileNotFoundError(
        "boot_app0.bin: build a firmware env first (pixi run fw-build)"
    )


def fw_version() -> str:
    ini = (ROOT / "firmware" / "platformio.ini").read_text()
    return re.search(r'-DFW_VERSION=\\"([^\\"]+)\\"', ini).group(1)


def built_parts(board: str, env: str, boot_at: int, prefix: str) -> dict[str, bytes]:
    """The parts from this checkout's PlatformIO build."""
    build_dir = ROOT / "firmware" / ".pio" / "build" / env
    parts = {
        p: (build_dir / p).read_bytes()
        for p in ("bootloader.bin", "partitions.bin", "firmware.bin")
    }
    parts["boot_app0.bin"] = boot_app0().read_bytes()
    return parts


def release_version(release: Path) -> str:
    """The version in a release's image names (t5-reader-<version>-ota.bin)."""
    for f in release.glob("t5-reader-*-ota.bin"):
        return f.name[len("t5-reader-") : -len("-ota.bin")]
    raise FileNotFoundError(f"{release}: no t5-reader-<version>-ota.bin")


def release_parts(release: Path, version: str):
    """Parts cut from a release's merged image (bootloader, partition table and
    boot_app0 at their offsets; firmware.factory.bin) and its -ota.bin (the app).
    The cut regions run up to the next part, padding included: the same bytes
    the merged image would write there, and NVS (0x9000-0xDFFF) is left alone."""

    def parts(board: str, env: str, boot_at: int, prefix: str) -> dict[str, bytes]:
        merged = (release / f"{prefix}-reader-{version}.bin").read_bytes()
        app = (release / f"{prefix}-reader-{version}-ota.bin").read_bytes()
        if merged[APP_AT:] != app:
            raise ValueError(
                f"{prefix}: the merged image doesn't end with the -ota.bin app"
            )
        return {
            "bootloader.bin": merged[boot_at:PARTITIONS_AT],
            "partitions.bin": merged[PARTITIONS_AT : PARTITIONS_AT + 0x1000],
            "boot_app0.bin": merged[BOOT_APP0_AT:APP_AT],
            "firmware.bin": app,
        }

    return parts


def build(out: Path, version: str, source=built_parts) -> list[str]:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rows = []
    for board, env, chip, boot_at, label, prefix in BOARDS:
        d = out / board
        d.mkdir()
        parts = source(board, env, boot_at, prefix)
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            (d / name).write_bytes(parts[name])
        (out / "boot_app0.bin").write_bytes(parts["boot_app0.bin"])  # the same for all
        manifest = {
            "name": f"T5 Reader for {label}",
            "version": version,
            "new_install_prompt_erase": True,  # ask, "erase" unticked: settings stay
            "builds": [{
                "chipFamily": chip,
                "parts": [
                    {"path": f"{board}/bootloader.bin", "offset": boot_at},
                    {"path": f"{board}/partitions.bin", "offset": PARTITIONS_AT},
                    {"path": "boot_app0.bin", "offset": BOOT_APP0_AT},
                    {"path": f"{board}/firmware.bin", "offset": APP_AT},
                ],
            }],
        }  # fmt: skip
        (out / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=1))
        rows.append(board)
    page = (
        (ROOT / "installer" / "index.html").read_text().replace("{{VERSION}}", version)
    )
    (out / "index.html").write_text(page)
    return rows


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, default=ROOT / "out" / "installer")
    p.add_argument(
        "--release", type=Path, help="folder with a release's *-reader-*.bin images"
    )
    args = p.parse_args(argv)
    if args.release:
        version = release_version(args.release)
        boards = build(args.out, version, release_parts(args.release, version))
    else:
        version = fw_version()
        boards = build(args.out, version)
    print(f"{args.out}: installer for {', '.join(boards)} (v{version})")
    print(
        f"try it: python -m http.server -d {args.out} 8001, then http://localhost:8001"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
