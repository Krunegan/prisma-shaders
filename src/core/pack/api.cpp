// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pack/api.hpp"
#include "core/hooks/glapi.hpp"
#include "glsl_embedded.hpp"

#include <string>

namespace prisma::api {

const char *const GLSL = embedded::api_glsl;
const char *const SHADOW_GLSL = embedded::shadows_glsl;

static const char *const k_names[F_COUNT] = {
	"viewMatrix", "viewMatrixInverse", "projectionMatrix", "projectionMatrixInverse", "prevViewMatrix",
	"prevProjectionMatrix", "cameraPosition", "prevCameraPosition", "cameraOffset", "sunAvailable", "sunDirection",
	"moonDirection", "lightDirection", "timeOfDay", "skyHidden", "dayLight", "fogColor", "fogDistance", "fogShading", "cloudColor", "liquidColor",
	"nearPlane", "farPlane", "shadowAvailable", "shadowMatrix", "shadowMatrixPrev", "shadowBlend", "shadowDistance", "shadowDepthRange", "shadowMapSize",
	"overheadAvailable", "overheadMatrix", "overheadDistance", "overheadDepthRange", "overheadMapSize",
	"viewSize", "texelSize", "frameTime", "frameTimeCounter", "animationTimer", "frameCounter",
};

void Locations::query(GLuint program)
{
	any = false;
	for (int f = 0; f < F_COUNT; f++) {
		std::string name = std::string("prisma.") + k_names[f];
		loc[f] = gl::GetUniformLocation(program, name.c_str());
		any |= loc[f] >= 0;
	}
}

void upload(const Locations &l, const GameState &s)
{
	if (!l.any)
		return;
	const GLint *loc = l.loc;
	auto m4 = [&](int f, const Mat4 &m) {
		if (loc[f] >= 0)
			gl::UniformMatrix4fv(loc[f], 1, 0, m.m);
	};
	auto v3 = [&](int f, const Vec3 &v) {
		if (loc[f] >= 0)
			gl::Uniform3f(loc[f], v.x, v.y, v.z);
	};
	auto f1 = [&](int f, float v) {
		if (loc[f] >= 0)
			gl::Uniform1f(loc[f], v);
	};
	auto v2 = [&](int f, float x, float y) {
		if (loc[f] >= 0)
			gl::Uniform2f(loc[f], x, y);
	};
	auto i1 = [&](int f, int v) {
		if (loc[f] >= 0)
			gl::Uniform1i(loc[f], v);
	};
	m4(F_viewMatrix, s.view);
	m4(F_viewMatrixInverse, s.view_inv);
	m4(F_projectionMatrix, s.proj);
	m4(F_projectionMatrixInverse, s.proj_inv);
	m4(F_prevViewMatrix, s.prev_view);
	m4(F_prevProjectionMatrix, s.prev_proj);
	v3(F_cameraPosition, s.camera_pos);
	v3(F_prevCameraPosition, s.prev_camera_pos);
	v3(F_cameraOffset, s.camera_offset);
	i1(F_sunAvailable, s.sun_available ? 1 : 0);
	v3(F_sunDirection, s.sun_dir);
	v3(F_moonDirection, s.moon_dir);
	v3(F_lightDirection, s.light_dir);
	f1(F_timeOfDay, s.time_of_day);
	i1(F_skyHidden, s.sky_hidden ? 1 : 0);
	v3(F_dayLight, {s.day_light[0], s.day_light[1], s.day_light[2]});
	v3(F_fogColor, {s.fog_color[0], s.fog_color[1], s.fog_color[2]});
	f1(F_fogDistance, s.fog_distance);
	f1(F_fogShading, s.fog_shading);
	if (loc[F_cloudColor] >= 0)
		gl::Uniform4f(loc[F_cloudColor], s.cloud_color[0], s.cloud_color[1], s.cloud_color[2], s.cloud_color[3]);
	if (loc[F_liquidColor] >= 0)
		gl::Uniform4f(loc[F_liquidColor], s.liquid_color[0], s.liquid_color[1], s.liquid_color[2], s.liquid_color[3]);
	f1(F_nearPlane, s.near_plane);
	f1(F_farPlane, s.far_plane);
	i1(F_shadowAvailable, s.shadow_available ? 1 : 0);
	m4(F_shadowMatrix, s.shadow_matrix);
	m4(F_shadowMatrixPrev, s.shadow_matrix_prev);
	f1(F_shadowBlend, s.shadow_blend);
	f1(F_shadowDistance, s.shadow_distance);
	f1(F_shadowDepthRange, s.shadow_depth_range);
	v2(F_shadowMapSize, (float)s.shadow_size, (float)s.shadow_size);
	i1(F_overheadAvailable, s.overhead_available ? 1 : 0);
	m4(F_overheadMatrix, s.overhead_matrix);
	f1(F_overheadDistance, s.overhead_distance);
	f1(F_overheadDepthRange, s.overhead_depth_range);
	v2(F_overheadMapSize, (float)s.overhead_size, (float)s.overhead_size);
	v2(F_viewSize, (float)s.width, (float)s.height);
	v2(F_texelSize, s.width > 0 ? 1.f / s.width : 0.f, s.height > 0 ? 1.f / s.height : 0.f);
	f1(F_frameTime, s.frame_time);
	f1(F_frameTimeCounter, s.time);
	f1(F_animationTimer, s.animation_timer);
	i1(F_frameCounter, s.frame);
}

} // namespace prisma::api
