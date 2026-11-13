// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	prisma_check --luanti <folder>: compares a Luanti installation (or source
	checkout) with the baseline of the supported version
	(common/luanti_baseline.hpp) and says exactly what differs: shader files
	changed or missing, patch anchors no longer found, uniforms no longer
	declared, and functions luanti.exe no longer imports. Files the baseline
	does not have are listed but not counted.
*/
#include "common/luanti_baseline.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace prisma::luanti {

struct Report {
	std::vector<std::string> lines; // what to print, one per line
	int differences = 0;            // 0: the same as the baseline
};

Report compare_installation(const std::filesystem::path &folder, const Baseline &baseline);

// The import table of a Windows executable: {lowercase DLL name: imported
// names}. Empty, with `error` set, if the file is not a PE executable.
std::map<std::string, std::vector<std::string>> pe_imports(const std::filesystem::path &exe, std::string &error);

} // namespace prisma::luanti
