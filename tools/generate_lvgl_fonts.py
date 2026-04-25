from __future__ import annotations

import argparse
import subprocess
import shutil
import sys
from pathlib import Path


BODY_SIZES = [16, 20, 24]
TITLE_SIZES = [18, 22, 26]
UNICODE_RANGE = (
    "0x20-0x7E,"
    "0xA0,"
    "0xC0-0x17F,"
    "0x2013-0x2014,"
    "0x2018-0x201A,"
    "0x201C-0x201E,"
    "0x2026"
)


def run_lv_font_conv(font_path: Path, out_path: Path, bpp: int, size: int, symbol_range: str) -> None:
    if sys.platform.startswith("win"):
        npx_exe = shutil.which("npx.cmd") or shutil.which("npx")
    else:
        npx_exe = shutil.which("npx")

    if not npx_exe:
        raise RuntimeError("npx executable not found. Install Node.js/npm and ensure npx is on PATH.")

    command = [
        npx_exe,
        "-y",
        "lv_font_conv",
        "--font",
        str(font_path),
        "--format",
        "lvgl",
        "--bpp",
        str(bpp),
        "--size",
        str(size),
        "--range",
        symbol_range,
        "--no-compress",
        "--output",
        str(out_path),
    ]
    subprocess.run(command, check=True)

    generated = out_path.read_text(encoding="utf-8")
    include_block = (
        "#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n"
        "#include \"lvgl.h\"\n"
        "#else\n"
        "#include \"lvgl/lvgl.h\"\n"
        "#endif\n"
    )
    if include_block in generated:
        generated = generated.replace(include_block, '#include "lvgl.h"\n')
        out_path.write_text(generated, encoding="utf-8")


def write_book_font_header(header_path: Path) -> None:
    header = """#ifndef BOOK_FONT_H
#define BOOK_FONT_H

#include \"lvgl.h\"

LV_FONT_DECLARE(book_font_body_16);
LV_FONT_DECLARE(book_font_body_20);
LV_FONT_DECLARE(book_font_body_24);

LV_FONT_DECLARE(book_font_title_18);
LV_FONT_DECLARE(book_font_title_22);
LV_FONT_DECLARE(book_font_title_26);

#endif
"""
    header_path.write_text(header, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate LVGL C fonts for reader runtime sizes")
    parser.add_argument("--font", required=True, type=Path, help="Path to source .ttf/.otf font file")
    parser.add_argument("--output-dir", type=Path, default=Path("main/fonts"), help="Directory for generated .c font files")
    parser.add_argument("--header", type=Path, default=Path("main/book_font.h"), help="Output header path with LV_FONT_DECLARE symbols")
    parser.add_argument("--bpp", type=int, default=4, help="Bits per pixel for generated glyphs")
    parser.add_argument("--range", default=UNICODE_RANGE, help="Unicode ranges for glyph generation")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.font.exists():
        raise FileNotFoundError(args.font)

    args.output_dir.mkdir(parents=True, exist_ok=True)

    for size in BODY_SIZES:
        run_lv_font_conv(
            font_path=args.font,
            out_path=args.output_dir / f"book_font_body_{size}.c",
            bpp=args.bpp,
            size=size,
            symbol_range=args.range,
        )

    for size in TITLE_SIZES:
        run_lv_font_conv(
            font_path=args.font,
            out_path=args.output_dir / f"book_font_title_{size}.c",
            bpp=args.bpp,
            size=size,
            symbol_range=args.range,
        )

    write_book_font_header(args.header)

    print("Generated LVGL fonts:")
    for path in sorted(args.output_dir.glob("book_font_*.c")):
        print(f" - {path}")
    print(f"Header: {args.header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
