// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check/luanti_check.hpp"
#include "common/sha256.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace prisma::luanti {

static bool read_file(const fs::path &p, std::string &out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	std::ostringstream s;
	s << in.rdbuf();
	out = s.str();
	return true;
}

// The text with \n line endings, as the baseline hashes it
static std::string without_cr(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	for (char c : text)
		if (c != '\r')
			out += c;
	return out;
}

static bool has_word(const std::string &text, const std::string &word)
{
	auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
	for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
		if ((at == 0 || !ident(text[at - 1])) && (at + word.size() >= text.size() || !ident(text[at + word.size()])))
			return true;
	return false;
}

// The product version in an executable's resources ("5.17.0"); "" if none.
// Only Windows builds of Luanti carry one.
static std::string exe_version(const fs::path &exe)
{
#ifdef _WIN32
	DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
	std::vector<char> data(size);
	VS_FIXEDFILEINFO *fixed = nullptr;
	UINT len = 0;
	if (size && GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) &&
			VerQueryValueW(data.data(), L"\\", (void **)&fixed, &len) && fixed && fixed->dwProductVersionMS)
		return std::to_string(HIWORD(fixed->dwProductVersionMS)) + "." +
			std::to_string(LOWORD(fixed->dwProductVersionMS)) + "." + std::to_string(HIWORD(fixed->dwProductVersionLS));
#else
	(void)exe;
#endif
	return "";
}

Report compare_installation(const fs::path &folder, const Baseline &b)
{
	Report r;
	auto line = [&](const std::string &text) { r.lines.push_back(text); };
	auto differ = [&](const std::string &text) {
		r.lines.push_back("  " + text);
		r.differences++;
	};
	if (!b.error.empty()) {
		differ("the baseline cannot be read: " + b.error);
		return r;
	}

	// ---- client/shaders
	const fs::path shaders = folder / "client" / "shaders";
	std::map<std::string, std::string> texts; // file -> text without \r
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(shaders, ec); !ec && it != fs::recursive_directory_iterator();
			it.increment(ec)) {
		std::string text;
		if (it->is_regular_file() && read_file(it->path(), text))
			texts[to_utf8_generic(fs::relative(it->path(), shaders))] = without_cr(text);
	}
	if (texts.empty()) {
		differ("no client/shaders/ in " + to_utf8(folder) + ": is it a Luanti folder?");
		return r;
	}
	int same = 0;
	std::set<std::string> known;
	std::vector<std::string> changed, missing, added;
	for (const auto &[file, hash] : b.shaders) {
		known.insert(file);
		auto t = texts.find(file);
		if (t == texts.end())
			missing.push_back(file);
		else if (sha256(t->second) != hash)
			changed.push_back(file);
		else
			same++;
	}
	for (const auto &[file, text] : texts)
		if (!known.count(file))
			added.push_back(file);
	line("Shader files: " + std::to_string(same) + " of " + std::to_string(b.shaders.size()) + " as in Luanti " +
		b.version);
	for (const std::string &f : changed)
		differ("changed: " + f);
	for (const std::string &f : missing)
		differ("missing: " + f);
	// Not a difference by itself: Luanti only loads the files its code names,
	// and an installation updated in place keeps those of older versions. A
	// new version that uses a new file also changes others.
	if (!added.empty())
		line("Files not in Luanti " + b.version + " (left by an older installation, or new), not counted:");
	for (const std::string &f : added)
		line("  extra: " + f);

	// ---- lines the packs' patches anchor to
	int found = 0;
	for (const auto &[file, anchor] : b.anchors) {
		auto t = texts.find(file);
		if (t != texts.end() && t->second.find(anchor) != std::string::npos)
			found++;
		else
			differ("anchor not found in " + file + ": " + anchor);
	}
	line("Patch anchors: " + std::to_string(found) + " of " + std::to_string(b.anchors.size()) + " found");

	// ---- uniforms Prisma reads, where client/shaders declares them
	int declared = 0, checkable = 0;
	for (const Uniform &u : b.uniforms) {
		if (u.file.empty())
			continue; // declared by Luanti's C++: only the report in the game can check it
		checkable++;
		bool any = std::any_of(texts.begin(), texts.end(), [&](const auto &t) { return has_word(t.second, u.name); });
		if (any)
			declared++;
		else
			differ("uniform no longer in client/shaders: " + u.name + " (was in " + u.file + ")");
	}
	line("Uniforms Prisma reads: " + std::to_string(declared) + " of " + std::to_string(checkable) +
		" still in client/shaders");

	// ---- luanti.exe's import table (Windows builds)
	const fs::path exe = folder / "bin" / "luanti.exe";
	if (fs::is_regular_file(exe, ec)) {
		std::string version = exe_version(exe);
		if (version == b.version)
			line("luanti.exe version: " + version);
		else if (version.empty())
			line("luanti.exe version: unknown (no version resource)");
		else
			differ("luanti.exe is version " + version + ", not " + b.version);
		std::string error;
		auto imports = pe_imports(exe, error);
		if (!error.empty()) {
			differ("luanti.exe: " + error);
		} else {
			auto check = [&](const char *dll, const std::vector<std::string> &wanted, const char *what) {
				const std::vector<std::string> &have = imports[dll];
				int ok = 0;
				for (const std::string &name : wanted) {
					if (std::find(have.begin(), have.end(), name) != have.end())
						ok++;
					else
						differ(std::string("luanti.exe no longer imports ") + dll + "!" + name);
				}
				line(std::string("luanti.exe imports Prisma intercepts (") + what + "): " + std::to_string(ok) + " of " +
					std::to_string(wanted.size()));
			};
			check("sdl2.dll", b.sdl_imports, "SDL2");
			check("opengl32.dll", b.gl_imports, "OpenGL");
		}
	} else {
		line("luanti.exe imports: not checked (no bin/luanti.exe: not a Windows build)");
	}
	return r;
}

// ---------------------------------------------------------------- PE import table

std::map<std::string, std::vector<std::string>> pe_imports(const fs::path &exe, std::string &error)
{
	std::map<std::string, std::vector<std::string>> out;
	std::string d;
	if (!read_file(exe, d)) {
		error = "can't read it";
		return out;
	}
	auto u16 = [&](size_t at) { return at + 2 <= d.size() ? uint32_t(uint8_t(d[at]) | uint8_t(d[at + 1]) << 8) : 0u; };
	auto u32 = [&](size_t at) { return at + 4 <= d.size() ? u16(at) | u16(at + 2) << 16 : 0u; };
	auto u64 = [&](size_t at) { return uint64_t(u32(at)) | uint64_t(u32(at + 4)) << 32; };
	if (d.size() < 0x40 || d.compare(0, 2, "MZ") != 0) {
		error = "not a Windows executable";
		return out;
	}
	const size_t pe = u32(0x3C);
	if (d.compare(pe, 4, std::string("PE\0\0", 4)) != 0) {
		error = "not a Windows executable";
		return out;
	}
	const size_t sections = u16(pe + 6), optional = pe + 24, optional_size = u16(pe + 20);
	const bool plus = u16(optional) == 0x20B;
	const uint32_t import_rva = u32(optional + (plus ? 120 : 104));
	auto offset = [&](uint32_t rva) -> size_t {
		for (size_t i = 0; i < sections; i++) {
			size_t s = optional + optional_size + i * 40;
			uint32_t vsize = u32(s + 8), vaddr = u32(s + 12), rsize = u32(s + 16), raw = u32(s + 20);
			if (rva >= vaddr && rva < vaddr + std::max(vsize, rsize))
				return rva - vaddr + raw;
		}
		return 0;
	};
	auto cstring = [&](uint32_t rva) {
		size_t at = offset(rva);
		size_t end = at ? d.find('\0', at) : std::string::npos;
		return end == std::string::npos ? std::string() : d.substr(at, end - at);
	};
	size_t entry = offset(import_rva);
	if (!import_rva || !entry) {
		error = "no import table";
		return out;
	}
	for (; entry + 20 <= d.size(); entry += 20) {
		uint32_t names = u32(entry), dll = u32(entry + 12), thunks = u32(entry + 16);
		if (!dll)
			break;
		std::string name = cstring(dll);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		std::vector<std::string> &list = out[name];
		size_t at = offset(names ? names : thunks);
		for (; at && at + (plus ? 8 : 4) <= d.size(); at += plus ? 8 : 4) {
			uint64_t thunk = plus ? u64(at) : u32(at);
			if (!thunk)
				break;
			if (!(thunk >> (plus ? 63 : 31))) // not by ordinal
				list.push_back(cstring(uint32_t(thunk) + 2));
		}
	}
	return out;
}

} // namespace prisma::luanti
