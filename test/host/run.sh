#!/usr/bin/env bash
# Build and run the host-side unit tests (no hardware / ESP-IDF required).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d)"

g++ -std=c++17 -Wall -Wextra \
    -I"$ROOT/main" \
    "$HERE/test_wav_builder.cpp" \
    -o "$OUT/test_wav_builder"

"$OUT/test_wav_builder"

g++ -std=c++17 -Wall -Wextra \
    -I"$ROOT/main" \
    "$HERE/test_tonegen.cpp" \
    -o "$OUT/test_tonegen"

"$OUT/test_tonegen"

g++ -std=c++17 -Wall -Wextra \
    -I"$ROOT/main" \
    "$HERE/test_tts_request.cpp" \
    -o "$OUT/test_tts_request"

"$OUT/test_tts_request"

g++ -std=c++17 -Wall -Wextra \
    -I"$ROOT/main" \
    "$HERE/test_miclevel.cpp" \
    -o "$OUT/test_miclevel"

"$OUT/test_miclevel"
