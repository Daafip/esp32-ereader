#!/usr/bin/env bash
# The book server add-on with this working copy's bookprep in it, ready to copy to
# Home Assistant's /addons folder as a local add-on (docs/book-server.md):
#   pixi run ha-addon               -> out/ha-addon/t5books/
#   pixi run ha-addon /path/addons  -> /path/addons/t5books/ (e.g. the Samba share "addons")
# Without bookprep in the folder (the add-on repository on GitHub), the Dockerfile
# takes it from the release of the add-on's version.
set -euo pipefail
cd "$(dirname "$0")/.."

dest="${1:-out/ha-addon}/t5books"
rm -rf "$dest"
mkdir -p "$dest/bookprep"
cp -r homeassistant/t5books/. "$dest/"
cp tools/bookprep/pyproject.toml "$dest/bookprep/"
cp -r tools/bookprep/src "$dest/bookprep/"
find "$dest" -name __pycache__ -type d -prune -exec rm -rf {} +
echo "$dest: version $(sed -n 's/^version: "\(.*\)"/\1/p' homeassistant/t5books/config.yaml)," \
  "bookprep from this working copy"
echo "In Home Assistant: Settings -> Add-ons -> Add-on store -> ⋮ -> Check for updates,"
echo "then 'T5 book server' under Local add-ons (an installed one: Rebuild)."
