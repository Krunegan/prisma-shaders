// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	What Prisma relies on in the supported Luanti version:
	src/common/luanti/<version>/baseline.toml, generated from Luanti's
	sources and embedded at build time. Read by
	prisma_check --luanti (an installation against it) and by the core's
	report in the game (core/tracking/compat.hpp).
*/
#include <string>
#include <utility>
#include <vector>

namespace prisma::luanti {

struct Uniform {
	std::string name;
	std::string file;      // a client/shaders file that declares it; "" if none does
	bool required = false; // sent whenever the world is drawn
};

struct Program {
	std::string name;      // Luanti shader, as the shader catalog names it
	bool required = false; // always there while the world is drawn
};

struct Baseline {
	std::string version; // "5.17.0"
	std::vector<std::pair<std::string, std::string>> shaders; // client/shaders file, SHA-256 of its text
	std::vector<std::pair<std::string, std::string>> anchors; // client/shaders file, line a patch anchors to
	std::vector<Uniform> uniforms;
	std::vector<Program> programs;
	std::vector<std::string> sdl_imports, gl_imports; // what luanti.exe imports and Prisma intercepts
	std::string error; // why the embedded file could not be read; "" if it was
};

// The baseline of the supported version, read once.
const Baseline &baseline();

// Reads a baseline from the text of a baseline.toml.
Baseline parse_baseline(const std::string &text);

} // namespace prisma::luanti
