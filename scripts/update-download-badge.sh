#!/bin/bash
# update-download-badge.sh — keep the README download badge accurate
# across release pruning.
#
# GitHub's /downloads/total counts only *existing* releases: deleting an
# old release takes its download count with it, so the badge drops.
# This script snapshots per-release counts on every run (downloads-cache.json)
# and moves the counts of deleted releases into a permanent baseline, so
# the badge total only ever grows.
#
# Run by .github/workflows/download-badge.yml (daily + on tag push), and
# usable locally: CURRENT_JSON=/path/to/releases.json ./update-download-badge.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CURRENT="${CURRENT_JSON:-/tmp/skey-releases-current.json}"

if [ -z "${CURRENT_JSON:-}" ]; then
    gh api "repos/collyn/skey/releases?per_page=100" --paginate --jq '
        [.[] | {key: .tag_name, value: ([.assets[].download_count] | add // 0)}]
        | from_entries' > "$CURRENT"
fi

if [ -f downloads-cache.json ]; then
    deleted=$(jq -s '
        .[0] as $prev | .[1] as $cur
        | [ $prev.releases | keys[] | select($cur[.] == null) ]
        | map($prev.releases[.]) | add // 0' downloads-cache.json "$CURRENT")
    baseline=$(jq '.baseline // 0' downloads-cache.json)
    baseline=$((baseline + deleted))
else
    baseline=0
fi

jq -n --argjson baseline "$baseline" --slurpfile cur "$CURRENT" \
    '{baseline: $baseline, releases: $cur[0]}' > downloads-cache.json

total=$((baseline + $(jq '[.[] | values] | add // 0' "$CURRENT")))
jq -n --argjson total "$total" \
    '{schemaVersion: 1, label: "downloads", message: ($total | tostring), color: "blue"}' \
    > downloads-badge.json

echo "baseline=$baseline current=$total"
