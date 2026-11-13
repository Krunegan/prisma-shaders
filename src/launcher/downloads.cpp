// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/downloads.hpp"
#include "common/sha256.hpp"
#include "common/toml.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace prisma::launcher::downloads {

namespace fs = std::filesystem;

// Limits: an index or a pack bigger than this is refused
constexpr std::size_t k_max_index = 4u << 20;
constexpr std::size_t k_max_refs = 4u << 20; // the branches, tags and pull requests of prisma-packs
constexpr std::uint64_t k_max_file = 64ull << 20;
constexpr std::uint64_t k_max_pack = 256ull << 20;
constexpr std::size_t k_max_files = 2000;

std::uint64_t RemotePack::total_size() const
{
	std::uint64_t sum = 0;
	for (const RemoteFile &f : files)
		sum += f.size;
	return sum;
}

std::vector<const RemoteFile *> RemotePack::screenshots() const
{
	std::vector<const RemoteFile *> out;
	for (const RemoteFile &f : files)
		if (is_screenshot(f.path))
			out.push_back(&f);
	std::sort(out.begin(), out.end(), [](const RemoteFile *a, const RemoteFile *b) { return a->path < b->path; });
	return out;
}

static std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// Names Windows cannot use for a file or folder, with any extension
static bool reserved_name(const std::string &name)
{
	std::string base = lower(name.substr(0, name.find('.')));
	static const std::set<std::string> reserved = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
		"com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
	return reserved.count(base) > 0;
}

bool is_screenshot(const std::string &path)
{
	return path.starts_with("screenshots/") && path.find('/', 12) == std::string::npos && path.size() > 16 &&
		lower(path.substr(path.size() - 4)) == ".png";
}

std::vector<fs::path> screenshots_in(const fs::path &pack_dir)
{
	std::vector<fs::path> out;
	std::error_code ec;
	for (const fs::directory_entry &e : fs::directory_iterator(pack_dir / "screenshots", ec))
		if (e.is_regular_file(ec) && is_screenshot("screenshots/" + to_utf8(e.path().filename())))
			out.push_back(e.path());
	std::sort(out.begin(), out.end());
	if (out.size() > k_max_screenshots)
		out.resize(k_max_screenshots);
	return out;
}

bool valid_folder(const std::string &name)
{
	if (name.empty() || name.size() > 64 || reserved_name(name))
		return false;
	for (unsigned char c : name)
		if (!std::isalnum(c) && c != '_' && c != '-')
			return false;
	return true;
}

std::string check_path(const std::string &path)
{
	if (path.empty() || path.size() > 200)
		return "empty or too long";
	if (path.front() == '/' || path.back() == '/')
		return "not a relative file path";
	std::stringstream parts(path);
	std::string part;
	while (std::getline(parts, part, '/')) {
		if (part.empty() || part == "." || part == "..")
			return "leaves the pack's folder";
		if (part.front() == '.')
			return "a hidden name";
		if (part.back() == '.' || part.back() == ' ')
			return "a name Windows cannot write";
		for (unsigned char c : part)
			if (c < 32 || c == 127 || std::string_view("\\:*?\"<>|").find((char)c) != std::string_view::npos)
				return "a character a file name cannot have";
		if (reserved_name(part))
			return "a name Windows reserves";
	}
	// Pack data only: the manifest, GLSL, audio, text, and PNG
	// screenshots in screenshots/
	static const std::set<std::string> allowed = {".toml", ".frag", ".comp", ".glsl", ".ogg", ".wav", ".txt", ".md"};
	std::string name = path.substr(path.rfind('/') + 1);
	size_t dot = name.rfind('.');
	std::string ext = dot == std::string::npos ? "" : lower(name.substr(dot));
	if (ext == ".png")
		return is_screenshot(path) ? "" : "images go in screenshots/ (PNG, without subfolders)";
	if (!allowed.count(ext))
		return "not a kind of file a pack has";
	return "";
}

// "<sha256> <size> <path>"
static bool parse_file_line(const std::string &line, RemoteFile &f, std::string &why)
{
	size_t a = line.find(' ');
	size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
	if (b == std::string::npos) {
		why = "a files entry is not \"<sha256> <size> <path>\": " + line;
		return false;
	}
	f.sha256 = line.substr(0, a);
	std::string size = line.substr(a + 1, b - a - 1);
	f.path = line.substr(b + 1);
	bool hex = f.sha256.size() == 64 &&
		std::all_of(f.sha256.begin(), f.sha256.end(), [](char c) { return std::isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'); });
	if (!hex) {
		why = "not a SHA-256 in lowercase hexadecimal: " + f.sha256;
		return false;
	}
	if (size.empty() || size.size() > 12 || !std::all_of(size.begin(), size.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
		why = "not a size in bytes: " + size;
		return false;
	}
	f.size = std::stoull(size);
	if (f.size > k_max_file) {
		why = f.path + " is too big";
		return false;
	}
	if (std::string bad = check_path(f.path); !bad.empty()) {
		why = f.path + ": " + bad;
		return false;
	}
	return true;
}

bool parse_index(const std::string &text, Index &index, std::string &error)
{
	index = Index();
	toml::Document doc;
	if (!toml::parse(text, doc, error)) {
		error = "index.toml: " + error;
		return false;
	}
	const toml::Value *format = doc.get("format");
	if (!format || format->type != toml::Value::Number || format->num != 1) {
		error = "index.toml: format = 1 expected (this Prisma may be too old for the repository)";
		return false;
	}
	// The pack folders, in file order
	std::vector<std::string> folders;
	for (const std::string &key : doc.order) {
		if (!key.starts_with("packs."))
			continue;
		std::string folder = key.substr(6, key.find('.', 6) - 6);
		if (std::find(folders.begin(), folders.end(), folder) == folders.end())
			folders.push_back(folder);
	}
	std::set<std::string> seen; // folder names, ignoring case (Windows)
	for (const std::string &folder : folders) {
		RemotePack p;
		p.folder = folder;
		auto str = [&](const char *field) {
			const toml::Value *v = doc.get("packs." + folder + "." + field);
			return v && v->type == toml::Value::String ? v->str : std::string();
		};
		p.name = str("name");
		p.version = str("version");
		p.author = str("author");
		p.license = str("license");
		p.description = str("description");
		if (p.name.empty())
			p.name = folder;
		std::string why;
		if (!valid_folder(folder))
			why = "not a valid folder name";
		else if (!seen.insert(lower(folder)).second)
			why = "listed twice";
		else if (p.version.empty())
			why = "no version";
		const toml::Value *files = doc.get("packs." + folder + ".files");
		if (why.empty() && (!files || files->type != toml::Value::Array || files->items.empty()))
			why = "no files";
		std::set<std::string> paths;
		if (why.empty()) {
			for (const toml::Value &item : files->items) {
				RemoteFile f;
				if (item.type != toml::Value::String) {
					why = "a files entry is not text";
					break;
				}
				if (!parse_file_line(item.str, f, why))
					break;
				if (!paths.insert(lower(f.path)).second) {
					why = f.path + " listed twice";
					break;
				}
				p.files.push_back(f);
			}
		}
		if (why.empty() && !paths.count("pack.toml"))
			why = "no pack.toml";
		if (why.empty() && p.screenshots().size() > k_max_screenshots)
			why = "more than " + std::to_string(k_max_screenshots) + " screenshots";
		// The thumbnails, one for each screenshot (an index without them is
		// fine: the page then shows the screenshots themselves)
		const toml::Value *thumbs = doc.get("packs." + folder + ".thumbnails");
		if (why.empty() && thumbs) {
			std::vector<std::string> wanted;
			for (const RemoteFile *s : p.screenshots())
				wanted.push_back(s->path.substr(12)); // after "screenshots/"
			if (thumbs->type != toml::Value::Array) {
				why = "thumbnails is not a list";
			} else {
				for (const toml::Value &item : thumbs->items) {
					if (item.type != toml::Value::String) {
						why = "a thumbnails entry is not text";
						break;
					}
					// "<sha256> <size> <name>", checked as the screenshot it stands for
					const std::string &line = item.str;
					size_t b = line.find(' ', line.find(' ') + 1);
					RemoteFile f;
					if (b == std::string::npos) {
						why = "a thumbnails entry is not \"<sha256> <size> <name>\": " + line;
						break;
					}
					if (!parse_file_line(line.substr(0, b + 1) + "screenshots/" + line.substr(b + 1), f, why))
						break;
					f.path = f.path.substr(12);
					if (f.size > k_max_thumbnail_bytes) {
						why = "the thumbnail " + f.path + " is too big";
						break;
					}
					p.thumbnails.push_back(f);
				}
				std::sort(p.thumbnails.begin(), p.thumbnails.end(),
					[](const RemoteFile &a, const RemoteFile &b) { return a.path < b.path; });
				std::vector<std::string> have;
				for (const RemoteFile &f : p.thumbnails)
					have.push_back(f.path);
				if (why.empty() && have != wanted)
					why = "the thumbnails are not one for each screenshot";
			}
		}
		if (why.empty())
			for (const RemoteFile *s : p.screenshots())
				if (s->size > k_max_screenshot_bytes)
					why = s->path + " is bigger than " + std::to_string(k_max_screenshot_bytes >> 20) + " MB";
		if (why.empty() && p.files.size() > k_max_files)
			why = "too many files";
		if (why.empty() && p.total_size() > k_max_pack)
			why = "too big";
		if (!why.empty()) {
			index.rejected.push_back(folder + ": " + why);
			continue;
		}
		index.packs.push_back(std::move(p));
	}
	std::sort(index.packs.begin(), index.packs.end(),
		[](const RemotePack &a, const RemotePack &b) { return lower(a.name) < lower(b.name); });
	return true;
}

// ---------------------------------------------------------------- sources

bool open_github(const HttpGet &get, Source &source, std::string &error)
{
	// The branch's commit from the list of references Git clients read
	// (Git's smart HTTP protocol): GitHub's API allows 60 requests an hour
	// to each address without an account, often shared by many players
	std::string refs;
	std::string url = std::string("https://github.com/") + k_repository + ".git/info/refs?service=git-upload-pack";
	if (!get(url, {}, k_max_refs, refs, error)) {
		bool missing = error.find("answered 404") != std::string::npos || error.find("answered 401") != std::string::npos;
		error = missing ? std::string(k_repository) + " is not on GitHub (yet): there are no packs to download" :
			"could not reach " + std::string(k_repository) + ": " + error;
		return false;
	}
	// Each reference is a line "<length><sha> <name>", and the first one ends in "\0" and the capabilities
	std::string sha;
	std::string name = std::string(" refs/heads/") + k_branch;
	for (std::size_t at = refs.find(name); at != std::string::npos; at = refs.find(name, at + 1)) {
		std::size_t end = at + name.size();
		if (at >= 40 && (end == refs.size() || refs[end] == '\n' || refs[end] == '\0')) {
			sha = refs.substr(at - 40, 40);
			break;
		}
	}
	bool hex = sha.size() == 40 &&
		std::all_of(sha.begin(), sha.end(), [](char c) { return std::isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'); });
	if (!hex) {
		error = "GitHub did not give the latest commit of " + std::string(k_repository);
		return false;
	}
	source.commit = sha;
	source.where = k_repository;
	source.fetch = [get, sha](const std::string &path, std::string &body, std::string &err) {
		std::string url = std::string("https://raw.githubusercontent.com/") + k_repository + "/" + sha + "/" + path;
		return get(url, {}, (std::size_t)k_max_file + 1, body, err);
	};
	return true;
}

static bool read_file(const fs::path &path, std::string &body)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	std::stringstream s;
	s << in.rdbuf();
	body = s.str();
	return true;
}

Source local_source(const fs::path &folder)
{
	Source s;
	s.commit = "local";
	s.where = to_utf8(folder);
	s.fetch = [folder](const std::string &path, std::string &body, std::string &error) {
		if (!read_file(folder / utf8_path(path), body)) {
			error = "missing " + path;
			return false;
		}
		return true;
	};
	return s;
}

bool fetch_index(const Source &source, Index &index, std::string &error)
{
	std::string text;
	if (!source.fetch("index.toml", text, error))
		return false;
	if (text.size() > k_max_index) {
		error = "index.toml is too big";
		return false;
	}
	return parse_index(text, index, error);
}

// ---------------------------------------------------------------- the packs folder

static std::string quoted(const std::string &s)
{
	std::string out = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\')
			out += '\\';
		if ((unsigned char)c >= 32)
			out += c;
	}
	return out + "\"";
}

bool downloaded_version(const fs::path &pack_dir, std::string &version)
{
	std::string text;
	if (!read_file(pack_dir / k_marker, text))
		return false;
	toml::Document doc;
	std::string error;
	version.clear();
	if (toml::parse(text, doc, error))
		if (const toml::Value *v = doc.get("version"); v && v->type == toml::Value::String)
			version = v->str;
	return true;
}

static bool write_file(const fs::path &path, const std::string &body)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::FILE *f = open_file(path, "wb");
	if (!f)
		return false;
	bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
	ok = std::fclose(f) == 0 && ok;
	return ok;
}

bool install(const Source &source, const RemotePack &pack, const fs::path &packs_folder,
	const std::function<void(std::uint64_t, std::uint64_t)> &progress, std::string &error)
{
	std::error_code ec;
	if (!valid_folder(pack.folder)) {
		error = "not a valid folder name: " + pack.folder;
		return false;
	}
	const fs::path dest = packs_folder / utf8_path(pack.folder);
	std::string old_version;
	if (fs::exists(dest, ec) && !downloaded_version(dest, old_version)) {
		error = "the packs folder already has a pack called " + pack.folder +
			" that was not downloaded here; rename or move it first";
		return false;
	}
	fs::create_directories(packs_folder, ec);
	const fs::path tmp = packs_folder / utf8_path(".download-" + pack.folder);
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	if (ec) {
		error = "cannot write in " + to_utf8(packs_folder);
		return false;
	}
	auto fail = [&](const std::string &why) {
		std::error_code ignore;
		fs::remove_all(tmp, ignore);
		error = why;
		return false;
	};

	const std::uint64_t total = pack.total_size();
	std::uint64_t done = 0;
	if (progress)
		progress(0, total);
	for (const RemoteFile &f : pack.files) {
		if (std::string bad = check_path(f.path); !bad.empty())
			return fail(f.path + ": " + bad);
		std::string body, why;
		if (!source.fetch("packs/" + pack.folder + "/" + f.path, body, why))
			return fail(f.path + ": " + why);
		if (body.size() != f.size || sha256(body) != f.sha256)
			return fail(f.path + " is not what the index says: refused");
		if (!write_file(tmp / utf8_path(f.path), body))
			return fail("cannot write " + f.path);
		done += f.size;
		if (progress)
			progress(done, total);
	}
	if (!write_file(tmp / k_marker, "version = " + quoted(pack.version) + "\ncommit = " + quoted(source.commit) +
			"\nfrom = " + quoted(source.where) + "\n"))
		return fail("cannot write the pack's marker");

	// The earlier download aside, the new one in its place, the old one gone
	const fs::path old = packs_folder / utf8_path(".old-" + pack.folder);
	fs::remove_all(old, ec);
	bool had = fs::exists(dest, ec);
	if (had) {
		fs::rename(dest, old, ec);
		if (ec)
			return fail("cannot replace " + pack.folder + " (is a file in it open?)");
	}
	fs::rename(tmp, dest, ec);
	if (ec) {
		std::error_code ignore;
		if (had)
			fs::rename(old, dest, ignore);
		return fail("cannot move the pack into the packs folder");
	}
	fs::remove_all(old, ec);
	return true;
}

bool remove(const fs::path &packs_folder, const std::string &folder, std::string &error)
{
	std::string version;
	const fs::path dir = packs_folder / utf8_path(folder);
	if (!valid_folder(folder) || !downloaded_version(dir, version)) {
		error = folder + " was not downloaded here: Prisma leaves it alone";
		return false;
	}
	std::error_code ec;
	fs::remove_all(dir, ec);
	if (ec) {
		error = "cannot remove " + folder + " (is a file in it open?)";
		return false;
	}
	return true;
}

} // namespace prisma::launcher::downloads
