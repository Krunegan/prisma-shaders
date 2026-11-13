// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The look shared by the launcher's window and the in-game menu: an
	instrument panel. Square corners, one-pixel hairlines on a warm near-black,
	labels in a monospaced capital face, segmented sliders, and one amber
	accent kept for values and what the player is touching.

	Only the launcher and the core link this file (Dear ImGui); prisma_check
	does not.
*/
#include <imgui.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace prisma::ui {

namespace col {
constexpr ImU32 bg = IM_COL32(14, 14, 13, 255);
constexpr ImU32 panel = IM_COL32(20, 20, 18, 255);       // sidebar, footer, help
constexpr ImU32 line = IM_COL32(58, 57, 52, 255);        // hairlines, empty segments
constexpr ImU32 line_strong = IM_COL32(92, 90, 82, 255); // hover, corner marks
constexpr ImU32 text = IM_COL32(222, 219, 210, 255);
constexpr ImU32 muted = IM_COL32(150, 146, 136, 255);    // help, meta
constexpr ImU32 accent = IM_COL32(232, 163, 60, 255);    // values, active control
constexpr ImU32 accent_hi = IM_COL32(246, 190, 104, 255);
constexpr ImU32 error = IM_COL32(232, 122, 106, 255);
constexpr ImU32 ok = IM_COL32(150, 196, 128, 255);
constexpr ImU32 ink = IM_COL32(14, 14, 13, 255);         // text on an inverted block
} // namespace col

inline ImVec4 v4(ImU32 c)
{
	return ImGui::ColorConvertU32ToFloat4(c);
}

struct Fonts {
	ImFont *regular = nullptr;
	ImFont *bold = nullptr;
	ImFont *mono = nullptr;
	ImFont *mono_bold = nullptr;
};
const Fonts &fonts();

// Paths that are empty or missing fall back: bold to regular, mono to
// ImGui's own font. `size` is the body size in pixels, already scaled.
void load_fonts(const std::filesystem::path &regular, const std::filesystem::path &bold,
	const std::filesystem::path &mono, const std::filesystem::path &mono_bold, float size);

// Style and colours. `scale` is the screen scaling (1 = 96 dpi); `alpha`
// the opacity of the window background (the game shows through the menu).
void apply_style(float scale, float alpha);

// Sizes in points, multiplied by the scale given to apply_style()
float S(float v);

// Capitals, also for the Latin-1 letters in UTF-8 (á → Á, ñ → Ñ)
std::string upper(std::string_view text);

// Monospaced capitals, a little smaller than the body, with spaced letters.
// `size` is relative to the body size.
void label(std::string_view text, ImU32 color = col::muted, float size = 0.78f, bool bold = false);
ImVec2 label_size(std::string_view text, float size = 0.78f, bool bold = false);
// The same, drawn at a point without taking layout space
void draw_label(ImDrawList *dl, ImVec2 pos, std::string_view text, ImU32 color, float size = 0.78f,
	bool bold = false);

// A section title: label, and a hairline to the right edge
void section(std::string_view text);

// ON / OFF in a box
bool switch_box(const char *id, bool &on);

// A slider made of segments. `t` goes from 0 to 1; `mark`, if 0..1, puts a
// tick under the default value. Returns true while it changes.
bool segments(const char *id, float &t, float width, int count, float mark = -1);

// An entry of a sidebar: a number column (may be ""), the label in capitals,
// a detail on the right. Selected, the row is inverted.
bool nav_item(const char *number, std::string_view label, bool selected, const char *detail = nullptr,
	ImU32 color = col::text);

// Buttons with their label in monospaced capitals: framed, and inverted for
// the one main action of a view.
bool button(const char *label, ImVec2 size = ImVec2(0, 0));
bool primary_button(const char *label, ImVec2 size = ImVec2(0, 0));

// A × drawn with two lines, in a square the height of a frame
bool close_button(const char *id);

// Crop marks at the corners of a rectangle
void corner_marks(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 color, float length);

} // namespace prisma::ui
