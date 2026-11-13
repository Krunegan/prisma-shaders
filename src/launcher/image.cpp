// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
// What image.hpp does the same on every system; decode_png() is each system's
#include "launcher/image.hpp"

#include <algorithm>
#include <cmath>

namespace prisma::launcher {

Image crop_to(const Image &image, int aspect_w, int aspect_h)
{
	if (image.width <= 0 || image.height <= 0 || aspect_w <= 0 || aspect_h <= 0)
		return image;
	int w = image.width, h = image.height;
	if ((long long)w * aspect_h > (long long)h * aspect_w)
		w = std::max(1, (int)((long long)h * aspect_w / aspect_h));
	else
		h = std::max(1, (int)((long long)w * aspect_h / aspect_w));
	if (w == image.width && h == image.height)
		return image;
	Image out;
	out.width = w;
	out.height = h;
	out.rgba.resize((size_t)w * h * 4);
	int x0 = (image.width - w) / 2, y0 = (image.height - h) / 2;
	for (int y = 0; y < h; y++)
		std::copy_n(&image.rgba[((size_t)(y0 + y) * image.width + x0) * 4], (size_t)w * 4, &out.rgba[(size_t)y * w * 4]);
	return out;
}

Image shrink(const Image &image, int max_width)
{
	if (image.width <= max_width || max_width <= 0)
		return image;
	Image out;
	out.width = max_width;
	out.height = std::max(1, (int)std::lround((double)image.height * max_width / image.width));
	out.rgba.resize((size_t)out.width * out.height * 4);
	const double sx = (double)image.width / out.width, sy = (double)image.height / out.height;
	for (int y = 0; y < out.height; y++) {
		int y0 = (int)(y * sy), y1 = std::max(y0 + 1, std::min(image.height, (int)std::ceil((y + 1) * sy)));
		for (int x = 0; x < out.width; x++) {
			int x0 = (int)(x * sx), x1 = std::max(x0 + 1, std::min(image.width, (int)std::ceil((x + 1) * sx)));
			unsigned sum[4] = {0, 0, 0, 0};
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++) {
					const std::uint8_t *p = &image.rgba[((size_t)yy * image.width + xx) * 4];
					for (int c = 0; c < 4; c++)
						sum[c] += p[c];
				}
			unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
			std::uint8_t *q = &out.rgba[((size_t)y * out.width + x) * 4];
			for (int c = 0; c < 4; c++)
				q[c] = (std::uint8_t)((sum[c] + n / 2) / n);
		}
	}
	return out;
}

} // namespace prisma::launcher
