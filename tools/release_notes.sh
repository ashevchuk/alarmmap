#!/bin/sh
# Prints the CHANGELOG.md section of a version without its heading.
# Usage: tools/release_notes.sh VERSION
# Fails when the section is missing or empty, so a release never goes out
# with blank notes.
set -eu

version="$1"
changelog="$(dirname "$0")/../CHANGELOG.md"

notes="$(awk -v header="## [$version]" '
    index($0, header) == 1 { found = 1; next }
    found && /^## \[/ { exit }
    found && /^\[[^]]+\]: / { exit }
    found && /^[[:space:]]*$/ { if (started) blank = blank "\n"; next }
    found { printf "%s%s\n", blank, $0; blank = ""; started = 1 }
' "$changelog")"

if [ -z "$notes" ]; then
    echo "no changelog section for version $version" >&2
    exit 1
fi
printf '%s\n' "$notes"
