// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dev/developer.hpp"
#include "common/ini.hpp"
#include "common/log.hpp"

namespace prisma::developer {

static bool g_enabled = false;

void init(const std::filesystem::path &ini)
{
	g_enabled = ini::get_int(ini, "debug", "developer_mode", 0) != 0;
	if (g_enabled)
		log::info("Developer mode: reload on save, frame traces, and shader dumps in logs/shaders/");
}

bool enabled()
{
	return g_enabled;
}

} // namespace prisma::developer
