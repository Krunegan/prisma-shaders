@echo off
rem SPDX-FileCopyrightText: 2026 Flay Krunegan
rem SPDX-License-Identifier: GPL-3.0-or-later
setlocal
rem Build Prisma. Requires LLVM-MinGW, CMake, and Ninja on PATH (see README.md).
cd /d "%~dp0"

where clang++ >nul 2>nul || (echo clang++ not found. Install LLVM-MinGW: winget install MartinStorsjo.LLVM-MinGW.UCRT & exit /b 1)
where cmake >nul 2>nul || (echo cmake not found. Install CMake: winget install Kitware.CMake & exit /b 1)
where ninja >nul 2>nul || (echo ninja not found. Install Ninja: winget install Ninja-build.Ninja & exit /b 1)

if not exist build\build.ninja (
	cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
)
cmake --build build || exit /b 1
echo.
echo Built: build\dist\prisma.exe
