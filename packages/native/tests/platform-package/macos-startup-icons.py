"""Verify the relocated application loaded its SVG decoder and drew its startup icons."""
from pathlib import Path
import re
import sys

log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
failures = [line for line in log.splitlines()
            if "qrc:/workspace/" in line and ".svg" in line and "Error decoding:" in line]
if failures:
    raise SystemExit(f"{len(failures)} embedded SVG draws failed. First failure: {failures[0]}")
if not re.search(r'workspace-native\.app/Contents/PlugIns/imageformats/libqsvg\.dylib" loaded library', log):
    raise SystemExit("The packaged application did not load its own SVG image reader.")
print("The packaged application loaded its own SVG reader and decoded its embedded startup icons.")
