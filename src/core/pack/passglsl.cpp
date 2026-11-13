// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pack/passglsl.hpp"
#include "core/pack/api.hpp"
#include "core/pack/options.hpp"
#include "glsl_embedded.hpp"

namespace prisma::passglsl {

// `text` with every `marker` replaced by `value` (the @...@ markers of the
// templates in glsl/internal/)
static std::string replace(std::string text, const std::string &marker, const std::string &value)
{
	for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size()))
		text.replace(at, marker.size(), value);
	return text;
}

// Prepended to every pass, after the shared API block. Keep in sync with
// docs/shaderpacks.md.
static const char *k_pass_header = embedded::pass_header_glsl;

const char *const VERTEX = embedded::pass_vert;

// The declarations, without #version and the #defines that only Prisma sets
static std::string declarations(const std::vector<pack::Buffer> &buffers)
{
	std::string out = api::GLSL + options::glsl() + k_pass_header + api::SHADOW_GLSL;
	if (!buffers.empty()) {
		out += "\n// ---- Buffers of the pack (pack.toml [buffers.*]) ----\n";
		for (const pack::Buffer &b : buffers)
			out += "uniform sampler2D " + b.id + ";\n";
	}
	return out;
}

static std::string defines(bool depth_zero_to_one, bool compute)
{
	return std::string("#define PRISMA 1\n#define PRISMA_API ") + std::to_string(api::VERSION) +
		"\n#define PRISMA_COMPUTE " + (compute ? "1" : "0") + "\n#define PRISMA_DEPTH_ZERO_TO_ONE " +
		(depth_zero_to_one ? "1" : "0") + "\n";
}

std::string fragment_header(bool depth_zero_to_one, const std::vector<pack::Buffer> &buffers, bool compute)
{
	return "#version 150\n" + defines(depth_zero_to_one, compute) + declarations(buffers) +
		"in vec2 texcoord;\nout vec4 fragColor;\n";
}

static const char *image_format(const std::string &format)
{
	return format == "rgba8" ? "rgba8" : format == "rgba32f" ? "rgba32f" : "rgba16f";
}

std::string compute_header(bool depth_zero_to_one, const std::vector<pack::Buffer> &buffers, const pack::Pass &pass)
{
	std::string format = "rgba16f";
	for (const pack::Buffer &b : buffers)
		if (b.id == pass.target)
			format = image_format(b.format);
	// The helpers that use gl_FragCoord (the shadow lookup's noise) get the
	// centre of this invocation's pixel instead
	std::string out = "#version 430\n" + defines(depth_zero_to_one, true) +
		"#define gl_FragCoord vec4(vec2(gl_GlobalInvocationID.xy) + 0.5, 0.0, 1.0)\n" + declarations(buffers);
	out += replace(embedded::compute_glsl, "@IMAGE_FORMAT@", format);
	// Workgroups of 8x8 unless the pass says otherwise
	if (pass.code.code.find("local_size_x") == std::string::npos)
		out += "layout(local_size_x = 8, local_size_y = 8) in;\n";
	return out;
}

std::string editor_stub(const pack::Pack &pack)
{
	std::string out = replace(embedded::editor_stub_glsl, "@PACK@", pack.name);
	out = replace(out, "@API@", std::to_string(api::VERSION));
	return replace(out, "@DECLARATIONS@", declarations(pack.buffers));
}

} // namespace prisma::passglsl
