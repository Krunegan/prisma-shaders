// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Developer mode: `[debug] developer_mode = 1` in
	prisma.ini. By default the core does only what a player needs; developer
	mode adds the tools for pack authors and for Prisma's own development:
	reload on save, the frame trace, and the dumps of the game's shaders and
	GL functions (core/dev/trace.hpp).
*/
#include <filesystem>

namespace prisma::developer {

// Reads prisma.ini. Called once at startup, before anything asks enabled().
void init(const std::filesystem::path &ini);

bool enabled();

} // namespace prisma::developer
