#!/usr/bin/env python3
"""Specialise a binary add-on's branch to a single Kodi channel.

Applied once per branch after the split. Three edits:

  1. addon.xml.in version -> <kodi-major>.<existing minor>.<existing patch>
  2. workflow matrices -> only this channel's rows
  3. release.yml -> refuse a tag whose major is not this channel's

Usage: split_channel.py <repo-dir> <addon-id> <Omega|Piers>
"""
import re
import sys
from pathlib import Path

CHANNEL_MAJOR = {"Omega": "21", "Piers": "22"}


def bump_version(path, major):
    text = path.read_text(encoding="utf-8")
    m = re.search(r'^(\s*version=")([0-9][0-9.]*)(")', text, re.M)
    if not m:
        sys.exit(f"no version= line in {path}")
    old = m.group(2)
    parts = old.split(".")
    # Continue the existing minor/patch under the new major, per the decision:
    # 0.13.0 -> 21.13.0, so the changelog still lines up with the old numbering.
    minor_patch = parts[1:] if len(parts) > 1 else ["0", "0"]
    new = ".".join([major, *minor_patch])
    text = text[: m.start(2)] + new + text[m.end(2) :]
    path.write_text(text, encoding="utf-8")
    return old, new


def filter_matrix(path, channel):
    """Drop matrix rows belonging to the other channel.

    Refuses to leave a matrix empty. Running this twice over the same tree — which
    is exactly what happens if you forget to commit between the two branches —
    would otherwise strip every row and leave `include:` with nothing under it:
    still valid YAML, so nothing complains, and the workflow silently builds
    nothing at all.
    """
    other = "Piers" if channel == "Omega" else "Omega"
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    kept, dropped, remaining = [], 0, 0
    for line in lines:
        stripped = line.strip()
        is_row = stripped.startswith("- {") and "channel:" in stripped
        if is_row and f"channel: {other}" in stripped:
            dropped += 1
            continue
        if is_row:
            remaining += 1
        kept.append(line)
    if dropped and not remaining:
        sys.exit(
            f"{path}: dropping the {other} rows would leave no matrix rows at all. "
            f"This tree looks as though it has already been split — start from the "
            f"unsplit branch, and commit between the two channels."
        )
    path.write_text("".join(kept), encoding="utf-8")
    return dropped, remaining


def add_tag_guard(path, channel, major):
    text = path.read_text(encoding="utf-8")
    anchor = "      - name: Download all artifacts"
    if anchor not in text:
        sys.exit(f"could not find the anchor step in {path}")
    guard = f"""      - name: Assert the tag belongs to this channel
        if: startsWith(github.ref, 'refs/tags/v')
        shell: bash
        run: |
          set -euo pipefail
          # This branch builds {channel} only, so a v{major}.* tag is the only kind that
          # can legitimately be cut here. Without this, a tag pushed on the wrong
          # branch produces a complete, plausible release of the wrong Kodi version
          # — and because the channel is derived from the version major, it would be
          # filed into the wrong directory in the repository too.
          tag="${{GITHUB_REF_NAME#v}}"
          major="${{tag%%.*}}"
          echo "tag=$GITHUB_REF_NAME major=$major expected={major} ({channel})"
          if [[ "$major" != "{major}" ]]; then
            echo "::error::$GITHUB_REF_NAME is not a {channel} tag (expected v{major}.y.z)." \\
                 "Cut {channel} releases on this branch and the other channel's on its own." >&2
            exit 1
          fi

"""
    text = text.replace(anchor, guard + anchor, 1)
    path.write_text(text, encoding="utf-8")



def retarget_branch(path, channel):
    """ci.yml's push filter follows the branch this becomes."""
    text = path.read_text(encoding="utf-8")
    # pvr.kofin still says main (pre-rename); inputstream.tempo already lists both
    # channel branches. Either way it becomes this branch alone.
    for old in ("branches: [main]", "branches: [Piers, Omega]", "branches: [Omega, Piers]"):
        if old in text:
            path.write_text(text.replace(old, f"branches: [{channel}]"), encoding="utf-8")
            return True
    return False


def halve_expected_count(path):
    """The release job counted 12 zips: 6 platforms x 2 Kodi versions.

    After the split a release builds one channel, so the expected count halves.
    Left at 12 it would fail every release — which is at least loud, but the number
    has to move with the matrix or it is just wrong in the other direction later.
    """
    text = path.read_text(encoding="utf-8")
    before = text
    text = text.replace('"$count" -ne 12', '"$count" -ne 6')
    text = text.replace(
        "expected 12 zips (6 platforms x Kodi 21/22), found $count",
        "expected 6 zips (6 platforms, one Kodi version), found $count",
    )
    text = text.replace(
        "          # 6 platforms x 2 Kodi versions. A short count means",
        "          # 6 platforms, one Kodi version. A short count means",
    )
    if text == before:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def restrict_drift(path, channel):
    """drift.yml watches both channels; each branch only needs its own."""
    text = path.read_text(encoding="utf-8")
    pairs = {
        "Omega": '\n          for pair in "Omega:${kodi_omega_ref}:Omega"; do',
        "Piers": '\n          for pair in "Piers:${kodi_piers_ref}:master"; do',
    }
    old = '\n          for pair in "Omega:${kodi_omega_ref}:Omega" "Piers:${kodi_piers_ref}:master"; do'
    if old not in text:
        return False
    text = text.replace(old, pairs[channel])
    # And the tip-compile matrix rows, which key on `tip` not `channel`.
    drop = "master" if channel == "Omega" else "Omega"
    lines = text.splitlines(keepends=True)
    kept = [l for l in lines if not (l.strip().startswith("- {") and f"tip: {drop}" in l)]
    path.write_text("".join(kept), encoding="utf-8")
    return True


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    repo, addon_id, channel = Path(argv[0]), argv[1], argv[2]
    if channel not in CHANNEL_MAJOR:
        sys.exit(f"channel must be Omega or Piers, got {channel!r}")
    major = CHANNEL_MAJOR[channel]

    old, new = bump_version(repo / addon_id / "addon.xml.in", major)
    print(f"  version: {old} -> {new}")

    for name in ("ci.yml", "release.yml", "drift.yml"):
        path = repo / ".github/workflows" / name
        if path.exists():
            dropped, remaining = filter_matrix(path, channel)
            print(f"  {name}: dropped {dropped}, kept {remaining} row(s)")

    add_tag_guard(repo / ".github/workflows/release.yml", channel, major)
    print(f"  release.yml: added v{major}.* tag guard")
    if retarget_branch(repo / ".github/workflows/ci.yml", channel):
        print(f"  ci.yml: push filter -> [{channel}]")
    if halve_expected_count(repo / ".github/workflows/release.yml"):
        print("  release.yml: expected zip count 12 -> 6")
    drift = repo / ".github/workflows/drift.yml"
    if drift.exists() and restrict_drift(drift, channel):
        print(f"  drift.yml: watching {channel} only")


if __name__ == "__main__":
    main(sys.argv[1:])
