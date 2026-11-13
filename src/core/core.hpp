// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace prisma {

// Sets Prisma up inside the game. Called once by the platform's entry point
// (platform_win32.cpp, platform_linux.cpp).
void start();

} // namespace prisma
