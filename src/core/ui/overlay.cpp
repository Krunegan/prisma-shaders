// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dev/hookcost.hpp"
#include "core/ui/overlay.hpp"
#include "core/ui/debugview.hpp"
#include "core/render/frame.hpp"
#include "core/hooks/glapi.hpp"
#include "core/hooks/hooks.hpp"
#include "core/ui/input.hpp"
#include "common/log.hpp"
#include "core/pack/options.hpp"
#include "core/render/pipeline.hpp"
#include "core/hooks/sdl.hpp"
#include "core/dev/developer.hpp"
#include "core/dev/trace.hpp"
#include "common/ui_theme.hpp"
#include "common/util.hpp"
#include "common/version.hpp"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

// From imgui_impl_opengl3_loader.h (compiled inside imgui_impl_opengl3.cpp):
// lets us hand ImGui Prisma's raw GL functions instead of the hooked ones.
typedef void (*GL3WglProc)(void);
typedef GL3WglProc (*GL3WGetProcAddressProc)(const char *proc);
extern "C" int imgl3wInit2(GL3WGetProcAddressProc proc);

namespace prisma::overlay {

static std::filesystem::path g_fonts_dir;
static bool g_visible = false;
static bool g_ready = false;
static bool g_failed = false;
static double g_last_time = 0;

void init(const std::filesystem::path &fonts_dir)
{
	g_fonts_dir = fonts_dir;
}

void set_visible(bool visible)
{
	g_visible = visible;
}

bool visible()
{
	return g_visible;
}

static GL3WglProc resolve(const char *name)
{
	return reinterpret_cast<GL3WglProc>(hooks::raw_proc(name));
}

static bool setup()
{
	if (g_ready || g_failed)
		return g_ready;
	if (!gl::load() || imgl3wInit2(resolve) != 0) {
		g_failed = true;
		log::error("Could not prepare OpenGL for the menu");
		return false;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; // never write imgui.ini into the game folder
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.MouseDrawCursor = true;

	// Luanti's own fonts: Arimo for the text, Cousine for the labels and values
	ui::load_fonts(g_fonts_dir / "Arimo-Regular.ttf", g_fonts_dir / "Arimo-Bold.ttf", g_fonts_dir / "Cousine-Regular.ttf",
		g_fonts_dir / "Cousine-Bold.ttf", 17.0f);
	// The game shows a little through the menu
	ui::apply_style(1, 0.94f);

	if (!ImGui_ImplOpenGL3_Init("#version 150")) {
		g_failed = true;
		log::error("Could not start drawing the menu");
		return false;
	}
	g_ready = true;
	log::info("Menu ready (Dear ImGui %s)", ImGui::GetVersion());
	return true;
}

// ---------------------------------------------------------------- input

static ImGuiKey to_imgui_key(int sc)
{
	if (sc >= 4 && sc <= 29)
		return ImGuiKey(ImGuiKey_A + (sc - 4));
	if (sc >= 30 && sc <= 38)
		return ImGuiKey(ImGuiKey_1 + (sc - 30));
	if (sc >= 58 && sc <= 69)
		return ImGuiKey(ImGuiKey_F1 + (sc - 58));
	switch (sc) {
	case 39: return ImGuiKey_0;
	case 40: return ImGuiKey_Enter;
	case 41: return ImGuiKey_Escape;
	case 42: return ImGuiKey_Backspace;
	case 43: return ImGuiKey_Tab;
	case 44: return ImGuiKey_Space;
	case 73: return ImGuiKey_Insert;
	case 74: return ImGuiKey_Home;
	case 75: return ImGuiKey_PageUp;
	case 76: return ImGuiKey_Delete;
	case 77: return ImGuiKey_End;
	case 78: return ImGuiKey_PageDown;
	case 79: return ImGuiKey_RightArrow;
	case 80: return ImGuiKey_LeftArrow;
	case 81: return ImGuiKey_DownArrow;
	case 82: return ImGuiKey_UpArrow;
	case 88: return ImGuiKey_KeypadEnter;
	case 224: return ImGuiKey_LeftCtrl;
	case 225: return ImGuiKey_LeftShift;
	case 226: return ImGuiKey_LeftAlt;
	case 228: return ImGuiKey_RightCtrl;
	case 229: return ImGuiKey_RightShift;
	case 230: return ImGuiKey_RightAlt;
	}
	return ImGuiKey_None;
}

void mouse_move(float x, float y)
{
	if (g_ready)
		ImGui::GetIO().AddMousePosEvent(x, y);
}

void mouse_button(int sdl_button, bool down)
{
	// SDL: 1 left, 2 middle, 3 right
	int b = sdl_button == 1 ? 0 : sdl_button == 3 ? 1 : sdl_button == 2 ? 2 : -1;
	if (g_ready && b >= 0)
		ImGui::GetIO().AddMouseButtonEvent(b, down);
}

void mouse_wheel(float x, float y)
{
	if (g_ready)
		ImGui::GetIO().AddMouseWheelEvent(-x, y);
}

void key(int scancode, bool down, unsigned mods)
{
	if (!g_ready)
		return;
	ImGuiIO &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, (mods & 0x00C0) != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (mods & 0x0003) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (mods & 0x0300) != 0);
	ImGuiKey k = to_imgui_key(scancode);
	if (k != ImGuiKey_None)
		io.AddKeyEvent(k, down);
}

void text(const char *utf8)
{
	if (g_ready)
		ImGui::GetIO().AddInputCharactersUTF8(utf8);
}

// ---------------------------------------------------------------- menu

/*
	The menu: a window with the pack's option groups on the left
	and the options of the chosen group on the right. Below, the help for the setting
	under the mouse. "Pack and keys" and "Errors" are pages of their own.
	The look (an instrument panel) is shared with the launcher: common/ui_theme.
*/

namespace col = ui::col;
static const ImU32 k_accent = col::accent;
static const ImVec4 k_accent_text = ui::v4(col::accent);
static const ImVec4 k_error_text = ui::v4(col::error);

static const char *k_page_pack = "\x01pack";
static const char *k_page_errors = "\x01errors";
static const char *k_page_debug = "\x01" "debugging"; // (split: "\x01de" would be one hex escape)
static std::string g_page;      // group (or special page) being shown
static std::string g_page_pack; // pack that g_page applies to
static int g_hovered = -1;      // option under the mouse in this frame

// Value being dragged on a #define slider: applied (and the pack
// recompiled) only when the slider is released.
static std::map<size_t, double> g_pending;

static int decimals_for(const pack::OptionDef &d)
{
	if (d.type == pack::OptionDef::Int)
		return 0;
	return d.step > 0 ? std::max(0, (int)std::ceil(-std::log10(d.step) - 1e-6)) : 3;
}

static std::string format_value(const pack::OptionDef &d, double v)
{
	if (d.type == pack::OptionDef::Bool)
		return v != 0 ? "on" : "off";
	if (d.type == pack::OptionDef::Choice) {
		int k = (int)v;
		return k >= 0 && k < (int)d.values.size() ? d.values[k] : "?";
	}
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals_for(d), v);
	return buf;
}

// How many segments a slider has: one per step when an integer has few,
// otherwise as many as fit
static int segment_count(const pack::OptionDef &d, float width)
{
	if (d.type == pack::OptionDef::Int) {
		double steps = d.max - d.min;
		if (steps >= 4 && steps <= 32)
			return (int)steps;
	}
	return std::clamp((int)(width / 11), 8, 40);
}

// A row: the name on the left, the control in the middle and the value on the
// right. Right-click the row: back to its default value.
static void option_row(size_t i, const pack::OptionDef &d, float label_w, float value_w)
{
	ImGui::PushID((int)i);
	auto pending = g_pending.find(i);
	double v = pending != g_pending.end() ? pending->second : options::value(i);
	bool modified = std::abs(options::value(i) - d.def) > 1e-9;
	bool changed = false, released = false, hovered = false;
	double nv = v;

	float row_y = ImGui::GetCursorPosY();
	ImVec2 row_min = ImGui::GetCursorScreenPos();
	ImGui::AlignTextToFramePadding();
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_w - 18);
	ImGui::TextUnformatted(d.label.c_str());
	ImGui::PopTextWrapPos();
	hovered |= ImGui::IsItemHovered();
	bool reset = ImGui::IsItemClicked(ImGuiMouseButton_Right);
	if (modified) {
		// A square: this setting is not at its default value
		ImVec2 end = ImGui::GetItemRectMax();
		ImVec2 start = ImGui::GetItemRectMin();
		float x = std::round(std::min(end.x + 8, start.x + label_w - 12));
		float y = std::round(start.y + ImGui::GetTextLineHeight() * 0.5f);
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y - 2), ImVec2(x + 4, y + 2), k_accent);
	}
	float after_label = ImGui::GetCursorPosY();
	ImGui::SetCursorPosY(row_y);
	ImGui::SameLine(label_w);
	float widget_w = ImGui::GetContentRegionAvail().x - value_w;

	switch (d.type) {
	case pack::OptionDef::Bool: {
		bool b = v != 0;
		if (ui::switch_box("##b", b)) {
			nv = b ? 1 : 0;
			changed = released = true;
		}
		hovered |= ImGui::IsItemHovered();
		reset |= ImGui::IsItemClicked(ImGuiMouseButton_Right);
		break;
	}
	case pack::OptionDef::Int:
	case pack::OptionDef::Float: {
		// A step: the one from pack.toml, 1 for integers, or 1 % of the range
		double step = d.step > 0 ? d.step : d.type == pack::OptionDef::Int ? 1 : (d.max - d.min) / 100;
		auto snap = [&](double x) {
			x = std::clamp(x, d.min, d.max);
			if (d.type == pack::OptionDef::Int)
				return std::round(x);
			return d.step > 0 ? std::clamp(std::round(x / d.step) * d.step, d.min, d.max) : x;
		};
		auto nudge = [&](int dir) {
			double x = snap(v + step * dir);
			if (x != v) {
				nv = x;
				changed = released = true;
			}
		};
		const float arrow_w = ImGui::GetFrameHeight();
		const float gap = 6;

		// Arrows on the sides to adjust step by step; held down, they repeat
		ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		if (ImGui::ArrowButton("##menos", ImGuiDir_Left))
			nudge(-1);
		hovered |= ImGui::IsItemHovered();
		reset |= ImGui::IsItemClicked(ImGuiMouseButton_Right);
		ImGui::SameLine(0, gap);

		float range = (float)std::max(1e-9, d.max - d.min);
		float t = (float)((v - d.min) / range);
		float bar_w = std::max(40.f, widget_w - 2 * (arrow_w + gap));
		float mark = (float)((d.def - d.min) / range);
		if (ui::segments("##s", t, bar_w, segment_count(d, bar_w), mark)) {
			double x = snap(d.min + t * range);
			if (x != v) {
				nv = x;
				changed = true;
			}
		}
		hovered |= ImGui::IsItemHovered();
		reset |= ImGui::IsItemClicked(ImGuiMouseButton_Right);
		released |= ImGui::IsItemDeactivated();
		// The mouse wheel also moves one step
		float wheel = ImGui::GetIO().MouseWheel;
		if (ImGui::IsItemHovered() && wheel != 0)
			nudge(wheel > 0 ? 1 : -1);
		ImGui::SameLine(0, gap);

		if (ImGui::ArrowButton("##mas", ImGuiDir_Right))
			nudge(1);
		hovered |= ImGui::IsItemHovered();
		reset |= ImGui::IsItemClicked(ImGuiMouseButton_Right);
		ImGui::PopItemFlag();
		ImGui::SameLine(0, 0);
		std::string text = format_value(d, nv);
		float tw = ui::label_size(text, 0.88f).x;
		// Clear of the scrollbar
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + value_w - tw - 10);
		ImGui::AlignTextToFramePadding();
		ui::label(text, modified || changed ? col::accent : col::text, 0.88f);
		break;
	}
	case pack::OptionDef::Choice: {
		int x = (int)v;
		ImGui::SetNextItemWidth(widget_w + value_w - 10);
		if (ImGui::BeginCombo("##c", format_value(d, v).c_str())) {
			for (int k = 0; k < (int)d.values.size(); k++)
				if (ImGui::Selectable(d.values[k].c_str(), k == x)) {
					nv = k;
					changed = released = true;
				}
			ImGui::EndCombo();
		}
		hovered |= ImGui::IsItemHovered();
		reset |= ImGui::IsItemClicked(ImGuiMouseButton_Right);
		break;
	}
	}
	// The row takes as much height as the name needs if it takes two lines
	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), after_label));

	// The whole row counts: hover help, and right-click to reset
	ImVec2 row_max(row_min.x + label_w + widget_w + value_w, ImGui::GetCursorScreenPos().y);
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
			ImGui::IsMouseHoveringRect(row_min, row_max)) {
		hovered = true;
		reset |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
	}

	if (reset && std::abs(v - d.def) > 1e-9) {
		nv = d.def;
		changed = released = true;
	}
	if (hovered) {
		g_hovered = (int)i;
		// The row under the mouse: a faint band behind it
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(row_min.x - 8, row_min.y - 3), ImVec2(row_max.x + 4, row_max.y - 3),
			IM_COL32(255, 255, 255, 8));
	}

	if (!d.define) {
		// Uniforms: applied live while dragging
		if (changed)
			options::set(i, nv);
	} else {
		if (changed)
			g_pending[i] = nv;
		if (released && g_pending.count(i)) {
			double value = g_pending[i];
			g_pending.erase(i);
			if (options::set(i, value))
				pipeline::options_changed(true);
		}
	}
	ImGui::PopID();
}

static std::vector<std::string> option_groups()
{
	std::vector<std::string> groups;
	for (const auto &d : options::defs()) {
		std::string g = d.group.empty() ? "General" : d.group;
		if (std::find(groups.begin(), groups.end(), g) == groups.end())
			groups.push_back(g);
	}
	return groups;
}

static void draw_sidebar(const pack::Pack &pack, const std::vector<std::string> &groups, size_t errors)
{
	const float side_w = 196;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, col::panel);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 14));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
	ImGui::BeginChild("side", ImVec2(side_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 wpos = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
	// Its edge with the page
	dl->AddLine(ImVec2(wpos.x + wsize.x - 0.5f, wpos.y), ImVec2(wpos.x + wsize.x - 0.5f, wpos.y + wsize.y), col::line);

	// ■ PRISMA, and the pack below
	ImGui::Indent(14);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float lh = ImGui::GetTextLineHeight();
	float sq = 7, y = std::round(p.y + (lh - sq) * 0.5f);
	dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sq, y + sq), k_accent);
	ImGui::SetCursorScreenPos(ImVec2(p.x + sq + 9, p.y));
	ui::label("Prisma", col::text, 0.86f, true);
	std::string title = pack.title.empty() ? pipeline::selected_pack() : pack.title;
	if (!pack.version.empty())
		title += " · " + pack.version;
	ui::label(title, col::muted, 0.72f);
	ImGui::Unindent(14);
	ImGui::Dummy(ImVec2(0, 10));
	dl->AddLine(ImVec2(wpos.x, ImGui::GetCursorScreenPos().y - 4), ImVec2(wpos.x + wsize.x - 1, ImGui::GetCursorScreenPos().y - 4),
		col::line);

	// The groups, numbered; the rest scrolls if a pack has many
	const float specials_h = (ImGui::GetFrameHeight() * 1.05f + 2) * (errors > 0 ? 3 : 2) + 30;
	ImGui::BeginChild("groups", ImVec2(0, -specials_h), 0, ImGuiWindowFlags_NoBackground);
	int n = 1;
	for (const std::string &g : groups) {
		char num[8];
		std::snprintf(num, sizeof num, "%02d", n++);
		if (ui::nav_item(num, g, g_page == g))
			g_page = g;
	}
	ImGui::EndChild();

	float sy = ImGui::GetCursorScreenPos().y + 4;
	dl->AddLine(ImVec2(wpos.x, sy), ImVec2(wpos.x + wsize.x - 1, sy), col::line);
	ImGui::Dummy(ImVec2(0, 10));
	if (ui::nav_item("", "Pack and keys", g_page == k_page_pack))
		g_page = k_page_pack;
	if (ui::nav_item("", "Debugging", g_page == k_page_debug))
		g_page = k_page_debug;
	if (errors > 0) {
		std::string count = std::to_string(errors);
		if (ui::nav_item("", "Errors", g_page == k_page_errors, count.c_str(), col::error))
			g_page = k_page_errors;
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

// The frame rate in the menu: off unless the player turns it on (Pack and keys)
static bool g_show_fps = false;

// The page's title, and on the right the shaders switch and the × to close;
// a hairline under them
static void draw_header(const char *title)
{
	float top = ImGui::GetCursorPosY();
	float left = ImGui::GetCursorPosX();
	float right = left + ImGui::GetContentRegionAvail().x;
	float fh = ImGui::GetFrameHeight();
	ImGui::AlignTextToFramePadding();
	ui::label(title, col::text, 0.92f, true);

	std::string hint = "[" + input::toggle_key_name() + "] Shaders";
	float hint_w = ui::label_size(hint, 0.72f).x;
	float sw = ui::label_size("OFF", 0.74f, true).x + 16;
	ImGui::SetCursorPos(ImVec2(right - fh - 10 - sw - 10 - hint_w, top));
	ImGui::AlignTextToFramePadding();
	ui::label(hint, col::muted, 0.72f);
	ImGui::SameLine(0, 10);
	bool on = pipeline::enabled();
	if (ui::switch_box("##on", on))
		pipeline::set_enabled(on);
	ImGui::SetItemTooltip(on ? "Turn the pack off to compare" : "Turn the pack on");
	ImGui::SameLine(0, 10);
	if (ui::close_button("##close"))
		input::set_menu_open(false);

	float line_y = ImGui::GetWindowPos().y + top + fh + 10 - ImGui::GetScrollY();
	ImVec2 wpos = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, line_y), ImVec2(wpos.x + ImGui::GetWindowWidth(), line_y), col::line);
	ImGui::SetCursorPos(ImVec2(left, top + fh + 20));
}

// A key, in a small box
static void key_chip(const std::string &name)
{
	ImVec2 s = ui::label_size(name, 0.76f, true);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float fh = ImGui::GetFrameHeight(), h = std::round(fh * 0.8f);
	float w = s.x + 14;
	ImGui::Dummy(ImVec2(w, fh));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a(p.x, std::round(p.y + (fh - h) * 0.5f)), b(p.x + w, a.y + h);
	dl->AddRect(a, b, col::line_strong);
	ui::draw_label(dl, ImVec2(a.x + 7, std::round(a.y + (h - s.y) * 0.5f)), name, col::text, 0.76f, true);
}

static void draw_options_page(const std::string &group)
{
	const auto &defs = options::defs();
	// Name, two lines of description and the default value
	const float help_h = ImGui::GetTextLineHeightWithSpacing() * 4 + 16;
	const float footer_h = ImGui::GetFrameHeightWithSpacing() + 4;

	ImGui::BeginChild("opts", ImVec2(0, -(help_h + footer_h + 8)));
	float label_w = std::min(230.f, ImGui::GetContentRegionAvail().x * 0.42f);
	float value_w = 72;
	g_hovered = -1;
	for (size_t i = 0; i < defs.size(); i++) {
		std::string g = defs[i].group.empty() ? "General" : defs[i].group;
		if (g == group) {
			option_row(i, defs[i], label_w, value_w);
			ImGui::Dummy(ImVec2(0, 4));
		}
	}
	ImGui::EndChild();

	// The help for the setting under the mouse, under a hairline
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 wpos = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, p.y), ImVec2(wpos.x + ImGui::GetWindowWidth(), p.y), col::line);
	ImGui::Dummy(ImVec2(0, 8));
	ImGui::BeginChild("help", ImVec2(0, help_h - 8), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PushTextWrapPos(0);
	if (g_hovered >= 0 && (size_t)g_hovered < defs.size()) {
		const pack::OptionDef &d = defs[g_hovered];
		ImGui::PushFont(ui::fonts().bold, 0);
		ImGui::TextUnformatted(d.label.c_str());
		ImGui::PopFont();
		if (!d.description.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, col::muted);
			ImGui::TextUnformatted(d.description.c_str());
			ImGui::PopStyleColor();
		}
		std::string meta = "Default " + format_value(d, d.def) + "  ·  right-click resets";
		if (d.define)
			meta += "  ·  recompiles the pack";
		ui::label(meta, col::line_strong, 0.7f);
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, col::muted);
		ImGui::TextUnformatted("Hover over a setting to see what it does. The arrows and the wheel move it step by step; "
			"right-click returns it to its default value, marked by the tick under the bar. An amber square marks what "
			"you have changed.");
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndChild();

	// Footer
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	ImGui::AlignTextToFramePadding();
	std::string keys = "[" + input::menu_key_name() + "] [Esc] Close";
	if (g_show_fps) {
		char fps[32];
		std::snprintf(fps, sizeof fps, "  ·  %.0f fps", ImGui::GetIO().Framerate);
		keys += fps;
	}
	ui::label(keys, col::muted, 0.7f);
	const char *reset_label = "Reset group";
	float bw = ui::label_size(reset_label, 0.8f, true).x + ImGui::GetStyle().FramePadding.x * 2 + 4;
	ImGui::SameLine(right - bw);
	if (ui::button(reset_label, ImVec2(bw, 0))) {
		bool recompile = false;
		for (size_t i = 0; i < defs.size(); i++) {
			std::string g = defs[i].group.empty() ? "General" : defs[i].group;
			if (g == group && options::set(i, defs[i].def))
				recompile |= defs[i].define;
		}
		g_pending.clear();
		pipeline::options_changed(recompile);
	}
}

static void draw_pack_page(const pack::Pack &pack)
{
	ImGui::BeginChild("pack", ImVec2(0, 0));
	ImGui::PushTextWrapPos(0);
	const float label_w = 150;

	std::vector<std::string> packs = pipeline::available_packs();
	const std::string &selected = pipeline::selected_pack();
	ImGui::AlignTextToFramePadding();
	ui::label("Pack");
	ImGui::SameLine(label_w);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##pack", selected.c_str())) {
		for (const std::string &p : packs)
			if (ImGui::Selectable(p.c_str(), p == selected))
				pipeline::select_pack(p);
		ImGui::EndCombo();
	}
	ImGui::Dummy(ImVec2(0, 6));
	if (!pack.title.empty()) {
		ImGui::PushFont(ui::fonts().bold, ImGui::GetStyle().FontSizeBase * 1.3f);
		ImGui::TextUnformatted(pack.title.c_str());
		ImGui::PopFont();
	}
	if (!pack.author.empty() || !pack.version.empty()) {
		std::string by = pack.author;
		if (!pack.version.empty())
			by += (by.empty() ? "Version " : "  ·  version ") + pack.version;
		ui::label(by, col::muted, 0.72f);
	}
	if (!pack.description.empty()) {
		ImGui::Dummy(ImVec2(0, 2));
		ImGui::TextUnformatted(pack.description.c_str());
	}
	const GameState *state = frame::last_state();
	for (const std::string &need : pack.needs)
		if (need == "shadows" && state && !state->luanti_shadows)
			ImGui::TextColored(k_accent_text, "This pack needs Luanti's dynamic shadows (in Luanti's settings: Shadows).");

	ImGui::Dummy(ImVec2(0, 6));
	if (ui::button("Open pack folder"))
		pipeline::open_pack_folder();
	ImGui::SameLine();
	if (!options::defs().empty() && ui::button("Reset all options")) {
		const auto &defs = options::defs();
		bool had_defines = std::any_of(defs.begin(), defs.end(), [](const auto &d) { return d.define; });
		options::reset_all();
		g_pending.clear();
		pipeline::options_changed(had_defines);
	}

	ImGui::Dummy(ImVec2(0, 12));
	ui::section("Keys");
	auto key_row = [&](const char *what, const std::string &key, const char *extra) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(what);
		ImGui::SameLine(std::max(label_w, ImGui::GetContentRegionAvail().x * 0.5f));
		key_chip(key);
		if (extra) {
			ImGui::SameLine(0, 6);
			key_chip(extra);
		}
	};
	key_row("Open and close this menu", input::menu_key_name(), "Esc");
	key_row("Turn the pack on or off", input::toggle_key_name(), nullptr);
	ImGui::TextDisabled("They are changed in the prisma.exe window, before opening the game.");

	ImGui::Dummy(ImVec2(0, 12));
	ui::section("Display");
	ui::switch_box("##fps", g_show_fps);
	ImGui::SameLine(0, 10);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Show the frame rate in the menu");
	if (ImGui::IsItemClicked())
		g_show_fps = !g_show_fps;
	if (g_show_fps) {
		ImGui::SameLine(0, 12);
		char fps[32];
		std::snprintf(fps, sizeof fps, "%.0f fps", ImGui::GetIO().Framerate);
		ui::label(fps, col::accent, 0.82f);
	}

	if (developer::enabled()) {
		ImGui::Dummy(ImVec2(0, 12));
		ui::section("Tools");
		if (ui::button("Trace one frame"))
			trace::request();
		ImGui::SameLine(0, 10);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Saves to logs/ everything Luanti draws in the next frame.");
	}

	ImGui::PopTextWrapPos();
	ImGui::EndChild();
}

static void draw_errors_page(const std::vector<std::string> &errors)
{
	ImGui::BeginChild("errors", ImVec2(0, 0));
	ImGui::PushTextWrapPos(0);
	ui::label("Full details in logs/prisma.log", col::muted, 0.72f);
	ImGui::Dummy(ImVec2(0, 6));
	size_t first = errors.size() > 30 ? errors.size() - 30 : 0;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	for (size_t i = first; i < errors.size(); i++) {
		// A red bar on the left of each
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::Indent(12);
		ImGui::TextColored(k_error_text, "%s", errors[i].c_str());
		ImGui::Unindent(12);
		dl->AddRectFilled(p, ImVec2(p.x + 2, ImGui::GetItemRectMax().y), col::error);
		ImGui::Dummy(ImVec2(0, 4));
	}
	ImGui::PopTextWrapPos();
	ImGui::EndChild();
}

// ---------------------------------------------------------------- debugging

static std::string material_name(int id)
{
	static const char *names[] = {"nothing (sky)", "solid", "leaves", "plants", "liquid", "opaque liquid (lava)",
		"entity"};
	return id >= 0 && id < (int)std::size(names) ? names[id] : "unknown";
}

static const debugview::Source *selected_source()
{
	for (const debugview::Source &s : debugview::sources())
		if (s.name == debugview::settings().source)
			return &s;
	return nullptr;
}

static void draw_timings(bool compact)
{
	if (!debugview::timings_supported()) {
		ImGui::TextDisabled("The driver does not allow measuring GPU times.");
		return;
	}
	const auto &list = debugview::timings();
	if (list.empty()) {
		ImGui::TextDisabled("Measuring...");
		return;
	}
	double total = 0, longest = 1e-6;
	for (const debugview::Timing &t : list) {
		total += t.ms;
		longest = std::max(longest, t.ms);
	}
	if (ImGui::BeginTable("timings", compact ? 2 : 3, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("pass", ImGuiTableColumnFlags_WidthStretch, 3);
		ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 64);
		if (!compact)
			ImGui::TableSetupColumn("barra", ImGuiTableColumnFlags_WidthStretch, 2);
		for (const debugview::Timing &t : list) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(t.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextColored(k_accent_text, "%6.2f ms", t.ms);
			if (!compact) {
				ImGui::TableNextColumn();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = ImGui::GetContentRegionAvail().x * (float)(t.ms / longest);
				float h = ImGui::GetTextLineHeight();
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f),
					ImVec2(p.x + std::max(1.f, w), p.y + h * 0.75f), k_accent);
				ImGui::Dummy(ImVec2(0, h));
			}
		}
		ImGui::EndTable();
	}
	ImGui::Text("Prisma total: %.2f ms  ·  %.0f FPS", total, ImGui::GetIO().Framerate);
	if (debugview::timings_slow_down() && compact)
		ImGui::TextDisabled("(measured by waiting for the GPU: the game runs slower)");
	else if (debugview::timings_slow_down())
		ImGui::TextDisabled("Luanti's OpenGL has no time queries: they are measured by waiting for the GPU at each "
			"pass, so while the timings are shown the game runs slower.");
}

// The values of the texel under the mouse (or under the crosshair)
static void draw_probe(const debugview::Source &src, bool at_mouse)
{
	const debugview::Probe &p = debugview::probe();
	if (!p.valid || p.source != src.name)
		return;
	ImGui::Text("%s, texel (%d, %d):  %.4g  %.4g  %.4g  %.4g", at_mouse ? "Under the mouse" : "At the center", p.x, p.y,
		p.raw[0], p.raw[1], p.raw[2], p.raw[3]);
	if (src.kind == debugview::KIND_MATERIAL) {
		int id = (int)std::lround(p.raw[0] * 255);
		ImGui::Text("Material %d: %s", id, material_name(id).c_str());
	} else if (p.distance >= 1e8f) {
		ImGui::TextUnformatted("Sky (nothing drawn)");
	} else if (p.distance >= 0) {
		ImGui::Text("%.2f nodes from the camera", p.distance);
	}
}

static void draw_viewer()
{
	debugview::Settings &st = debugview::settings();
	const auto &list = debugview::sources();
	const debugview::Source *cur = selected_source();

	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##fuente", st.source.c_str())) {
		for (const debugview::Source &s : list) {
			bool available = s.texture != 0;
			std::string label = available ? s.name : s.name + "  (not available)";
			ImGui::BeginDisabled(!available);
			if (ImGui::Selectable(label.c_str(), s.name == st.source)) {
				st.source = s.name;
				st.auto_range = true;
			}
			ImGui::EndDisabled();
		}
		ImGui::EndCombo();
	}
	if (list.empty()) {
		ImGui::TextDisabled("Textures appear when the pack draws with a world open.");
		return;
	}
	if (!cur || !cur->texture) {
		ImGui::TextDisabled("Not present in this frame: the pack does not request it in pack.toml, or the graphics card does not allow it.");
		return;
	}

	if (cur->kind == debugview::KIND_COLOR) {
		static const char *channels[] = {"RGB", "R", "G", "B", "A"};
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Channel");
		for (int i = 0; i < 5; i++) {
			ImGui::SameLine();
			ImGui::RadioButton(channels[i], &st.channel, i);
		}
	}
	if (cur->kind != debugview::KIND_MATERIAL) {
		float speed = std::max(0.0005f, (st.hi - st.lo) * 0.005f);
		ImGui::SetNextItemWidth(260);
		ImGui::DragFloatRange2("##rango", &st.lo, &st.hi, speed, -1e6f, 1e6f, "black %.4g", "white %.4g");
		ImGui::SameLine();
		if (ImGui::Button("Reset"))
			st.auto_range = true;
		ImGui::SameLine();
		ImGui::TextDisabled(cur->kind == debugview::KIND_DEPTH ? "(distance in nodes)" : "(stored values)");
	} else {
		ImGui::TextDisabled("Sky: black · solid: grey · leaves: green · plants: lime · liquid: blue · "
			"lava: orange · entity: pink");
	}
	if (cur->levels > 1) {
		ImGui::SetNextItemWidth(260);
		ImGui::SliderInt("Mipmap level", &st.lod, 0, cur->levels - 1);
	}
	ImGui::Checkbox("Full screen", &st.fullscreen);
	ImGui::SetItemTooltip("The texture covers the game, also with the menu closed (the values at the center, where "
		"the crosshair points, show up at the top right).");
	ImGui::SameLine(0, 16);
	ImGui::Checkbox("Timings over the game", &st.hud);
	ImGui::SetItemTooltip("Shows the cost of each pass at the top right while playing.");

	GLuint tex = debugview::view_texture();
	bool at_mouse = false;
	if (tex && debugview::view_width() > 0) {
		float aspect = (float)debugview::view_height() / (float)debugview::view_width();
		float w = ImGui::GetContentRegionAvail().x, h = w * aspect;
		const float max_h = 320;
		if (h > max_h) {
			h = max_h;
			w = h / aspect;
		}
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
		if (ImGui::IsItemHovered()) {
			ImVec2 m = ImGui::GetIO().MousePos;
			debugview::set_probe((m.x - p0.x) / w, 1.f - (m.y - p0.y) / h);
			at_mouse = true;
		}
	}
	if (!at_mouse)
		debugview::set_probe(-1, -1);
	draw_probe(*cur, at_mouse);
}

static void draw_state()
{
	const GameState *s = debugview::state();
	if (!s) {
		ImGui::TextDisabled("No data yet.");
		return;
	}
	if (!ImGui::BeginTable("prisma", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
		return;
	auto row = [](const char *name, const char *fmt, auto... args) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(name);
		ImGui::TableNextColumn();
		ImGui::Text(fmt, args...);
	};
	auto v3 = [&](const char *name, const Vec3 &v) { row(name, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); };
	auto f3 = [&](const char *name, const float *v) { row(name, "(%.3f, %.3f, %.3f)", v[0], v[1], v[2]); };
	auto f4 = [&](const char *name, const float *v) { row(name, "(%.3f, %.3f, %.3f, %.3f)", v[0], v[1], v[2], v[3]); };
	auto yes = [](bool b) { return b ? "yes" : "no"; };
	v3("cameraPosition", s->camera_pos);
	v3("cameraOffset", s->camera_offset);
	row("sunAvailable", "%s", yes(s->sun_available));
	v3("sunDirection", s->sun_dir);
	v3("moonDirection", s->moon_dir);
	v3("lightDirection", s->light_dir);
	row("timeOfDay", "%.4f", s->time_of_day);
	row("skyHidden", "%s", yes(s->sky_hidden));
	f3("dayLight", s->day_light);
	f3("fogColor", s->fog_color);
	row("fogDistance", "%.1f", s->fog_distance);
	row("fogShading", "%.3f", s->fog_shading);
	f4("cloudColor", s->cloud_color);
	f4("liquidColor", s->liquid_color);
	row("nearPlane / farPlane", "%.3f / %.1f", s->near_plane, s->far_plane);
	row("shadowAvailable", "%s", yes(s->shadow_available));
	if (s->shadow_available)
		row("shadowDistance / MapSize / Blend", "%.0f / %d / %.2f", s->shadow_distance, s->shadow_size, s->shadow_blend);
	row("overheadAvailable", "%s", yes(s->overhead_available));
	if (s->overhead_available)
		row("overheadDistance / MapSize", "%.0f / %d", s->overhead_distance, s->overhead_size);
	row("viewSize", "%d x %d", s->width, s->height);
	row("frameTime", "%.2f ms", s->frame_time * 1000);
	row("frameTimeCounter", "%.2f", s->time);
	row("animationTimer", "%.3f", s->animation_timer);
	row("frameCounter", "%d", s->frame);
	ImGui::EndTable();
}

static void draw_debug_page()
{
	ImGui::BeginChild("debug", ImVec2(0, 0));
	ImGui::PushTextWrapPos(0);
	ui::section("GPU timings");
	draw_timings(false);
	if (hookcost::g_on) {
		// Developer mode: the CPU side, averaged over the last few seconds
		const hookcost::Summary &c = hookcost::last();
		ImGui::TextDisabled("CPU in Prisma per frame: %.3f ms in the hooks of %.0f GL calls, %.3f ms in the passes, "
			"%.3f ms in other work at the end of the frame.", c.hooks_ms, c.calls_per_frame, c.passes_ms,
			c.frame_end_ms);
	}
	ImGui::Dummy(ImVec2(0, 10));
	ui::section("Texture viewer");
	draw_viewer();
	ImGui::Dummy(ImVec2(0, 10));
	ui::section("Prisma values");
	ImGui::TextDisabled("What the passes received in the last frame.");
	draw_state();
	ImGui::PopTextWrapPos();
	ImGui::EndChild();
}

// Over the game: the texture viewer in full screen and the timings, which
// stay while playing with the menu closed.
static void draw_game_overlay()
{
	const debugview::Settings &st = debugview::settings();
	ImGuiIO &io = ImGui::GetIO();
	GLuint tex = debugview::view_texture();
	bool full = st.fullscreen && tex && debugview::view_width() > 0;
	if (full) {
		ImDrawList *bg = ImGui::GetBackgroundDrawList();
		bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
		float aspect = (float)debugview::view_width() / (float)debugview::view_height();
		float w = io.DisplaySize.x, h = w / aspect;
		if (h > io.DisplaySize.y) {
			h = io.DisplaySize.y;
			w = h * aspect;
		}
		ImVec2 p0((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f);
		bg->AddImage((ImTextureID)(intptr_t)tex, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 1), ImVec2(1, 0));
		if (!g_visible) {
			// Where the probe reads
			ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
			bg->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), k_accent, 2);
			bg->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), k_accent, 2);
		}
	}
	if (g_visible || (!full && !st.hud))
		return;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
	ImGui::SetNextWindowBgAlpha(0.86f);
	ImGui::SetNextWindowSizeConstraints(ImVec2(260, 0), ImVec2(520, io.DisplaySize.y - 24));
	ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoSavedSettings);
	// First, so a long list of timings can't push it out of the screen
	ui::label("Prisma menu [" + input::menu_key_name() + "] > Debugging to remove it", col::muted, 0.68f);
	if (full) {
		ImGui::PushFont(ui::fonts().bold, 0);
		ImGui::TextUnformatted(st.source.c_str());
		ImGui::PopFont();
		if (const debugview::Source *src = selected_source())
			draw_probe(*src, false);
	}
	if (st.hud) {
		if (full)
			ImGui::Dummy(ImVec2(0, 4));
		draw_timings(true);
	}
	ImGui::End();
}

static void draw_menu()
{
	const pack::Pack &pack = pipeline::current_pack();
	std::vector<std::string> groups = option_groups();
	std::vector<std::string> errors = log::recent_errors();

	// The chosen page stops applying when the pack changes or its errors get fixed
	if (g_page_pack != pipeline::selected_pack()) {
		g_page_pack = pipeline::selected_pack();
		g_page.clear();
	}
	bool valid = (g_page == k_page_pack) || (g_page == k_page_debug) || (g_page == k_page_errors && !errors.empty()) ||
		std::find(groups.begin(), groups.end(), g_page) != groups.end();
	if (!valid)
		g_page = groups.empty() ? k_page_pack : groups.front();

	ImGuiIO &io = ImGui::GetIO();
	ImVec2 size(std::min(720.f, io.DisplaySize.x - 40), std::min(500.f, io.DisplaySize.y - 40));
	ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
	// Top left, with a margin; it can be dragged afterwards
	ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(560, 360), ImVec2(io.DisplaySize.x, io.DisplaySize.y));
	// One pixel of padding, so the children leave the window's hairline border visible
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1, 1));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	ImGui::Begin("Prisma", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar(2);
	// Crop marks at the corners, over everything in the window
	ImVec2 wmin = ImGui::GetWindowPos();
	ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
	ui::corner_marks(ImGui::GetForegroundDrawList(), wmin, wmax, k_accent, 10);

	draw_sidebar(pack, groups, errors.size());
	ImGui::SameLine(0, 0);

	// No scrollbar of its own: the options, the pack and the errors scroll inside
	ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if (g_page == k_page_pack) {
		draw_header("Pack and keys");
		draw_pack_page(pack);
	} else if (g_page == k_page_errors) {
		draw_header("Errors");
		draw_errors_page(errors);
	} else if (g_page == k_page_debug) {
		draw_header("Debugging");
		draw_debug_page();
	} else {
		draw_header(g_page.c_str());
		draw_options_page(g_page);
	}
	ImGui::EndChild();
	ImGui::End();
}

void render(void *window)
{
	// What the next frame of the pipeline must prepare for the debugging tools
	const bool debug_page = g_visible && g_page == k_page_debug;
	debugview::set_viewer_open(debug_page);
	debugview::set_profiling(debug_page || debugview::settings().hud);
	if (!g_visible)
		debugview::set_probe(-1, -1); // the crosshair
	bool draw = g_visible || debugview::wants_overlay();
	if (!draw || !setup()) {
		debugview::end_frame();
		return;
	}

	// In screen points, like the mouse events; drawn at the window's pixels
	int w = 0, h = 0, pw = 0, ph = 0;
	sdl::window_size(window, w, h);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(float(w), float(h));
	if (w > 0 && h > 0 && sdl::drawable_size(window, pw, ph))
		io.DisplayFramebufferScale = ImVec2(float(pw) / w, float(ph) / h);
	double now = uptime();
	io.DeltaTime = g_last_time > 0 ? std::max(1e-4f, float(now - g_last_time)) : 1.f / 60;
	g_last_time = now;

	// The menu takes the mouse; while playing, the game's own cursor (none)
	io.MouseDrawCursor = g_visible;
	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();
	draw_game_overlay();
	if (g_visible)
		draw_menu();
	ImGui::Render();
	debugview::end_frame();

	// Onto the window, whatever the game left bound
	GLint draw_fbo = 0;
	gl::GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
}

} // namespace prisma::overlay
