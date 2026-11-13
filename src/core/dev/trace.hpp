// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Developer tools over the tracker's records.
	They only work in developer mode (core/dev/developer.hpp):

	  * the trace of one whole frame, in logs/frame_trace_NN.txt: framebuffer
	    switches with their attachments, programs used, draw calls, clears,
	    buffer uploads, and the value of every uniform of the programs used.
	    From the menu's button, and once by itself a few seconds after the
	    world appears;
	  * every game shader as compiled, in logs/shaders/, and Luanti's own
	    sources before any change, in logs/shaders/luanti/;
	  * every GL function the game asks for, in logs/gl_procs.txt.

	The tracker calls in on the GL thread. The recording calls below are made
	only while active(), so outside a traced frame they cost one test.
*/
#include "core/hooks/gl_defs.hpp"

#include <filesystem>
#include <string>

namespace prisma::trace {

extern bool g_active;

// A frame is being traced
inline bool active()
{
	return g_active;
}

// Clears the traces and dumps of the previous run from `out_dir` (logs/).
void init(const std::filesystem::path &out_dir);

// Traces the next frame (the menu's button).
void request();

// Frame boundaries, from the tracker: frame_ended() finishes a trace in
// progress; frame_started() starts a requested one, or the automatic one
// once the world has been visible for `world_frames` frames.
void frame_ended();
void frame_started(long long world_frames);

void proc_requested(const char *name, bool found);
void program_linked(GLuint program); // dumps its shaders
// Luanti's own source of one of its shaders, before any change, into
// logs/shaders/luanti/<name>.<stage>.<n>.glsl: one file per distinct variant.
void luanti_source(const std::string &name, GLenum type, const std::string &source);

// While active()
void framebuffer_bound(GLuint fbo);
void attached(GLuint fbo, GLenum attachment, bool renderbuffer, GLuint id, GLint level);
void blit(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask);
void draw_buffers(GLsizei n, const GLenum *bufs);
void draw(const char *kind, GLsizei count, GLsizei instances, GLenum index_type, const void *indices);
void clear(GLbitfield mask);
void viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void buffer_upload(GLuint buffer);

} // namespace prisma::trace
