// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Game state handed to shader packs, rebuilt every frame from the uniforms
	Luanti sends to its own world shaders.

	Conventions (also documented in docs/shaderpacks.md):
	  * Units are nodes (Luanti renders in BS units, 10 per node).
	  * "Player space": world axes, origin at the camera.
	    viewMatrix maps player space to view space.
	  * View space follows Luanti/Irrlicht: left-handed, +Z points forward.
	  * cameraPosition is absolute world position, so
	    world = playerPos + cameraPosition.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/render/math.hpp"

namespace prisma {

struct GameState {
	Mat4 view, view_inv, proj, proj_inv;
	Mat4 prev_view, prev_proj;
	Vec3 camera_pos, prev_camera_pos;
	Vec3 camera_offset; // Luanti's render origin, in nodes

	bool sun_available = false; // from Luanti's shadow uniforms, or read from the sky
	bool luanti_shadows = false; // Luanti's own dynamic shadows are on (pack.toml: requires = ["shadows"])
	Vec3 sun_dir{0, 1, 0};      // towards the sun, world space
	Vec3 moon_dir{0, -1, 0};
	Vec3 light_dir{0, 1, 0};    // towards whichever body casts the shadows
	Vec3 light_body{0, 1, 0};   // the same body as found this frame; light_dir becomes the direction
	                            // the shadow maps show (lighting.cpp), which lags behind it
	float time_of_day = 0.5f;   // 0..1, 0.5 = noon; -1 if unknown
	bool sky_hidden = false;    // the game hid the sky: Luanti draws no sun or moon, far from its clouds

	float day_light[3] = {1, 1, 1};
	float fog_color[3] = {0.5f, 0.7f, 1};
	float fog_distance = 100; // nodes
	float fog_shading = 1.6667f; // Luanti's fogShadingParameter: 1 / (1 - fog_start)
	float cloud_color[4] = {1, 1, 1, 0}; // Luanti's cloud colour; alpha 0 = unknown
	float liquid_color[4] = {0, 0, 0, 0}; // camera in a liquid: Luanti's screen tint; alpha 0 = not in one
	float animation_timer = 0;
	float near_plane = 0.1f, far_plane = 100;
	bool depth_zero_to_one = false; // NDC depth range of Luanti's projection

	// Prisma's own shadow map (lighting.cpp), when the pack asks for it
	bool shadow_available = false;
	Mat4 shadow_matrix;            // player space -> [0, 1] texture coords and depth
	Mat4 shadow_matrix_prev;       // same, for the map at the previous direction
	float shadow_blend = 1;        // weight of the latest map
	float shadow_distance = 0;     // nodes from the centre to the edge
	float shadow_depth_range = 1;  // nodes between depth 0 and 1
	int shadow_size = 0;           // texels per side

	// Prisma's overhead map (lighting.cpp): the world from straight above
	bool overhead_available = false;
	Mat4 overhead_matrix;          // player space -> [0, 1] texture coords and depth
	float overhead_distance = 0;   // nodes from the centre to the edge
	float overhead_depth_range = 1; // nodes between depth 0 and 1
	int overhead_size = 0;         // texels per side

	int width = 0, height = 0;
	float time = 0, frame_time = 0; // seconds
	int frame = 0;
};

// Builds the state from `world_program` (a nodes_shader program used to
// draw this frame), or with 0 from the sky's camera, for a map where no
// block has been drawn yet (mapgen singlenode). Returns false if the camera
// matrices are not known yet.
bool build_state(GLuint world_program, int width, int height, GameState &state);

// Called with each state built from this frame's world: where Luanti's
// camera offset stands.
void track_world_camera(const GameState &state);

// The camera's absolute position (nodes) and Luanti's camera offset in a
// frame without world geometry, from an entity drawn with the sky's view
// (geometry::object_camera) and the offset followed since the last world
// frame. False if no entity gave the camera this frame.
bool sky_camera_position(Vec3 &camera, Vec3 &offset);

// For frames without world geometry: replaces the camera rotation and
// projection with the sky's (see geometry::sky_camera) and, if known, the
// camera position. False (nothing changed) if the matrices are unusable.
bool apply_sky_camera(GameState &state, const float *wv, const float *wvp, const Vec3 *camera_pos);

// Remembers this frame's matrices as the "previous" ones for the next frame.
void end_state_frame(const GameState &state);

} // namespace prisma
