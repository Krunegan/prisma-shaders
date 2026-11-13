// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Sound for shader packs (pack.toml, [sounds.*]), played with miniaudio on
	its own output, mixed by Windows with Luanti's sound.

	The passes decide what sounds and how: they write into the pack's sound
	buffer (sound_buffer = "<buffer>"), and every frame Prisma reads it back
	(asynchronously, a couple of frames late) and applies it. Column
	`control` of a sound holds:

	  row 0: r = volume (0..1, more is louder)
	         g = pitch (playback speed; 0 = 1)
	         b = trigger: one-shots play every time this value changes to a
	             value above 0 (a counter, the number of the event...)
	         a = pan (-1 left .. 1 right)
	  row 1: r = muffle: 0 = clear .. 1 = heard through a wall (low-pass)

	Loops play all the time at the volume the passes ask for; one-shots
	play on their trigger, with the volume, pitch and pan of that moment.

	Whenever the sounds start (a new pack, back in the world, a menu
	closed), the loops fade in over a few seconds instead of starting at
	full volume. While Luanti's pause menu is open everything fades out.
*/
#include <filesystem>
#include "core/pack/pack.hpp"

#include <string>

namespace prisma::audio {

// prisma.ini, [audio] volume (0 = no sound at all; Prisma doesn't even
// open the audio device).
void init(const std::filesystem::path &ini);

// The pack's sounds (they start at the next update). Safe at startup: the
// audio device is opened later, from the game's thread.
void set_pack(const pack::Pack &pack);

// The sound buffer of this frame: `width` x `height` RGBA floats, row by row
// from the bottom (row 0 first).
void update(const float *texels, int width, int height);

// Once per frame: when nothing has updated the sounds for a moment (the
// pack is switched off, the game is in its main menu...), they fade out.
void on_frame_end();

// Whether Luanti's pause menu is open (frame.cpp tells, once per frame):
// the sounds fade out while it is, and fade back in when it closes.
void set_paused(bool paused);

} // namespace prisma::audio
