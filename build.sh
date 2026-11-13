#!/bin/sh
# SPDX-FileCopyrightText: 2026 Flay Krunegan
# SPDX-License-Identifier: GPL-3.0-or-later
# Build Prisma on Linux. Requires clang (or g++), CMake, Ninja, pkg-config,
# and the SDL2 and OpenGL headers (see README.md). On Debian/Ubuntu:
#   sudo apt install clang cmake ninja-build pkg-config libsdl2-dev libgl-dev
set -e
cd "$(dirname "$0")"

for tool in cmake ninja pkg-config; do
	command -v "$tool" >/dev/null || { echo "$tool not found (see README.md)"; exit 1; }
done
pkg-config --exists sdl2 || { echo "SDL2 headers are missing (libsdl2-dev)"; exit 1; }

if [ ! -f build-linux/build.ninja ]; then
	if command -v clang++ >/dev/null; then
		cmake -S . -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
	else
		cmake -S . -B build-linux -G Ninja
	fi
fi
cmake --build build-linux
echo
echo "Built: build-linux/dist/prisma"
