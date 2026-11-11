#!/bin/sh
# Builds the AutoHDR Vulkan layer (x86_64 + x86) into ../bin.
#
# zig is recommended: it targets glibc 2.28, so the library loads inside the
# Steam Linux Runtime used by Proton. gcc works too, but the result depends on
# your system's glibc version.
#
#   ZIG=/path/to/zig VULKAN_HEADERS=/path/to/Vulkan-Headers/include sh layer/build.sh
#
# Requirements: glslangValidator (glslang), Vulkan headers, and zig or gcc (+ multilib).
set -e
cd "$(dirname "$0")"

VK_INC="${VULKAN_HEADERS:-/usr/include}"
ZIG="${ZIG:-zig}"

glslangValidator -V --target-env vulkan1.0 --vn autohdr_spv -o autohdr_spv.h autohdr.comp
mkdir -p ../bin
FLAGS="-shared -fPIC -O2 -std=c11 -fvisibility=hidden -Wall -I. -I$VK_INC"

if command -v "$ZIG" >/dev/null 2>&1; then
    "$ZIG" cc -target x86_64-linux-gnu.2.28 $FLAGS -o ../bin/libautohdr_layer_x86_64.so autohdr_layer.c -lpthread -lm
    "$ZIG" cc -target x86-linux-gnu.2.28 $FLAGS -o ../bin/libautohdr_layer_x86.so autohdr_layer.c -lpthread -lm
else
    echo "zig not found: building with gcc (may require a newer glibc than the Steam Runtime)" >&2
    gcc $FLAGS -o ../bin/libautohdr_layer_x86_64.so autohdr_layer.c -lpthread -lm
    gcc -m32 $FLAGS -o ../bin/libautohdr_layer_x86.so autohdr_layer.c -lpthread -lm
fi
echo "OK: bin/libautohdr_layer_x86_64.so bin/libautohdr_layer_x86.so"
