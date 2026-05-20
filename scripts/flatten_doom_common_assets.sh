#!/usr/bin/env bash
set -euo pipefail

root="/run/media/alberto/POCKETDEV/Assets/doom/common"
apply=0
declare -A explicit_moves=()

usage() {
    cat <<EOF
Usage: $0 [--apply] [COMMON_DIR]

Flatten Doom common assets so instance files can load WAD/DEH assets directly
from the common folder.

Default COMMON_DIR:
  /run/media/alberto/POCKETDEV/Assets/doom/common

Without --apply this only prints the planned moves.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply)
            apply=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            root="$1"
            shift
            ;;
    esac
done

if [[ ! -d "$root" ]]; then
    echo "Missing common directory: $root" >&2
    exit 1
fi

run() {
    if [[ "$apply" -eq 1 ]]; then
        "$@"
    else
        printf '  '
        printf '%q ' "$@"
        printf '\n'
    fi
}

move_one() {
    local rel_src="$1"
    local dest_name="${2:-$(basename "$rel_src")}"
    local src="$root/$rel_src"
    local dst="$root/$dest_name"

    [[ -e "$src" ]] || return 0

    if [[ "$src" == "$dst" ]]; then
        return 0
    fi

    if [[ -e "$dst" ]]; then
        if cmp -s "$src" "$dst"; then
            echo "duplicate: $rel_src -> $dest_name"
            run rm -f "$src"
            return 0
        fi

        echo "conflict: $rel_src would overwrite existing $dest_name" >&2
        exit 1
    fi

    echo "move: $rel_src -> $dest_name"
    run mv "$src" "$dst"
}

move_explicit() {
    explicit_moves["$1"]=1
    move_one "$1" "$2"
}

# Canonical names used by the checked-in instance files.  These run first so
# case-only names such as PLUTONIA.wad become exactly PLUTONIA.WAD.
move_explicit "doom/DOOM.WAD" "DOOM.WAD"
move_explicit "doom2/DOOM2.WAD" "DOOM2.WAD"
move_explicit "doomu/doomu.wad" "doomu.wad"
move_explicit "plutonia/PLUTONIA.wad" "PLUTONIA.WAD"
move_explicit "tnt/TNT.WAD" "TNT.WAD"
move_explicit "tnt/TNT31.WAD" "TNT31.WAD"
move_explicit "sigil/SIGIL_COMPAT_V1_23.wad" "SIGIL_COMPAT_V1_23.wad"
move_explicit "sigil2/SIGIL_II_V1_0.WAD" "SIGIL_II_V1_0.WAD"
move_explicit "earth/EARTH.WAD" "EARTH.WAD"
move_explicit "revolution/TVR!.WAD" "TVR!.WAD"
move_explicit "hellbound/HBOUND.WAD" "HBOUND.WAD"
move_explicit "phobos64/PHOBOS63.WAD" "PHOBOS63.WAD"
move_explicit "rekkr/REKKR.WAD" "REKKR.WAD"
move_explicit "rekkr/REKKR.DEH" "REKKR.DEH"
move_explicit "batman/BATMAN.WAD" "BATMAN.WAD"
move_explicit "batman/BATMAN.DEH" "BATMAN.DEH"
move_explicit "aldente/ALDENTE.wad" "ALDENTE.wad"
move_explicit "aldente/ALDENTE.deh" "ALDENTE.deh"
move_explicit "blackmagwell/BLACK_MAGWELL.wad" "BLACK_MAGWELL.wad"
move_explicit "gorenuggets/dehacked_gibs.wad" "dehacked_gibs.wad"
move_explicit "ohm/OHM_Dehacked.wad" "OHM_Dehacked.wad"
move_explicit "d4v/D4V.WAD" "D4V.WAD"
move_explicit "d4v/D4V.DEH" "D4V.DEH"

# Flatten any remaining subfolder files.  This removes the legacy layout
# without needing to maintain an allowlist for optional mod files/readmes.
while IFS= read -r -d '' path; do
    rel="${path#"$root"/}"
    [[ -z "${explicit_moves[$rel]:-}" ]] || continue
    move_one "$rel" "$(basename "$path")"
done < <(find "$root" -mindepth 2 -type f -print0 | sort -z)

echo "remove empty subdirectories"
if [[ "$apply" -eq 1 ]]; then
    find "$root" -mindepth 1 -type d -empty -delete
else
    find "$root" -mindepth 1 -type d -empty -print
fi

if [[ "$apply" -eq 0 ]]; then
    echo
    echo "Dry run only. Re-run with --apply to move files."
fi
