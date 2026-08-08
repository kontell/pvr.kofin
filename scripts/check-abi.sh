#!/bin/bash
# Assert a built Linux addon will actually load on the oldest target we support.
#
# Usage: scripts/check-abi.sh <artifacts-dir> [max-glibc]
#
# Two things are checked, and both are assertions rather than printouts. The
# previous version of this logic printed the maximum required glibc symbol
# version, which is only ever read by someone who is already suspicious — the
# whole point is to catch the floor rising when nobody is looking.
#
#  1. libstdc++ and libgcc_s must NOT be dynamic dependencies. Kodi targets ship
#     wildly different libstdc++ versions and a GLIBCXX_3.4.x the box does not
#     have is an unloadable addon. The release build links them statically; this
#     confirms the flags actually took effect, because the Kodi addon superbuild
#     forwards compiler flags but not linker flags and it is easy to lose them.
#
#  2. No required glibc symbol may exceed MAX_GLIBC (default 2.38, the CoreELEC
#     21.x floor). This is the one that catches a build-host change: the floor is
#     set by the host's headers, not by this source. Measured on glibc 2.42, a
#     .so using nothing but std::vector/std::string/make_shared still pulls
#     __isoc23_strtoul@GLIBC_2.38 (a C23 symbol the headers substitute in),
#     arc4random@GLIBC_2.36, and _dl_find_object@GLIBC_2.35 from the static
#     unwinder. Nothing in the addon asks for any of them.
#
# Android is skipped: bionic, no glibc, no versioned symbols. Windows never gets
# here. readelf reads any architecture, so the host tool handles the armv7 and
# aarch64 cross builds too.
set -euo pipefail

ARTIFACTS="${1:-}"
MAX_GLIBC="${2:-2.38}"

if [[ -z "$ARTIFACTS" || ! -d "$ARTIFACTS" ]]; then
    echo "usage: $0 <artifacts-dir> [max-glibc]" >&2
    exit 2
fi

shopt -s nullglob
zips=("$ARTIFACTS"/*.zip)
if [[ ${#zips[@]} -eq 0 ]]; then
    echo "no zip in $ARTIFACTS — nothing to check, which is itself wrong" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

status=0
for zip in "${zips[@]}"; do
    name="$(basename "$zip")"
    case "$name" in
        *android*|*windows*)
            echo "== $name: skipped (no glibc)"
            continue
            ;;
    esac

    dest="$tmp/$(basename "$zip" .zip)"
    mkdir -p "$dest"
    unzip -q "$zip" -d "$dest"

    # zip -r without -y dereferences symlinks, so every match is a real ELF.
    mapfile -t sos < <(find "$dest" -type f -name '*.so*' | sort)
    if [[ ${#sos[@]} -eq 0 ]]; then
        echo "== $name: FAIL — no .so inside" >&2
        status=1
        continue
    fi

    for so in "${sos[@]}"; do
        rel="${so#"$dest"/}"
        echo "== $name :: $rel"
        needed="$(readelf -d "$so" | grep NEEDED || true)"
        echo "$needed" | sed 's/^/     /'

        if grep -qE 'libstdc\+\+\.so|libgcc_s\.so' <<<"$needed"; then
            echo "   FAIL: libstdc++/libgcc_s is a dynamic dependency — the static" >&2
            echo "         C++ runtime flags did not take effect (LDFLAGS lost?)." >&2
            status=1
        fi

        # Highest GLIBC_x.y any undefined symbol requires.
        highest="$(readelf --dyn-syms -W "$so" \
            | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' \
            | sed 's/^GLIBC_//' | sort -uV | tail -1)"
        if [[ -z "$highest" ]]; then
            echo "     no versioned glibc symbols required"
            continue
        fi
        echo "     max required glibc: $highest  (ceiling $MAX_GLIBC)"

        # sort -V puts the larger last; if the ceiling is not last, we exceeded it.
        if [[ "$(printf '%s\n%s\n' "$highest" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]]; then
            echo "   FAIL: requires glibc $highest, above the $MAX_GLIBC ceiling." >&2
            echo "         Offending symbols:" >&2
            readelf --dyn-syms -W "$so" \
                | grep -oE '[A-Za-z_][A-Za-z0-9_]*@GLIBC_[0-9.]+' \
                | sort -u -t@ -k2 -V | tail -8 | sed 's/^/           /' >&2
            echo "         This is normally the build host, not this code: a newer" >&2
            echo "         glibc's headers substitute newer symbols (__isoc23_* and" >&2
            echo "         friends). Check the container image pin in the workflow." >&2
            status=1
        fi
    done
done

if [[ $status -eq 0 ]]; then
    echo
    echo "OK: C++ runtime static, and no glibc symbol above $MAX_GLIBC."
fi
exit $status
