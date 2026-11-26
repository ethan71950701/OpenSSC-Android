#!/usr/bin/env bash
# Usage:
#   ANDROID_NDK_HOME=/path/to/ndk ./build.sh        -> out/openssc-probe-<version>.zip   (normal build)
#   XCC_INC="<-isystem ...>" ./build.sh              -> same, with host clang + explicit bionic headers
#                                                      (only for sandboxes without an NDK; see README)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OBJ="$ROOT/out/obj"; STAGE="$ROOT/out/stage"
rm -rf "$OBJ" "$STAGE"; mkdir -p "$OBJ" "$STAGE/zygisk"
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
  CC="$BIN/clang"; CXX="$BIN/clang++"; STRIP="$BIN/llvm-strip"; XINC=()
else
  : "${XCC_INC:?set ANDROID_NDK_HOME (or XCC_INC for a no-NDK build)}"
  CC=clang; CXX=clang++; STRIP=llvm-strip
  # shellcheck disable=SC2206
  XINC=(-nostdinc -isystem "$(clang -print-resource-dir)/include" $XCC_INC)
fi
TGT=(--target=aarch64-linux-android31)
CFLAGS=(-DXZ_USE_CRC64 "${TGT[@]}" "${XINC[@]}" -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections
        -Wall -Wextra -I"$ROOT/src" -I"$ROOT/third_party/xz" -I"$ROOT/third_party/zygisk"
        -I"$ROOT/third_party/shadowhook/include")
for f in xz_crc32 xz_crc64 xz_dec_stream xz_dec_lzma2; do
  "$CC" "${CFLAGS[@]}" -Wno-unused-parameter -c "$ROOT/third_party/xz/$f.c" -o "$OBJ/$f.o"
done
# ShadowHook (MIT) compiled in statically, arm64 only, without its JNI glue.
SH="$ROOT/third_party/shadowhook"
SHF=("${TGT[@]}" "${XINC[@]}" -std=gnu11 -Os -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -w
     -I"$SH" -I"$SH/include" -I"$SH/arch/arm64" -I"$SH/common" -I"$SH/third_party/xdl" -I"$SH/third_party/bsd"
     -I"$SH/third_party/lss")
for f in "$SH"/*.c "$SH"/arch/arm64/*.c "$SH"/arch/arm64/*.S "$SH"/common/*.c "$SH"/third_party/xdl/*.c; do
  "$CC" "${SHF[@]}" -c "$f" -o "$OBJ/sh_$(basename "$f").o"
done
"$CC" "${CFLAGS[@]}" -c "$ROOT/src/clear_cache.c" -o "$OBJ/clear_cache.o"
for f in module symres guard hooks; do
  "$CXX" "${CFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wno-missing-field-initializers -c "$ROOT/src/$f.cpp" -o "$OBJ/$f.o"
done
# NDK build: real stub libraries. No-NDK build: empty stand-in libraries with the right names, so the
# result still records DT_NEEDED for libc/libm/libdl/liblog (the undefined symbols were checked separately).
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  LDX=(-Wl,-z,defs -llog -ldl)
else
  STUBS="$ROOT/out/stubs"; mkdir -p "$STUBS"; echo "" > "$STUBS/empty.c"
  for l in c m dl log; do
    "$CC" "${TGT[@]}" -shared -nostdlib -fuse-ld=lld -Wl,-soname,lib$l.so "$STUBS/empty.c" -o "$STUBS/lib$l.so"
  done
  LDX=(-fuse-ld=lld -L"$STUBS" -Wl,--no-as-needed -lc -lm -ldl -llog)
fi
"$CXX" "${TGT[@]}" -shared -nostdlib -Wl,--gc-sections -Wl,-z,max-page-size=16384 \
  -Wl,--version-script="$ROOT/src/exports.map" -Wl,--exclude-libs,ALL "$OBJ"/*.o "${LDX[@]}" \
  -o "$STAGE/zygisk/arm64-v8a.so"
"$STRIP" --strip-unneeded "$STAGE/zygisk/arm64-v8a.so"
cp "$ROOT"/module/* "$STAGE/"
cp "$ROOT/third_party/README.txt" "$STAGE/THIRD_PARTY.txt"
cp "$ROOT/third_party/shadowhook/LICENSE" "$STAGE/LICENSE.shadowhook.txt"
V="$(sed -n 's/^version=//p' "$ROOT/module/module.prop")"
(cd "$STAGE" && rm -f "../openssc-probe-$V.zip" && zip -q -r -X "../openssc-probe-$V.zip" .)
echo "built out/openssc-probe-$V.zip"
