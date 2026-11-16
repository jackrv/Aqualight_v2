#!/usr/bin/env python3
"""Stamp web/index.html with a hash of the editor scripts.

The scripts are loaded as "app.js?v=<stamp>", so a changed file gets a new
address and is never taken from the browser cache. Run after editing web/*.js;
tests/web_test.js fails when the stamp is out of date.
"""
import hashlib
from pathlib import Path
import re
import sys

WEB = Path(__file__).resolve().parents[1] / "web"
SCRIPTS = ("schedule.js", "presets.js", "app.js")


def stamp():
    digest = hashlib.sha256()
    for name in SCRIPTS:
        digest.update((WEB / name).read_bytes())
    return digest.hexdigest()[:8]


def main():
    page = WEB / "index.html"
    text = page.read_text(encoding="utf-8")
    value = stamp()
    for name in SCRIPTS:
        text, count = re.subn(r'src="%s(\?v=[0-9a-f]*)?"' % re.escape(name), 'src="%s?v=%s"' % (name, value), text)
        if count != 1:
            print("web/index.html: ожидался один тег для " + name, file=sys.stderr)
            return 1
    page.write_text(text, encoding="utf-8")
    print("web/index.html: v=" + value)
    return 0


if __name__ == "__main__":
    sys.exit(main())
