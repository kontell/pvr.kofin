"""PO parsing/escaping helpers for the pvr.kofin translation generator."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]  # <repo>/scripts/i18n/po_lib.py -> <repo>
LANG_DIR = REPO / "pvr.kofin/resources/language"
EN = LANG_DIR / "resource.language.en_gb/strings.po"

_quoted = re.compile(r'"((?:[^"\\]|\\.)*)"')


def po_unescape(s: str) -> str:
    return s.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')


def po_escape(s: str) -> str:
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


def parse_entries(path: Path = EN):
    """Parse a Kodi strings.po. Return list of {comment, ctx, msgid}.
    Skips the PO header block (the msgid "" entry with no msgctxt)."""
    lines = path.read_text(encoding="utf-8").splitlines()
    entries = []
    comment = None
    ctx = None
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.startswith("#"):
            comment = line          # remember last comment line; attaches to next entry
            i += 1
        elif line.startswith("msgctxt"):
            ctx = po_unescape(_quoted.search(line).group(1))
            i += 1
        elif line.startswith("msgid"):
            msgid = po_unescape(_quoted.search(line).group(1))
            i += 1
            while i < n and lines[i].startswith('"'):
                msgid += po_unescape(_quoted.search(lines[i]).group(1))
                i += 1
            if ctx is not None:     # header msgid "" has no msgctxt -> skipped
                entries.append({"comment": comment, "ctx": ctx, "msgid": msgid})
            comment = None
            ctx = None
        elif line.startswith("msgstr"):
            i += 1
            while i < n and lines[i].startswith('"'):
                i += 1
        else:                       # blank / stray line
            i += 1
    return entries


if __name__ == "__main__":
    es = parse_entries()
    print(f"entries: {len(es)}")
    print(f"with comments: {sum(1 for e in es if e['comment'])}")
    for e in es:
        if e["comment"]:
            print(f"  {e['comment']}  ->  {e['ctx']}")
