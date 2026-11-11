#!/bin/sh
# Build guest C++ library code; never execute x86 programs with the host loader.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/minecraft/libcxx"
SOURCE="$WORK/source"
VERSION=18.1.8
mkdir -p "$SOURCE/runtimes/cmake/Modules"
fetch() {
    digest=$1 url=$2 destination=$3
    if ! [ -f "$destination" ] || ! printf '%s  %s\n' "$digest" "$destination" | shasum -a 256 -c - >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$destination.download"
        printf '%s  %s\n' "$digest" "$destination.download" | shasum -a 256 -c - >/dev/null
        mv "$destination.download" "$destination"
    fi
}
while read -r digest project; do
    archive="$project-$VERSION.src.tar.xz"
    fetch "$digest" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/$archive" "$SOURCE/$archive"
    tar -xJf "$SOURCE/$archive" -C "$SOURCE"
done <<'ARCHIVES'
bdecf90be0072bc720fd5c9c8ab061cdb197edd0c8ad3e170dc3e6bfaa49f388 libcxx
256c30d724eeb72713bc08ae1692f53aaf4ebe8a1d662c92bf59e69d6c53dce9 libcxxabi
59badef592dd34893cd319d42b323aaa990b452d05c7180ff20f23ab1b41e837 cmake
ARCHIVES
while read -r digest module; do
    fetch "$digest" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$VERSION/runtimes/cmake/Modules/$module.cmake" "$SOURCE/runtimes/cmake/Modules/$module.cmake"
done <<'MODULES'
a45404becfd848a5d8cb1074f44e32a38d570197479f71df26a56e5a2b075e1b HandleFlags
7bc97fa37f0bce7860e810c0743acd15f33ecff2612a8589c79431a9b3aada01 WarningFlags
MODULES
ln -sfn "cmake-$VERSION.src" "$SOURCE/cmake"
cmake -S "$SOURCE/libcxx-$VERSION.src" -B "$WORK/build" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_BUILD_TYPE=Release -DLIBCXX_CXX_ABI=none \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_DOCS=OFF \
    '-DCMAKE_CXX_FLAGS=-fno-stack-protector -fno-vectorize -fno-slp-vectorize'
cmake --build "$WORK/build" --target cxx_static -j4
# The minimal ABI probe supplies the same pinned LLVM RTTI implementation.
if ! [ -f "$ROOT/build/minecraft/cxxabi/private_typeinfo.o" ]; then
    sh "$ROOT/scripts/build_cxxabi.sh"
fi
clang++ -target x86_64-apple-macos11 -std=c++17 -O1 -g -fno-exceptions \
    -fno-stack-protector -DR2_FULL_LIBCXX -c "$ROOT/ports/cxxabi_support.cpp" -o "$WORK/support.o"
clang++ -target x86_64-apple-macos11 -std=c++17 -O1 -fno-exceptions \
    -fno-stack-protector -fno-vectorize -fno-slp-vectorize -nostdinc++ \
    -I"$WORK/build/include/c++/v1" -I"$SOURCE/libcxxabi-$VERSION.src/include" \
    -I"$SOURCE/libcxx-$VERSION.src/src" \
    -c "$SOURCE/libcxxabi-$VERSION.src/src/cxa_guard.cpp" -o "$WORK/guard.o"
clang++ -target x86_64-apple-macos11 -std=c++17 -O1 -fno-exceptions -fno-stack-protector \
    -I"$SOURCE/libcxxabi-$VERSION.src/include" -I"$SOURCE/libcxxabi-$VERSION.src/src" \
    -c "$ROOT/ports/cxxabi_abort.cpp" -o "$WORK/abort.o"
clang++ -target x86_64-apple-macos11 -dynamiclib -nostdlib++ \
    -Wl,-force_load,"$WORK/build/lib/libc++.a" "$ROOT/build/minecraft/cxxabi/private_typeinfo.o" \
    "$WORK/support.o" "$ROOT/build/minecraft/cxxabi/qsort.o" "$WORK/guard.o" "$WORK/abort.o" \
    -Wl,-no_fixup_chains -Wl,-install_name,@rpath/librozetka2-cxxabi.dylib \
    -o "$WORK/librozetka2-cxxabi.dylib"
file "$WORK/librozetka2-cxxabi.dylib"
