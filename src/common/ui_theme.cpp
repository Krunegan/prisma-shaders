// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/ui_theme.hpp"
#include "common/util.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace prisma::ui {

static Fonts g_fonts;
static float g_scale = 1;

const Fonts &fonts()
{
	return g_fonts;
}

float S(float v)
{
	return v * g_scale;
}

static ImFont *add_font(const std::filesystem::path &path, float size)
{
	std::error_code ec;
	if (path.empty() || !std::filesystem::is_regular_file(path, ec))
		return nullptr;
	return ImGui::GetIO().Fonts->AddFontFromFileTTF(to_utf8(path).c_str(), size);
}

void load_fonts(const std::filesystem::path &regular, const std::filesystem::path &bold,
	const std::filesystem::path &mono, const std::filesystem::path &mono_bold, float size)
{
	ImGuiIO &io = ImGui::GetIO();
	g_fonts.regular = add_font(regular, size);
	if (!g_fonts.regular)
		g_fonts.regular = io.Fonts->AddFontDefault();
	g_fonts.bold = add_font(bold, size);
	if (!g_fonts.bold)
		g_fonts.bold = g_fonts.regular;
	g_fonts.mono = add_font(mono, size);
	if (!g_fonts.mono)
		g_fonts.mono = g_fonts.regular;
	g_fonts.mono_bold = add_font(mono_bold, size);
	if (!g_fonts.mono_bold)
		g_fonts.mono_bold = g_fonts.mono;
	io.FontDefault = g_fonts.regular;
}

void apply_style(float scale, float alpha)
{
	g_scale = std::max(1.f, scale);
	ImGui::StyleColorsDark();
	ImGuiStyle &s = ImGui::GetStyle();
	s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
	s.GrabRounding = s.ScrollbarRounding = s.TabRounding = 0;
	s.WindowBorderSize = 1;
	s.FrameBorderSize = 1;
	s.PopupBorderSize = 1;
	s.ChildBorderSize = 0;
	s.ScrollbarSize = 6;
	s.GrabMinSize = 8;
	s.WindowPadding = ImVec2(16, 14);
	s.FramePadding = ImVec2(10, 5);
	s.ItemSpacing = ImVec2(8, 8);
	s.ItemInnerSpacing = ImVec2(8, 6);
	s.CellPadding = ImVec2(10, 5);
	s.SelectableTextAlign = ImVec2(0, 0.5f);

	ImVec4 *c = s.Colors;
	auto rgb = [](int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
	c[ImGuiCol_Text] = v4(col::text);
	c[ImGuiCol_TextDisabled] = v4(col::muted);
	c[ImGuiCol_WindowBg] = rgb(14, 14, 13, alpha);
	c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
	c[ImGuiCol_PopupBg] = rgb(20, 20, 18, 0.99f);
	c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.6f);
	c[ImGuiCol_Border] = v4(col::line);
	c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
	c[ImGuiCol_FrameBg] = rgb(255, 255, 255, 0.025f);
	c[ImGuiCol_FrameBgHovered] = rgb(255, 255, 255, 0.06f);
	c[ImGuiCol_FrameBgActive] = rgb(255, 255, 255, 0.09f);
	c[ImGuiCol_Button] = rgb(0, 0, 0, 0);
	c[ImGuiCol_ButtonHovered] = rgb(255, 255, 255, 0.07f);
	c[ImGuiCol_ButtonActive] = rgb(232, 163, 60, 0.22f);
	c[ImGuiCol_Header] = rgb(255, 255, 255, 0.08f);
	c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.06f);
	c[ImGuiCol_HeaderActive] = rgb(232, 163, 60, 0.22f);
	c[ImGuiCol_CheckMark] = v4(col::accent);
	c[ImGuiCol_SliderGrab] = v4(col::accent);
	c[ImGuiCol_SliderGrabActive] = v4(col::accent_hi);
	c[ImGuiCol_TextSelectedBg] = rgb(232, 163, 60, 0.35f);
	c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = v4(col::line);
	c[ImGuiCol_ScrollbarGrabHovered] = v4(col::line_strong);
	c[ImGuiCol_ScrollbarGrabActive] = v4(col::accent);
	c[ImGuiCol_Separator] = v4(col::line);
	c[ImGuiCol_SeparatorHovered] = v4(col::line_strong);
	c[ImGuiCol_SeparatorActive] = v4(col::accent);
	c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
	c[ImGuiCol_ResizeGripHovered] = v4(col::line_strong);
	c[ImGuiCol_ResizeGripActive] = v4(col::accent);
	c[ImGuiCol_TableHeaderBg] = rgb(255, 255, 255, 0.04f);
	c[ImGuiCol_TableBorderStrong] = v4(col::line);
	c[ImGuiCol_TableBorderLight] = rgb(58, 57, 52, 0.7f);
	c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
	c[ImGuiCol_PlotHistogram] = v4(col::accent);
	c[ImGuiCol_PlotLines] = v4(col::accent);
	c[ImGuiCol_NavCursor] = v4(col::accent);
	s.ScaleAllSizes(g_scale);
}

std::string upper(std::string_view text)
{
	std::string out(text);
	for (size_t i = 0; i < out.size(); i++) {
		unsigned char ch = (unsigned char)out[i];
		if (ch >= 'a' && ch <= 'z') {
			out[i] = (char)(ch - 32);
		} else if (ch == 0xC3 && i + 1 < out.size()) {
			// U+00E0..U+00FE (but ÷, U+00F7) are U+00C0..U+00DE in capitals
			unsigned char next = (unsigned char)out[i + 1];
			if (next >= 0xA0 && next <= 0xBE && next != 0xB7)
				out[i + 1] = (char)(next - 0x20);
			i++;
		}
	}
	return out;
}

// ---------------------------------------------------------------- labels

static float base_size()
{
	return ImGui::GetStyle().FontSizeBase;
}

static float letter_spacing(float size)
{
	return std::round(base_size() * size * 0.12f);
}

static size_t utf8_len(unsigned char lead)
{
	return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
}

// Draws (if dl) and measures capitals with spaced letters, with the font already pushed
static float spaced(ImDrawList *dl, ImVec2 pos, const std::string &text, ImU32 color, float spacing)
{
	float x = 0;
	bool first = true;
	for (size_t i = 0; i < text.size();) {
		size_t n = std::min(utf8_len((unsigned char)text[i]), text.size() - i);
		const char *b = text.data() + i, *e = b + n;
		if (!first)
			x += spacing;
		if (dl)
			dl->AddText(ImVec2(std::round(pos.x + x), pos.y), color, b, e);
		x += ImGui::CalcTextSize(b, e).x;
		first = false;
		i += n;
	}
	return x;
}

ImVec2 label_size(std::string_view text, float size, bool bold)
{
	ImGui::PushFont(bold ? g_fonts.mono_bold : g_fonts.mono, base_size() * size);
	ImVec2 r(spaced(nullptr, ImVec2(), upper(text), 0, letter_spacing(size)), ImGui::GetFontSize());
	ImGui::PopFont();
	return r;
}

void draw_label(ImDrawList *dl, ImVec2 pos, std::string_view text, ImU32 color, float size, bool bold)
{
	ImGui::PushFont(bold ? g_fonts.mono_bold : g_fonts.mono, base_size() * size);
	spaced(dl, pos, upper(text), color, letter_spacing(size));
	ImGui::PopFont();
}

void label(std::string_view text, ImU32 color, float size, bool bold)
{
	ImGuiWindow *window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return;
	float body_h = ImGui::GetTextLineHeight();
	ImVec2 sz = label_size(text, size, bold);
	// Centred on the line of body text it sits in
	ImVec2 pos(window->DC.CursorPos.x,
		window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset + std::round((body_h - sz.y) * 0.5f));
	ImVec2 item(sz.x, std::max(sz.y, body_h));
	ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + item.x, window->DC.CursorPos.y + item.y));
	ImGui::ItemSize(item, 0.0f);
	if (!ImGui::ItemAdd(bb, 0))
		return;
	draw_label(window->DrawList, pos, text, color, size, bold);
}

void section(std::string_view text)
{
	ImGui::Dummy(ImVec2(0, S(2)));
	float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	label(text, col::muted, 0.74f, true);
	ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
	float y = std::round((min.y + max.y) * 0.5f) + 0.5f;
	if (right > max.x + S(10))
		ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x + S(10), y), ImVec2(right, y), col::line);
	ImGui::Dummy(ImVec2(0, S(2)));
}

// ---------------------------------------------------------------- controls

bool switch_box(const char *id, bool &on)
{
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float fh = ImGui::GetFrameHeight();
	ImVec2 text = label_size("OFF", 0.74f, true);
	float w = std::round(text.x + S(16)), h = std::round(fh * 0.8f);
	bool clicked = ImGui::InvisibleButton(id, ImVec2(w, fh));
	if (clicked)
		on = !on;
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 a(pos.x, std::round(pos.y + (fh - h) * 0.5f)), b(pos.x + w, a.y + h);
	const char *t = on ? "ON" : "OFF";
	ImVec2 ts = label_size(t, 0.74f, true);
	ImVec2 tp(std::round(a.x + (w - ts.x) * 0.5f), std::round(a.y + (h - ts.y) * 0.5f));
	if (on) {
		dl->AddRectFilled(a, b, hovered ? col::accent_hi : col::accent);
		draw_label(dl, tp, t, col::ink, 0.74f, true);
	} else {
		if (hovered)
			dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14));
		dl->AddRect(a, b, hovered ? col::line_strong : col::line);
		draw_label(dl, tp, t, hovered ? col::text : col::muted, 0.74f, true);
	}
	return clicked;
}

bool segments(const char *id, float &t, float width, int count, float mark)
{
	count = std::max(1, count);
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float h = ImGui::GetFrameHeight();
	ImGui::InvisibleButton(id, ImVec2(width, h));
	bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
	bool changed = false;
	if (active) {
		float nt = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / std::max(1.f, width), 0.f, 1.f);
		if (nt != t) {
			t = nt;
			changed = true;
		}
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float gap = std::max(1.f, std::round(S(2)));
	const float cell_h = std::round(S(10));
	float y0 = std::round(pos.y + (h - cell_h) * 0.5f), y1 = y0 + cell_h;
	float cell_w = (width - gap * (count - 1)) / count;
	float fill = std::clamp(t, 0.f, 1.f) * count;
	ImU32 on = active || hovered ? col::accent : col::text;
	for (int k = 0; k < count; k++) {
		float x0 = std::round(pos.x + k * (cell_w + gap)), x1 = std::round(pos.x + k * (cell_w + gap) + cell_w);
		dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col::line);
		float part = std::clamp(fill - k, 0.f, 1.f);
		if (part > 0)
			dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::round(x0 + (x1 - x0) * part), y1), on);
	}
	if (mark >= 0 && mark <= 1) {
		// The default value: a tick under the bar
		float x = std::round(pos.x + mark * width);
		dl->AddRectFilled(ImVec2(x - 1, y1 + S(3)), ImVec2(x + 1, y1 + S(6)), col::muted);
	}
	return changed;
}

bool nav_item(const char *number, std::string_view text, bool selected, const char *detail, ImU32 color)
{
	ImGui::PushStyleColor(ImGuiCol_Header, col::text);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? col::text : IM_COL32(255, 255, 255, 14));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, selected ? col::text : IM_COL32(232, 163, 60, 50));
	ImGui::PushID(text.data(), text.data() + text.size());
	bool clicked = ImGui::Selectable("##nav", selected, 0, ImVec2(0, std::round(ImGui::GetFrameHeight() * 1.05f)));
	bool hovered = ImGui::IsItemHovered();
	ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
	ImGui::PopID();
	ImGui::PopStyleColor(3);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float size = 0.8f;
	float lh = label_size("A", size).y;
	float y = std::round((min.y + max.y - lh) * 0.5f);
	float x = min.x + S(14);
	if (number && *number) {
		draw_label(dl, ImVec2(x, y), number, selected ? col::ink : col::line_strong, size);
		x += label_size("00", size).x + S(10);
	}
	float right = max.x - S(10);
	if (detail) {
		float w = label_size(detail, 0.7f).x;
		float dy = std::round((min.y + max.y - label_size("A", 0.7f).y) * 0.5f);
		draw_label(dl, ImVec2(right - w, dy), detail, selected ? col::ink : col::error, 0.7f);
		right -= w + S(8);
	}
	dl->PushClipRect(ImVec2(x, min.y), ImVec2(right, max.y), true);
	draw_label(dl, ImVec2(x, y), text, selected ? col::ink : hovered ? col::text : color, size, selected);
	dl->PopClipRect();
	return clicked;
}

static bool mono_button(const char *text, ImVec2 size)
{
	float fh = ImGui::GetFrameHeight();
	std::string caps = upper(text);
	ImGui::PushFont(g_fonts.mono_bold, base_size() * 0.8f);
	if (size.y == 0)
		size.y = fh;
	bool clicked = ImGui::Button(caps.c_str(), size);
	ImGui::PopFont();
	return clicked;
}

bool button(const char *text, ImVec2 size)
{
	return mono_button(text, size);
}

bool primary_button(const char *text, ImVec2 size)
{
	ImGui::PushStyleColor(ImGuiCol_Button, col::text);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent_hi);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::accent);
	ImGui::PushStyleColor(ImGuiCol_Border, col::text);
	ImGui::PushStyleColor(ImGuiCol_Text, col::ink);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, col::ink);
	bool clicked = mono_button(text, size);
	ImGui::PopStyleColor(6);
	return clicked;
}

bool close_button(const char *id)
{
	float s = ImGui::GetFrameHeight();
	ImVec2 pos = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 b(pos.x + s, pos.y + s);
	if (hovered) {
		dl->AddRectFilled(pos, b, IM_COL32(255, 255, 255, 14));
		dl->AddRect(pos, b, col::line_strong);
	}
	float r = std::round(s * 0.2f);
	ImVec2 c(std::round(pos.x + s * 0.5f), std::round(pos.y + s * 0.5f));
	ImU32 color = hovered ? col::text : col::muted;
	float thick = std::max(1.f, S(1.4f));
	dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, thick);
	dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, thick);
	return clicked;
}

void corner_marks(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float l)
{
	float t = std::max(1.f, std::round(S(2)));
	// Top left, top right, bottom left, bottom right: one horizontal and one vertical bar each
	dl->AddRectFilled(a, ImVec2(a.x + l, a.y + t), color);
	dl->AddRectFilled(a, ImVec2(a.x + t, a.y + l), color);
	dl->AddRectFilled(ImVec2(b.x - l, a.y), ImVec2(b.x, a.y + t), color);
	dl->AddRectFilled(ImVec2(b.x - t, a.y), ImVec2(b.x, a.y + l), color);
	dl->AddRectFilled(ImVec2(a.x, b.y - t), ImVec2(a.x + l, b.y), color);
	dl->AddRectFilled(ImVec2(a.x, b.y - l), ImVec2(a.x + t, b.y), color);
	dl->AddRectFilled(ImVec2(b.x - l, b.y - t), b, color);
	dl->AddRectFilled(ImVec2(b.x - t, b.y - l), b, color);
}

} // namespace prisma::ui
