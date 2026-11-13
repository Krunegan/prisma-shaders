// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/luanti_baseline.hpp"
#include "common/toml.hpp"
#include "baseline_embedded.hpp" // generated: prisma::embedded::baseline_toml

namespace prisma::luanti {

static std::vector<std::string> strings(const toml::Document &doc, const std::string &key)
{
	std::vector<std::string> out;
	if (const toml::Value *v = doc.get(key))
		for (const toml::Value &item : v->items)
			out.push_back(item.str);
	return out;
}

static std::vector<bool> flags(const toml::Document &doc, const std::string &key)
{
	std::vector<bool> out;
	if (const toml::Value *v = doc.get(key))
		for (const toml::Value &item : v->items)
			out.push_back(item.boolean);
	return out;
}

Baseline parse_baseline(const std::string &text)
{
	Baseline b;
	toml::Document doc;
	if (!toml::parse(text, doc, b.error))
		return b;
	if (const toml::Value *v = doc.get("luanti"))
		b.version = v->str;

	auto pairs = [&](const std::string &a, const std::string &c, auto &out) {
		std::vector<std::string> first = strings(doc, a), second = strings(doc, c);
		if (first.size() != second.size())
			b.error = a + " and " + c + " have different lengths";
		for (size_t i = 0; i < first.size() && i < second.size(); i++)
			out.emplace_back(first[i], second[i]);
	};
	pairs("shaders.files", "shaders.sha256", b.shaders);
	pairs("anchors.files", "anchors.lines", b.anchors);

	std::vector<std::string> names = strings(doc, "uniforms.names"), files = strings(doc, "uniforms.files");
	std::vector<bool> required = flags(doc, "uniforms.required");
	if (names.size() != files.size() || names.size() != required.size())
		b.error = "the [uniforms] lists have different lengths";
	for (size_t i = 0; i < names.size() && i < files.size() && i < required.size(); i++)
		b.uniforms.push_back({names[i], files[i], required[i]});

	names = strings(doc, "programs.names");
	required = flags(doc, "programs.required");
	if (names.size() != required.size())
		b.error = "the [programs] lists have different lengths";
	for (size_t i = 0; i < names.size() && i < required.size(); i++)
		b.programs.push_back({names[i], required[i]});

	b.sdl_imports = strings(doc, "imports.sdl");
	b.gl_imports = strings(doc, "imports.gl");
	return b;
}

const Baseline &baseline()
{
	static const Baseline b = parse_baseline(embedded::baseline_toml);
	return b;
}

} // namespace prisma::luanti
