// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SHA-256 (FIPS 180-4), to compare Luanti's files with a baseline
// (src/common/luanti/). Not used for anything secret.
#include <string>
#include <string_view>

namespace prisma {

// Lowercase hexadecimal digest of `data`
std::string sha256(std::string_view data);

} // namespace prisma
