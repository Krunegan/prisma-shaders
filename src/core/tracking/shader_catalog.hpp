// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Recognises which of Luanti's own shader files a compiled GL shader came from.

	Luanti builds every shader as `<generated header> + <file contents>`, so the
	header changes with settings and material type while the body is the file
	verbatim. We compare line sets rather than looking for an exact substring so
	that a slightly different header layout (or a future engine version that
	splices things in) does not break recognition.
*/
#include <filesystem>
#include <string>
#include <string_view>

namespace prisma::catalog {

struct Match {
	std::string name;   // e.g. "nodes_shader", "shadow/pass1", "Irrlicht/Solid"
	std::string stage;  // "vertex", "fragment" or ""
	float score = 0.f;  // fraction of the file's lines found in the source
};

// Where Luanti's shader files are. They are scanned on the first lookup.
void set_dir(const std::filesystem::path &shaders_dir);
size_t size();

// `stage` is "vertex" or "fragment"; only files of that stage are considered.
Match identify(std::string_view source, std::string_view stage);

} // namespace prisma::catalog
