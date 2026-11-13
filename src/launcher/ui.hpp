// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The launcher's configuration window, with Dear ImGui and the look of
	Prisma's menu inside the game. The same on Windows and Linux: each system
	only provides the window, the OpenGL context and the ImGui backends
	(window_win32.cpp, window_sdl.cpp), and calls Ui::frame() on each
	frame, between ImGui::NewFrame() and ImGui::Render().
*/
#include "launcher/downloads.hpp"
#include "launcher/launcher.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace prisma::launcher {

struct DownloadJob; // ui.cpp: the download page's work, on its own thread
struct ShotLoader;  // ui.cpp: screenshots read and decoded on threads

class Ui {
public:
	Ui(const std::filesystem::path &prisma_dir, const std::filesystem::path &ini, Config &config, Platform platform);
	~Ui();

	// Fonts and style: after ImGui::CreateContext(). `scale` is the
	// screen scaling (1 = 96 dpi).
	void setup(float scale);

	// Draws the whole window (it fills the viewport). Returns false
	// when it has to be closed; result() says whether it was with Play.
	bool frame();

	// Work in the background whose end the window must show (the list of
	// packs, a download, screenshots): while there is some, the window
	// loop wakes up on its own instead of waiting for input.
	bool working();

	// The system's close button: if there are unsaved changes, it asks.
	void request_close();

	// The window got the focus: the packs folder is read again at the next
	// frame (a pack may have been added or changed by hand meanwhile).
	void window_focused() { reread_packs_ = true; }

	WindowResult result() const { return result_; }

private:
	void draw_sidebar();
	void draw_pack_page();
	void draw_downloads_page();
	void draw_settings_page();
	void draw_footer();
	void draw_close_dialog();
	void draw_remove_dialog();
	void start_download(bool install, const std::string &folder);
	void refresh_installed();
	// A pack's screenshots: read on a thread the first time `key` is asked
	// for, shown as a row of thumbnails; a click opens the viewer
	void local_screenshots(const std::string &folder);
	void remote_screenshots(const std::string &folder);
	// Starts reading the thumbnails of a pack to download; the key to draw
	// them with, "" if it has none
	std::string request_thumbnails(const std::string &folder);
	// ...and its screenshot i whole, for the viewer
	std::string request_full(const std::string &folder, size_t i);
	void draw_screenshots(const std::string &key, size_t expected, const std::string &folder);
	// The first screenshot in a w x h frame; true when clicked
	bool draw_cover(const std::string &key, float w, float h);
	std::shared_ptr<struct ShotSet> ready_screenshots(const std::string &key);
	void draw_download_details(const downloads::RemotePack &pack, bool busy);
	void draw_pack_actions(const downloads::RemotePack &pack, bool busy, bool with_details = false);
	void draw_screenshot_viewer();
	void forget_local_screenshots();
	bool key_button(const char *id, int which, std::string &key);
	void capture_key();
	void refresh_packs();
	const std::string &luanti_version();
	bool play();

	std::filesystem::path prisma_dir_, ini_;
	Config &config_;
	Config saved_; // what is in prisma.ini: to know whether there are changes
	Platform platform_;
	std::vector<PackInfo> packs_;
	std::filesystem::path listed_folder_;
	enum class Page { Pack, Downloads, Settings };
	Page page_ = Page::Pack;
	std::shared_ptr<DownloadJob> job_;
	bool asked_index_ = false; // the list of packs was asked for once
	bool reread_packs_ = false; // window_focused()
	struct Installed {
		bool present = false;    // a folder of that name in the packs folder
		bool downloaded = false; // ...downloaded here (it can be updated and removed)
		std::string version;
	};
	std::map<std::string, Installed> installed_; // by folder
	std::string removing_;                       // the pack whose removal is being asked
	std::shared_ptr<ShotLoader> shots_;
	std::string detail_;  // "Download packs" shows this pack in full ("" = the list)
	int list_page_ = 0;   // the page of that list, from 0
	std::string viewing_; // the screenshots shown large ("" = none)
	std::string viewing_folder_; // ...of this pack
	size_t viewing_index_ = 0;
	int capturing_ = 0; // 1: menu key, 2: toggle key
	bool asking_close_ = false;
	bool done_ = false;
	std::string message_;
	std::map<std::string, std::string> versions_; // path of Luanti -> version ("" = unknown)
	WindowResult result_ = WindowResult::Quit;
};

} // namespace prisma::launcher
