"""Validate generated strings.po files against the en_gb source (post-write).

For each locale: parse the written file and assert
  * exactly the same ordered msgctxt keys as en_gb
  * every msgid byte-identical to en_gb (translations must not alter the key)
  * every non-passthrough msgstr non-empty
  * every passthrough msgstr == msgid (verbatim token)
  * no literal English left in a translatable msgstr (msgstr != msgid), with a
    small allowlist of strings that are legitimately identical (proper nouns).
"""
import sys
from po_lib import parse_entries, LANG_DIR, EN
from classify import PASSTHROUGH
from gen import LANG_META

# translatable ctxs whose translation may legitimately equal the English source
# in some languages (proper nouns / shared spellings). Not an error if identical.
OK_IDENTICAL = {
    "#30660",  # InputStream (component name)
    "#30707",  # Quick Connect (Jellyfin feature)
    "#30793", "#30794", "#30795",  # Stereo / Surround 5.1 / 7.1
    "#30620",  # Transcoding (some langs keep the English term)
    "#30730",  # Catchup (kept as the IPTV term in many languages)
    "#30700",  # Account (standard loanword in it/nl/de-adjacent UIs)
    "#30761",  # Default (kept in some languages)
}


def validate(locale, en_entries):
    path = LANG_DIR / f"resource.language.{locale}/strings.po"
    got = parse_entries(path)
    errs = []
    if [e["ctx"] for e in got] != [e["ctx"] for e in en_entries]:
        errs.append("msgctxt sequence differs from en_gb")
        return errs
    en_by = {e["ctx"]: e["msgid"] for e in en_entries}
    # need msgstr too: re-read with a tiny msgstr parser
    msgstrs = _read_msgstrs(path)
    for e in got:
        ctx = e["ctx"]
        if e["msgid"] != en_by[ctx]:
            errs.append(f"{ctx}: msgid altered")
        ms = msgstrs.get(ctx, "")
        if ctx in PASSTHROUGH:
            if ms != en_by[ctx]:
                errs.append(f"{ctx}: passthrough msgstr != msgid ({ms!r})")
        else:
            if not ms.strip():
                errs.append(f"{ctx}: empty translation")
            elif ms == en_by[ctx] and ctx not in OK_IDENTICAL:
                errs.append(f"{ctx}: untranslated (== English): {ms!r}")
    return errs


def _read_msgstrs(path):
    import re
    q = re.compile(r'"((?:[^"\\]|\\.)*)"')
    lines = path.read_text(encoding="utf-8").splitlines()
    out = {}
    ctx = None
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.startswith("msgctxt"):
            ctx = q.search(line).group(1).replace('\\"', '"')
        elif line.startswith("msgstr") and ctx is not None:
            val = q.search(line).group(1)
            i += 1
            while i < n and lines[i].startswith('"'):
                val += q.search(lines[i]).group(1)
                i += 1
            out[ctx] = val.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')
            ctx = None
            continue
        i += 1
    return out


if __name__ == "__main__":
    en = parse_entries(EN)
    locales = sys.argv[1:] or list(LANG_META)
    total_err = 0
    for loc in locales:
        path = LANG_DIR / f"resource.language.{loc}/strings.po"
        if not path.exists():
            print(f"SKIP {loc}: not generated yet")
            continue
        errs = validate(loc, en)
        if errs:
            total_err += len(errs)
            print(f"FAIL {loc}: {len(errs)} issue(s)")
            for e in errs[:20]:
                print(f"   - {e}")
        else:
            print(f"OK   {loc}")
    print(f"\n{'ALL GOOD' if total_err == 0 else str(total_err)+' ISSUES'}")
    sys.exit(1 if total_err else 0)
