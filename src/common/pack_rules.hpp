// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Rules of pack.toml that the launcher, the core and prisma_check must
	apply the same way:

	  * `api`: required, a whole number no higher than this Prisma's.
	  * [luanti]: only Luanti's rendering settings, the ones that turn its own
	    effects on or off or tune them. Security, network, account,
	    path and gameplay settings are never allowed, nor shader_path (it
	    would load shaders from anywhere, around Prisma). Names and types
	    from Luanti 5.17.0's builtin/settingtypes.txt.
*/
#include "common/toml.hpp"
#include "common/version.hpp"

#include <cmath>
#include <string>
#include <string_view>

namespace prisma::pack {

enum class LuantiSettingType { Bool, Number };

struct LuantiSetting {
	std::string_view name;
	LuantiSettingType type;
};

inline constexpr LuantiSetting k_luanti_settings[] = {
	// Fog and clouds
	{"enable_fog", LuantiSettingType::Bool},
	{"directional_colored_fog", LuantiSettingType::Bool},
	{"fog_start", LuantiSettingType::Number},
	{"enable_3d_clouds", LuantiSettingType::Bool},
	{"soft_clouds", LuantiSettingType::Bool},
	// Liquids and waving nodes
	{"translucent_liquids", LuantiSettingType::Bool},
	{"enable_waving_leaves", LuantiSettingType::Bool},
	{"enable_waving_plants", LuantiSettingType::Bool},
	{"enable_waving_water", LuantiSettingType::Bool},
	{"water_wave_height", LuantiSettingType::Number},
	{"water_wave_length", LuantiSettingType::Number},
	{"water_wave_speed", LuantiSettingType::Number},
	// Dynamic shadows
	{"enable_dynamic_shadows", LuantiSettingType::Bool},
	{"shadow_strength_gamma", LuantiSettingType::Number},
	{"shadow_map_max_distance", LuantiSettingType::Number},
	{"shadow_map_texture_size", LuantiSettingType::Number},
	{"shadow_map_texture_32bit", LuantiSettingType::Bool},
	{"shadow_filters", LuantiSettingType::Number},
	{"shadow_map_color", LuantiSettingType::Bool},
	{"shadow_soft_radius", LuantiSettingType::Number},
	{"shadow_sky_body_orbit_tilt", LuantiSettingType::Number},
	{"shadow_poisson_filter", LuantiSettingType::Bool},
	{"shadow_update_frames", LuantiSettingType::Number},
	// Post-processing and other effects
	{"enable_post_processing", LuantiSettingType::Bool},
	{"post_processing_texture_bits", LuantiSettingType::Number},
	{"tone_mapping", LuantiSettingType::Bool},
	{"enable_auto_exposure", LuantiSettingType::Bool},
	{"exposure_compensation", LuantiSettingType::Number},
	{"debanding", LuantiSettingType::Bool},
	{"enable_bloom", LuantiSettingType::Bool},
	{"enable_volumetric_lighting", LuantiSettingType::Bool},
	{"enable_translucent_foliage", LuantiSettingType::Bool},
	{"enable_water_reflections", LuantiSettingType::Bool},
	// Lighting curve
	{"smooth_lighting", LuantiSettingType::Bool},
	{"lighting_alpha", LuantiSettingType::Number},
	{"lighting_beta", LuantiSettingType::Number},
	{"lighting_boost", LuantiSettingType::Number},
	{"lighting_boost_center", LuantiSettingType::Number},
	{"lighting_boost_spread", LuantiSettingType::Number},
};

// Why the pack's `api` (null if missing) is not acceptable, or "" if it is.
inline std::string check_api(const toml::Value *api)
{
	const std::string current = std::to_string(PRISMA_API_VERSION);
	if (!api)
		return "missing api = " + current + " (the Prisma API version the pack is written for)";
	if (api->type != toml::Value::Number || api->num < 1 || api->num != (double)(int)api->num)
		return "api must be a whole number (the Prisma API version, " + current + " in this one)";
	if ((int)api->num > PRISMA_API_VERSION)
		return "api = " + std::to_string((int)api->num) + ": the pack needs a newer Prisma (this one has API " +
			current + "). Update Prisma.";
	return {};
}

// Why `name = value` in [luanti] is not allowed, or "" if it is.
inline std::string check_luanti_setting(std::string_view name, const toml::Value &value)
{
	for (const LuantiSetting &s : k_luanti_settings) {
		if (s.name != name)
			continue;
		if (s.type == LuantiSettingType::Bool && value.type != toml::Value::Bool)
			return "[luanti] " + std::string(name) + " must be true or false";
		if (s.type == LuantiSettingType::Number && (value.type != toml::Value::Number || !std::isfinite(value.num)))
			return "[luanti] " + std::string(name) + " must be a number";
		return {};
	}
	return "[luanti] " + std::string(name) +
		" is not allowed: a pack may only change Luanti's rendering settings (see docs/shaderpacks.md)";
}

} // namespace prisma::pack
