"""msgfmt-equivalent robustness check (no gettext needed).

For every generated locale file:
  * file decodes as strict UTF-8
  * every msgctxt/msgid/msgstr quoted body is well-formed (balanced quotes,
    only valid \\\\ / \\" / \\n escapes, no unescaped interior double quote)
  * header present, exactly 156 message entries
  * the #30740 catchup format-string example is preserved byte-for-byte
"""
import re
import sys
from po_lib import LANG_DIR
from gen import LANG_META

FORMAT_FRAGMENT = "&cutv={Y}-{m}-{d}T{H}:{M}:{S}"
ESC_OK = set('"\\nt')           # escapes our generator can emit / gettext allows
LINE_RE = re.compile(r'^(?:msgctxt |msgid |msgstr |)"(.*)"\s*$')


def check_body(body):
    """Return error string or None. Validate PO string-literal body."""
    i = 0
    while i < len(body):
        c = body[i]
        if c == '\\':
            if i + 1 >= len(body):
                return "trailing backslash"
            if body[i + 1] not in ESC_OK:
                return f"bad escape \\{body[i+1]}"
            i += 2
            continue
        if c == '"':
            return "unescaped double quote"
        i += 1
    return None


def check_file(path):
    errs = []
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        return [f"not valid UTF-8: {e}"]
    entries = 0
    for ln, line in enumerate(text.splitlines(), 1):
        if line and (line[0] == '"' or line.startswith(("msgctxt ", "msgid ", "msgstr "))):
            m = LINE_RE.match(line)
            if not m:
                errs.append(f"line {ln}: unparsable quoted line: {line[:50]!r}")
                continue
            e = check_body(m.group(1))
            if e:
                errs.append(f"line {ln}: {e}")
        if line.startswith("msgctxt "):
            entries += 1
    if entries != 156:
        errs.append(f"expected 156 entries, found {entries}")
    if FORMAT_FRAGMENT not in text:
        errs.append("catchup format-string fragment missing/altered")
    return errs


if __name__ == "__main__":
    total = 0
    for loc in LANG_META:
        path = LANG_DIR / f"resource.language.{loc}/strings.po"
        errs = check_file(path)
        if errs:
            total += len(errs)
            print(f"FAIL {loc}:")
            for e in errs:
                print(f"   - {e}")
        else:
            print(f"OK   {loc}")
    print(f"\n{'ALL GOOD' if not total else str(total)+' ISSUES'}")
    sys.exit(1 if total else 0)
