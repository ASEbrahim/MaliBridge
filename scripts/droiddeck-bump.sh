#!/bin/bash
# Prepares the branch that moves DroidDeck's pin (tools/malibridge/release.env) to a MaliBridge release, on the
# owner's fork. It pushes the branch and prints the compare link; it never opens a pull request.
#   scripts/droiddeck-bump.sh <tag> <asset-dir>
# <asset-dir> holds the release's malibridge-droiddeck.tar.zst, malibridge-source.tar.zst and their .sha256 files.
# Environment:
#   FORK_URL      the fork to push to (default https://github.com/ASEbrahim/DroidDeck.git)
#   UPSTREAM_URL  where its main comes from (default https://github.com/Droid-Deck/DroidDeck.git)
#   MALIBRIDGE_URL  where the tag is resolved (default https://github.com/ASEbrahim/MaliBridge.git)
#   PUSH_TOKEN    token with contents write on the fork; used for the push only
#   SUMMARY_FILE  where the result goes (default $GITHUB_STEP_SUMMARY, else stdout)
#   WORKDIR       scratch directory (default a new temp dir)
# Exit 0 also when there is nothing to do; it says why in the summary.
set -euo pipefail
tag=$1
dir=$(cd "$2" && pwd)
fork=${FORK_URL:-https://github.com/ASEbrahim/DroidDeck.git}
upstream=${UPSTREAM_URL:-https://github.com/Droid-Deck/DroidDeck.git}
mali=${MALIBRIDGE_URL:-https://github.com/ASEbrahim/MaliBridge.git}
sum=${SUMMARY_FILE:-${GITHUB_STEP_SUMMARY:-/dev/stdout}}
work=${WORKDIR:-$(mktemp -d)}
branch=malibridge-$tag
asset=$dir/malibridge-droiddeck.tar.zst
src=$dir/malibridge-source.tar.zst

say() { echo "$*" >> "$sum"; }
die() { echo "ERROR: $*" >&2; say "**Stopped:** $*"; exit 1; }

# 1. recompute the hashes; the sidecar files are not trusted
asset_sha=$(sha256sum "$asset" | cut -d' ' -f1)
src_sha=$(sha256sum "$src" | cut -d' ' -f1)
[ "$asset_sha" = "$(cut -d' ' -f1 "$asset.sha256")" ] || die "the asset does not match its .sha256 file"
[ "$src_sha" = "$(cut -d' ' -f1 "$src.sha256")" ] || die "the source archive does not match its .sha256 file"

# 2. what the asset says about itself, and what the tag says
manifest=$(tar --zstd -xOf "$asset" ./MALIBRIDGE.json)
field() { printf '%s' "$manifest" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }
[ "$(field tag)" = "$tag" ] || die "the asset's manifest names $(field tag), not $tag"
commit=$(field commit)
interface=$(field interface)
resolved=$(git ls-remote "$mali" "refs/tags/$tag" "refs/tags/$tag^{}" | awk '{print $1}' | tail -1)
[ "$resolved" = "$commit" ] || die "tag $tag resolves to ${resolved:-nothing}, the asset was built from $commit"

# 3. the fork's branch, on top of the official main. The fork's main is first brought level with the official
# one (GitHub's own sync, a fast-forward only), so the pushed branch adds a single commit to what the fork has.
authopt=()
if [ -n "${PUSH_TOKEN:-}" ]; then
    authopt=(-c "http.extraheader=Authorization: basic $(printf 'x-access-token:%s' "$PUSH_TOKEN" | base64 -w0)")
fi
owner=$(printf '%s' "$fork" | sed -E 's|.*[:/]([^/]+)/[^/]+$|\1|')
repo=$(basename "$fork" .git)
if [ -n "${SYNC_FORK:-}" ]; then
    GH_TOKEN=$PUSH_TOKEN gh api -X POST "repos/$owner/$repo/merge-upstream" -f branch=main > /dev/null \
        || die "could not bring $owner/$repo main level with the official main (it has commits of its own on main?)"
fi
cd "$work"
git init -q repo && cd repo
git remote add upstream "$upstream"
git remote add fork "$fork"
git fetch -q --depth=1 upstream main
up_sha=$(git rev-parse FETCH_HEAD)
git "${authopt[@]}" fetch -q --depth=1 fork main
git checkout -q -b "$branch" FETCH_HEAD
if [ "$(git rev-parse HEAD)" != "$up_sha" ]; then
    say "Note: the fork's main (\`$(git rev-parse --short HEAD)\`) is not at official main (\`${up_sha:0:9}\`); the branch is based on the fork's main."
fi
envf=tools/malibridge/release.env
if [ ! -f "$envf" ]; then
    say "Nothing to do: official DroidDeck main has no $envf yet (the pin change is not merged there)."
    echo "no $envf on upstream main"; exit 0
fi
cur_if=$(sed -n 's/^MALIBRIDGE_INTERFACE=//p' "$envf")
[ "$cur_if" = "$interface" ] || die "interface mismatch: DroidDeck expects $cur_if, $tag is $interface. This release needs an app change first; no branch was pushed."
setkey() { grep -q "^$1=" "$envf" || die "$envf has no $1"; sed -i "s|^$1=.*|$1=$2|" "$envf"; }
setkey MALIBRIDGE_TAG "$tag"
setkey MALIBRIDGE_SHA256 "$asset_sha"
setkey MALIBRIDGE_SOURCE_COMMIT "$commit"
setkey MALIBRIDGE_SOURCE_SHA256 "$src_sha"
if git diff --quiet -- "$envf"; then
    say "Nothing to do: official DroidDeck main already pins $tag."
    exit 0
fi
if [ -f tools/tests/test_malibridge_pin.py ]; then
    python3 -m unittest tools/tests/test_malibridge_pin.py || die "DroidDeck's pin test fails with the new values"
fi
git -c user.name=ASEbrahim -c user.email=264076441+ASEbrahim@users.noreply.github.com \
    commit -q -a -m "chore: bump MaliBridge to $tag"

# 4. push to the fork; the branch is ours alone, so a re-run replaces it
git "${authopt[@]}" push -q --force fork "$branch"
say "### DroidDeck pin branch ready"
say ""
say "Branch \`$branch\` is on the fork ($owner), one commit on top of the fork's main: \`$tag\`, asset \`$asset_sha\`."
say "No pull request was opened. To open it by hand:"
say ""
say "https://github.com/Droid-Deck/DroidDeck/compare/main...$owner:$repo:$branch?expand=1"
echo "pushed $branch"
