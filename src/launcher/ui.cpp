// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/ui.hpp"
#include "common/ini.hpp"
#include "common/sha256.hpp"
#include "common/ui_theme.hpp"
#include "common/util.hpp"
#include "common/version.hpp"
#include "launcher/downloads.hpp"
#include "launcher/image.hpp"

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace prisma::launcher {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- style

// The look is the in-game menu's (common/ui_theme)
namespace col = ui::col;
using ui::S;
static const ImVec4 k_accent_text = ui::v4(col::accent);
static const ImVec4 k_error_text = ui::v4(col::error);
static const ImVec4 k_ok_text = ui::v4(col::ok);

struct FontFiles {
	fs::path regular, bold, mono, mono_bold;
};

// Fonts with accented characters, including ñ: Luanti's (Arimo, and Cousine
// for the labels) if found, otherwise the system's
static FontFiles find_fonts(const std::string &luanti)
{
	std::error_code ec;
	std::vector<fs::path> dirs;
	if (!luanti.empty())
		dirs.push_back(utf8_path(luanti).parent_path().parent_path() / "fonts");
#ifdef _WIN32
	const char *windir = std::getenv("WINDIR");
	fs::path system = fs::path(windir ? windir : "C:\\Windows") / "Fonts";
	FontFiles fallback = {system / "segoeui.ttf", system / "segoeuib.ttf", system / "consola.ttf", system / "consolab.ttf"};
#else
	for (const char *d : {"/usr/share/luanti/fonts", "/usr/local/share/luanti/fonts", "/usr/share/fonts/truetype/croscore"})
		dirs.push_back(d);
	FontFiles fallback = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"};
#endif
	FontFiles found = fallback;
	for (const fs::path &d : dirs) {
		if (fs::is_regular_file(d / "Arimo-Regular.ttf", ec)) {
			found.regular = d / "Arimo-Regular.ttf";
			found.bold = d / "Arimo-Bold.ttf";
		}
		if (fs::is_regular_file(d / "Cousine-Regular.ttf", ec)) {
			found.mono = d / "Cousine-Regular.ttf";
			found.mono_bold = d / "Cousine-Bold.ttf";
		}
		if (found.regular != fallback.regular)
			break;
	}
	return found;
}

// ---------------------------------------------------------------- controls

static void heading(const char *text, float size = 1.25f)
{
	ImGui::PushFont(ui::fonts().bold, ImGui::GetStyle().FontSizeBase * size);
	ImGui::TextUnformatted(text);
	ImGui::PopFont();
}

static void hint(const std::string &text, const ImVec4 *color = nullptr)
{
	ImGui::PushStyleColor(ImGuiCol_Text, color ? *color : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

// The title of a page, and a hairline across under it
static void page_title(const char *text)
{
	ImGui::AlignTextToFramePadding();
	ui::label(text, col::text, 0.92f, true);
	float y = ImGui::GetCursorScreenPos().y + S(6);
	ImVec2 wpos = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, y), ImVec2(wpos.x + ImGui::GetWindowWidth(), y), col::line);
	ImGui::Dummy(ImVec2(0, S(16)));
}

// A switch with its text next to it, which also toggles it
static bool toggle_row(const char *id, const char *label, bool &on)
{
	bool changed = ui::switch_box(id, on);
	ImGui::SameLine(0, S(10));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (ImGui::IsItemClicked()) {
		on = !on;
		changed = true;
	}
	return changed;
}

// A text field over a std::string; `placeholder` is shown while it is empty
static bool input(const char *id, std::string &value, float width, const char *placeholder = "")
{
	char buf[2048];
	std::snprintf(buf, sizeof buf, "%s", value.c_str());
	ImGui::SetNextItemWidth(width);
	if (!ImGui::InputTextWithHint(id, placeholder, buf, sizeof buf))
		return false;
	value = buf;
	return true;
}

// "true" / "false" from pack.toml, in words
static std::string pretty_value(const std::string &v)
{
	if (v == "true")
		return "on";
	if (v == "false")
		return "off";
	return v;
}

// The width of a ui::button
static float button_w(const char *text)
{
	return ui::label_size(text, 0.8f, true).x + ImGui::GetStyle().FramePadding.x * 2 + S(4);
}

// ---------------------------------------------------------------- downloads

/*
	The download page's work (downloads.hpp), on a thread of its own so the
	window keeps drawing. The thread and the window share this; a thread
	still running when the window closes keeps it alive and ends on its own.
*/
struct DownloadJob {
	std::mutex lock;
	bool busy = false;
	std::string doing;  // what is being done, for the player
	std::string error;  // the last thing that failed
	std::string done;   // the last thing that worked
	bool have_index = false;
	downloads::Index index;
	downloads::Source source;
	std::uint64_t bytes = 0, total = 0;
	bool folder_changed = false; // a pack was installed or removed
};

static std::string megabytes(std::uint64_t bytes)
{
	char text[32];
	if (bytes < (1u << 20))
		std::snprintf(text, sizeof text, "%.0f KB", std::max(1.0, bytes / 1024.0));
	else
		std::snprintf(text, sizeof text, "%.1f MB", bytes / 1048576.0);
	return text;
}

// ---------------------------------------------------------------- screenshots

/*
	A pack's screenshots, read and decoded on a thread; the window turns
	them into textures. Thumbnails are cut to 16:9 and shrunk to one size, so
	every picture in the lists is alike; the viewer shows the whole image.

	  local:<folder>                 an installed pack: thumbnails and the
	                                 images for the viewer, from its folder
	  thumbs:<commit>:<folder>       a pack to download: the thumbnails
	                                 prisma-packs keeps for it (small files)
	  full:<commit>:<folder>:<i>     its screenshot i, fetched only when the
	                                 viewer shows it
*/
struct Shot {
	Image thumb, large;   // decoded, until they are textures
	int large_w = 0, large_h = 0;
	GLuint thumb_tex = 0, large_tex = 0;
	bool ok = false;
};

struct ShotSet {
	bool done = false;     // the thread finished
	bool uploaded = false; // the window made the textures
	std::vector<Shot> shots;
	std::string error;
};

struct ShotLoader {
	std::mutex lock;
	std::map<std::string, std::shared_ptr<ShotSet>> sets; // by key
};

constexpr int k_thumb_width = 480, k_large_width = 1280;

static GLuint upload(const Image &img)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
	return tex;
}

static ImTextureRef texture_ref(GLuint tex)
{
	return ImTextureRef((ImTextureID)(uintptr_t)tex);
}

// Reads item i of a set (false and `error` when it cannot)
using ShotFetch = std::function<bool(size_t i, std::string &bytes, std::string &error)>;

enum class Make { Thumbs, Large, Both };

static void load_shots(const std::shared_ptr<ShotLoader> &loader, const std::string &key, size_t count, Make make,
	ShotFetch fetch)
{
	{
		std::lock_guard<std::mutex> guard(loader->lock);
		if (loader->sets.count(key))
			return;
		loader->sets[key] = std::make_shared<ShotSet>();
	}
	std::thread([loader, key, count, make, fetch] {
		std::vector<Shot> shots(count);
		std::string error;
		for (size_t i = 0; i < count; i++) {
			std::string bytes, why;
			Image img;
			if (!fetch(i, bytes, why) || !decode_png(bytes, img, why)) {
				error = why;
				continue;
			}
			Shot &s = shots[i];
			if (make != Make::Large)
				s.thumb = shrink(crop_to(img, 16, 9), k_thumb_width);
			if (make != Make::Thumbs) {
				s.large = shrink(img, k_large_width);
				s.large_w = s.large.width;
				s.large_h = s.large.height;
			}
			s.ok = true;
		}
		std::lock_guard<std::mutex> guard(loader->lock);
		auto it = loader->sets.find(key);
		if (it == loader->sets.end())
			return; // forgotten meanwhile
		it->second->shots = std::move(shots);
		it->second->error = error;
		it->second->done = true;
	}).detach();
}

// What a pack to download shows: its thumbnails (or, from an index without
// them, its screenshots) and where they are in the repository
struct RemoteShots {
	downloads::Source source;
	std::string folder;
	std::vector<downloads::RemoteFile> thumbs, full;
	std::string thumbs_dir; // "thumbnails/<folder>/" or "packs/<folder>/"
};

static bool remote_shots(DownloadJob &job, const std::string &folder, RemoteShots &out)
{
	std::lock_guard<std::mutex> guard(job.lock);
	out.source = job.source;
	out.folder = folder;
	for (const downloads::RemotePack &p : job.index.packs) {
		if (p.folder != folder)
			continue;
		for (const downloads::RemoteFile *f : p.screenshots())
			out.full.push_back(*f);
		if (!p.thumbnails.empty()) {
			out.thumbs = p.thumbnails;
			out.thumbs_dir = "thumbnails/" + folder + "/";
		} else {
			out.thumbs = out.full;
			out.thumbs_dir = "packs/" + folder + "/";
		}
	}
	return !out.full.empty() && out.source.fetch;
}

// A file of the repository, checked against the index
static bool fetch_checked(const downloads::Source &source, const std::string &path, const downloads::RemoteFile &f,
	std::string &bytes, std::string &error)
{
	if (!source.fetch(path, bytes, error))
		return false;
	if (bytes.size() != f.size || sha256(bytes) != f.sha256) {
		error = f.path + " is not what the index says";
		bytes.clear();
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- Ui

Ui::Ui(const fs::path &prisma_dir, const fs::path &ini, Config &config, Platform platform)
	: prisma_dir_(prisma_dir), ini_(ini), config_(config), saved_(config), platform_(std::move(platform)),
	  job_(std::make_shared<DownloadJob>()), shots_(std::make_shared<ShotLoader>())
{
	refresh_packs();
	// No Luanti yet: start where it is chosen. No pack yet (Prisma ships
	// none): start where they are downloaded.
	if (!is_executable(config_.luanti))
		page_ = Page::Settings;
	else if (packs_.empty())
		page_ = Page::Downloads;
	else
		page_ = Page::Pack;
}

Ui::~Ui() = default;

void Ui::setup(float scale)
{
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	FontFiles f = find_fonts(config_.luanti);
	ui::load_fonts(f.regular, f.bold, f.mono, f.mono_bold, 17.f * std::max(1.f, scale));
	ui::apply_style(scale, 1);
}

void Ui::refresh_packs()
{
	listed_folder_ = config_.packs_folder(prisma_dir_);
	packs_ = list_packs(listed_folder_);
	// No pack chosen, or the chosen one is not there: the first one there is,
	// as the core does. A pack just downloaded into an empty folder is the
	// one played. Not a change of the player's to ask about when closing.
	bool there = std::any_of(packs_.begin(), packs_.end(), [&](const PackInfo &p) { return p.folder == config_.pack; });
	if (!there && !packs_.empty()) {
		if (saved_.pack == config_.pack)
			saved_.pack = packs_.front().folder;
		config_.pack = packs_.front().folder;
	}
	refresh_installed();
	forget_local_screenshots();
}

// The installed packs' screenshots are read again (a pack was installed,
// updated or removed, or the folder changed)
void Ui::forget_local_screenshots()
{
	std::lock_guard<std::mutex> guard(shots_->lock);
	for (auto it = shots_->sets.begin(); it != shots_->sets.end();) {
		if (!it->first.starts_with("local:")) {
			++it;
			continue;
		}
		if (viewing_ == it->first)
			viewing_.clear();
		for (Shot &s : it->second->shots)
			for (GLuint t : {s.thumb_tex, s.large_tex})
				if (t)
					glDeleteTextures(1, &t);
		it = shots_->sets.erase(it);
	}
}

void Ui::local_screenshots(const std::string &folder)
{
	std::vector<fs::path> files = downloads::screenshots_in(listed_folder_ / utf8_path(folder));
	if (files.empty())
		return;
	std::string key = "local:" + folder;
	load_shots(shots_, key, files.size(), Make::Both, [files](size_t i, std::string &bytes, std::string &error) {
		std::ifstream in(files[i], std::ios::binary);
		if (!in) {
			error = "cannot read " + to_utf8(files[i].filename());
			return false;
		}
		std::stringstream s;
		s << in.rdbuf();
		bytes = s.str();
		if (bytes.size() > downloads::k_max_screenshot_bytes) {
			error = to_utf8(files[i].filename()) + " is too big";
			return false;
		}
		return true;
	});
	draw_screenshots(key, files.size(), folder);
}

std::string Ui::request_thumbnails(const std::string &folder)
{
	RemoteShots r;
	if (!remote_shots(*job_, folder, r))
		return "";
	std::string key = "thumbs:" + r.source.commit + ":" + folder;
	load_shots(shots_, key, r.thumbs.size(), Make::Thumbs, [r](size_t i, std::string &bytes, std::string &error) {
		return fetch_checked(r.source, r.thumbs_dir + r.thumbs[i].path, r.thumbs[i], bytes, error);
	});
	return key;
}

// Screenshot i of a pack to download, for the viewer
std::string Ui::request_full(const std::string &folder, size_t i)
{
	RemoteShots r;
	if (!remote_shots(*job_, folder, r) || i >= r.full.size())
		return "";
	std::string key = "full:" + r.source.commit + ":" + folder + ":" + std::to_string(i);
	downloads::RemoteFile f = r.full[i];
	load_shots(shots_, key, 1, Make::Large, [r, f](size_t, std::string &bytes, std::string &error) {
		return fetch_checked(r.source, "packs/" + r.folder + "/" + f.path, f, bytes, error);
	});
	return key;
}

void Ui::remote_screenshots(const std::string &folder)
{
	std::string key = request_thumbnails(folder);
	if (key.empty())
		return;
	RemoteShots r;
	remote_shots(*job_, folder, r);
	draw_screenshots(key, r.thumbs.size(), folder);
}

// The set of `key`, its textures made once its images are decoded (null if
// it was never asked for)
std::shared_ptr<ShotSet> Ui::ready_screenshots(const std::string &key)
{
	std::lock_guard<std::mutex> guard(shots_->lock);
	auto it = shots_->sets.find(key);
	if (it == shots_->sets.end())
		return nullptr;
	std::shared_ptr<ShotSet> set = it->second;
	if (set->done && !set->uploaded) {
		for (Shot &s : set->shots) {
			if (s.thumb.width > 0)
				s.thumb_tex = upload(s.thumb);
			if (s.large.width > 0)
				s.large_tex = upload(s.large);
			s.thumb = Image(); // on the GPU now
			s.large = Image();
		}
		set->uploaded = true;
	}
	return set;
}

bool Ui::draw_cover(const std::string &key, float w, float h)
{
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton("cover", ImVec2(w, h));
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::panel);
	std::shared_ptr<ShotSet> set = key.empty() ? nullptr : ready_screenshots(key);
	if (set && set->uploaded && !set->shots.empty() && set->shots[0].thumb_tex) {
		dl->AddImage(texture_ref(set->shots[0].thumb_tex), p, ImVec2(p.x + w, p.y + h)); // both 16:9
	} else {
		const char *text = key.empty() ? "No screenshots" : (set && set->done ? "Not shown" : "Loading…");
		ImVec2 ts = ui::label_size(text, 0.66f, false);
		ui::draw_label(dl, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), text, col::muted, 0.66f, false);
	}
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), hovered ? col::accent : col::line);
	return clicked;
}

// A row of thumbnails, all 16:9; a click opens the viewer
void Ui::draw_screenshots(const std::string &key, size_t expected, const std::string &folder)
{
	std::shared_ptr<ShotSet> set = ready_screenshots(key);
	if (!set)
		return;
	const float h = std::round(S(96));
	const float w = std::round(h * 16.f / 9.f);
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	size_t n = set->uploaded ? set->shots.size() : expected;
	for (size_t i = 0; i < n; i++) {
		if (i > 0) {
			ImGui::SameLine(0, S(8));
			if (ImGui::GetCursorPosX() + w > right)
				ImGui::NewLine();
		}
		ImGui::PushID((int)i);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool ready = set->uploaded && set->shots[i].thumb_tex;
		if (ImGui::InvisibleButton("shot", ImVec2(w, h)) && ready) {
			viewing_ = key;
			viewing_folder_ = folder;
			viewing_index_ = i;
		}
		if (ready)
			dl->AddImage(texture_ref(set->shots[i].thumb_tex), p, ImVec2(p.x + w, p.y + h));
		dl->AddRect(p, ImVec2(p.x + w, p.y + h), ready && ImGui::IsItemHovered() ? col::accent : col::line);
		ImGui::PopID();
	}
	if (!set->uploaded)
		ui::label("Loading screenshots…", col::muted, 0.7f);
	else if (!set->error.empty())
		hint("A screenshot could not be shown: " + set->error, &k_error_text);
}

void Ui::draw_screenshot_viewer()
{
	std::shared_ptr<ShotSet> gallery = viewing_.empty() ? nullptr : ready_screenshots(viewing_);
	if (!gallery || !gallery->uploaded || gallery->shots.empty()) {
		viewing_.clear();
		return;
	}
	const size_t n = gallery->shots.size();
	viewing_index_ = std::min(viewing_index_, n - 1);
	// The whole image: in the gallery (an installed pack) or fetched now
	const Shot *full = nullptr;
	bool loading = false;
	std::string failed;
	std::shared_ptr<ShotSet> one;
	if (viewing_.starts_with("local:")) {
		full = &gallery->shots[viewing_index_];
	} else {
		one = ready_screenshots(request_full(viewing_folder_, viewing_index_));
		if (one && one->uploaded && !one->shots.empty() && one->shots[0].ok)
			full = &one->shots[0];
		else if (one && one->done)
			failed = one->error.empty() ? "it could not be read" : one->error;
		else
			loading = true;
	}

	if (!ImGui::IsPopupOpen("Screenshot"))
		ImGui::OpenPopup("Screenshot");
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(14)));
	if (ImGui::BeginPopupModal("Screenshot", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
		// The image's own shape once it is here, 16:9 meanwhile
		float iw = full && full->large_tex ? (float)full->large_w : 16.f, ih = full && full->large_tex ? (float)full->large_h : 9.f;
		const float bar = ImGui::GetFrameHeight() + S(10);
		float scale = std::min((vp->Size.x * 0.92f - S(28)) / iw, (vp->Size.y * 0.92f - S(28) - bar) / ih);
		ImVec2 size(std::round(iw * scale), std::round(ih * scale));
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool close = ImGui::InvisibleButton("image", size); // a click on it closes
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 q(p.x + size.x, p.y + size.y);
		if (full && full->large_tex) {
			dl->AddImage(texture_ref(full->large_tex), p, q);
		} else {
			// The thumbnail, blown up, until the image arrives
			if (gallery->shots[viewing_index_].thumb_tex)
				dl->AddImage(texture_ref(gallery->shots[viewing_index_].thumb_tex), p, q, ImVec2(0, 0), ImVec2(1, 1),
					IM_COL32(255, 255, 255, 110));
			const std::string text = loading ? "Loading…" : "Not shown: " + failed;
			ImVec2 ts = ui::label_size(text, 0.8f, true);
			ui::draw_label(dl, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), text,
				loading ? col::text : col::error, 0.8f, true);
		}
		ImVec2 wmin = ImGui::GetWindowPos();
		ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
		ui::corner_marks(dl, wmin, wmax, col::accent, S(10));

		ImGui::Dummy(ImVec2(0, S(2)));
		ImGui::BeginDisabled(n < 2);
		if (ui::button("‹ Previous") || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
			viewing_index_ = (viewing_index_ + n - 1) % n;
		ImGui::SameLine();
		if (ui::button("Next ›") || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
			viewing_index_ = (viewing_index_ + 1) % n;
		ImGui::EndDisabled();
		ImGui::SameLine(0, S(14));
		ImGui::AlignTextToFramePadding();
		ui::label(std::to_string(viewing_index_ + 1) + " / " + std::to_string(n), col::muted, 0.72f);
		const char *close_text = "Close";
		ImGui::SameLine(ImGui::GetWindowWidth() - S(14) - button_w(close_text));
		if (ui::button(close_text) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			close = true;
		if (close) {
			viewing_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

void Ui::refresh_installed()
{
	installed_.clear();
	std::lock_guard<std::mutex> guard(job_->lock);
	std::error_code ec;
	for (const downloads::RemotePack &p : job_->index.packs) {
		Installed &i = installed_[p.folder];
		fs::path dir = listed_folder_ / utf8_path(p.folder);
		i.present = fs::exists(dir, ec);
		i.downloaded = i.present && downloads::downloaded_version(dir, i.version);
	}
}

// Asks for the list of packs (install = false) or downloads one, on a thread
void Ui::start_download(bool install, const std::string &folder)
{
	std::shared_ptr<DownloadJob> job = job_;
	{
		std::lock_guard<std::mutex> guard(job->lock);
		if (job->busy)
			return;
		job->busy = true;
		job->error.clear();
		job->done.clear();
		job->bytes = job->total = 0;
		job->doing = install ? "Downloading " + folder + "…" : "Reading the list of packs…";
	}
	std::string local = ini::get(ini_, "debug", "packs_source");
	fs::path packs = listed_folder_;
	std::thread([job, install, folder, local, packs] {
		std::string error, done;
		if (!install) {
			downloads::Source source;
			downloads::Index index;
			bool ok;
			if (!local.empty()) {
				source = downloads::local_source(utf8_path(local));
				ok = true;
			} else {
				ok = downloads::open_github(downloads::https_get, source, error);
			}
			ok = ok && downloads::fetch_index(source, index, error);
			std::lock_guard<std::mutex> guard(job->lock);
			if (ok) {
				job->source = source;
				job->index = std::move(index);
				job->have_index = true;
				job->folder_changed = true; // to look again at what is installed
			}
			job->error = ok ? "" : error;
			job->busy = false;
			return;
		}
		downloads::Source source;
		downloads::RemotePack pack;
		{
			std::lock_guard<std::mutex> guard(job->lock);
			source = job->source;
			for (const downloads::RemotePack &p : job->index.packs)
				if (p.folder == folder)
					pack = p;
		}
		bool ok = !pack.folder.empty() &&
			downloads::install(source, pack, packs, [&](std::uint64_t bytes, std::uint64_t total) {
				std::lock_guard<std::mutex> guard(job->lock);
				job->bytes = bytes;
				job->total = total;
			}, error);
		std::lock_guard<std::mutex> guard(job->lock);
		job->error = ok ? "" : (pack.folder.empty() ? folder + " is no longer in the list" : error);
		job->done = ok ? pack.name + " " + pack.version + " is installed." : "";
		job->folder_changed = true;
		job->busy = false;
	}).detach();
}

const std::string &Ui::luanti_version()
{
	auto it = versions_.find(config_.luanti);
	if (it == versions_.end()) {
		std::string v = platform_.luanti_version && is_executable(config_.luanti) ?
			platform_.luanti_version(config_.luanti) : "";
		it = versions_.emplace(config_.luanti, v).first;
	}
	return it->second;
}

bool Ui::play()
{
	if (!is_executable(config_.luanti)) {
		page_ = Page::Settings;
		return false;
	}
	config_.save(ini_);
	saved_ = config_;
	result_ = WindowResult::Play;
	done_ = true;
	return true;
}

void Ui::request_close()
{
	if (config_ == saved_)
		done_ = true;
	else
		asking_close_ = true;
}

bool Ui::working()
{
	{
		std::lock_guard<std::mutex> guard(job_->lock);
		if (job_->busy)
			return true;
	}
	std::lock_guard<std::mutex> guard(shots_->lock);
	for (const auto &entry : shots_->sets)
		if (!entry.second->done)
			return true;
	return false;
}

bool Ui::frame()
{
	if (config_.packs_folder(prisma_dir_) != listed_folder_)
		refresh_packs();
	if (reread_packs_) {
		// Only when something changed: refreshing also reloads the screenshots
		reread_packs_ = false;
		std::vector<PackInfo> now = list_packs(listed_folder_);
		auto same = [](const PackInfo &a, const PackInfo &b) {
			return a.folder == b.folder && a.title == b.title && a.description == b.description &&
				a.author == b.author && a.version == b.version && a.luanti == b.luanti && a.error == b.error;
		};
		if (!std::equal(now.begin(), now.end(), packs_.begin(), packs_.end(), same))
			refresh_packs();
	}
	bool changed = false;
	{
		std::lock_guard<std::mutex> guard(job_->lock);
		std::swap(changed, job_->folder_changed);
	}
	if (changed)
		refresh_packs();

	ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("##prisma", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();

	draw_sidebar();
	ImGui::SameLine(0, 0);
	ImGui::BeginGroup();
	const float footer_h = S(64);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(22)));
	ImGui::BeginChild("main", ImVec2(0, -footer_h), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	if (page_ == Page::Settings)
		draw_settings_page();
	else if (page_ == Page::Downloads)
		draw_downloads_page();
	else
		draw_pack_page();
	ImGui::EndChild();
	draw_footer();
	ImGui::EndGroup();

	capture_key();
	draw_close_dialog();
	draw_remove_dialog();
	draw_screenshot_viewer();
	// Enter plays (unless typing in a field or with a dialog open)
	if (!asking_close_ && removing_.empty() && viewing_.empty() && !capturing_ && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
		play();
	ImGui::End();
	return !done_;
}

// ---------------------------------------------------------------- sidebar

void Ui::draw_sidebar()
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, col::panel);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, S(20)));
	ImGui::BeginChild("side", ImVec2(S(240), 0), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 wpos = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
	dl->AddLine(ImVec2(wpos.x + wsize.x - 0.5f, wpos.y), ImVec2(wpos.x + wsize.x - 0.5f, wpos.y + wsize.y), col::line);
	auto hairline = [&](float y) { dl->AddLine(ImVec2(wpos.x, y), ImVec2(wpos.x + wsize.x - 1, y), col::line); };

	// ■ PRISMA
	ImGui::Indent(S(14));
	ImVec2 p = ImGui::GetCursorScreenPos();
	float big = 1.15f;
	float lh = ui::label_size("P", big, true).y;
	float sq = std::round(S(9)), y = std::round(p.y + (lh - sq) * 0.5f);
	dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sq, y + sq), col::accent);
	ui::draw_label(dl, ImVec2(p.x + sq + S(11), p.y), "Prisma", col::text, big, true);
	ImGui::Dummy(ImVec2(0, lh + S(4)));
	ui::label("Shaders for Luanti", col::muted, 0.7f);
	ui::label("v" PRISMA_VERSION, col::line_strong, 0.7f);
	ImGui::Unindent(S(14));
	ImGui::Dummy(ImVec2(0, S(14)));
	hairline(ImGui::GetCursorScreenPos().y - S(6));
	ImGui::Dummy(ImVec2(0, S(6)));
	ImGui::Indent(S(14));
	ui::label("Shader packs", col::line_strong, 0.7f, true);
	ImGui::Unindent(S(14));
	ImGui::Dummy(ImVec2(0, S(4)));

	// The list scrolls; "Download packs" and "Settings" always stay at the bottom
	const float bottom_h = 2 * std::round(ImGui::GetFrameHeight() * 1.05f) + S(24);
	ImGui::BeginChild("packs", ImVec2(0, -bottom_h), 0, ImGuiWindowFlags_NoBackground);
	if (packs_.empty()) {
		ImGui::Indent(S(14));
		hint("There are no packs in the packs folder.");
		ImGui::Unindent(S(14));
	}
	int n = 1;
	for (const PackInfo &p : packs_) {
		char num[8];
		std::snprintf(num, sizeof num, "%02d", n++);
		if (ui::nav_item(num, p.title, page_ == Page::Pack && p.folder == config_.pack, p.error.empty() ? nullptr : "error")) {
			config_.pack = p.folder;
			page_ = Page::Pack;
		}
	}
	ImGui::EndChild();

	hairline(ImGui::GetCursorScreenPos().y + S(6));
	ImGui::Dummy(ImVec2(0, S(14)));
	if (ui::nav_item("", "Download packs", page_ == Page::Downloads))
		page_ = Page::Downloads;
	if (ui::nav_item("", "Settings", page_ == Page::Settings, is_executable(config_.luanti) ? nullptr : "Luanti missing"))
		page_ = Page::Settings;
	ImGui::PopStyleVar();
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- pages

void Ui::draw_pack_page()
{
	const PackInfo *pack = nullptr;
	for (const PackInfo &p : packs_)
		if (p.folder == config_.pack)
			pack = &p;
	if (!pack) {
		page_title("Choose a pack");
		if (packs_.empty())
			hint("There is no pack in " + to_utf8(listed_folder_) +
				". Change the packs folder in Settings, or put one there.");
		else
			hint("The pack \"" + config_.pack + "\" is not in the packs folder: choose another one in the list.");
		return;
	}

	// Title, and on the right the button for its folder
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	float top = ImGui::GetCursorPosY();
	const char *open = "Open folder";
	float open_w = button_w(open);
	ImGui::PushTextWrapPos(right - open_w - S(16));
	ImGui::PushFont(ui::fonts().bold, ImGui::GetStyle().FontSizeBase * 1.7f);
	ImGui::TextWrapped("%s", pack->title.c_str());
	ImGui::PopFont();
	ImGui::PopTextWrapPos();
	float after = ImGui::GetCursorPosY();
	ImGui::SetCursorPos(ImVec2(right - open_w, top + S(6)));
	if (ui::button(open, ImVec2(open_w, 0)) && platform_.open_folder)
		platform_.open_folder(listed_folder_ / utf8_path(pack->folder));
	ImGui::SetCursorPosY(std::max(after, ImGui::GetCursorPosY()));

	std::string by;
	if (!pack->author.empty())
		by = "by " + pack->author;
	if (!pack->version.empty())
		by += (by.empty() ? "version " : "  ·  version ") + pack->version;
	if (pack->folder != pack->title)
		by += (by.empty() ? "folder " : "  ·  folder ") + pack->folder;
	if (!by.empty())
		ui::label(by, col::muted, 0.72f);
	ImGui::Dummy(ImVec2(0, S(8)));

	if (!pack->error.empty())
		hint(pack->error, &k_error_text);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, S(640)));
	ImGui::TextUnformatted(pack->description.empty() ? "No description." : pack->description.c_str());
	ImGui::PopTextWrapPos();
	if (!downloads::screenshots_in(listed_folder_ / utf8_path(pack->folder)).empty()) {
		ImGui::Dummy(ImVec2(0, S(10)));
		local_screenshots(pack->folder);
	}

	ImGui::Dummy(ImVec2(0, S(16)));
	ui::section("Luanti settings");
	if (!pack->error.empty()) {
		// read_pack() applies nothing of a pack it rejects
		hint("None: Prisma won't use this pack until the error above is fixed.");
		return;
	}
	if (pack->luanti.empty()) {
		hint("This pack works with your Luanti settings as they are.");
		return;
	}
	toggle_row("##apply", "Apply the Luanti settings the pack asks for", config_.apply_luanti_settings);
	hint(config_.apply_luanti_settings ?
			"Luanti opens with a copy of your minetest.conf that has them: your file does not change, and whatever "
			"you change while playing (keys, volume...) is saved to it when the game closes." :
			"Luanti will use your own settings even if the pack asks for others: it may look different or run slower.");
	ImGui::Dummy(ImVec2(0, S(4)));

	if (!config_.apply_luanti_settings)
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
	if (ImGui::BeginTable("luanti", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
			ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 2);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1);
		for (const auto &[key, value] : pack->luanti) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(key.c_str());
			ImGui::TableNextColumn();
			ui::label(pretty_value(value), value == "true" ? col::accent : value == "false" ? col::muted : col::text, 0.8f);
		}
		ImGui::EndTable();
	}
	if (!config_.apply_luanti_settings)
		ImGui::PopStyleVar();
}

void Ui::draw_downloads_page()
{
	if (!asked_index_) {
		asked_index_ = true;
		start_download(false, "");
	}
	page_title("Download packs");

	// A copy of the shared state for this frame
	bool busy, have_index;
	std::string doing, error, done, where, commit;
	std::uint64_t bytes, total;
	std::vector<std::string> rejected;
	std::vector<downloads::RemotePack> list;
	{
		std::lock_guard<std::mutex> guard(job_->lock);
		busy = job_->busy;
		have_index = job_->have_index;
		doing = job_->doing;
		error = job_->error;
		done = job_->done;
		bytes = job_->bytes;
		total = job_->total;
		where = job_->source.where;
		commit = job_->source.commit;
		rejected = job_->index.rejected;
		list = job_->index.packs;
	}

	// What this is, and on the right the button to read the list again
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const char *again = "Check again";
	float again_w = button_w(again);
	float top = ImGui::GetCursorPosY();
	ImGui::PushTextWrapPos(right - again_w - S(16));
	ImGui::TextWrapped("Packs reviewed for Prisma, from %s. They are installed in your packs folder; the ones "
					   "downloaded here can be updated and removed here.", downloads::k_repository);
	ImGui::PopTextWrapPos();
	float after = ImGui::GetCursorPosY();
	ImGui::SetCursorPos(ImVec2(right - again_w, top));
	ImGui::BeginDisabled(busy);
	if (ui::button(again, ImVec2(again_w, 0)))
		start_download(false, "");
	ImGui::EndDisabled();
	ImGui::SetCursorPosY(std::max(after, ImGui::GetCursorPosY()) + S(4));
	if (have_index && commit == "local")
		hint("[debug] packs_source: from the folder " + where, &k_accent_text);

	// Status: working, failed, done
	if (busy) {
		std::string text = doing;
		if (total > 0)
			text += "  " + std::to_string((int)(100.0 * bytes / total)) + " %  (" + megabytes(total) + ")";
		hint(text, &k_accent_text);
	} else if (!error.empty()) {
		hint(error, &k_error_text);
	} else if (!done.empty()) {
		hint(done, &k_ok_text);
	}
	ImGui::Dummy(ImVec2(0, S(10)));

	if (!have_index) {
		if (!busy && error.starts_with("could not reach"))
			hint("Check your internet connection and try again.");
		return;
	}
	if (list.empty())
		hint("There are no packs to download yet.");

	// One pack in full, or the list: a picture, a few lines and the buttons each
	const downloads::RemotePack *shown = nullptr;
	for (const downloads::RemotePack &p : list)
		if (p.folder == detail_)
			shown = &p;
	if (!shown)
		detail_.clear();
	if (shown) {
		draw_download_details(*shown, busy);
		return;
	}
	// A page at a time: only the shown packs' thumbnails are downloaded
	const int per_page = 8;
	const int pages = std::max(1, ((int)list.size() + per_page - 1) / per_page);
	list_page_ = std::clamp(list_page_, 0, pages - 1);
	const size_t first = (size_t)(list_page_ * per_page), last = std::min(list.size(), first + per_page);
	const float cover_w = std::round(S(200)), cover_h = std::round(cover_w * 9.f / 16.f);
	for (size_t i = first; i < last; ++i) {
		const downloads::RemotePack &p = list[i];
		ImGui::PushID(p.folder.c_str());
		if (draw_cover(request_thumbnails(p.folder), cover_w, cover_h))
			detail_ = p.folder;
		ImGui::SameLine(0, S(18));
		ImGui::BeginGroup();
		ImGui::PushFont(ui::fonts().bold, ImGui::GetStyle().FontSizeBase * 1.15f);
		ImGui::TextUnformatted(p.name.c_str());
		ImGui::PopFont();
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (ImGui::IsItemClicked())
			detail_ = p.folder;
		std::string by;
		if (!p.author.empty())
			by = "by " + p.author + "  ·  ";
		by += "version " + p.version + "  ·  " + megabytes(p.total_size());
		ui::label(by, col::muted, 0.7f);
		// The description cut to a couple of lines
		std::string brief = p.description;
		if (brief.size() > 110) {
			size_t cut = brief.rfind(' ', 107);
			brief = brief.substr(0, cut == std::string::npos ? 107 : cut) + "…";
		}
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(brief.c_str());
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, S(2)));
		draw_pack_actions(p, busy, true);
		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0, S(16)));
		ImGui::PopID();
	}
	if (pages > 1) {
		int to = list_page_;
		ImGui::BeginDisabled(list_page_ == 0);
		if (ui::button("Previous"))
			to = list_page_ - 1;
		ImGui::EndDisabled();
		ImGui::SameLine(0, S(14));
		ImGui::AlignTextToFramePadding();
		ui::label("Page " + std::to_string(list_page_ + 1) + " of " + std::to_string(pages), col::muted, 0.8f);
		ImGui::SameLine(0, S(14));
		ImGui::BeginDisabled(list_page_ == pages - 1);
		if (ui::button("Next"))
			to = list_page_ + 1;
		ImGui::EndDisabled();
		if (to != list_page_) {
			list_page_ = to;
			ImGui::SetScrollY(0);
		}
		ImGui::Dummy(ImVec2(0, S(10)));
	}
	if (!rejected.empty()) {
		ui::section("Left out");
		for (const std::string &r : rejected)
			hint(r);
	}
}

// One pack in full: its description, all its screenshots and the buttons
void Ui::draw_download_details(const downloads::RemotePack &p, bool busy)
{
	if (ui::button("‹ All packs")) {
		detail_.clear();
		return;
	}
	ImGui::Dummy(ImVec2(0, S(10)));
	ImGui::PushFont(ui::fonts().bold, ImGui::GetStyle().FontSizeBase * 1.7f);
	ImGui::TextWrapped("%s", p.name.c_str());
	ImGui::PopFont();
	std::string by;
	if (!p.author.empty())
		by = "by " + p.author + "  ·  ";
	by += "version " + p.version;
	if (!p.license.empty())
		by += "  ·  " + p.license;
	by += "  ·  " + megabytes(p.total_size());
	ui::label(by, col::muted, 0.72f);
	ImGui::Dummy(ImVec2(0, S(8)));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, S(640)));
	ImGui::TextUnformatted(p.description.empty() ? "No description." : p.description.c_str());
	ImGui::PopTextWrapPos();
	if (!p.screenshots().empty()) {
		ImGui::Dummy(ImVec2(0, S(10)));
		remote_screenshots(p.folder);
	}
	ImGui::Dummy(ImVec2(0, S(14)));
	draw_pack_actions(p, busy);
}

// Install, or Update / Use it / Remove, then (in the list) Details, and
// what is installed: on the same line when it fits, else below
void Ui::draw_pack_actions(const downloads::RemotePack &p, bool busy, bool with_details)
{
	const Installed &state = installed_[p.folder];
	auto details = [&] {
		if (!with_details)
			return;
		ImGui::EndDisabled();
		if (ui::button("Details"))
			detail_ = p.folder;
		ImGui::BeginDisabled(busy);
	};
	ImGui::BeginDisabled(busy);
	if (!state.present) {
		if (ui::primary_button("Install"))
			start_download(true, p.folder);
		ImGui::SameLine();
		details();
	} else if (!state.downloaded) {
		hint("Your packs folder already has a pack called " + p.folder + " that was not downloaded here: "
			 "Prisma leaves it alone.", &k_accent_text);
		details();
	} else {
		bool newer = state.version != p.version;
		if (newer) {
			if (ui::primary_button("Update"))
				start_download(true, p.folder);
			ImGui::SameLine();
		}
		if (ui::button("Use it")) {
			config_.pack = p.folder;
			page_ = Page::Pack;
		}
		ImGui::SameLine();
		if (ui::button("Remove"))
			removing_ = p.folder;
		if (with_details) {
			ImGui::SameLine();
			details();
		}
		std::string text = newer ? "version " + state.version + " installed" : "installed";
		// The label is drawn in capitals at 0.72 of the font: a generous estimate of its width
		float need = S(14) + ImGui::CalcTextSize(text.c_str()).x * 1.1f;
		ImGui::SameLine(0, S(14));
		if (ImGui::GetContentRegionAvail().x < need)
			ImGui::NewLine();
		ImGui::AlignTextToFramePadding();
		ui::label(text, newer ? col::accent : col::ok, 0.72f);
	}
	ImGui::EndDisabled();
}

void Ui::draw_remove_dialog()
{
	if (removing_.empty())
		return;
	if (!ImGui::IsPopupOpen("Remove pack?"))
		ImGui::OpenPopup("Remove pack?");
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
	if (ImGui::BeginPopupModal("Remove pack?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
		ImVec2 wmin = ImGui::GetWindowPos();
		ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
		ui::corner_marks(ImGui::GetWindowDrawList(), wmin, wmax, col::accent, S(10));
		std::string name = removing_;
		{
			std::lock_guard<std::mutex> guard(job_->lock);
			for (const downloads::RemotePack &p : job_->index.packs)
				if (p.folder == removing_)
					name = p.name;
		}
		ui::label("Remove " + name + "?", col::text, 0.92f, true);
		ImGui::Dummy(ImVec2(0, S(2)));
		ImGui::TextDisabled("Its folder is deleted from your packs folder. You can download it again.");
		ImGui::Dummy(ImVec2(0, S(8)));
		const ImVec2 size(S(130), std::round(ImGui::GetFrameHeight() * 1.1f));
		bool close = false;
		if (ui::primary_button("Remove", size)) {
			std::string error;
			bool ok = downloads::remove(listed_folder_, removing_, error);
			{
				std::lock_guard<std::mutex> guard(job_->lock);
				job_->error = ok ? "" : error;
				job_->done = ok ? removing_ + " was removed." : "";
			}
			refresh_packs();
			close = true;
		}
		ImGui::SameLine();
		if (ui::button("Cancel", size) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			close = true;
		if (close) {
			removing_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

void Ui::draw_settings_page()
{
	page_title("Settings");
	const float label_w = S(190);
	auto label = [&](const char *text) {
		ImGui::AlignTextToFramePadding();
		ui::label(text, col::muted, 0.74f);
		ImGui::SameLine(label_w);
	};
	const ImGuiStyle &style = ImGui::GetStyle();
	// Under the field: SameLine() counts from the window's edge, Indent() from its padding
	auto under = [&](const std::string &text, const ImVec4 *color = nullptr) {
		ImGui::Indent(label_w - style.WindowPadding.x);
		hint(text, color);
		ImGui::Unindent(label_w - style.WindowPadding.x);
	};

	// ---- Luanti
	ui::section("Luanti");
#ifdef _WIN32
	const char *exe_label = "luanti.exe";
	const char *exe_placeholder = "The bin folder of your Luanti";
	const char *missing = "Choose your Luanti's luanti.exe: it is in its bin folder.";
#else
	const char *exe_label = "Executable";
	const char *exe_placeholder = "For example, /usr/games/luanti";
	const char *missing = "Choose the Luanti executable (or find it in the PATH with Browse).";
#endif
	label(exe_label);
	const char *browse = "Browse…";
	input("##luanti", config_.luanti, ImGui::GetContentRegionAvail().x - button_w(browse) - style.ItemSpacing.x,
		exe_placeholder);
	ImGui::SameLine();
	if (ui::button(browse, ImVec2(button_w(browse), 0)) && platform_.choose_luanti) {
		std::string chosen = platform_.choose_luanti(config_.luanti);
		if (!chosen.empty())
			config_.luanti = chosen;
	}
	if (config_.luanti.empty()) {
		under(missing, &k_accent_text);
	} else if (!is_executable(config_.luanti)) {
		under("That file was not found.", &k_error_text);
	} else {
		const std::string &v = luanti_version();
		std::string warning = version_warning(v);
		if (!warning.empty())
			under(warning, &k_accent_text);
		else
			under(v.empty() ? "Found." : "Luanti " + v, &k_ok_text);
	}

	label("Arguments");
	input("##args", config_.arguments, -1, "None");
	under("Optional. For example, --go --worldname world1 opens that world directly.");

	// ---- packs
	ImGui::Dummy(ImVec2(0, S(10)));
	ui::section("Shader packs");
	label("Packs folder");
	const char *open = "Open";
	std::string default_folder = to_utf8(prisma_dir_ / "shaderpacks");
	input("##packs", config_.packs_dir,
		ImGui::GetContentRegionAvail().x - button_w(browse) - button_w(open) - style.ItemSpacing.x * 2,
		default_folder.c_str());
	ImGui::SameLine();
	ImGui::PushID("packs");
	if (ui::button(browse, ImVec2(button_w(browse), 0)) && platform_.choose_folder) {
		std::string chosen = platform_.choose_folder(to_utf8(config_.packs_folder(prisma_dir_)));
		if (!chosen.empty())
			config_.packs_dir = chosen == default_folder ? "" : chosen;
	}
	ImGui::PopID();
	ImGui::SameLine();
	if (ui::button(open, ImVec2(button_w(open), 0)) && platform_.open_folder)
		platform_.open_folder(config_.packs_folder(prisma_dir_));
	under(config_.packs_dir.empty() ? "Empty: the shaderpacks folder next to Prisma." :
		std::to_string(packs_.size()) + " pack(s) in that folder.");

	// ---- keys
	ImGui::Dummy(ImVec2(0, S(10)));
	ui::section("Keys in the game");
	label("Open Prisma's menu");
	key_button("##menu", 1, config_.menu_key);
	label("Turn on or off");
	key_button("##toggle", 2, config_.toggle_key);
	if (capturing_) {
		under("Press the key you want to use. Esc cancels.", &k_accent_text);
	} else {
		under("Click a button and press the new key. Valid: F1–F12, letters, numbers, Home, End, Insert, "
			  "Delete, Page Up, Page Down, Pause, Scroll Lock and Print Screen.");
		if (config_.menu_key == config_.toggle_key)
			under("Both are the same key: choose a different one for one of them.", &k_error_text);
		for (const std::string *k : {&config_.menu_key, &config_.toggle_key})
			if (key_used_by_luanti(*k)) {
				under("Luanti also uses " + *k + ": inside the game Prisma keeps it (it does not reach the chat either).",
					&k_accent_text);
				break;
			}
	}

	// ---- on startup
	ImGui::Dummy(ImVec2(0, S(10)));
	ui::section("When Prisma opens");
	toggle_row("##show", "Show this window", config_.show_window);
#ifdef _WIN32
	hint(config_.show_window ? "If you turn it off, Prisma will open the game directly." :
							   "Prisma will open the game directly. To show it again, hold Shift while "
							   "opening Prisma.");
#else
	hint(config_.show_window ? "If you turn it off, Prisma will open the game directly." :
							   "Prisma will open the game directly. To show it again: prisma --configure");
#endif
}

// ---------------------------------------------------------------- keys

bool Ui::key_button(const char *id, int which, std::string &key)
{
	bool active = capturing_ == which;
	std::string text = (active ? std::string("Press a key…") : key) + id;
	if (active) {
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(232, 163, 60, 40));
		ImGui::PushStyleColor(ImGuiCol_Border, col::accent);
		ImGui::PushStyleColor(ImGuiCol_Text, col::accent);
	}
	bool clicked = ui::button(text.c_str(), ImVec2(S(220), 0));
	if (active)
		ImGui::PopStyleColor(3);
	if (clicked)
		capturing_ = active ? 0 : which;
	return clicked;
}

// The key pressed while a key button waits: the name that
// the core understands (src/core/ui/input.cpp)
void Ui::capture_key()
{
	if (!capturing_)
		return;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		capturing_ = 0;
		return;
	}
	std::string name;
	for (int k = ImGuiKey_F1; k <= ImGuiKey_F12 && name.empty(); k++)
		if (ImGui::IsKeyPressed((ImGuiKey)k, false))
			name = "F" + std::to_string(k - ImGuiKey_F1 + 1);
	for (int k = ImGuiKey_A; k <= ImGuiKey_Z && name.empty(); k++)
		if (ImGui::IsKeyPressed((ImGuiKey)k, false))
			name = std::string(1, (char)('A' + (k - ImGuiKey_A)));
	for (int k = ImGuiKey_0; k <= ImGuiKey_9 && name.empty(); k++)
		if (ImGui::IsKeyPressed((ImGuiKey)k, false))
			name = std::string(1, (char)('0' + (k - ImGuiKey_0)));
	static const std::pair<ImGuiKey, const char *> named[] = {
		{ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"}, {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Delete, "Delete"},
		{ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Pause, "Pause"},
		{ImGuiKey_ScrollLock, "ScrollLock"}, {ImGuiKey_PrintScreen, "PrintScreen"},
	};
	for (const auto &[k, n] : named)
		if (name.empty() && ImGui::IsKeyPressed(k, false))
			name = n;
	if (name.empty())
		return;
	(capturing_ == 1 ? config_.menu_key : config_.toggle_key) = name;
	capturing_ = 0;
}

// ---------------------------------------------------------------- footer

void Ui::draw_footer()
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, col::panel);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), 0));
	ImGui::BeginChild("footer", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	ImVec2 wmin = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(wmin.x, wmin.y + 0.5f),
		ImVec2(wmin.x + ImGui::GetWindowWidth(), wmin.y + 0.5f), col::line);

	const ImGuiStyle &style = ImGui::GetStyle();
	float h = ImGui::GetWindowHeight();
	float button_h = std::round(ImGui::GetFrameHeight() * 1.2f);
	float y = std::round((h - button_h) * 0.5f);

	// Luanti status, on the left: a square, green when it is ready
	bool ok = is_executable(config_.luanti);
	bool supported = !ok || version_warning(luanti_version()).empty();
	std::string status;
	if (!message_.empty())
		status = message_;
	else if (ok && !supported)
		status = "Luanti " + luanti_version() + ": not supported (" PRISMA_LUANTI_VERSION " is)";
	else if (ok)
		status = luanti_version().empty() ? "Luanti ready" : "Luanti " + luanti_version();
	else
		status = config_.luanti.empty() ? "Choose where Luanti is" : "Luanti not found";
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float cy = std::round(wmin.y + h * 0.5f);
	float sq = std::round(S(4));
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, cy - sq), ImVec2(pos.x + 2 * sq, cy + sq),
		ok && supported ? col::ok : col::accent);
	ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + S(18), (h - ImGui::GetTextLineHeight()) * 0.5f));
	ui::label(status, ok && supported ? col::text : col::accent, 0.76f);
	if (!ok) {
		ImGui::SameLine(0, S(12));
		ImGui::SetCursorPosY(std::round((h - ImGui::GetFrameHeight()) * 0.5f));
		if (ui::button("Choose…"))
			page_ = Page::Settings;
	}

	// Save and Play, on the right
	const float play_w = S(150), save_w = S(110);
	float right = ImGui::GetWindowWidth() - style.WindowPadding.x;
	bool dirty = !(config_ == saved_);
	ImGui::SetCursorPos(ImVec2(right - play_w - save_w - style.ItemSpacing.x, y));
	ImGui::BeginDisabled(!dirty);
	if (ui::button("Save", ImVec2(save_w, button_h))) {
		config_.save(ini_);
		saved_ = config_;
		message_ = "Saved";
	}
	ImGui::EndDisabled();
	if (dirty)
		message_.clear();
	ImGui::SameLine();
	ImGui::BeginDisabled(!ok);
	if (ui::primary_button("Play", ImVec2(play_w, button_h)))
		play();
	ImGui::EndDisabled();
	if (!ok)
		ImGui::SetItemTooltip("First, choose where Luanti is");

	ImGui::EndChild();
	ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- close

void Ui::draw_close_dialog()
{
	if (asking_close_ && !ImGui::IsPopupOpen("Save changes?"))
		ImGui::OpenPopup("Save changes?");
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
	if (ImGui::BeginPopupModal("Save changes?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
		ImVec2 wmin = ImGui::GetWindowPos();
		ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
		ui::corner_marks(ImGui::GetWindowDrawList(), wmin, wmax, col::accent, S(10));
		ui::label("Save changes?", col::text, 0.92f, true);
		ImGui::Dummy(ImVec2(0, S(2)));
		ImGui::TextDisabled("You changed the settings and did not save them.");
		ImGui::Dummy(ImVec2(0, S(8)));
		const ImVec2 size(S(130), std::round(ImGui::GetFrameHeight() * 1.1f));
		if (ui::primary_button("Save", size)) {
			config_.save(ini_);
			saved_ = config_;
			done_ = true;
		}
		ImGui::SameLine();
		if (ui::button("Don't save", size)) {
			config_ = saved_;
			done_ = true;
		}
		ImGui::SameLine();
		if (ui::button("Cancel", size) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			asking_close_ = false;
		if (!asking_close_ || done_)
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

} // namespace prisma::launcher
