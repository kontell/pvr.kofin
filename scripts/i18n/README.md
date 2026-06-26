# Translation toolchain (`scripts/i18n`)

Regenerates the Kodi `.po` translation files for pvr.kofin from authored
translation data, keyed off the English source.

**The translations here are machine-generated (LLM) and tagged
`pending native review` in each file header.** They are a starting point, not a
substitute for a native speaker's pass — especially for the longer help texts.

## Source of truth

`pvr.kofin/resources/language/resource.language.en_gb/strings.po` is the English
source. In every other locale file the `msgid` stays English (it is the lookup
key) and only `msgstr` is filled in. Kodi auto-discovers `resource.language.*`
directories and falls back to English for any string a locale doesn't translate,
so partial or absent translations never regress.

## Files

| File | Purpose |
|------|---------|
| `po_lib.py` | PO parser + escaping helpers; resolves repo paths relative to this dir. |
| `classify.py` | `PASSTHROUGH` — the allowlist of msgctxt IDs copied **verbatim** (codec / bitrate / resolution / HDR names, `Opus`, `FFmpeg Direct`, `Adaptive`). Translating these is a defect. |
| `gen.py` | `LANG_META` (per-locale header: `Language`, team name, `Plural-Forms`) + the generator. Reads `tr/<locale>.json`, writes the `.po`. Refuses to write unless every translatable entry is present and non-empty. |
| `validate.py` | Post-write structural check: msgctxt set/order matches source, every `msgid` byte-identical to source, no empty/untranslated strings, passthrough `msgstr == msgid`. |
| `pocheck.py` | `msgfmt`-equivalent robustness check (strict UTF-8, well-formed PO escapes/quotes, entry count, catchup format-string preserved) for when `gettext` isn't installed. |
| `tr/<locale>.json` | `{ msgctxt: translation }` for each locale. The authored content. |

## Usage

Run from the repo root (sibling imports resolve via the script's own directory):

```bash
python3 scripts/i18n/gen.py                # regenerate all locales in LANG_META
python3 scripts/i18n/gen.py fr_fr de_de    # regenerate specific locales
python3 scripts/i18n/validate.py           # structural validation (all locales)
python3 scripts/i18n/pocheck.py            # PO well-formedness (all locales)
# Optional, if gettext is installed — the authoritative syntax check:
#   for f in pvr.kofin/resources/language/resource.language.*/strings.po; do
#     msgfmt -c -o /dev/null "$f"; done
```

Output is deterministic (the `PO-Revision-Date` is fixed), so regenerating with
no data changes produces no diff.

## Updating after an `en_gb` change

1. Edit the English source as usual.
2. If you added/removed an entry, update each `tr/<locale>.json` (the generator
   errors and lists any missing or unknown keys, so it can't silently drift).
   If the entry is a technical token, add its ID to `PASSTHROUGH` in
   `classify.py` instead.
3. Regenerate and validate with the commands above.

## Adding a new locale

1. Confirm `pvr.kofin/resources/language/resource.language.<locale>/` exists.
2. Add an entry to `LANG_META` in `gen.py`.
3. Create `tr/<locale>.json` with a translation for every non-passthrough entry.
4. `python3 scripts/i18n/gen.py <locale> && python3 scripts/i18n/validate.py <locale>`.
