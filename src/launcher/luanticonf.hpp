// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Luanti settings that the active pack sets (pack.toml, [luanti] section).

	Luanti does not allow changing single settings at startup, but it can read
	another configuration file (--config). So the launcher:

	  1. copies the player's minetest.conf into luanti_prisma.conf (next to the
	     launcher) with the pack's settings on top, and starts Luanti with
	     --config pointing at that copy;
	  2. saves how the copy looked at the start (luanti_prisma.start);
	  3. when the game closes, passes to the player's minetest.conf whatever
	     changed during the session (keys, volume...), except the settings the
	     pack sets.

	The player's minetest.conf only receives its own changes: playing without
	Prisma, Luanti keeps its settings as they were. If the launcher did not get
	to step 3 (it was closed by force), it does it at the next startup.

	Shared by the Windows and Linux launchers: paths as std::filesystem::path
	and text in UTF-8.
*/
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace prisma::luanticonf {

// A minetest.conf, line by line, to rewrite it without touching the rest.
struct Conf {
	struct Entry {
		std::string key;   // empty: comment or blank line (goes in `raw`)
		std::string value; // the text after "=" (several lines for """ or { })
		std::string raw;
	};
	std::vector<Entry> entries;

	bool load(const std::filesystem::path &path); // false if it does not exist (stays empty)
	bool save(const std::filesystem::path &path) const;
	void set(const std::string &key, const std::string &value);
	void remove(const std::string &key);
	std::map<std::string, std::string> values() const;
};

struct Session {
	bool active = false;
	std::filesystem::path user_conf;   // the player's minetest.conf
	std::filesystem::path prisma_conf; // the copy with the pack's settings
	std::filesystem::path snapshot;    // the copy as it was at the start
	std::map<std::string, std::string> forced;
};

// Prepares the copy with the `forced` settings (the ones in the [luanti]
// section of the active pack) and adds "--config <copy>" to Luanti's
// arguments (UTF-8). Without settings it does nothing and returns an inactive
// session. `log` receives one line for each thing it does.
Session prepare(const std::filesystem::path &prisma_dir, const std::map<std::string, std::string> &forced,
	const std::filesystem::path &luanti, std::vector<std::string> &args, std::vector<std::string> &log);

// After the game closes: passes the session's changes to the player's
// minetest.conf. It also picks up a previous session that was left unfinished.
void finish(const std::filesystem::path &prisma_dir, std::vector<std::string> &log);

} // namespace prisma::luanticonf
