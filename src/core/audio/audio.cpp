// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/audio.hpp"
#include "common/ini.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include "miniaudio.h"


#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

// fs::path::c_str() is wchar_t on Windows and char elsewhere
#ifdef _WIN32
#define sound_from_file ma_sound_init_from_file_w
#else
#define sound_from_file ma_sound_init_from_file
#endif

namespace prisma::audio {

namespace {

struct Voice {
	ma_sound sound;
	bool ready = false;
	size_t file = 0;            // index into the sound's files
	unsigned long long age = 0; // when it last started (to reuse the oldest)
};

// One [sounds.<id>]. ma_sound and ma_lpf_node must not move once
// initialised: tracks live behind pointers and their vectors are sized once.
struct Track {
	pack::Sound def;
	ma_lpf_node lpf;
	bool lpf_ready = false;
	double cutoff = 0;
	std::vector<Voice> voices;    // a loop has one
	std::vector<ma_sound> cached; // one-shots: every file decoded and kept, not playing
	std::vector<bool> cached_ready;
	float trigger = 0;
	bool have_trigger = false;
	float volume = -1; // last volume given to the loop
};

ma_engine g_engine;
bool g_engine_ok = false, g_engine_failed = false;
float g_master = 1;
std::vector<pack::Sound> g_defs;
bool g_defs_changed = false;
std::vector<std::unique_ptr<Track>> g_tracks;
double g_last_update = -1;
bool g_silenced = true;
bool g_paused = false;
double g_resumed = -1e9; // when the sounds last started (the loops fade in from there)
constexpr double k_fade_in = 3.0; // seconds
unsigned long long g_starts = 0;
std::mt19937 g_random{std::random_device{}()};

constexpr ma_uint32 k_flags = MA_SOUND_FLAG_NO_SPATIALIZATION;

bool engine()
{
	if (g_engine_ok)
		return true;
	if (g_engine_failed || g_master <= 0)
		return false;
	ma_engine_config config = ma_engine_config_init();
	ma_result r = ma_engine_init(&config, &g_engine);
	if (r != MA_SUCCESS) {
		g_engine_failed = true;
		log::error("Prisma: can't open the audio output (%s); the pack's sounds will be silent",
			ma_result_description(r));
		return false;
	}
	g_engine_ok = true;
	ma_engine_set_volume(&g_engine, g_master);
	log::info("Audio: %u Hz, %u channels", ma_engine_get_sample_rate(&g_engine), ma_engine_get_channels(&g_engine));
	return true;
}

void destroy(Track &t)
{
	for (Voice &v : t.voices)
		if (v.ready)
			ma_sound_uninit(&v.sound);
	for (size_t i = 0; i < t.cached.size(); i++)
		if (t.cached_ready[i])
			ma_sound_uninit(&t.cached[i]);
	if (t.lpf_ready)
		ma_lpf_node_uninit(&t.lpf, nullptr);
}

void clear()
{
	for (auto &t : g_tracks)
		destroy(*t);
	g_tracks.clear();
}

ma_node *output_of(Track &t)
{
	return t.lpf_ready ? (ma_node *)&t.lpf : ma_engine_get_endpoint(&g_engine);
}

bool open_voice(Track &t, Voice &v, size_t file, ma_uint32 flags)
{
	if (v.ready) {
		ma_sound_uninit(&v.sound);
		v.ready = false;
	}
	ma_result r = sound_from_file(&g_engine, t.def.files[file].c_str(), flags | k_flags, nullptr,
		nullptr, &v.sound);
	if (r != MA_SUCCESS) {
		log::error("Sound %s: can't open %s (%s)", t.def.id.c_str(), to_utf8(t.def.files[file]).c_str(),
			ma_result_description(r));
		return false;
	}
	ma_node_attach_output_bus(&v.sound, 0, output_of(t), 0);
	v.ready = true;
	v.file = file;
	return true;
}

void build()
{
	clear();
	g_defs_changed = false;
	for (const pack::Sound &def : g_defs) {
		auto t = std::make_unique<Track>();
		t->def = def;
		// A low-pass per sound for "muffle"; wide open to start with
		ma_lpf_node_config c = ma_lpf_node_config_init(ma_engine_get_channels(&g_engine),
			ma_engine_get_sample_rate(&g_engine), 20000.0, 2);
		if (ma_lpf_node_init(ma_engine_get_node_graph(&g_engine), &c, nullptr, &t->lpf) == MA_SUCCESS) {
			ma_node_attach_output_bus(&t->lpf, 0, ma_engine_get_endpoint(&g_engine), 0);
			t->lpf_ready = true;
			t->cutoff = 20000.0;
		}
		if (def.loop) {
			// Streamed: long loops don't need to be decoded whole in memory
			t->voices.resize(1);
			Voice &v = t->voices[0];
			if (open_voice(*t, v, 0, MA_SOUND_FLAG_STREAM)) {
				// Its volume is all in the fader (which multiplies the
				// sound's volume): it starts silent and update() fades it
				ma_sound_set_looping(&v.sound, MA_TRUE);
				ma_sound_set_fade_in_milliseconds(&v.sound, 0.f, 0.f, 0);
				t->volume = 0.f;
				ma_sound_start(&v.sound);
			}
		} else {
			// Decoded in the background now, so the first time it plays is instant
			t->cached.resize(def.files.size());
			t->cached_ready.assign(def.files.size(), false);
			for (size_t i = 0; i < def.files.size(); i++)
				t->cached_ready[i] = sound_from_file(&g_engine, def.files[i].c_str(),
					MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | k_flags, nullptr, nullptr,
					&t->cached[i]) == MA_SUCCESS;
			t->voices.resize(def.voices);
		}
		g_tracks.push_back(std::move(t));
	}
	if (!g_defs.empty())
		log::info("Audio: %zu sound(s) in the pack", g_defs.size());
}

// muffle 0 = clear .. 1 = through a wall: the cutoff falls exponentially
void set_muffle(Track &t, float muffle)
{
	if (!t.lpf_ready)
		return;
	double cutoff = 20000.0 * std::pow(350.0 / 20000.0, std::clamp((double)muffle, 0.0, 1.0));
	if (std::fabs(cutoff - t.cutoff) < t.cutoff * 0.03)
		return;
	t.cutoff = cutoff;
	ma_lpf_config c = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&g_engine),
		ma_engine_get_sample_rate(&g_engine), cutoff, 2);
	ma_lpf_node_reinit(&c, &t.lpf);
}

void play(Track &t, float volume, float pitch, float pan)
{
	size_t file = std::uniform_int_distribution<size_t>(0, t.def.files.size() - 1)(g_random);
	// A free voice, or else the one that started longest ago
	Voice *voice = nullptr;
	for (Voice &v : t.voices)
		if (!v.ready || !ma_sound_is_playing(&v.sound)) {
			voice = &v;
			break;
		}
	if (!voice)
		voice = &*std::min_element(t.voices.begin(), t.voices.end(),
			[](const Voice &a, const Voice &b) { return a.age < b.age; });
	if (!voice->ready || voice->file != file) {
		// Shares the decoded data of the cached copy
		if (!open_voice(t, *voice, file, MA_SOUND_FLAG_DECODE))
			return;
	} else {
		ma_sound_stop(&voice->sound);
		ma_sound_seek_to_pcm_frame(&voice->sound, 0);
	}
	ma_sound_set_volume(&voice->sound, volume);
	ma_sound_set_pitch(&voice->sound, pitch);
	ma_sound_set_pan(&voice->sound, pan);
	ma_sound_start(&voice->sound);
	voice->age = ++g_starts;
}

void fade_loops_out(ma_uint64 ms)
{
	for (auto &t : g_tracks)
		if (t->def.loop && t->voices[0].ready && t->volume != 0.f) {
			ma_sound_set_fade_in_milliseconds(&t->voices[0].sound, -1.f, 0.f, ms);
			t->volume = 0.f;
		}
}

// One-shots still sounding (a thunder) fade out too
void fade_one_shots_out(ma_uint64 ms)
{
	for (auto &t : g_tracks)
		if (!t->def.loop)
			for (Voice &v : t->voices)
				if (v.ready && ma_sound_is_playing(&v.sound))
					ma_sound_stop_with_fade_in_milliseconds(&v.sound, ms);
}

// How much of their volume the loops have while fading in (0..1)
float fade_in_now()
{
	double x = std::clamp((uptime() - g_resumed) / k_fade_in, 0.0, 1.0);
	return (float)(x * x * (3.0 - 2.0 * x));
}

} // namespace

void init(const std::filesystem::path &ini)
{
	g_master = std::clamp((float)std::atof(ini::get(ini, "audio", "volume", "1").c_str()), 0.f, 4.f);
	if (g_master <= 0)
		log::info("Audio disabled ([audio] volume = 0 in prisma.ini)");
}

void set_pack(const pack::Pack &pack)
{
	g_defs = pack.sounds;
	g_defs_changed = true;
}

void update(const float *texels, int width, int height)
{
	if (g_defs.empty() && g_tracks.empty())
		return;
	if (!engine())
		return;
	if (g_defs_changed) {
		build();
		g_resumed = uptime();
	}
	g_last_update = uptime();
	if (g_silenced)
		g_resumed = uptime();
	g_silenced = false;
	float fade_in = fade_in_now();
	for (auto &tp : g_tracks) {
		Track &t = *tp;
		int x = t.def.control;
		if (x < 0 || x >= width)
			continue;
		const float *a = texels + x * 4;
		const float *b = height > 1 ? texels + (width + x) * 4 : nullptr;
		auto finite = [](float v) { return std::isfinite(v) ? v : 0.f; };
		float volume = std::clamp(finite(a[0]), 0.f, 4.f) * (float)t.def.volume;
		float pitch = finite(a[1]) > 0.f ? std::clamp(a[1], 0.05f, 4.f) : 1.f;
		float trigger = finite(a[2]);
		float pan = std::clamp(finite(a[3]), -1.f, 1.f);
		set_muffle(t, b ? finite(b[0]) : 0.f);

		if (t.def.loop) {
			Voice &v = t.voices[0];
			if (!v.ready || g_paused)
				continue;
			volume *= fade_in;
			// A short fade to every new volume: the passes' values arrive once
			// per frame, and steps would crackle
			if (std::fabs(volume - t.volume) > 0.002f) {
				ma_sound_set_fade_in_milliseconds(&v.sound, -1.f, volume, 80);
				t.volume = volume;
			}
			ma_sound_set_pitch(&v.sound, pitch);
			ma_sound_set_pan(&v.sound, pan);
		} else {
			// The first value seen is the starting point, not an event
			if (!t.have_trigger) {
				t.trigger = trigger;
				t.have_trigger = true;
				continue;
			}
			if (trigger != t.trigger) {
				t.trigger = trigger;
				// Paused: the event passes without a sound (and doesn't wait
				// to burst out when the menu closes)
				if (trigger > 0.f && volume > 0.f && !g_paused)
					play(t, volume, pitch, pan);
			}
		}
	}
}

void on_frame_end()
{
	if (!g_engine_ok)
		return;
	if (g_defs_changed && g_defs.empty()) {
		clear();
		g_defs_changed = false;
	}
	if (!g_silenced && uptime() - g_last_update > 0.3) {
		fade_loops_out(400);
		fade_one_shots_out(400);
		g_silenced = true;
	}
}

void set_paused(bool paused)
{
	if (paused == g_paused)
		return;
	g_paused = paused;
	if (!g_engine_ok)
		return;
	if (paused) {
		fade_loops_out(500);
		fade_one_shots_out(500);
		log::info("Audio: pause menu open, sounds muted");
	} else {
		g_resumed = uptime();
	}
}

} // namespace prisma::audio
