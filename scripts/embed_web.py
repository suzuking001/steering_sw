"""Embed offline web assets as deterministic gzip streams in the build directory."""
Import("env")
from pathlib import Path
import gzip
import re

root = Path(env.subst("$PROJECT_DIR"))
version = (root / "VERSION").read_text().strip()
if not re.fullmatch(r"(?:0|[1-9][0-9]{0,3})(?:\.(?:0|[1-9][0-9]{0,3})){2}", version):
    raise ValueError("VERSION must contain major.minor.patch, with up to four digits per component")
env.Append(CPPDEFINES=[("SW_FIRMWARE_VERSION", env.StringifyMacro(version))])
out_dir = Path(env.subst("$BUILD_DIR")) / "generated"
out_dir.mkdir(parents=True, exist_ok=True)
parts = ["#pragma once\n#include <Arduino.h>\n"]
for filename, symbol in [("index.html", "web_index"), ("app.js", "web_js"), ("style.css", "web_css")]:
    data = gzip.compress((root / "web" / filename).read_bytes(), mtime=0)
    parts.append("static const uint8_t %s[] PROGMEM = {%s};\n" % (symbol, ",".join(map(str, data))))
header = out_dir / "web_assets.h"
content = "".join(parts)
if not header.exists() or header.read_text() != content:
    header.write_text(content)
env.Append(CPPPATH=[str(out_dir)])
