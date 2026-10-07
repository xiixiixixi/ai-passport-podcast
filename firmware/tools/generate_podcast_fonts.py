#!/usr/bin/env python3
"""Rebuild licensed 4bpp fonts; pack title pixels losslessly after conversion."""
import argparse
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "assets/fonts/NotoSansSC-Regular.otf"
EXPECTED_SHA = "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9"
PUNCTUATION = "。，？！：；“”‘’、（）《》【】—·…–「」"
TITLE_SYMBOLS = ROOT / "assets/fonts/title-symbols.txt"


def convert(name, size, ranges, symbols):
    subprocess.run(["lv_font_conv", "--font", str(FONT.relative_to(ROOT)),
                    "-r", ranges, "--symbols", symbols, "--size", str(size),
                    "--bpp", "4", "--no-kerning", "--no-compress", "--format", "lvgl",
                    "--lv-font-name", name, "--lv-include", "lvgl.h",
                    "--output", f"main/{name}.c"], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--ui-only", action="store_true",
                        help="Regenerate only the fixed 16px UI vocabulary; preserve the full title font")
    selection.add_argument("--title-only", action="store_true",
                        help="Regenerate and losslessly pack only the full 18px title font")
    args = parser.parse_args()
    if hashlib.sha256(FONT.read_bytes()).hexdigest() != EXPECTED_SHA:
        raise SystemExit("Source font differs from the documented licensed artifact")
    if subprocess.check_output(["lv_font_conv", "--version"], text=True).strip() != "1.5.3":
        raise SystemExit("Use documented lv_font_conv version 1.5.3")
    if not args.title_only:
        texts = []
        for path in [ROOT / "main/demo_podcast.c", *sorted((ROOT / "main").glob("podcast*.c"))]:
            # Credentials are never included in the glyph manifest or converter command.
            texts.append("\n".join(line for line in path.read_text().splitlines()
                                   if not line.strip().startswith("#define WIFI_")))
        symbols = "".join(sorted({c for c in "".join(texts) if "\u3400" <= c <= "\u9fff"})) + PUNCTUATION
        (ROOT / "assets/fonts/ui-symbols.txt").write_text(symbols + "\n")
        convert("app_ui_16", 16, "0x20-0x7e", symbols)
    # Only corpus-backed missing characters, not the entire supplementary CJK range.
    if not args.ui_only:
        extra = TITLE_SYMBOLS.read_text().strip()
        convert("app_cjk_18", 18, "0x20-0x7e,0xa0-0xff,0x3400-0x4dbf,0x4e00-0x9fff", PUNCTUATION + extra)
        subprocess.run(["python3", str(ROOT / "tools/compress_podcast_title_font.py"),
                        "--input", "main/app_cjk_18.c", "--output", "main/app_cjk_18.c"],
                       cwd=ROOT, check=True)
    print("Fonts generated; rerun actual-widget coverage and bitmap checksum checks")


if __name__ == "__main__":
    main()
