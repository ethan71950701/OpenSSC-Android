#!/usr/bin/env bash
# Host tests (Linux x86_64: clang, gcc with ASan, binutils, xz). Builds a fixture whose function names exist only in
# .gnu_debugdata, like Android system libraries, then checks lookup, damaged input and the guard.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; O="$ROOT/out/host"; rm -rf "$O"; mkdir -p "$O"
clang++ -O1 -shared -fPIC "$ROOT/tests/fixture.cpp" -o "$O/lib.so"
objcopy --only-keep-debug "$O/lib.so" "$O/mini"
nm "$O/lib.so" --defined-only -f posix | awk '$2 ~ /^[tT]$/ {print $1}' > "$O/keep"
objcopy -S --remove-section .comment --keep-symbols="$O/keep" "$O/mini" "$O/mini.keep"
xz -f --check=crc64 "$O/mini.keep"
strip --strip-all "$O/lib.so"
objcopy --add-section .gnu_debugdata="$O/mini.keep.xz" "$O/lib.so"
for f in xz_crc32 xz_crc64 xz_dec_stream xz_dec_lzma2; do
  gcc -g -O1 -fsanitize=address,undefined -DXZ_USE_CRC64 -I"$ROOT/third_party/xz" -c "$ROOT/third_party/xz/$f.c" -o "$O/$f.o"
done
g++ -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -DXZ_USE_CRC64 -Wno-missing-field-initializers \
  -I"$ROOT/src" -I"$ROOT/third_party/xz" "$ROOT/tests/test_host.cpp" "$ROOT/src/symres.cpp" "$ROOT/src/guard.cpp" "$O"/*.o -o "$O/test"
"$O/test" "$O/lib.so"
g++ -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wno-missing-field-initializers \
  -I"$ROOT/tests/stubs" -I"$ROOT/src" \
  "$ROOT/tests/test_hooks.cpp" "$ROOT/src/hooks.cpp" "$ROOT/src/guard.cpp" -o "$O/test_hooks"
"$O/test_hooks"
for s in "$ROOT"/module/*.sh; do sh -n "$s"; done
echo "shell scripts: syntax OK"
