// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Keyboard and mouse, taken from Luanti's SDL event loop (SDL_PollEvent is
	detoured in SDL2.dll).

	  * Prisma's keys (menu, on/off) are configurable in prisma.ini:
	        [keys]
	        menu = Home
	        toggle = End
	    and are never seen by the game.
	  * While the menu is open, mouse and keyboard go to the menu only (key
	    releases still reach the game, so nothing stays pressed), and Luanti's
	    attempts to capture or move the cursor are held back until it closes.
*/
#include <filesystem>
#include <string>

namespace prisma::input {

bool install(const std::filesystem::path &ini);

bool menu_open();
void set_menu_open(bool open);

// Display names of the configured keys, for the menu.
const std::string &menu_key_name();
const std::string &toggle_key_name();

} // namespace prisma::input
