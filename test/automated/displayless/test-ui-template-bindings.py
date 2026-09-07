#!/usr/bin/env python3
"""Check template bindings against resources in the executable we actually ship."""
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

binary, source = sys.argv[1:]
checked = 0
for path in sorted(Path(source).glob("*.c")):
    text = path.read_text()
    match = re.search(r'gtk_widget_class_set_template_from_resource\s*\(\s*widget_class,\s*"([^"]+)"', text)
    if not match:
        continue
    bindings = re.findall(r'gtk_widget_class_bind_template_child(?:_private|_internal|_internal_private)?\s*\(\s*widget_class,\s*\w+,\s*(\w+)\s*\)', text)
    if not bindings:
        continue
    template = subprocess.check_output(["gresource", "extract", binary, match[1]])
    objects = {node.get("id") for node in ET.fromstring(template).iter()}
    missing = sorted(set(bindings) - objects)
    if missing:
        raise SystemExit(f"{path.name}: embedded {match[1]} is missing {', '.join(missing)}")
    checked += len(bindings)
if not checked:
    raise SystemExit("No template bindings checked")
print(f"Verified {checked} template bindings in {binary}")
