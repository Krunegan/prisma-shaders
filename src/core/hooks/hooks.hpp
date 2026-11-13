// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace prisma::hooks {

// Routes the game's OpenGL calls and the end of its frames through Prisma.
// Must run before the game creates its GL context: the launcher injects us
// while the process is still suspended.
bool install();

// Instead of install(), for [debug] measure_only = 1: only counts frames and
// writes the frame rate to the log, to measure the game without Prisma.
bool install_frame_counter();

// Ask for an OpenGL 4.3 context when the game asks for 3.x core (on by
// default; prisma.ini [prisma] modern_context = 0 turns it off).
void set_modern_context(bool enabled);

// The real implementation of a GL function, bypassing our detours (from the
// real SDL_GL_GetProcAddress). Only once the game has its GL context.
void *raw_proc(const char *name);

} // namespace prisma::hooks
