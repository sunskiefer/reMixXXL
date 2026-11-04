#!/usr/bin/env python3
"""Builds the UI editor page: tools/ui_editor_template.html + build/Layout/editor_data.json -> build/Layout/ui_editor.html
    python3 tools/dump_editor.py && python3 tools/make_editor.py"""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
data = open(os.path.join(ROOT, "build", "Layout", "editor_data.json")).read().replace("</", "<\\/")
tpl = open(os.path.join(ROOT, "tools", "ui_editor_template.html")).read()
assert "/*__DATA__*/null" in tpl
out = os.path.join(ROOT, "build", "Layout", "ui_editor.html")
open(out, "w").write(tpl.replace("/*__DATA__*/null", data))
print("make_editor: %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))
