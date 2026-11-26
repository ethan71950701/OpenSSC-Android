#!/usr/bin/env bash
# Usage: ANDROID_NDK_HOME=/path/to/ndk ./build.sh   ->  out/openssc-probe-<version>.zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
OBJ="$ROOT/out/obj"; STAGE="$ROOT/out/stage"
rm -rf "$OBJ" "$STAGE"; mkdir -p "$OBJ" "$STAGE/zygisk"
CFLAGS=(-DXZ_USE_CRC64 --target=aarch64-linux-android31 -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections
        -Wall -Wextra -I"$ROOT/src" -I"$ROOT/third_party/xz" -I"$ROOT/third_party/zygisk")
for f in xz_crc32 xz_crc64 xz_dec_stream xz_dec_lzma2; do
  "$BIN/clang" "${CFLAGS[@]}" -Wno-unused-parameter -c "$ROOT/third_party/xz/$f.c" -o "$OBJ/$f.o"
done
for f in module symres guard; do
  "$BIN/clang++" "${CFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wno-missing-field-initializers -c "$ROOT/src/$f.cpp" -o "$OBJ/$f.o"
done
"$BIN/clang++" --target=aarch64-linux-android31 -shared -nostdlib++ -Wl,--gc-sections -Wl,-z,max-page-size=16384 \
  -Wl,-z,defs -Wl,--exclude-libs,ALL "$OBJ"/*.o -llog -o "$STAGE/zygisk/arm64-v8a.so"
"$BIN/llvm-strip" --strip-unneeded "$STAGE/zygisk/arm64-v8a.so"
cp "$ROOT"/module/* "$STAGE/"
cp "$ROOT/third_party/README.txt" "$STAGE/THIRD_PARTY.txt"
V="$(sed -n 's/^version=//p' "$ROOT/module/module.prop")"
(cd "$STAGE" && rm -f "../openssc-probe-$V.zip" && zip -q -r -X "../openssc-probe-$V.zip" .)
echo "built out/openssc-probe-$V.zip"
