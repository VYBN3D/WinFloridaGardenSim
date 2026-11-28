#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMXX="${EMXX:-em++}"
WORK="${TMPDIR:-/tmp}/winflorida-webport-build"
OUT="${1:-$ROOT/webport-build/out}"
FIRMWARE_PREPROCESSED_SOURCE="${FIRMWARE_PREPROCESSED_SOURCE:?Set this to Arduino CLI --preprocess output from the S3 firmware}"
mkdir -p "$WORK" "$OUT"
python3 - "$FIRMWARE_PREPROCESSED_SOURCE" "$WORK/sketch.cpp" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1])
out = Path(sys.argv[2])
if not source.is_file():
    raise SystemExit(f"Preprocessed firmware source not found: {source}")
bridge = r"""
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern "C" EMSCRIPTEN_KEEPALIVE void garden_reload_economy_from_nvs() {
  credits = preferences.getUShort("credits", credits);
  if (preferences.getBytesLength("seedArr") == sizeof(seedsByStrain)) preferences.getBytes("seedArr", seedsByStrain, sizeof(seedsByStrain));
  if (preferences.getBytesLength("stashArr") == sizeof(stashByStrain)) preferences.getBytes("stashArr", stashByStrain, sizeof(stashByStrain));
  if (preferences.getBytesLength("hashArr") == sizeof(hashByStrain)) preferences.getBytes("hashArr", hashByStrain, sizeof(hashByStrain));
  if (preferences.getBytesLength("sqArr") == sizeof(stashQualityByStrain)) preferences.getBytes("sqArr", stashQualityByStrain, sizeof(stashQualityByStrain));
  if (preferences.getBytesLength("hqArr") == sizeof(hashQualityByStrain)) preferences.getBytes("hqArr", hashQualityByStrain, sizeof(hashQualityByStrain));
  untrimmedUnits = preferences.getUShort("rawFlower", untrimmedUnits);
  untrimmedStrain = preferences.getUChar("rawStrain", untrimmedStrain);
  untrimmedQuality = preferences.getUChar("rawQual", untrimmedQuality);
  waterPacks = preferences.getUChar("waterPk", waterPacks);
  careKits = preferences.getUChar("careKit", careKits);
  pestDefenseKits = preferences.getUChar("pestKit", pestDefenseKits);
  remediationKits = preferences.getUChar("remKit", remediationKits);
  bongs = preferences.getUChar("bongs", bongs);
  lighters = preferences.getUChar("lighters", lighters);
  rollingPapers = preferences.getUChar("papers", rollingPapers);
  saveGame();
  render();
}
#endif
"""
out.write_text(source.read_text() + "\n" + bridge)
PY
"$EMXX" -std=c++17 -O2 -Wno-c++11-narrowing -DS3_CYD -DARDUINO_ARCH_ESP32 -I"$ROOT/webport-build/include" -I"$ROOT/firmware" -I"$ROOT/webport-build/src" -include "$ROOT/webport-build/include/Arduino.h" \
  "$ROOT/webport-build/src/shim.cpp" "$WORK/sketch.cpp" \
  "$ROOT/webport-build/src/Font16.cxx" "$ROOT/webport-build/src/Font32rle.cxx" "$ROOT/webport-build/src/glcdfont.cxx" \
  -sWASM=1 -sMODULARIZE=1 -sEXPORT_NAME=createGardenSim -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 -sNO_EXIT_RUNTIME=1 -sASYNCIFY=1 \
  -sEXPORTED_RUNTIME_METHODS=["HEAP8","HEAP16","HEAPU8","HEAPU16","HEAP32","HEAPU32","UTF8ToString","stringToUTF8"] \
  -sEXPORTED_FUNCTIONS='["_main","_garden_reload_economy_from_nvs","_garden_fb_ptr","_garden_millis"]' \
  -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=1048576 -sASYNCIFY_STACK_SIZE=65536 \
  -o "$OUT/garden.js"
