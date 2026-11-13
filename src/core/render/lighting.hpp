// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Prisma's own lighting data, drawn from the geometry recorded by
	geometry.cpp, for packs that ask for it in pack.toml:

	  * a shadow map: the world seen from the sun (or the moon at night),
	    depth only, covering a square around the camera. Independent of
	    Luanti's dynamic shadows.
	  * a G-buffer: for every pixel of the scene, the surface normal and how
	    much of its light comes from the sky (Luanti's vertex light), which a
	    pass cannot recover from the final colour; in a second target, the
	    sky and torch light levels and whether a liquid surface is there; in
	    a third, what kind of thing each pixel is (geometry::Material).
	  * an overhead map: the world seen from straight above, depth only, to
	    know what is under a roof (rain, snow).
*/
#include "core/hooks/gl_defs.hpp"
#include "core/tracking/state.hpp"

namespace prisma::lighting {

struct Request {
	bool shadows = false;
	bool hidden_light = false; // the sky is hidden and the pack lights it itself (state.light_body)
	int resolution = 2048; // texels per side
	float distance = 64;   // nodes from the camera to the edge of the map
	bool gbuffer = false;
	float gbuffer_distance = 0; // nodes; map blocks farther than this are left out (0: none)
	bool overhead = false;
	int overhead_resolution = 1024;
	float overhead_distance = 64;
};

// Draws what was asked for and fills the shadow fields of `state`. Prisma's
// GL state must already be saved: this leaves bindings changed.
void render(const Request &request, GameState &state);

// For the game shaders of the next frame (they draw before this frame's
// map): fills the shadow fields of `state` from the maps drawn last, with
// their matrices moved to `state`'s camera. Nothing if none were drawn.
void previous_shadow(GameState &state);

GLuint shadow_texture();      // latest direction; 0 if not drawn this frame
GLuint shadow_texture_prev(); // previous direction (blended, see lighting.cpp)
GLuint gbuffer_texture(); // 0 if not drawn this frame
GLuint gbuffer_light_texture(); // the G-buffer's light banks and liquids; 0 if not drawn
GLuint gbuffer_material_texture(); // the G-buffer's material ids (R8); 0 if not drawn
GLuint overhead_texture(); // the world from above; 0 if not drawn this frame

} // namespace prisma::lighting
