#!/bin/sh
# format-xml.sh - In-place format GNOME-style XML / GtkBuilder files.
#
# Usage: format-xml.sh DIR [DIR...]
#
# Style: 2-space indentation, UTF-8, one element per line. Matches the
# convention used across GNOME core projects (gnome-text-editor,
# gnome-software, ...). xmllint reformats the tree while preserving
# attributes verbatim, so libadwaita / custom-widget classes round-trip
# without losing properties — unlike gtk4-builder-tool simplify, which
# strips unknown properties.
#
# Files touched: *.xml and *.ui under each given DIR. Idempotent: if a
# file is already formatted the script leaves it untouched.
set -eu

if ! command -v xmllint >/dev/null 2>&1; then
    echo "format-xml.sh: xmllint not found (install libxml2-devel); skipping XML" >&2
    exit 0
fi

if [ $# -eq 0 ]; then
    echo "format-xml.sh: usage: $0 DIR [DIR...]" >&2
    exit 2
fi

# Two-space indent. xmllint reads this from the environment.
XMLLINT_INDENT="  "
export XMLLINT_INDENT

find "$@" \( -name '*.xml' -o -name '*.ui' \) -exec sh -c '
    for f do
        tmp="$f.fmt-tmp"
        if ! xmllint --format --output "$tmp" "$f"; then
            echo "format-xml.sh: failed to format $f" >&2
            rm -f "$tmp"
            exit 1
        fi
        if ! cmp -s "$f" "$tmp"; then
            mv "$tmp" "$f"
            echo "  formatted $f"
        else
            rm -f "$tmp"
        fi
    done
' _ {} +
