// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	OpenGL entry points for Prisma's own rendering. They bypass our hooks
	(trampolines for the detoured exports, driver pointers for the rest), so
	nothing Prisma draws is mistaken for something the game drew.
*/
#include "core/hooks/gl_defs.hpp"

#define PRISMA_GL_FUNCS(X) \
	X(void, GetIntegerv, (GLenum, GLint *)) \
	X(void, GetBooleanv, (GLenum, GLboolean *)) \
	X(GLboolean, IsEnabled, (GLenum)) \
	X(void, Enable, (GLenum)) \
	X(void, Disable, (GLenum)) \
	X(GLenum, GetError, ()) \
	X(void, DepthMask, (GLboolean)) \
	X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
	X(void, PolygonMode, (GLenum, GLenum)) \
	X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, ActiveTexture, (GLenum)) \
	X(void, BindTexture, (GLenum, GLuint)) \
	X(void, GenTextures, (GLsizei, GLuint *)) \
	X(void, DeleteTextures, (GLsizei, const GLuint *)) \
	X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(void, TexParameteri, (GLenum, GLenum, GLint)) \
	X(void, GenFramebuffers, (GLsizei, GLuint *)) \
	X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
	X(void, BindFramebuffer, (GLenum, GLuint)) \
	X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(GLenum, CheckFramebufferStatus, (GLenum)) \
	X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
	X(void, GenVertexArrays, (GLsizei, GLuint *)) \
	X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
	X(void, BindVertexArray, (GLuint)) \
	X(GLuint, CreateShader, (GLenum)) \
	X(void, DeleteShader, (GLuint)) \
	X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(void, CompileShader, (GLuint)) \
	X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(GLuint, CreateProgram, ()) \
	X(void, DeleteProgram, (GLuint)) \
	X(void, AttachShader, (GLuint, GLuint)) \
	X(void, LinkProgram, (GLuint)) \
	X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, UseProgram, (GLuint)) \
	X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
	X(void, GetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
	X(void, Uniform1i, (GLint, GLint)) \
	X(void, Uniform1f, (GLint, GLfloat)) \
	X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
	X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
	X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(void, UniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(void, UniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	X(void, Uniform1fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, Uniform2fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
	X(void, Uniform1iv, (GLint, GLsizei, const GLint *)) \
	X(void, Uniform2iv, (GLint, GLsizei, const GLint *)) \
	X(void, Uniform3iv, (GLint, GLsizei, const GLint *)) \
	X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
	X(void, GetAttachedShaders, (GLuint, GLsizei, GLsizei *, GLuint *)) \
	X(void, DetachShader, (GLuint, GLuint)) \
	X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
	X(void, DrawElements, (GLenum, GLsizei, GLenum, const void *)) \
	X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
	X(void, BindBuffer, (GLenum, GLuint)) \
	X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *)) \
	X(void, EnableVertexAttribArray, (GLuint)) \
	X(void, DisableVertexAttribArray, (GLuint)) \
	X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	X(void, Clear, (GLbitfield)) \
	X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, ClearDepth, (double)) \
	X(void, DepthFunc, (GLenum)) \
	X(void, GetFloatv, (GLenum, GLfloat *)) \
	X(void, GenRenderbuffers, (GLsizei, GLuint *)) \
	X(void, DeleteRenderbuffers, (GLsizei, const GLuint *)) \
	X(void, BindRenderbuffer, (GLenum, GLuint)) \
	X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	X(void, DrawBuffer, (GLenum)) \
	X(void, ReadBuffer, (GLenum)) \
	X(void, GenBuffers, (GLsizei, GLuint *)) \
	X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
	X(void, GetBufferParameteriv, (GLenum, GLenum, GLint *)) \
	X(void, CopyBufferSubData, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr)) \
	X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
	X(void, BindBufferBase, (GLenum, GLuint, GLuint)) \
	X(void, GetIntegeri_v, (GLenum, GLuint, GLint *)) \
	X(void, GetInteger64i_v, (GLenum, GLuint, long long *)) \
	X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar *)) \
	X(void, GetActiveUniformBlockiv, (GLuint, GLuint, GLenum, GLint *)) \
	X(void, UniformBlockBinding, (GLuint, GLuint, GLuint)) \
	X(void, CullFace, (GLenum)) \
	X(void, FrontFace, (GLenum)) \
	X(void, DrawBuffers, (GLsizei, const GLenum *)) \
	X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar *)) \
	X(void, ColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean)) \
	X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	X(void, PixelStorei, (GLenum, GLint)) \
	X(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
	X(GLboolean, UnmapBuffer, (GLenum))

// Not needed to draw the pack: if the driver lacks them, Prisma works
// without what they give (mipmaps of the buffers, GPU timings).
#define PRISMA_GL_OPTIONAL_FUNCS(X) \
	X(void, GenerateMipmap, (GLenum)) \
	X(void, GenQueries, (GLsizei, GLuint *)) \
	X(void, DeleteQueries, (GLsizei, const GLuint *)) \
	X(void, QueryCounter, (GLuint, GLenum)) \
	X(void, GetQueryObjectiv, (GLuint, GLenum, GLint *)) \
	X(void, GetQueryObjectui64v, (GLuint, GLenum, unsigned long long *)) \
	X(void *, FenceSync, (GLenum, GLbitfield)) \
	X(GLenum, ClientWaitSync, (void *, GLbitfield, unsigned long long)) \
	X(void, DeleteSync, (void *)) \
	X(void, Finish, ()) \
	X(const GLubyte *, GetString, (GLenum)) \
	X(const GLubyte *, GetStringi, (GLenum, GLuint)) \
	X(void, DispatchCompute, (GLuint, GLuint, GLuint)) \
	X(void, MemoryBarrier, (GLbitfield)) \
	X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))

namespace prisma::gl {

#define PRISMA_GL_DECLARE(ret, name, params) \
	using PFN_##name = ret(APIENTRY *) params; \
	extern PFN_##name name;
PRISMA_GL_FUNCS(PRISMA_GL_DECLARE)
PRISMA_GL_OPTIONAL_FUNCS(PRISMA_GL_DECLARE)
#undef PRISMA_GL_DECLARE

// Resolves every function. Needs a current GL context. Returns false (and
// logs which one) if anything is missing; optional ones may stay null.
bool load();
bool loaded();
bool has_mipmaps();      // glGenerateMipmap
// GPU timestamps (glQueryCounter) and fences: the functions exist. Whether the
// context really supports them (OpenGL 3.3 or GL_ARB_timer_query) is up to
// the caller (debugview.cpp).
bool has_timer_queries();
// Compute shaders and image load/store: an OpenGL 4.3 context (see
// hooks::set_modern_context) and the functions.
bool has_compute();

} // namespace prisma::gl

// Enums used by Prisma's own rendering
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_HALF_FLOAT = 0x140B;
// Compute shaders (OpenGL 4.3)
constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL_COMPUTE_WORK_GROUP_SIZE = 0x8267;
constexpr GLenum GL_READ_WRITE = 0x88BA;
constexpr GLbitfield GL_ALL_BARRIER_BITS = 0xFFFFFFFF;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ACTIVE_UNIFORMS = 0x8B86;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
constexpr GLenum GL_POLYGON_MODE = 0x0B40;
constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
constexpr GLenum GL_FILL = 0x1B02;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum GL_RASTERIZER_DISCARD = 0x8C89;
constexpr GLenum GL_CONTEXT_PROFILE_MASK = 0x9126;
constexpr GLenum GL_SAMPLER_2D = 0x8B5E;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_BINDING_2D_ARRAY = 0x8C1D;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_RENDERBUFFER_BINDING = 0x8CA7;
constexpr GLenum GL_DEPTH_FUNC = 0x0B74;
constexpr GLenum GL_DEPTH_CLEAR_VALUE = 0x0B73;
constexpr GLenum GL_COLOR_CLEAR_VALUE = 0x0C22;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_UNIFORM_BUFFER_BINDING = 0x8A28;
constexpr GLenum GL_UNIFORM_BUFFER_START = 0x8A29;
constexpr GLenum GL_UNIFORM_BUFFER_SIZE = 0x8A2A;
constexpr GLenum GL_UNIFORM_BLOCK_BINDING = 0x8A3F;
constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
constexpr GLenum GL_BUFFER_SIZE = 0x8764;
constexpr GLenum GL_DYNAMIC_COPY = 0x88EA;
constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFFu;
constexpr GLenum GL_CULL_FACE_MODE = 0x0B45;
constexpr GLenum GL_FRONT_FACE = 0x0B46;
constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
constexpr GLenum GL_STREAM_READ = 0x88E1;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_COLOR_ATTACHMENT2 = 0x8CE2;
constexpr GLenum GL_TIMESTAMP = 0x8E28;
constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_QUERY_RESULT = 0x8866;
constexpr GLenum GL_QUERY_RESULT_AVAILABLE = 0x8867;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x0001;
constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
