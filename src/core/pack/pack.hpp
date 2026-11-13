// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Shader pack on disk:

	    shaderpacks/<Pack>/
	        pack.toml                    required: api = N; name, description, options...
	        passes/NN_name.frag          run in name order at the end of the scene;
	                                     `#prisma target <buffer>` writes a buffer
	                                     instead of the scene; `#prisma chain a b c`
	                                     runs it once per step (a -> b, b -> c);
	                                     `#prisma fallback`: only without compute
	        passes/NN_name.comp          compute shader writing a buffer (OpenGL 4.3),
	                                     in the same order as the .frag ones
	        game/<shader>/...            changes to Luanti's own shaders, where
	                                     <shader> is the folder name in Luanti's
	                                     client/shaders (e.g. nodes_shader, shadow/pass1):
	            opengl_vertex.glsl       replaces Luanti's file
	            opengl_fragment.glsl
	            extend_vertex.glsl       appended to Luanti's file; may define
	                                     void prisma_vertex()
	            extend_fragment.glsl     appended; may define
	                                     void prisma_fragment(inout vec4 color)
	            patch_vertex.glsl        code put *inside* Luanti's file, next to
	            patch_fragment.glsl      given lines (see gameshaders.hpp)
	        include/                     shared code (.glsl), pulled in with #include "file"
	        sounds/                      audio files of [sounds.*] (any folder works)

	`#include "x"` is resolved relative to the including file, then to the
	pack root, then to include/. `#include <x>` is Prisma's standard library
	(the glsl/ folder next to prisma_core.dll, e.g. <prisma/bloom.glsl>).
	Prisma emits `#line` directives so compiler errors point at the pack's
	own files and lines.
*/
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace prisma::pack {

// Preprocessed code plus the table that maps GLSL source-string numbers
// (as used in #line) back to file names.
struct Code {
	std::string code;
	std::vector<std::string> files;
	bool empty() const { return code.empty(); }
};

struct Pass {
	std::string name;   // file name, e.g. "10_fog.frag"; chain steps add " (buffer)"
	Code code;          // files[0] is the Prisma header
	std::string target; // buffer it writes; "" = the scene
	bool compute = false;  // a .comp: runs only where compute shaders are available
	bool fallback = false; // `#prisma fallback`: runs only where they are not
};


// [buffers.<id>]: a texture of the pack's own that passes can write and read.
// It keeps its contents between frames.
struct Buffer {
	std::string id;            // GLSL name of its sampler
	double scale = 1;          // of the scene size...
	int width = 0, height = 0; // ...or a fixed size (size = [w, h])
	std::string format = "rgba16f"; // rgba8, rgba16f, rgba32f
	bool linear = true;        // filter = "linear" | "nearest"
	bool mipmaps = false;      // smaller levels rebuilt after each pass that writes it (textureLod)
};

struct GameStage {
	Code replace; // full replacement of Luanti's file
	Code extend;  // appended after it
	Code patch;   // anchored insertions into it
};

struct GameShader {
	GameStage vertex, fragment;
};

// An option the player can change from the menu (pack.toml, [options.<id>]).
struct OptionDef {
	enum Type { Float, Int, Bool, Choice };
	std::string id; // GLSL identifier
	std::string label, description, group;
	Type type = Float;
	bool define = false; // #define (needs a recompile) instead of a uniform
	double min = 0, max = 1, step = 0, def = 0;
	std::vector<std::string> values; // Choice
	// Choice: numbers = [...], what each value stands for where pack.toml
	// reads a number from the option ([shadows], [overhead], gbuffer_distance).
	// Empty: the value's text read as a number ("2048").
	std::vector<double> numbers;
};

// [sounds.<id>]: an audio file Prisma plays (audio.hpp), driven by what the
// passes write into the pack's sound buffer: column `control` holds its
// volume, pitch, trigger and pan (row 0) and how muffled it is (row 1).
struct Sound {
	std::string id;
	std::vector<std::filesystem::path> files; // absolute; one-shots pick one at random each time
	bool loop = false;  // plays all the time, at the volume the passes ask for
	double volume = 1;  // multiplies the volume written by the passes
	int control = 0;    // column of the sound buffer with its controls
	int voices = 4;     // one-shots: how many may sound at the same time
};

// A number written in pack.toml, or the id of an option that holds it.
struct Setting {
	double number = 0;
	std::string option; // empty: use `number`
};

struct Pack {
	std::string name; // folder name
	std::string title, description, author, version;
	int api = 0; // api = N: the Prisma API the pack was written for (0 if pack.toml was rejected)
	std::vector<std::string> needs; // requires = [...]: "shadows" = needs Luanti's dynamic shadows
	std::vector<OptionDef> options;

	// [shadows]: Prisma draws its own shadow map for the passes
	bool shadows = false;
	Setting shadow_resolution{2048}, shadow_distance{64};
	// [shadows] hidden_sky_light = [x, y, z]: under a sky the game hides,
	// the pack's own light from this direction (towards it, world axes)
	bool hidden_sky_light = false;
	double hidden_sky_dir[3] = {0, 1, 0};
	// gbuffer = true: normals and sky light of the scene for the passes
	bool gbuffer = false;
	// gbuffer_distance: map blocks farther than this (nodes) are left out of
	// the G-buffer; 0 = none are
	Setting gbuffer_distance{0};
	// [overhead]: Prisma draws the world from straight above (what is under a roof)
	bool overhead = false;
	Setting overhead_resolution{1024}, overhead_distance{64};
	// hide = [...]: Luanti's own clouds, dynamic shadows and post-processing
	// effects ("bloom", "volumetric_light", "exposure", "tonemapping"), for
	// packs that replace them
	bool hide_clouds = false, hide_shadows = false;
	bool hide_bloom = false, hide_volumetric_light = false, hide_exposure = false, hide_tonemapping = false;
	bool hide_underwater = false; // Luanti's screen tint while the camera is in a liquid

	// sound_buffer = "<buffer>" and [sounds.*]: sounds driven by the passes
	std::string sound_buffer;
	std::vector<Sound> sounds;

	std::vector<Pass> passes;
	std::vector<Buffer> buffers;
	std::map<std::string, GameShader> game; // by Luanti shader name
	std::vector<std::string> errors;   // the pack is not used
	std::vector<std::string> warnings; // it is, but something deserves a look
};

// Whether a pack needs compute shaders to run as written (it has .comp passes)
bool uses_compute(const Pack &pack);

// Reading and preprocessing only: no GL involved, safe at startup.
Pack load(const std::filesystem::path &dir);

// Prisma's standard library of GLSL (#include <prisma/...>): the glsl/
// folder next to the core, unless set otherwise (prisma_check, tests).
void set_library_dir(const std::filesystem::path &dir);
const std::filesystem::path &library_dir();

// The samplers every pass has besides the pack's buffers (colorTex,
// depthTex...): a chain may start at one of them.
bool is_builtin_sampler(const std::string &name);

// Changes whenever any file of the pack is added, removed or modified.
std::string signature(const std::filesystem::path &dir);

// Rewrites "0:12:" / "0(12)" style locations of a driver's info log into
// "file:line:".
std::string translate_log(const std::string &log, const std::vector<std::string> &files);

} // namespace prisma::pack
