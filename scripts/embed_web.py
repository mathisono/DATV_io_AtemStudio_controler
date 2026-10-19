"""Embed local web assets as reproducible gzip in flash; no filesystem upload."""
from pathlib import Path
import gzip

Import("env")  # noqa: F821 - PlatformIO/SCons supplies env.

project = Path(env.subst("$PROJECT_DIR"))
generated = Path(env.subst("$BUILD_DIR")) / "generated"
generated.mkdir(parents=True, exist_ok=True)
lines = ["#pragma once", "#include <Arduino.h>"]
for filename, symbol in [("index.html", "INDEX_HTML_GZ"), ("app.css", "APP_CSS_GZ"), ("app.js", "APP_JS_GZ")]:
    data = gzip.compress((project / "web" / filename).read_bytes(), compresslevel=9, mtime=0)
    lines.append(f"static const uint8_t {symbol}[] PROGMEM = {{")
    for start in range(0, len(data), 20):
        lines.append(",".join(str(byte) for byte in data[start:start + 20]) + ",")
    lines.append("};")
    lines.append(f"static constexpr size_t {symbol}_LEN = {len(data)};")
target = generated / "web_assets.h"
contents = "\n".join(lines) + "\n"
if not target.exists() or target.read_text() != contents:
    target.write_text(contents)
env.Append(CPPPATH=[str(generated)])
