// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Prisma's launcher (prisma.exe on Windows, prisma on Linux): what the two
	systems share. The configuration and the packs (launcher.cpp), the
	configuration window (ui.cpp, with Dear ImGui) and what each system
	provides for it (window_win32.cpp, window_sdl.cpp). Text in UTF-8, paths
	as std::filesystem::path.

	prisma.ini, section [prisma]:
	    luanti=                 path of the Luanti executable
	    arguments=              arguments for Luanti (optional, e.g. --go --worldname world)
	    pack=                   active pack (folder)
	    packs=                  packs folder (by default, shaderpacks next to the launcher)
	    apply_luanti_settings=  0: don't apply the Luanti settings the pack asks for
	    show_window=            0: open the game directly, without the configuration window
	section [keys]: menu=, toggle=
	section [debug]: packs_source=  a folder laid out as prisma-packs, read
	                                instead of GitHub (downloads.hpp)
*/
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace prisma::launcher {

struct Config {
	std::string luanti;
	std::string arguments;
	std::string pack;          // empty: none chosen (the first installed one is used)
	std::string packs_dir;     // empty: shaderpacks next to the launcher
	std::string menu_key = "Home";
	std::string toggle_key = "End";
	bool apply_luanti_settings = true;
	bool show_window = true;

	void load(const std::filesystem::path &ini);
	void save(const std::filesystem::path &ini) const;
	// The packs folder in use: the chosen one or the default
	std::filesystem::path packs_folder(const std::filesystem::path &prisma_dir) const;
	bool operator==(const Config &) const = default;
};

struct PackInfo {
	std::string folder; // folder name (what goes in pack=)
	std::string title;  // name from pack.toml, or the folder
	std::string description;
	std::string author, version;
	std::vector<std::pair<std::string, std::string>> luanti; // [luanti] section, in its order
	std::string error;                                       // pack.toml could not be read
};

std::vector<PackInfo> list_packs(const std::filesystem::path &folder);
PackInfo read_pack(const std::filesystem::path &pack_dir, const std::string &folder_name);
// The [luanti] settings of a pack, as luanticonf wants them
std::map<std::string, std::string> luanti_settings(const PackInfo &pack);

// Luanti in the PATH (luanti, or minetest in old versions), or empty
std::string find_luanti();
// Whether `path` is an executable that exists
bool is_executable(const std::string &path);

// Splits a line of arguments the way the system console would
std::vector<std::string> split_arguments(const std::string &line);

// Whether it is a key that Luanti also uses (letters, numbers, F1-F12)
bool key_used_by_luanti(const std::string &name);

// The version of a Luanti executable ("5.17.0"), or "" if unknown. Each
// system implements it (window_win32.cpp, window_sdl.cpp).
std::string luanti_version(const std::string &luanti);
// What to tell the player about that version: "" for the supported one
// (PRISMA_LUANTI_VERSION) or an unknown one, a warning for any other
std::string version_warning(const std::string &version);

// ---------------------------------------------------------------- the window

enum class WindowResult { Play, Quit };

// What the window needs from the system
struct Platform {
	// Dialog to choose the Luanti executable / a folder; "" if cancelled
	std::function<std::string(const std::string &current)> choose_luanti;
	std::function<std::string(const std::string &current)> choose_folder;
	// The version of that Luanti ("5.17.0"), or "" if unknown
	std::function<std::string(const std::string &luanti)> luanti_version;
	std::function<void(const std::filesystem::path &folder)> open_folder;
};

// Shows the window until the player presses Play or closes it. `config`
// ends up with the choices; Play and Save write them to `ini`. Each system
// implements it (window_win32.cpp, window_sdl.cpp) with the ui.hpp interface.
WindowResult run_window(const std::filesystem::path &prisma_dir, const std::filesystem::path &ini, Config &config);

} // namespace prisma::launcher
