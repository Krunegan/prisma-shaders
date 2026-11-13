// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	PNG images for the launcher (a pack's screenshots), decoded by the
	system: WIC on Windows (image_win32.cpp), the system's libpng opened at
	run time on Linux (image_linux.cpp). Nothing to ship for it.
*/
#include <cstdint>
#include <string>
#include <vector>

namespace prisma::launcher {

struct Image {
	int width = 0, height = 0;
	std::vector<std::uint8_t> rgba; // width * height * 4, rows from the top
};

// Bigger images are refused (a screenshot needs no more)
inline constexpr int k_max_image_side = 4096;

// Decodes a PNG file held in `bytes`. False (and `error`) if it is not a PNG
// this system can read, or is bigger than k_max_image_side on a side.
bool decode_png(const std::string &bytes, Image &image, std::string &error);

// The middle of the image with the shape aspect_w : aspect_h (the sides,
// or the top and bottom, cut off)
Image crop_to(const Image &image, int aspect_w, int aspect_h);

// A copy at most `max_width` wide (and as tall as the aspect asks), each
// pixel the average of those it covers; the image itself if it is smaller
Image shrink(const Image &image, int max_width);

} // namespace prisma::launcher
