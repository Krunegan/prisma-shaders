// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	prisma (Linux) — starts Luanti with libprisma_core.so preloaded into it.

	Nothing of the game is touched: Luanti is started with LD_PRELOAD naming
	the core, which intercepts the SDL and OpenGL symbols the game uses (see
	src/core/platform/platform_linux.cpp). The core tells the launcher it got in by
	writing a byte to a pipe (PRISMA_READY_FD).

	If the active pack fixes some of Luanti's settings (pack.toml, [luanti]),
	the game runs with a copy of the player's minetest.conf that has them, and
	the launcher waits for it to close to copy back whatever else the player
	changed (see luanticonf.hpp).

	Usage:
	    prisma                          shows the configuration window, unless prisma.ini says show_window=0
	    prisma --configure              shows the window
	    prisma <path/to/luanti>         starts the game right away with that path, and remembers it
	    prisma [<path>] -- <args>       starts it right away; everything after -- goes to Luanti
*/
#include "launcher/launcher.hpp"
#include "launcher/luanticonf.hpp"
#include "common/ini.hpp"
#include "common/util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

extern char **environ;

using namespace prisma;
using namespace prisma::launcher;
namespace fs = std::filesystem;

static void say(const std::string &text)
{
	std::cout << text << std::endl;
}

// The launcher's own log (logs/launcher.log): what happens after the game
// starts may have no terminal to go to.
static void log_lines(const fs::path &dir, const std::vector<std::string> &lines)
{
	if (lines.empty())
		return;
	std::error_code ec;
	fs::create_directories(dir / "logs", ec);
	std::ofstream f(dir / "logs" / "launcher.log", std::ios::app);
	std::time_t now = std::time(nullptr);
	char stamp[32];
	std::strftime(stamp, sizeof stamp, "[%Y-%m-%d %H:%M] ", std::localtime(&now));
	for (const std::string &l : lines)
		f << stamp << l << "\n";
}

static void say_and_log(const fs::path &dir, const std::vector<std::string> &lines)
{
	for (const std::string &l : lines)
		say(l);
	log_lines(dir, lines);
}

static int fail(const std::string &text)
{
	std::cerr << text << std::endl;
	return 1;
}

static bool executable(const std::string &path)
{
	return is_executable(path);
}

int main(int argc, char **argv)
{
	const fs::path here = host_exe_dir();
	const fs::path ini = here / "prisma.ini";
	const fs::path core = here / "libprisma_core.so";
	if (!fs::exists(core))
		return fail("Missing " + core.string() + " beside prisma");

	// ---- arguments
	bool configure = false, has_args = false;
	std::string luanti;
	std::vector<std::string> passthrough;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--configure") {
			configure = true;
			continue;
		}
		has_args = true;
		if (a == "--") {
			for (i++; i < argc; i++)
				passthrough.push_back(argv[i]);
			break;
		}
		if (luanti.empty() && executable(a))
			luanti = a;
		else
			passthrough.push_back(a);
	}

	Config config;
	config.load(ini);
	if (config.luanti.empty())
		config.luanti = find_luanti();

	// ---- the configuration window: when started without arguments (unless
	// prisma.ini says show_window=0), or if Luanti can't be found
	const std::string &wanted = luanti.empty() ? config.luanti : luanti;
	if (configure || (!has_args && (config.show_window || !executable(wanted)))) {
		if (run_window(here, ini, config) != WindowResult::Play)
			return 0;
		luanti = config.luanti;
		passthrough = split_arguments(config.arguments);
	} else {
		if (luanti.empty())
			luanti = config.luanti;
		if (!has_args)
			passthrough = split_arguments(config.arguments);
		if (!executable(luanti))
			return fail(luanti.empty() ? "Luanti not found: give its path (prisma /path/to/luanti) or "
										 "open it with prisma --configure" :
										 "Not found: " + luanti);
	}
	luanti = fs::absolute(luanti).string();
	ini::set(ini, "prisma", "luanti", luanti);
	say("Game: " + luanti);
	if (std::string warning = version_warning(luanti_version(luanti)); !warning.empty())
		say_and_log(here, {warning});

	// ---- Luanti's settings fixed by the pack. A previous session that never
	// got to copy back the player's changes (the launcher was killed) is
	// finished first.
	std::vector<std::string> lines;
	luanticonf::finish(here, lines);
	std::map<std::string, std::string> forced;
	if (config.apply_luanti_settings)
		forced = luanti_settings(read_pack(config.packs_folder(here) / config.pack, config.pack));
	luanticonf::Session settings = luanticonf::prepare(here, forced, luanti, passthrough, lines);
	say_and_log(here, lines);
	lines.clear();

	// ---- start it with the core preloaded
	int ready[2];
	if (pipe2(ready, O_CLOEXEC) != 0)
		return fail(std::string("pipe: ") + std::strerror(errno));
	fcntl(ready[1], F_SETFD, 0); // the game keeps the end it writes to

	std::vector<std::string> env_text;
	std::string preload = core.string();
	for (char **e = environ; *e; e++) {
		std::string v = *e;
		if (v.starts_with("LD_PRELOAD="))
			preload += ":" + v.substr(11);
		else if (!v.starts_with("PRISMA_READY_FD="))
			env_text.push_back(v);
	}
	env_text.push_back("LD_PRELOAD=" + preload);
	env_text.push_back("PRISMA_READY_FD=" + std::to_string(ready[1]));
	std::vector<char *> envp;
	for (std::string &v : env_text)
		envp.push_back(v.data());
	envp.push_back(nullptr);

	std::vector<std::string> args_text = {luanti};
	args_text.insert(args_text.end(), passthrough.begin(), passthrough.end());
	std::vector<char *> args;
	for (std::string &a : args_text)
		args.push_back(a.data());
	args.push_back(nullptr);

	pid_t pid = 0;
	int err = posix_spawn(&pid, luanti.c_str(), nullptr, nullptr, args.data(), envp.data());
	close(ready[1]);
	if (err != 0) {
		close(ready[0]);
		luanticonf::finish(here, lines); // nothing changed: just tidy up
		return fail("Could not start the game: " + std::string(std::strerror(err)));
	}

	// The core answers when the game first uses SDL (before its window opens);
	// if the game ends first, the pipe closes with nothing in it.
	char answer = 0;
	pollfd p{ready[0], POLLIN, 0};
	bool hooked = poll(&p, 1, 60000) > 0 && read(ready[0], &answer, 1) == 1 && answer == '1';
	close(ready[0]);

	int result = 0;
	if (!hooked) {
		result = fail(answer == '0' ? "Prisma loaded but could not hook into OpenGL. Check " +
					(here / "logs" / "prisma.log").string() + "\nThe game was opened anyway." :
					"Prisma did not get to load in the game (does it use SDL2? is it a Flatpak or a Snap?). The game "
					"was opened anyway, without Prisma.");
	} else {
		say("Prisma is active. Log: " + (here / "logs" / "prisma.log").string());
	}

	if (settings.active) {
		say("When the game closes, the settings you change will be saved to your minetest.conf.");
		waitpid(pid, nullptr, 0);
		luanticonf::finish(here, lines);
		log_lines(here, lines);
	}
	return result;
}
