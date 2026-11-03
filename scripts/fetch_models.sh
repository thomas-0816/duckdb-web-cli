#!/usr/bin/env bash
set -euo pipefail

# Download the Q8_0 GGUF embedding model. The build embeds this file into the
# docs binary, so it is only needed at build time.
#
# Usage:
#   scripts/fetch_models.sh

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root"
target="bge-small-en-v1.5-q8_0.gguf"
url="https://huggingface.co/ggml-org/bge-small-en-v1.5-Q8_0-GGUF/resolve/main/bge-small-en-v1.5-q8_0.gguf"

mkdir -p "$dest"

if [[ -s "$dest/$target" ]]; then
  echo "already present: $dest/$target"
  exit 0
fi

echo "downloading: $target"
curl --fail --location --retry 3 --retry-delay 2 \
  --output "$dest/$target.tmp" "$url"
mv "$dest/$target.tmp" "$dest/$target"

echo
echo "Model file is in $dest"
echo "Re-run the build so CMake embeds it into the docs binary."
