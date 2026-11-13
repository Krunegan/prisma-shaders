// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
// decode_png() with the system's libpng (Luanti itself needs it), through
// its simplified API, opened when it is first used
#include "launcher/image.hpp"

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>

namespace prisma::launcher {

// png_image of png.h (libpng 1.6), PNG_IMAGE_VERSION 1
struct PngImage {
	void *opaque;
	std::uint32_t version, width, height, format, flags, colormap_entries, warning_or_error;
	char message[64];
};
constexpr std::uint32_t k_png_image_version = 1;
constexpr std::uint32_t k_png_format_rgba = 3; // PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA

struct Png {
	void *lib = nullptr;
	int (*begin_read_from_memory)(PngImage *, const void *, std::size_t) = nullptr;
	int (*finish_read)(PngImage *, const void *, void *, std::int32_t, void *) = nullptr;
	void (*free_image)(PngImage *) = nullptr;
};

static const Png *load_png()
{
	static Png p;
	static bool tried = false;
	if (tried)
		return p.lib ? &p : nullptr;
	tried = true;
	for (const char *name : {"libpng16.so.16", "libpng16.so", "libpng.so"})
		if ((p.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
			break;
	if (!p.lib)
		return nullptr;
	p.begin_read_from_memory = reinterpret_cast<decltype(p.begin_read_from_memory)>(dlsym(p.lib, "png_image_begin_read_from_memory"));
	p.finish_read = reinterpret_cast<decltype(p.finish_read)>(dlsym(p.lib, "png_image_finish_read"));
	p.free_image = reinterpret_cast<decltype(p.free_image)>(dlsym(p.lib, "png_image_free"));
	if (!p.begin_read_from_memory || !p.finish_read || !p.free_image) {
		dlclose(p.lib);
		p.lib = nullptr;
		return nullptr;
	}
	return &p;
}

bool decode_png(const std::string &bytes, Image &image, std::string &error)
{
	image = Image();
	static const char signature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
	if (bytes.size() < 8 || bytes.compare(0, 8, signature, 8) != 0) {
		error = "not a PNG image";
		return false;
	}
	const Png *png = load_png();
	if (!png) {
		error = "libpng was not found (install the libpng16 package)";
		return false;
	}
	PngImage img = {};
	img.version = k_png_image_version;
	if (!png->begin_read_from_memory(&img, bytes.data(), bytes.size())) {
		error = "this PNG could not be read";
		return false;
	}
	if (img.width == 0 || img.height == 0 || img.width > (std::uint32_t)k_max_image_side ||
		img.height > (std::uint32_t)k_max_image_side) {
		png->free_image(&img);
		error = "the image is bigger than " + std::to_string(k_max_image_side) + " pixels on a side";
		return false;
	}
	img.format = k_png_format_rgba;
	image.width = (int)img.width;
	image.height = (int)img.height;
	image.rgba.resize((std::size_t)img.width * img.height * 4);
	if (!png->finish_read(&img, nullptr, image.rgba.data(), (std::int32_t)(img.width * 4), nullptr)) {
		png->free_image(&img);
		image = Image();
		error = "this PNG could not be read";
		return false;
	}
	return true;
}

} // namespace prisma::launcher
