#!/usr/bin/env bash
# Fetch the Khronos glTF-Sample-Assets listed in test/corpus.json into test/assets/khronos,
# at the pinned commit, with a sparse partial clone: only the listed variant folders are downloaded.
# Idempotent: rerun after adding an asset to the corpus.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
corpus="$repo_root/test/corpus.json"
url="$(jq -r '.sources.khronos.repository' "$corpus")"
commit="$(jq -r '.sources.khronos.commit' "$corpus")"
prefix="$(jq -r '.sources.khronos.prefix' "$corpus")"
dest="$repo_root/$(jq -r '.sources.khronos.root' "$corpus")"
checkout="$dest/.checkout"

# One sparse pattern per variant folder: Models/<model>/<variant>/
mapfile -t patterns < <(jq -r --arg p "$prefix" '.assets[] | select(.source == "khronos") | .path | split("/") | "/\($p)/\(.[0])/\(.[1])/"' "$corpus" | sort -u)

if [[ ! -d "$checkout/.git" ]]; then
  mkdir -p "$dest"
  git clone --quiet --filter=blob:none --no-checkout "$url" "$checkout"
fi
git -C "$checkout" sparse-checkout set --no-cone "${patterns[@]}"
git -C "$checkout" fetch --quiet --filter=blob:none origin "$commit" 2>/dev/null || true
git -C "$checkout" -c advice.detachedHead=false checkout --quiet "$commit"

# Corpus paths are relative to $dest: link each model folder there.
for model in "$checkout/$prefix"/*/; do
  name="$(basename "$model")"
  [[ -e "$dest/$name" ]] || ln -s ".checkout/$prefix/$name" "$dest/$name"
done

missing=0
while IFS= read -r path; do
  [[ -f "$dest/$path" ]] || { echo "missing: $path" >&2; missing=1; }
done < <(jq -r '.assets[] | select(.source == "khronos") | .path' "$corpus")
du -shL "$dest" 2>/dev/null | cut -f1 | xargs -I{} echo "Khronos assets at $commit in $dest ({})"
exit "$missing"
