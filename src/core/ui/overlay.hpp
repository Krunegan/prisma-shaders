// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The in-game menu (Dear ImGui), drawn on top of the finished frame right
	before the buffer swap. Input comes from input.cpp.
*/
#include <filesystem>
#include <string>

namespace prisma::overlay {

// `fonts_dir` is Luanti's fonts folder: the menu uses one of them (with accents).
void init(const std::filesystem::path &fonts_dir);

void set_visible(bool visible);
bool visible();

// Draws the menu if it is open. On the GL thread, context current.
void render(void *window);

// Input, in window coordinates / SDL scancodes
void mouse_move(float x, float y);
void mouse_button(int sdl_button, bool down);
void mouse_wheel(float x, float y);
void key(int scancode, bool down, unsigned mods);
void text(const char *utf8);

} // namespace prisma::overlay
