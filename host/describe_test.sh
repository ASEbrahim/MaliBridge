#!/bin/bash
# The build name the report and Send logs show (ledger row 13, 684f731). In v0.5.3 -> v0.5.4 the bridge named
# itself by `git describe --always --dirty`, which ignores lightweight tags: the build of the release commit
# said "v0.5.3-31-g..." (or a bare hash) instead of the release. Twice the wrong name reached the report and was
# caught by hand. Both places that name the build (host/Makefile's VKB_GIT, scripts/build-server-android.sh's
# `git=`) must name the nearest release tag, lightweight ones included, and skip tags that are not releases.
# The describe commands are taken from those two files and run in a repository made here (nothing is read
# from the host's repositories, config or clock).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
root=$(cd .. && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# the arguments each file gives `git describe`
mk=$(sed -n 's/^VKB_GIT := .*describe \(.*\) 2>\/dev\/null.*/\1/p' Makefile)
sh=$(sed -n 's/^git=.*describe \(.*\) 2>\/dev\/null.*/\1/p' "$root/scripts/build-server-android.sh")
[ -n "$mk" ] && [ -n "$sh" ]
step $? "found the describe arguments (Makefile: $mk | build-server-android.sh: $sh)"

repo=$work/repo
git init -q "$repo" && git -C "$repo" commit -q --allow-empty -m one
named() { # named <args>: the name git gives HEAD with those arguments (the quotes in the Makefile are shell quotes)
  eval "git -C '$repo' describe $1 2>/dev/null || echo unknown"
}

git -C "$repo" tag v9.9.9      # a lightweight tag on the built commit: how a release is tagged before it is built
for pair in "Makefile:$mk" "build-server-android.sh:$sh"; do
  f=${pair%%:*}; a=${pair#*:}
  [ "$(named "$a")" = v9.9.9 ]
  step $? "$f: the release commit with a lightweight tag is named v9.9.9 (got $(named "$a"))"
done

git -C "$repo" commit -q --allow-empty -m two
git -C "$repo" tag nightly   # not a release tag, and the nearest tag: --match must skip it
for pair in "Makefile:$mk" "build-server-android.sh:$sh"; do
  f=${pair%%:*}; a=${pair#*:}
  n=$(named "$a")
  [[ $n =~ ^v9\.9\.9-1-g[0-9a-f]+$ ]]
  step $? "$f: one commit later is v9.9.9-1-g<hash> (got $n), the 'nightly' tag ignored"
done

git -C "$repo" tag -a v9.10.0 -m annotated
n=$(named "$mk")
[ "$n" = v9.10.0 ]
step $? "an annotated release tag is named too (got $n)"

[ $fails = 0 ] && echo "OK: $steps steps" || { echo "FAILED: $fails of $steps steps"; exit 1; }
