#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copies wiki/*.md into a fresh clone of the repository's GitHub wiki, with
# each screenshot the pages show from ../docs/ui/ pointed at its address
# in the main repository, and shows what would change.
# Pushes only with --push.
#
#     tools/wiki-publish.sh WIKI_GIT_URL            clone, replace, show the diff
#     tools/wiki-publish.sh --push WIKI_GIT_URL     the same, then commit and push
#
# WIKI_GIT_URL is the wiki's own repository, such as
# https://github.com/OWNER/REPO.wiki.git. GitHub creates it only once the
# wiki's first page has been saved on the website.
#
# The commit takes this repository's user.name and user.email, in UTC, with no
# trailers. tools/leak-check.py checks the pages first.

set -eu

usage() {
	echo "usage: $0 [--push] WIKI_GIT_URL" >&2
	exit 2
}

push=0
url=
while [ $# -gt 0 ]; do
	case $1 in
	--push) push=1 ;;
	-h | --help) usage ;;
	-*) usage ;;
	*)
		[ -z "$url" ] || usage
		url=$1
		;;
	esac
	shift
done
[ -n "$url" ] || usage

root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/wiki
[ -f "$src/Home.md" ] || { echo "no $src/Home.md" >&2; exit 1; }

name=$(git -C "$root" config user.name || true)
email=$(git -C "$root" config user.email || true)
if [ "$push" -eq 1 ] && { [ -z "$name" ] || [ -z "$email" ]; }; then
	echo "set user.name and user.email in this repository first" >&2
	exit 1
fi

# The pages, as the hooks check a commit's files.
python3 -I -B "$root/tools/leak-check.py" files "$src"/*.md

tmp=$(mktemp -d "${TMPDIR:-/tmp}/wiki-publish.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
dst=$tmp/wiki

git clone --quiet "$url" "$dst"

# Replace everything but .git.
git -C "$dst" rm -r -q --ignore-unmatch -- . >/dev/null
for f in "$dst"/* "$dst"/.[!.]* "$dst"/..?*; do
	[ -e "$f" ] || continue
	[ "$f" = "$dst/.git" ] || rm -rf "$f"
done

cp "$src"/*.md "$dst"/

# The pages show screenshots as ../docs/ui/<name>.png, which is where they
# are when a page is read in the repository. The wiki's repository holds no
# pictures: there each becomes the address of docs/ui/<name>.png in the main
# repository, so a picture follows docs/ui/ and shows while that repository
# is public.
case $url in
*github.com[:/]*) slug=${url#*github.com[:/]} ;;
*) slug=$(git -C "$root" config remote.origin.url || true); slug=${slug#*github.com[:/]} ;;
esac
slug=${slug%.git}
slug=${slug%.wiki}
case $slug in
*/*) ;;
*) echo "cannot tell OWNER/REPO from $url" >&2; exit 1 ;;
esac
raw=https://raw.githubusercontent.com/$slug/main/docs/ui
images=$(grep -ho '\.\./docs/ui/[A-Za-z0-9._-]*\.png' "$src"/*.md | sort -u)
for img in $images; do
	file=$root/docs/ui/${img#../docs/ui/}
	[ -f "$file" ] || { echo "missing $file, named in wiki/" >&2; exit 1; }
done
if [ -n "$images" ]; then
	for f in "$dst"/*.md; do
		sed "s|\.\./docs/ui/\([A-Za-z0-9._-]*\.png\)|$raw/\1|g" "$f" > "$f.tmp" && mv "$f.tmp" "$f"
	done
fi

git -C "$dst" add -A
echo "== git status"
git -C "$dst" status --short
echo "== diff stat"
git -C "$dst" diff --cached --stat

if git -C "$dst" diff --cached --quiet; then
	echo "The wiki already matches wiki/. Nothing to push."
	exit 0
fi
if [ "$push" -eq 0 ]; then
	echo "Not pushed. Run again with --push to commit and push this."
	exit 0
fi

TZ=UTC0 git -C "$dst" -c user.name="$name" -c user.email="$email" \
	commit --quiet -m "Update the wiki from the repository's wiki/ directory"
git -C "$dst" push --quiet origin HEAD
echo "Pushed to $url."
