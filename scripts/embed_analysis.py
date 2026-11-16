Import("env")
from pathlib import Path
import gzip

root = Path(env["PROJECT_DIR"])
src = root / "web" / "analysis.html"
out = root / "include" / "analysis_page.h"
raw = src.read_bytes()
# Store compressed bytes in flash, not heap.
compressed = gzip.compress(raw, compresslevel=9, mtime=0)
lines = []
for i in range(0, len(compressed), 16):
    lines.append("  " + ", ".join(f"0x{b:02x}" for b in compressed[i:i+16]) + ",")
out.write_text(
    "#pragma once\n#include <Arduino.h>\n"
    "static const uint8_t ANALYSIS_HTML_GZ[] PROGMEM = {\n" +
    "\n".join(lines) +
    "\n};\nstatic const size_t ANALYSIS_HTML_GZ_SIZE = sizeof(ANALYSIS_HTML_GZ);\n",
    encoding="utf-8",
)
print(f"Embedded VocalLab analysis: {len(raw)} -> {len(compressed)} bytes")
