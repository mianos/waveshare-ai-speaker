#!/usr/bin/env bash
# Stable build wrapper so the command string never changes (one approval covers
# all builds). Sets the esp32s3 target on first run, then builds.
set -e
set -o pipefail  # so a build failure isn't masked by the `tee | tail` pipeline
export IDF_PATH=/Users/robertfowler/.espressif/v6.0.1/esp-idf
. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd /Users/robertfowler/walocal/ws-voice
if [ ! -f sdkconfig ] || ! grep -q 'CONFIG_IDF_TARGET="esp32s3"' sdkconfig 2>/dev/null; then
    idf.py set-target esp32s3
fi
idf.py build 2>&1 | tee /tmp/ws_voice_build.log | tail -n 60
