// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The core's start: runs inside the game as soon as it is loaded, before
	the game has created any window or GL context (Windows: DllMain of
	prisma_core.dll, injected while luanti.exe is still suspended; Linux: the
	constructor of libprisma_core.so, preloaded with LD_PRELOAD).
*/
#include "core/core.hpp"
#include "core/dev/developer.hpp"
#include "core/dev/hookcost.hpp"
#include "core/dev/trace.hpp"
#include "core/render/frame.hpp"
#include "core/hooks/hooks.hpp"
#include "common/ini.hpp"
#include "core/ui/input.hpp"
#include "common/log.hpp"
#include "core/ui/overlay.hpp"
#include "core/render/pipeline.hpp"
#include "core/platform/platform.hpp"
#include "core/tracking/tracker.hpp"
#include "common/util.hpp"
#include "common/version.hpp"

#include <filesystem>

namespace prisma {

void start()
{
	namespace fs = std::filesystem;
	const fs::path base = core_dir();
	const fs::path ini = base / "prisma.ini";

	fs::path out_dir = base / "logs";
	std::error_code ec;
	fs::create_directories(out_dir, ec);
	log::open(out_dir / "prisma.log");
	log::info("Prisma %s loaded in process %lu", PRISMA_VERSION, platform::process_id());

	// [debug] measure_only = 1: the game as it is, with only a frame counter
	if (ini::get_int(ini, "debug", "measure_only", 0) != 0) {
		bool ok = hooks::install_frame_counter();
		log::info("Measure only: Prisma counts frames and does nothing else");
		platform::report_ready(ok);
		return;
	}

	const fs::path game_dir = platform::game_data_dir();
	log::info("Game folder: %s", to_utf8(game_dir).c_str());
	developer::init(ini);
	hookcost::init(developer::enabled());
	tracker::init(game_dir / "client" / "shaders");
	trace::init(out_dir);
	pipeline::init(base);
	frame::init(ini);

	overlay::init(game_dir / "fonts");
	hooks::set_modern_context(ini::get_int(ini, "prisma", "modern_context", 1) != 0);
	bool ok = hooks::install();
	if (ok) {
		log::info("OpenGL hooks installed");
		// The menu and keys are optional: Prisma works without them
		input::install(ini);
	} else {
		log::error("Installing the hooks failed; the game will run without Prisma");
	}
	platform::report_ready(ok);
}

} // namespace prisma
