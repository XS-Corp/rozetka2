#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/minecraft/cxxabi"
SOURCE="$WORK/source"
mkdir -p "$SOURCE"
BASE=https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/libcxxabi
while read -r digest path; do
    target="$SOURCE/${path##*/}"
    if ! [ -f "$target" ] || ! printf '%s  %s\n' "$digest" "$target" | shasum -a 256 -c - >/dev/null 2>&1; then
        curl -fsSL "$BASE/$path" -o "$target.download"
        printf '%s  %s\n' "$digest" "$target.download" | shasum -a 256 -c - >/dev/null
        mv "$target.download" "$target"
    fi
done <<'FILES'
6269d03fbcb77b218f7ab434f5059473c6e3c8355a56ef838b76ad11f0b5e576 src/private_typeinfo.cpp
308dd443446dfa0e6972e897f575c4e379f50bbf3aee3f5126f955f44768c803 src/private_typeinfo.h
17143f9c5cfec39e2b741b4b319dec733eb3ba859324859f760f4ad87bd1cb09 include/__cxxabi_config.h
ebefbcabd5ed6b790299d740eba3ed49965072b9e77abff6f0963cf67682a064 src/abort_message.h
5d0b584b9341eb49ea5e640a475dc690d44b0770712222e2667185b1e814367f src/stdlib_typeinfo.cpp
d609597f8f536fec59f7afa2dbd352c69a9b50ce8aaf85082fd1fbad08c79a25 src/stdlib_exception.cpp
e2b35be49f7284a45b7baca8fc7b3ab7440e7902392b2528a457816b5bb2a15c LICENSE.TXT
FILES
for name in private_typeinfo stdlib_typeinfo stdlib_exception; do
    clang++ -target x86_64-apple-macos11 -std=c++17 -O1 -g -fno-exceptions \
        -fno-stack-protector -fno-vectorize -fno-slp-vectorize \
        -I"$SOURCE" -c "$SOURCE/$name.cpp" -o "$WORK/$name.o"
done
clang++ -target x86_64-apple-macos11 -std=c++17 -O1 -g -fno-exceptions \
    -fno-stack-protector -c "$ROOT/ports/cxxabi_support.cpp" -o "$WORK/support.o"
clang -target x86_64-apple-macos11 -std=c17 -O1 -g -fno-stack-protector \
    -fno-vectorize -fno-slp-vectorize -c "$ROOT/ports/guest_qsort.c" -o "$WORK/qsort.o"
clang++ -target x86_64-apple-macos11 -dynamiclib -nostdlib++ \
    "$WORK/private_typeinfo.o" "$WORK/stdlib_typeinfo.o" "$WORK/stdlib_exception.o" "$WORK/support.o" "$WORK/qsort.o" \
    -Wl,-no_fixup_chains \
    -Wl,-install_name,@rpath/librozetka2-cxxabi.dylib -o "$WORK/librozetka2-cxxabi.dylib"
file "$WORK/librozetka2-cxxabi.dylib"
