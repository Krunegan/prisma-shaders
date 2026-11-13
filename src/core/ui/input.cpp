// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ui/input.hpp"
#include "common/ini.hpp"
#include "common/log.hpp"
#include "core/ui/overlay.hpp"
#include "core/render/pipeline.hpp"
#include "core/platform/platform.hpp"
#include "core/hooks/sdl.hpp"
#include "common/util.hpp"


#include <cctype>
#include <cstdint>
#include <cstring>

namespace prisma::input {

// ---------------------------------------------------------------- SDL2 ABI
// Only the parts we read. SDL2 keeps its event layout stable across 2.x.

constexpr uint32_t SDL_QUIT = 0x100;
constexpr uint32_t SDL_WINDOWEVENT = 0x200;
constexpr uint32_t SDL_KEYDOWN = 0x300;
constexpr uint32_t SDL_KEYUP = 0x301;
constexpr uint32_t SDL_TEXTEDITING = 0x302;
constexpr uint32_t SDL_TEXTINPUT = 0x303;
constexpr uint32_t SDL_MOUSEMOTION = 0x400;
constexpr uint32_t SDL_MOUSEBUTTONDOWN = 0x401;
constexpr uint32_t SDL_MOUSEBUTTONUP = 0x402;
constexpr uint32_t SDL_MOUSEWHEEL = 0x403;

struct SDL_Keysym {
	int32_t scancode;
	int32_t sym;
	uint16_t mod;
	uint32_t unused;
};
struct SDL_KeyboardEvent {
	uint32_t type, timestamp, windowID;
	uint8_t state, repeat, padding2, padding3;
	SDL_Keysym keysym;
};
struct SDL_MouseMotionEvent {
	uint32_t type, timestamp, windowID, which, state;
	int32_t x, y, xrel, yrel;
};
struct SDL_MouseButtonEvent {
	uint32_t type, timestamp, windowID, which;
	uint8_t button, state, clicks, padding1;
	int32_t x, y;
};
struct SDL_MouseWheelEvent {
	uint32_t type, timestamp, windowID, which;
	int32_t x, y;
	uint32_t direction;
	float preciseX, preciseY;
};
struct SDL_TextInputEvent {
	uint32_t type, timestamp, windowID;
	char text[32];
};
union SDL_Event {
	uint32_t type;
	SDL_KeyboardEvent key;
	SDL_MouseMotionEvent motion;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
	SDL_TextInputEvent text;
	uint8_t padding[56];
};

using PFN_PollEvent = int(SDLCALL *)(SDL_Event *);
using PFN_SetRelativeMouseMode = int(SDLCALL *)(int);
using PFN_GetRelativeMouseMode = int(SDLCALL *)();
using PFN_ShowCursor = int(SDLCALL *)(int);
using PFN_WarpMouseInWindow = void(SDLCALL *)(void *, int, int);
using PFN_GetMouseFocus = void *(SDLCALL *)();
using PFN_Void = void(SDLCALL *)();
using PFN_Bool = int(SDLCALL *)();

static PFN_PollEvent o_PollEvent;
static PFN_SetRelativeMouseMode o_SetRelativeMouseMode;
static PFN_ShowCursor o_ShowCursor;
static PFN_WarpMouseInWindow o_WarpMouseInWindow;
static PFN_GetRelativeMouseMode p_GetRelativeMouseMode;
static PFN_GetMouseFocus p_GetMouseFocus;
static PFN_Void p_StartTextInput, p_StopTextInput;
static PFN_Bool p_IsTextInputActive;

// ---------------------------------------------------------------- keys

struct KeyName {
	const char *name;
	int scancode;
};
// Named keys (in any case); letters, digits and F1..F12 are handled apart.
static const KeyName k_keys[] = {
	{"home", 74}, {"end", 77}, {"insert", 73}, {"delete", 76}, {"pageup", 75}, {"pagedown", 78}, {"pause", 72},
	{"scrolllock", 71}, {"printscreen", 70},
};

static int parse_key(std::string name)
{
	for (char &c : name)
		c = (char)std::tolower((unsigned char)c);
	if (name.size() >= 2 && name[0] == 'f' && std::isdigit((unsigned char)name[1])) {
		int n = std::atoi(name.c_str() + 1);
		if (n >= 1 && n <= 12)
			return 58 + n - 1;
	}
	if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
		return 4 + (name[0] - 'a');
	if (name.size() == 1 && name[0] >= '1' && name[0] <= '9')
		return 30 + (name[0] - '1');
	if (name == "0")
		return 39;
	for (const KeyName &k : k_keys)
		if (name == k.name)
			return k.scancode;
	return -1;
}

static int g_menu_key = 74;   // Home
static int g_toggle_key = 77; // End
static std::string g_menu_name = "Home", g_toggle_name = "End";

static void read_key(const std::filesystem::path &ini, const char *entry, const char *fallback, int &scancode,
	std::string &name)
{
	std::string wanted = ini::get(ini, "keys", entry, fallback);
	int sc = parse_key(wanted);
	if (sc < 0) {
		log::error("prisma.ini: unknown key \"%s\", using %s", wanted.c_str(), fallback);
		wanted = fallback;
		sc = parse_key(wanted);
	}
	scancode = sc;
	name = wanted;
}

// ---------------------------------------------------------------- menu state

static bool g_open = false;
// What Luanti asked for while the menu had the mouse, applied on close.
static int g_wanted_relative = 0;
static int g_wanted_cursor = 0;
static bool g_text_was_active = false;
static bool g_swallow_keyup[512] = {};

static void center_mouse()
{
	void *window = p_GetMouseFocus ? p_GetMouseFocus() : nullptr;
	int w = 0, h = 0;
	if (sdl::window_size(window, w, h))
		o_WarpMouseInWindow(window, w / 2, h / 2);
}

bool menu_open()
{
	return g_open;
}

void set_menu_open(bool open)
{
	if (open == g_open)
		return;
	if (open) {
		g_wanted_relative = p_GetRelativeMouseMode ? p_GetRelativeMouseMode() : 0;
		g_wanted_cursor = o_ShowCursor(-1);
		g_text_was_active = p_IsTextInputActive && p_IsTextInputActive();
		g_open = true;
		o_SetRelativeMouseMode(0);
		o_ShowCursor(0); // the menu draws its own cursor
		center_mouse();
		if (p_StartTextInput)
			p_StartTextInput();
	} else {
		g_open = false;
		center_mouse();
		o_ShowCursor(g_wanted_cursor);
		o_SetRelativeMouseMode(g_wanted_relative);
		if (!g_text_was_active && p_StopTextInput)
			p_StopTextInput();
	}
	overlay::set_visible(open);
}

const std::string &menu_key_name()
{
	return g_menu_name;
}

const std::string &toggle_key_name()
{
	return g_toggle_name;
}

// ---------------------------------------------------------------- hooks

// Returns true if the event is consumed by Prisma.
static bool consume(const SDL_Event &e)
{
	if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
		int sc = e.key.keysym.scancode;
		bool down = e.type == SDL_KEYDOWN;
		if (down && (sc == g_menu_key || sc == g_toggle_key)) {
			if (!e.key.repeat) {
				if (sc == g_menu_key)
					set_menu_open(!g_open);
				else
					pipeline::set_enabled(!pipeline::enabled());
			}
			if (sc >= 0 && sc < 512)
				g_swallow_keyup[sc] = true;
			return true;
		}
		if (!down && sc >= 0 && sc < 512 && g_swallow_keyup[sc]) {
			g_swallow_keyup[sc] = false;
			return true;
		}
	}
	if (!g_open)
		return false;

	switch (e.type) {
	case SDL_QUIT:
	case SDL_WINDOWEVENT:
		return false;
	case SDL_KEYDOWN:
		if (e.key.keysym.scancode == 41 /* Escape */) {
			set_menu_open(false);
			g_swallow_keyup[41] = true;
			return true;
		}
		overlay::key(e.key.keysym.scancode, true, e.key.keysym.mod);
		return true;
	case SDL_KEYUP:
		overlay::key(e.key.keysym.scancode, false, e.key.keysym.mod);
		return false; // releases reach the game, so nothing stays held
	case SDL_TEXTINPUT:
		overlay::text(e.text.text);
		return true;
	case SDL_TEXTEDITING:
		return true;
	case SDL_MOUSEMOTION:
		overlay::mouse_move((float)e.motion.x, (float)e.motion.y);
		return true;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		overlay::mouse_button(e.button.button, e.type == SDL_MOUSEBUTTONDOWN);
		return e.type == SDL_MOUSEBUTTONDOWN;
	case SDL_MOUSEWHEEL:
		overlay::mouse_wheel(e.wheel.preciseX, e.wheel.preciseY);
		return true;
	}
	return false;
}

static int SDLCALL hk_PollEvent(SDL_Event *e)
{
	for (;;) {
		int r = o_PollEvent(e);
		if (!r || !e || !consume(*e))
			return r;
	}
}

static int SDLCALL hk_SetRelativeMouseMode(int enabled)
{
	if (g_open) {
		g_wanted_relative = enabled;
		return 0;
	}
	return o_SetRelativeMouseMode(enabled);
}

static int SDLCALL hk_ShowCursor(int toggle)
{
	if (g_open && toggle >= 0) {
		g_wanted_cursor = toggle;
		return toggle;
	}
	return o_ShowCursor(toggle);
}

static void SDLCALL hk_WarpMouseInWindow(void *window, int x, int y)
{
	if (!g_open)
		o_WarpMouseInWindow(window, x, y);
}

bool install(const std::filesystem::path &ini)
{
	read_key(ini, "menu", "Home", g_menu_key, g_menu_name);
	read_key(ini, "toggle", "End", g_toggle_key, g_toggle_name);

	using platform::Library;
	auto sdl = [](const char *symbol) { return platform::library_symbol(Library::sdl, symbol); };
	if (!sdl("SDL_PollEvent")) {
		log::error("SDL2 is not loaded: no Prisma menu or keys");
		return false;
	}
	p_GetRelativeMouseMode = (PFN_GetRelativeMouseMode)sdl("SDL_GetRelativeMouseMode");
	p_GetMouseFocus = (PFN_GetMouseFocus)sdl("SDL_GetMouseFocus");
	p_StartTextInput = (PFN_Void)sdl("SDL_StartTextInput");
	p_StopTextInput = (PFN_Void)sdl("SDL_StopTextInput");
	p_IsTextInputActive = (PFN_Bool)sdl("SDL_IsTextInputActive");

	platform::Interpose hooks[] = {
		{Library::sdl, "SDL_PollEvent", (void *)&hk_PollEvent, (void **)&o_PollEvent},
		{Library::sdl, "SDL_SetRelativeMouseMode", (void *)&hk_SetRelativeMouseMode, (void **)&o_SetRelativeMouseMode},
		{Library::sdl, "SDL_ShowCursor", (void *)&hk_ShowCursor, (void **)&o_ShowCursor},
		{Library::sdl, "SDL_WarpMouseInWindow", (void *)&hk_WarpMouseInWindow, (void **)&o_WarpMouseInWindow},
	};
	if (!platform::interpose(hooks))
		return false;
	log::info("Keys: %s = Prisma menu, %s = toggle on/off", g_menu_name.c_str(), g_toggle_name.c_str());
	return true;
}

} // namespace prisma::input
