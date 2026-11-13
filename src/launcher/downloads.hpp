// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Packs to download, from the repository prisma-packs. Only the launcher
	uses the network; the core,
	inside Luanti, never does.

	The repository holds one folder per pack under packs/, as it is
	installed, and index.toml at its root, written by a script there:

	    format = 1

	    [packs.Aurora]                 # the pack's folder
	    name = "Aurora"
	    version = "0.1"
	    author = "Flay Krunegan"
	    license = "GPL-3.0-or-later"
	    description = "Bright and natural..."
	    files = [
	        "<sha256> <size> pack.toml",
	        "<sha256> <size> passes/00_noise.frag",
	        "<sha256> <size> screenshots/day.png",
	    ]
	    thumbnails = [                 # thumbnails/Aurora/ in the repository
	        "<sha256> <size> day.png",
	    ]

	The launcher asks GitHub for the latest commit of the main branch and
	reads the index and every file of that one commit, so a download never
	mixes two versions. A file whose size or SHA-256 is not the index's is
	refused, and so is a path that leaves the pack's folder or a file that is
	not pack data. Images are a pack's screenshots: PNG files in
	screenshots/, at most k_max_screenshots of them. The script also makes a
	small thumbnail of each, kept apart in thumbnails/<pack>/ (not
	installed): the page shows those, and fetches a whole screenshot only
	when the player opens it. A pack is written to a hidden folder next to the
	others and moved into place only when all of it arrived; a marker file in
	it (.prisma-download) says it was downloaded, and which version.
*/
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace prisma::launcher::downloads {

// GitHub's owner/name of the repository and its branch
inline constexpr const char *k_repository = "Krunegan/prisma-packs";
inline constexpr const char *k_branch = "main";
// In each downloaded pack's folder
inline constexpr const char *k_marker = ".prisma-download";
// A pack's screenshots: PNG files in screenshots/, this many at most
inline constexpr std::size_t k_max_screenshots = 5;
inline constexpr std::uint64_t k_max_screenshot_bytes = 4ull << 20;
inline constexpr std::uint64_t k_max_thumbnail_bytes = 512ull << 10;

struct RemoteFile {
	std::string path; // inside the pack's folder, with '/'
	std::uint64_t size = 0;
	std::string sha256; // lowercase hexadecimal
};

struct RemotePack {
	std::string folder; // what it is installed as (and what goes in pack=)
	std::string name, version, author, license, description;
	std::vector<RemoteFile> files;
	// One per screenshot, in the same order; path = the screenshot's file name
	std::vector<RemoteFile> thumbnails;
	std::uint64_t total_size() const;
	// The screenshots among the files, in name order
	std::vector<const RemoteFile *> screenshots() const;
};

struct Index {
	std::vector<RemotePack> packs; // sorted by name
	std::vector<std::string> rejected; // packs left out, and why
};

// Reads index.toml. A pack with a problem is left out and named in
// `rejected`; false (and `error`) only if the file itself is unusable.
bool parse_index(const std::string &text, Index &index, std::string &error);

// Whether `name` can be a pack's folder: letters, digits, '_' and '-'
bool valid_folder(const std::string &name);
// Whether `path` (inside a pack, with '/') is one of its screenshots
bool is_screenshot(const std::string &path);
// The screenshots of an installed pack, in name order (at most k_max_screenshots)
std::vector<std::filesystem::path> screenshots_in(const std::filesystem::path &pack_dir);
// Why `path` (inside a pack) is refused, or "" if it is fine: it must stay
// inside the pack, be a name every system can write, and be pack data
std::string check_path(const std::string &path);

// ---------------------------------------------------------------- where from

// Reads one file of the repository (its path from the repository's root)
using Fetch = std::function<bool(const std::string &path, std::string &body, std::string &error)>;

struct Source {
	std::string commit; // what is being read ("local" for a folder)
	std::string where;  // for the player: "Krunegan/prisma-packs" or the folder
	Fetch fetch;
};

// An HTTPS GET. `headers` are "Name: value" lines; at most `max_bytes` are
// read. False (and `error`) on any failure, including a status other than
// 200. Each system implements it (http_win32.cpp, http_linux.cpp).
bool https_get(const std::string &url, const std::vector<std::string> &headers, std::size_t max_bytes,
	std::string &body, std::string &error);

// The repository on GitHub at the latest commit of its branch, through `get`
// (https_get, or another one in the tests)
using HttpGet = std::function<bool(const std::string &url, const std::vector<std::string> &headers,
	std::size_t max_bytes, std::string &body, std::string &error)>;
bool open_github(const HttpGet &get, Source &source, std::string &error);

// A folder laid out as the repository (for developers: [debug] packs_source)
Source local_source(const std::filesystem::path &folder);

// index.toml of the source
bool fetch_index(const Source &source, Index &index, std::string &error);

// ---------------------------------------------------------------- the packs folder

// The version of the downloaded pack in `pack_dir`, or false if it was not
// downloaded (no marker: a bundled pack, or one put there by hand)
bool downloaded_version(const std::filesystem::path &pack_dir, std::string &version);

// Downloads `pack` into `packs_folder`, replacing an earlier download of it.
// `progress(done, total)` in bytes. Refuses to replace a folder that was not
// downloaded.
bool install(const Source &source, const RemotePack &pack, const std::filesystem::path &packs_folder,
	const std::function<void(std::uint64_t, std::uint64_t)> &progress, std::string &error);

// Removes a downloaded pack; refuses one that was not downloaded
bool remove(const std::filesystem::path &packs_folder, const std::string &folder, std::string &error);

} // namespace prisma::launcher::downloads
