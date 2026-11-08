#!/usr/bin/env bash
# Builds the F-Droid repository of the firmware switcher for GitHub Pages
# (https://daafip.github.io/esp32-ereader/fdroid/repo): the APK, its metadata
# (android/fdroid/metadata) and an index signed with the repository key. Used by
# release.yml and installer.yml; needs fdroidserver (pip install fdroidserver), a JDK,
# and the keystore of the release key, which also holds the repository key:
#   FWSWITCH_KEYSTORE           the keystore file
#   FWSWITCH_KEYSTORE_PASSWORD  its password (PKCS12: the keys' as well)
#   FDROID_KEY_ALIAS            the repository key's alias (default: fdroid)
#   FDROID_REPO_URL             where it is served (default: the GitHub Pages address)
# Usage: tools/make_fdroid_repo.sh <app.apk> <out_dir>   -> <out_dir>/repo/
# An APK signed with a debug key is refused (exit 3): every build has another debug
# key, and F-Droid only installs an update with the same signature.
set -euo pipefail
apk=$(realpath "$1")
out=$2
: "${FWSWITCH_KEYSTORE:?the keystore file}" "${FWSWITCH_KEYSTORE_PASSWORD:?its password}"
root=$(cd "$(dirname "$0")/.." && pwd)
keystore=$(realpath "$FWSWITCH_KEYSTORE")

apksigner=$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/build-tools/*/apksigner | sort -V | tail -1)
if "$apksigner" verify --print-certs "$apk" | grep -q "CN=Android Debug"; then
  echo "$apk is signed with a debug key: no F-Droid repository (set up the release key)" >&2
  exit 3
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/repo" "$work/metadata"
cp -r "$root"/android/fdroid/metadata/. "$work/metadata/"  # <appid>.yml, <appid>/en-US/icon.png
# repo_icon (default icon.png) is read from here and copied to repo/icons/
cp "$root"/android/fdroid/metadata/io.github.daafip.fwswitch/en-US/icon.png "$work/icon.png"
cp "$apk" "$work/repo/"

# Passwords stay in the environment ({env: ...}), not in the file.
cat > "$work/config.yml" <<EOF
repo_url: ${FDROID_REPO_URL:-https://daafip.github.io/esp32-ereader/fdroid/repo}
repo_name: E-reader firmware switcher
repo_description: >-
  The Android app of github.com/Daafip/esp32-ereader: puts the e-reader firmware or
  ESPHome on the home-built FireBeetle 7.5" reader over USB. Built and signed by the
  project's release workflow on GitHub.
repo_keyalias: ${FDROID_KEY_ALIAS:-fdroid}
keystore: $keystore
keystorepass: {env: FWSWITCH_KEYSTORE_PASSWORD}
keypass: {env: FWSWITCH_KEYSTORE_PASSWORD}
EOF
chmod 600 "$work/config.yml"

(cd "$work" && fdroid update)

rm -rf "$work/repo/status"  # fdroidserver's run log: paths and tools of this machine
mkdir -p "$out"
rm -rf "$out/repo"
cp -r "$work/repo" "$out/repo"
echo "F-Droid repository: $out/repo"
