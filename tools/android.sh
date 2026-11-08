#!/usr/bin/env bash
# Runs Gradle on the firmware switcher app (android/) with the Android SDK from
# $ANDROID_HOME, or ~/Android/Sdk (Android Studio's default).
#   pixi run android-build   -> out/android/fw-switch.apk
#   pixi run android-test    -> unit tests (PC)
set -euo pipefail
cd "$(dirname "$0")/.."

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
if [ ! -d "$ANDROID_HOME/platforms" ]; then
  echo "No Android SDK in $ANDROID_HOME: install Android Studio (or its command-line" >&2
  echo "tools with platform 36), or point ANDROID_HOME at an SDK." >&2
  exit 1
fi

gradle -p android --console=plain "$@"

if [[ " $* " == *" assembleDebug "* ]]; then
  mkdir -p out/android
  cp android/app/build/outputs/apk/debug/app-debug.apk out/android/fw-switch.apk
  echo "APK: out/android/fw-switch.apk"
fi
