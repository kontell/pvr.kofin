"""Generate Kodi strings.po files for pvr.kofin from authored translations.

Reads:  tr/<locale>.json   (ctx -> translated msgstr)
Writes: <repo>/pvr.kofin/resources/language/resource.language.<locale>/strings.po
Verbatim tokens (PASSTHROUGH) get msgstr == msgid automatically.
Refuses to write a file unless every translatable entry is present & non-empty.
"""
import json
import sys
from pathlib import Path
from po_lib import parse_entries, po_escape, LANG_DIR

from classify import PASSTHROUGH

HERE = Path(__file__).parent
TR = HERE / "tr"

# locale -> (Language header value, Language-Team name, Plural-Forms)
LANG_META = {
    "fr_fr": ("fr_FR", "French", "nplurals=2; plural=(n > 1);"),
    "fr_ca": ("fr_CA", "French (Canada)", "nplurals=2; plural=(n > 1);"),
    "de_de": ("de_DE", "German", "nplurals=2; plural=(n != 1);"),
    "es_es": ("es_ES", "Spanish", "nplurals=2; plural=(n != 1);"),
    "es_mx": ("es_MX", "Spanish (Mexico)", "nplurals=2; plural=(n != 1);"),
    "it_it": ("it_IT", "Italian", "nplurals=2; plural=(n != 1);"),
    "pt_pt": ("pt_PT", "Portuguese", "nplurals=2; plural=(n != 1);"),
    "pt_br": ("pt_BR", "Portuguese (Brazil)", "nplurals=2; plural=(n > 1);"),
    "nl_nl": ("nl_NL", "Dutch", "nplurals=2; plural=(n != 1);"),
    "pl_pl": ("pl_PL", "Polish",
              "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"),
    "ru_ru": ("ru_RU", "Russian",
              "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"),
    "sv_se": ("sv_SE", "Swedish", "nplurals=2; plural=(n != 1);"),
    "uk_ua": ("uk_UA", "Ukrainian",
              "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"),
    "cs_cz": ("cs_CZ", "Czech", "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;"),
    "sk_sk": ("sk_SK", "Slovak", "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;"),
    "da_dk": ("da_DK", "Danish", "nplurals=2; plural=(n != 1);"),
    "nb_no": ("nb_NO", "Norwegian Bokmål", "nplurals=2; plural=(n != 1);"),
    "fi_fi": ("fi_FI", "Finnish", "nplurals=2; plural=(n != 1);"),
    "el_gr": ("el_GR", "Greek", "nplurals=2; plural=(n != 1);"),
    "ro_ro": ("ro_RO", "Romanian",
              "nplurals=3; plural=(n==1 ? 0 : (n==0 || (n%100>0 && n%100<20)) ? 1 : 2);"),
    "hu_hu": ("hu_HU", "Hungarian", "nplurals=2; plural=(n != 1);"),
    "ca_es": ("ca_ES", "Catalan", "nplurals=2; plural=(n != 1);"),
    "ja_jp": ("ja_JP", "Japanese", "nplurals=1; plural=0;"),
    "zh_cn": ("zh_CN", "Chinese (Simplified)", "nplurals=1; plural=0;"),
    "zh_tw": ("zh_TW", "Chinese (Traditional)", "nplurals=1; plural=0;"),
    "ko_kr": ("ko_KR", "Korean", "nplurals=1; plural=0;"),
}

HEADER_COMMENTS = (
    "# Kodi Media Center language file\n"
    "# Addon Name: Kofin PVR for Jellyfin\n"
    "# Addon id: pvr.kofin\n"
    "# Addon Provider: Kontell\n"
    "# Note: machine-translated (LLM), pending native review.\n"
)


def header(locale):
    po_lang, team, plural = LANG_META[locale]
    return (
        HEADER_COMMENTS
        + 'msgid ""\n'
        'msgstr ""\n'
        '"Project-Id-Version: pvr.kofin\\n"\n'
        '"Report-Msgid-Bugs-To: \\n"\n'
        '"PO-Revision-Date: 2026-06-26 00:00+0000\\n"\n'
        '"Last-Translator: Claude (machine translation) <noreply@anthropic.com>\\n"\n'
        f'"Language-Team: {team}\\n"\n'
        '"MIME-Version: 1.0\\n"\n'
        '"Content-Type: text/plain; charset=UTF-8\\n"\n'
        '"Content-Transfer-Encoding: 8bit\\n"\n'
        f'"Language: {po_lang}\\n"\n'
        f'"Plural-Forms: {plural}\\n"\n'
    )


def load_tr(locale):
    p = TR / f"{locale}.json"
    if not p.exists():
        raise SystemExit(f"missing translation file: {p}")
    return json.loads(p.read_text(encoding="utf-8"))


def generate(locale, entries):
    tr = load_tr(locale)
    missing, empty = [], []
    for e in entries:
        if e["ctx"] in PASSTHROUGH:
            continue
        if e["ctx"] not in tr:
            missing.append(e["ctx"])
        elif not str(tr[e["ctx"]]).strip():
            empty.append(e["ctx"])
    extra = [k for k in tr if k not in {e["ctx"] for e in entries}]
    if missing or empty or extra:
        raise SystemExit(
            f"{locale}: missing={missing} empty={empty} unknown_keys={extra}")

    blocks = []
    for e in entries:
        block = []
        if e["comment"]:
            block.append(e["comment"])
        block.append(f'msgctxt "{po_escape(e["ctx"])}"')
        block.append(f'msgid "{po_escape(e["msgid"])}"')
        msgstr = e["msgid"] if e["ctx"] in PASSTHROUGH else tr[e["ctx"]]
        block.append(f'msgstr "{po_escape(msgstr)}"')
        blocks.append("\n".join(block))
    text = header(locale) + "\n" + "\n\n".join(blocks) + "\n"
    out = LANG_DIR / f"resource.language.{locale}/strings.po"
    out.write_text(text, encoding="utf-8")
    return out, len(entries)


def main(argv):
    entries = parse_entries()
    locales = argv or list(LANG_META)
    for loc in locales:
        out, n = generate(loc, entries)
        print(f"wrote {out}  ({n} entries)")


if __name__ == "__main__":
    main(sys.argv[1:])
