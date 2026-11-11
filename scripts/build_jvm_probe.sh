#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JDK=${1:-${R2_JDK_HOME:-/Library/Java/JavaVirtualMachines/jdk-25-x64.jdk/Contents/Home}}
WORK="$ROOT/build/minecraft"
mkdir -p "$WORK"
case "$(file "$JDK/lib/server/libjvm.dylib")" in
    *x86_64*) ;;
    *) echo "Expected a macOS x86_64 JDK 25 at: $JDK" >&2; exit 1 ;;
esac
if ! grep -q 'JNI_VERSION_24' "$JDK/include/jni.h"; then
    echo "The JVM probe requires JDK 25 headers (JNI_VERSION_24)." >&2
    exit 1
fi
clang -target x86_64-apple-macos11 -O1 -g -fno-stack-protector \
    -fno-vectorize -fno-slp-vectorize \
    -I"$JDK/include" -I"$JDK/include/darwin" \
    "$ROOT/ports/jvm_probe.c" -L"$JDK/lib/server" -ljvm \
    -Wl,-rpath,"$JDK/lib/server" -Wl,-no_fixup_chains \
    -o "$WORK/jvm-probe-x86_64"
clang -target x86_64-apple-macos11 -O1 -g -fno-stack-protector \
    -fno-vectorize -fno-slp-vectorize \
    -I"$JDK/include" -I"$JDK/include/darwin" \
    "$ROOT/ports/java_launcher.c" -L"$JDK/lib/server" -ljvm \
    -Wl,-rpath,"$JDK/lib/server" -Wl,-no_fixup_chains \
    -o "$WORK/java-x86_64"
printf '%s\n' "$JDK" > "$WORK/jdk-home.txt"
file "$WORK/jvm-probe-x86_64"
