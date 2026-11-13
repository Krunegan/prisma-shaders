// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tracking/state.hpp"
#include "core/render/geometry.hpp"
#include "common/log.hpp"
#include "core/tracking/tracker.hpp"
#include "common/util.hpp"

namespace prisma {

static constexpr float BS = 10.f; // Luanti render units per node

static bool g_have_prev = false;
static Mat4 g_prev_view, g_prev_proj;
static Vec3 g_prev_camera;
static double g_last_time = -1;
static int g_frame = 0;
static bool g_warned_no_sun = false;
static bool g_logged_hidden = false;

// Luanti's cloud layer at its default height, with a margin
constexpr float CLOUD_LAYER_LOW = 100.f, CLOUD_LAYER_HIGH = 156.f;
// How long without a sun or moon before the sky counts as hidden: as long
// as geometry::sky() trusts the last one seen
constexpr double SKY_GONE_SECONDS = 3.0;
constexpr int SKY_GONE_FRAMES = 300;
static bool g_logged_projection = false;

// Luanti draws relative to a camera offset that jumps by 200 nodes as the
// camera moves (CAMERA_OFFSET_STEP, src/client/camera.h), and only its map
// blocks are told where it stands. Without them in view it is followed
// here: a jump of the camera in render space by about a step is the offset
// moving, not the camera.
constexpr float CAMERA_OFFSET_STEP = 200.f;
constexpr double CAMERA_TRACK_SECONDS = 5.0; // longer without a camera: another world, start over
static bool g_track_known = false;
static double g_track_time = -100;
static Vec3 g_track_render; // last camera in render space, nodes
static Vec3 g_track_offset; // the offset as followed, nodes

// Same as Luanti's getWickedTimeOfDay (src/client/sky.cpp): stretches the day
// so that the night is shorter than half of the cycle.
static float wicked_time_of_day(float t)
{
	const float wn = 0.415f / 2;
	if (t > wn && t < 1.f - wn)
		return (t - wn) / (1.f - wn * 2) * 0.5f + 0.25f;
	if (t < 0.5f)
		return t / wn * 0.25f;
	return 1.f - ((1.f - t) / wn * 0.25f);
}

// Luanti advances the sun in small steps every few frames (about 0.02° each
// at normal speed); shadows that follow those steps twitch. Follow it with a
// short exponential lag instead, and jump straight to it after a big change
// (a /time command).
static Vec3 g_smooth_sun;
static bool g_have_smooth_sun = false;
static double g_smooth_sun_time = 0;

static void smooth_sun(GameState &s)
{
	double now = uptime();
	Vec3 target = s.sun_dir;
	bool light_is_sun = s.light_dir.x * target.x + s.light_dir.y * target.y + s.light_dir.z * target.z > 0;
	float c = g_smooth_sun.x * target.x + g_smooth_sun.y * target.y + g_smooth_sun.z * target.z;
	if (!g_have_smooth_sun || c < 0.99939f) { // more than 2°
		g_smooth_sun = target;
	} else {
		float k = 1.f - (float)std::exp(-(now - g_smooth_sun_time) / 0.3);
		g_smooth_sun = (g_smooth_sun + (target - g_smooth_sun) * k).normalized();
	}
	g_have_smooth_sun = true;
	g_smooth_sun_time = now;
	s.sun_dir = g_smooth_sun;
	s.moon_dir = -g_smooth_sun;
	s.light_dir = light_is_sun ? s.sun_dir : s.moon_dir;
}

static Vec3 transform_point(const Mat4 &m, const Vec3 &p, float &w)
{
	float x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
	float y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
	float z = m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3);
	w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
	return {x, y, z};
}

// View-space depth (along +Z) of a point at the given NDC depth.
static float unproject_depth(const Mat4 &proj_inv, float ndc_z)
{
	float w;
	Vec3 v = transform_point(proj_inv, {0, 0, ndc_z}, w);
	return v.z / w;
}

// Luanti's own matrices from a world program, re-expressed in nodes with
// the origin at the camera.
static bool world_matrices(GLuint world_program, GameState &s)
{
	float wv[16], wvp[16], w[16], cam[3], offset[3];
	if (tracker::uniform(world_program, "mWorldView", wv, 16) != 16 ||
			tracker::uniform(world_program, "mWorldViewProj", wvp, 16) != 16)
		return false;
	Mat4 world;
	if (tracker::uniform(world_program, "mWorld", w, 16) == 16)
		world = Mat4::from(w);
	if (tracker::uniform(world_program, "cameraPosition", cam, 3) != 3 &&
			tracker::global_uniform("cameraPosition", cam, 3) != 3)
		return false;
	if (tracker::uniform(world_program, "cameraOffset", offset, 3) != 3 &&
			tracker::global_uniform("cameraOffset", offset, 3) != 3)
		offset[0] = offset[1] = offset[2] = 0;

	// Luanti's own matrices, in render space (BS units, shifted by the camera offset)
	Mat4 wv_m = Mat4::from(wv);
	Mat4 view_bs = wv_m * world.inverse();
	Mat4 proj_bs = Mat4::from(wvp) * wv_m.inverse();

	// Re-express them in nodes, with the origin at the camera.
	// Luanti's cameraPosition is absolute; render space is shifted by cameraOffset.
	Vec3 cam_render{cam[0] - offset[0], cam[1] - offset[1], cam[2] - offset[2]};
	Mat4 to_bs = Mat4::scale(BS);
	Mat4 from_bs = Mat4::scale(1.f / BS);
	s.view = from_bs * view_bs * Mat4::translation(cam_render) * to_bs;
	s.proj = proj_bs * to_bs;
	s.view_inv = s.view.inverse();
	s.proj_inv = s.proj.inverse();
	s.camera_pos = Vec3{cam[0], cam[1], cam[2]} * (1.f / BS);
	s.camera_offset = Vec3{offset[0], offset[1], offset[2]} * (1.f / BS);
	static bool logged = false;
	if (!logged) {
		logged = true;
		auto dump = [](const char *name, const Mat4 &m) {
			for (int r = 0; r < 4; r++)
				log::info("  %-10s [%10.4f %10.4f %10.4f %10.4f]", r ? "" : name, m.at(r, 0), m.at(r, 1), m.at(r, 2),
					m.at(r, 3));
		};
		dump("view_bs", view_bs);
		dump("proj_bs", proj_bs);
		dump("view", s.view);
	}
	return true;
}

void track_world_camera(const GameState &s)
{
	g_track_known = true;
	g_track_time = uptime();
	g_track_offset = s.camera_offset;
	g_track_render = s.camera_pos - s.camera_offset;
}

bool sky_camera_position(Vec3 &camera, Vec3 &offset)
{
	Vec3 render;
	if (!geometry::object_camera(render))
		return false;
	double now = uptime();
	if (!g_track_known || now - g_track_time > CAMERA_TRACK_SECONDS) {
		g_track_offset = {};
	} else {
		auto follow = [](float now_pos, float before, float &off) {
			float d = now_pos - before;
			if (std::fabs(d) < CAMERA_OFFSET_STEP / 2)
				return;
			float steps = std::round(d / CAMERA_OFFSET_STEP);
			// A teleport lands anywhere: then the offset is not known
			if (std::fabs(d - steps * CAMERA_OFFSET_STEP) < 20.f)
				off -= steps * CAMERA_OFFSET_STEP;
		};
		follow(render.x, g_track_render.x, g_track_offset.x);
		follow(render.y, g_track_render.y, g_track_offset.y);
		follow(render.z, g_track_render.z, g_track_offset.z);
	}
	g_track_known = true;
	g_track_time = now;
	g_track_render = render;
	camera = render + g_track_offset;
	offset = g_track_offset;
	return true;
}

// Without any world program (an empty map, mapgen singlenode: Luanti draws
// no map block, and only those carry cameraPosition): the camera as the sky
// saw it, placed where an entity's draw says it is (sky_camera_position).
// Such a map starts near the origin, where the camera offset is zero.
static bool sky_matrices(GameState &s)
{
	float wv[16], wvp[16];
	if (!geometry::sky_camera(wv, wvp) || !apply_sky_camera(s, wv, wvp, nullptr))
		return false;
	Vec3 camera, offset;
	if (sky_camera_position(camera, offset)) {
		s.camera_pos = camera;
		s.camera_offset = offset;
	} else {
		s.camera_pos = g_have_prev ? g_prev_camera : Vec3{};
		s.camera_offset = g_track_offset;
	}
	return true;
}

bool build_state(GLuint world_program, int width, int height, GameState &s)
{
	if (world_program ? !world_matrices(world_program, s) : !sky_matrices(s))
		return false;

	// Depth convention: with the GL [-1, 1] range, NDC -1 lies in front of
	// the camera (the near plane); with [0, 1] it would end up behind it.
	float z_minus_one = unproject_depth(s.proj_inv, -1.f);
	s.depth_zero_to_one = !(z_minus_one > 0.f);
	s.near_plane = unproject_depth(s.proj_inv, s.depth_zero_to_one ? 0.f : -1.f);
	s.far_plane = unproject_depth(s.proj_inv, 1.f);
	// Matrices can be incomplete during the first frames (nothing drawn yet).
	if (!std::isfinite(s.near_plane) || !std::isfinite(s.far_plane))
		return false;
	if (!g_logged_projection) {
		g_logged_projection = true;
		log::info("  camera %.2f %.2f %.2f (nodes)%s", s.camera_pos.x, s.camera_pos.y, s.camera_pos.z,
			world_program ? "" : ", from the sky (no world drawn)");
		log::info("Luanti projection: NDC depth %s, near plane %.3f nodes, far plane %.1f nodes",
			s.depth_zero_to_one ? "[0, 1]" : "[-1, 1]", s.near_plane, s.far_plane);
	}

	// Sun and moon. Luanti only sends these to its shaders when dynamic
	// shadows are on; the light direction belongs to the sun by day and to
	// the moon by night, which always sits opposite the sun.
	float tod = 0, light[3];
	bool have_tod = tracker::uniform(world_program, "f_timeofday", &tod, 1) == 1 ||
		tracker::global_uniform("f_timeofday", &tod, 1) == 1;
	bool have_light = tracker::uniform(world_program, "v_LightDirection", light, 3) == 3 ||
		tracker::global_uniform("v_LightDirection", light, 3) == 3;
	// The sky is always there and always current. Luanti's shadow uniforms
	// only exist with its dynamic shadows on, are zero before the first
	// frame that uses them, and keep their last value once they are
	// switched off, so they are only a fallback.
	// Luanti's dynamic shadows are on while their uniforms keep arriving
	const tracker::UniformValue *light_value = tracker::global_uniform_value("v_LightDirection");
	s.luanti_shadows = light_value && tracker::current_frame() - light_value->frame < 60;
	// A game can hide the sky (another dimension, a realm underground: a mod
	// calls set_sky, set_sun, set_moon and set_stars), and then Luanti draws
	// no sun and no moon at all. The other time it draws neither is with the
	// camera inside its cloud layer, 120 to 136 nodes up by default:
	// far from it, a sky that has not shown for a while was hidden.
	// Then there is no sun to work out from the last one seen.
	double sky_age = geometry::sky_age();
	bool near_clouds = s.camera_pos.y > CLOUD_LAYER_LOW && s.camera_pos.y < CLOUD_LAYER_HIGH;
	bool sky_gone = sky_age > SKY_GONE_SECONDS || (sky_age < 0 && g_frame > SKY_GONE_FRAMES);
	s.sky_hidden = sky_gone && !near_clouds;
	if (s.sky_hidden != g_logged_hidden) {
		g_logged_hidden = s.sky_hidden;
		log::info(s.sky_hidden
				? "Luanti draws no sun or moon, away from its clouds: the game has hidden the sky (skyHidden)"
				: "The sky shows again");
	}
	geometry::Sky sky;
	bool have_sky = !s.sky_hidden && geometry::sky(sky);
	float light_len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
	s.sun_available = !s.sky_hidden && !have_sky && have_tod && have_light && light_len > 0.5f &&
		std::isfinite(light_len);
	if (s.sun_available) {
		s.time_of_day = tod;
		float wicked = wicked_time_of_day(tod);
		bool is_day = wicked > 0.25f && wicked < 0.75f;
		s.light_dir = Vec3{-light[0], -light[1], -light[2]}.normalized();
		s.sun_dir = is_day ? s.light_dir : -s.light_dir;
		s.moon_dir = -s.sun_dir;
	} else if (have_sky) {
		// Without dynamic shadows: the sun as Luanti's sky draws it
		s.sun_available = true;
		s.time_of_day = sky.time_of_day;
		float wicked = wicked_time_of_day(sky.time_of_day);
		bool is_day = wicked > 0.25f && wicked < 0.75f;
		s.sun_dir = sky.sun_dir;
		s.moon_dir = -s.sun_dir;
		s.light_dir = is_day ? s.sun_dir : s.moon_dir;
	} else {
		s.time_of_day = -1;
		s.sun_dir = s.light_dir = {0, 1, 0};
		s.moon_dir = {0, -1, 0};
		if (!g_warned_no_sun && !s.sky_hidden && g_frame > 600) {
			g_warned_no_sun = true;
			log::info("Warning: the sun was not found (neither in Luanti's dynamic shadows nor in the sky). "
				"Packs will receive sunAvailable = false.");
		}
	}

	if (s.sun_available)
		smooth_sun(s);
	s.light_body = s.light_dir;

	float fog[4];
	if (tracker::uniform(world_program, "fogColor", fog, 4) >= 3 || tracker::global_uniform("fogColor", fog, 4) >= 3) {
		s.fog_color[0] = fog[0];
		s.fog_color[1] = fog[1];
		s.fog_color[2] = fog[2];
	}
	float fog_distance;
	if (tracker::uniform(world_program, "fogDistance", &fog_distance, 1) == 1 ||
			tracker::global_uniform("fogDistance", &fog_distance, 1) == 1)
		s.fog_distance = fog_distance / BS;
	tracker::global_uniform("fogShadingParameter", &s.fog_shading, 1);
	tracker::named_uniform("cloud_shader", "materialColor", s.cloud_color, 4);
	tracker::global_uniform("dayLight", s.day_light, 3);
	tracker::global_uniform("animationTimer", &s.animation_timer, 1);

	s.width = width;
	s.height = height;
	double now = uptime();
	s.frame_time = g_last_time < 0 ? 0.f : float(now - g_last_time);
	s.time = float(now);
	s.frame = g_frame;

	s.prev_view = g_have_prev ? g_prev_view : s.view;
	s.prev_proj = g_have_prev ? g_prev_proj : s.proj;
	s.prev_camera_pos = g_have_prev ? g_prev_camera : s.camera_pos;
	return true;
}

bool apply_sky_camera(GameState &s, const float *wv, const float *wvp, const Vec3 *camera_pos)
{
	// The sky is drawn at the camera, scaled: its world-view matrix is the
	// view rotation times that scale, with no translation.
	Mat4 wv_m = Mat4::from(wv);
	Mat4 rot;
	for (int c = 0; c < 3; c++) {
		float len = std::sqrt(wv_m.at(0, c) * wv_m.at(0, c) + wv_m.at(1, c) * wv_m.at(1, c) +
			wv_m.at(2, c) * wv_m.at(2, c));
		if (!(len > 0) || !std::isfinite(len))
			return false;
		for (int r = 0; r < 3; r++)
			rot.at(r, c) = wv_m.at(r, c) / len;
	}
	Mat4 proj_bs = Mat4::from(wvp) * wv_m.inverse();
	s.view = rot;
	s.proj = proj_bs * Mat4::scale(BS);
	s.view_inv = s.view.inverse();
	s.proj_inv = s.proj.inverse();
	if (camera_pos)
		s.camera_pos = *camera_pos;
	return true;
}

void end_state_frame(const GameState &s)
{
	g_have_prev = true;
	g_prev_view = s.view;
	g_prev_proj = s.proj;
	g_prev_camera = s.camera_pos;
	g_last_time = s.time;
	g_frame++;
}

} // namespace prisma
