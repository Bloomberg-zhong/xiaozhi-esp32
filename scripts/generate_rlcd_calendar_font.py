"""Generate the board's offline calendar CJK font from OFL Noto Sans CJK SC.

Requires Node and lv_font_conv 1.5.3. Runs only when updating UI strings;
the committed font source is used directly by ordinary firmware builds.
"""
import argparse
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-rlcd-4.2"

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=pathlib.Path)
    parser.add_argument("--converter", required=True, type=pathlib.Path)
    parser.add_argument("--license", required=True, type=pathlib.Path)
    args = parser.parse_args()
    text = (BOARD / "dashboard_model.cc").read_text() + (BOARD / "dashboard_ui.cc").read_text()
    characters = "".join(sorted({char for char in text if '\u4e00' <= char <= '\u9fff'})) + "：·"
    subprocess.run([
        "node", str(args.converter), "--font", str(args.font), "--size", "14", "--bpp", "1",
        "--format", "lvgl", "--no-compress", "--no-kerning", "--range", "0x20-0x7e",
        "--symbols", characters, "--lv-font-name", "font_rlcd_calendar_14",
        "-o", str(BOARD / "dashboard_calendar_font.c"),
    ], check=True)
    copyright_notice = subprocess.check_output([
        "node", "-e",
        'const load = require("module").createRequire(process.argv[1]); '
        'const font = load("opentype.js").loadSync(process.argv[2]); '
        'process.stdout.write(font.names.copyright.en || "");',
        str(args.converter.resolve()), str(args.font.resolve()),
    ], text=True)
    (BOARD / "dashboard_calendar_font.LICENSE").write_text(
        copyright_notice + "\nSource: https://github.com/notofonts/noto-cjk\n\n" + args.license.read_text())
