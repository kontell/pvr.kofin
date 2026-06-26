"""Define the verbatim-passthrough allowlist and dump the translatable checklist."""
import sys
from po_lib import parse_entries, EN

# msgctxt IDs whose msgstr must equal msgid verbatim (technical tokens / proper nouns)
PASSTHROUGH = {
    # video codecs
    "#30626", "#30627", "#30628",
    # audio codecs
    "#30631", "#30632", "#30633",
    # bitrates 1..120 Mbps
    *(f"#{n}" for n in range(30636, 30651)),
    # inputstream branded names
    "#30663", "#30664",
    # Opus (codec)
    "#30715",
    # more codec tokens
    "#30796", "#30797", "#30798", "#30799", "#30800", "#30801", "#30805",
    # codec-list technical labels
    "#30802", "#30803",
    # HDR brand tokens (no descriptor words)
    "#30812", "#30813", "#30814", "#30815", "#30819", "#30820", "#30821",
    # resolutions
    "#30824", "#30825", "#30826", "#30827", "#30828",
}

if __name__ == "__main__":
    entries = parse_entries(EN)
    trans = [e for e in entries if e["ctx"] not in PASSTHROUGH]
    pas = [e for e in entries if e["ctx"] in PASSTHROUGH]
    print(f"# translatable: {len(trans)}   passthrough: {len(pas)}   total: {len(entries)}",
          file=sys.stderr)
    # verify every PASSTHROUGH id actually exists
    ids = {e["ctx"] for e in entries}
    missing = [c for c in PASSTHROUGH if c not in ids]
    if missing:
        print("!! PASSTHROUGH ids not found in source:", missing, file=sys.stderr)
    for e in trans:
        print(f'{e["ctx"]}\t{e["msgid"]}')
