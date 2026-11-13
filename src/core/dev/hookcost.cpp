// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dev/hookcost.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace prisma::hookcost {

bool g_on = false;

int g_in_call = 0;
Counter *g_counter = nullptr;

static std::vector<Counter *> &counters()
{
	static std::vector<Counter *> list;
	return list;
}

// Hook counters are function statics, built on the hook's first call
Counter::Counter(const char *hook_name) : name(hook_name)
{
	counters().push_back(this);
}
int64_t g_passes_ns = 0;
static int64_t g_calls = 0, g_call_ns = 0, g_real_ns = 0, g_frame_end_ns = 0, g_period_passes_ns = 0;
static Summary g_last;

void init(bool on)
{
	g_on = on;
}

void add_call(Counter &c, int64_t ns)
{
	g_calls++;
	g_call_ns += ns;
	c.calls++;
	c.ns += ns;
}

void add_real(int64_t ns)
{
	g_real_ns += ns;
	if (g_counter)
		g_counter->real_ns += ns;
}

void add_passes(int64_t ns)
{
	g_passes_ns += ns;
	g_period_passes_ns += ns;
	if (g_in_call)
		add_real(ns); // not the hook's own time
}

void add_frame_end(int64_t ns)
{
	g_frame_end_ns += ns;
}

std::string report(int frames)
{
	if (!g_on || frames <= 0)
		return "";
	g_last.calls_per_frame = double(g_calls) / frames;
	g_last.hooks_ms = double(g_call_ns - g_real_ns) / frames / 1e6;
	g_last.passes_ms = double(g_period_passes_ns) / frames / 1e6;
	g_last.frame_end_ms = double(g_frame_end_ns) / frames / 1e6;
	g_calls = g_call_ns = g_real_ns = g_frame_end_ns = g_period_passes_ns = 0;
	char text[240];
	std::snprintf(text, sizeof text, "hooks: %.0f GL calls per frame, %.3f ms per frame in Prisma's hooks, %.3f ms "
		"in the passes, %.3f ms in other work at the end of the frame", g_last.calls_per_frame, g_last.hooks_ms,
		g_last.passes_ms, g_last.frame_end_ms);
	return text;
}

std::string report_top(int frames, int how_many)
{
	if (!g_on || frames <= 0)
		return "";
	std::vector<Counter *> list = counters();
	std::sort(list.begin(), list.end(), [](Counter *a, Counter *b) { return a->ns - a->real_ns > b->ns - b->real_ns; });
	std::string out;
	for (int i = 0; i < how_many && i < (int)list.size() && list[i]->calls; i++) {
		char item[120];
		std::snprintf(item, sizeof item, "%s%s %.0f calls %.3f ms", out.empty() ? "" : ", ", list[i]->name + 3,
			double(list[i]->calls) / frames, double(list[i]->ns - list[i]->real_ns) / frames / 1e6);
		out += item;
	}
	for (Counter *c : counters())
		c->calls = c->ns = c->real_ns = 0;
	return "per frame: " + out;
}

const Summary &last()
{
	return g_last;
}

} // namespace prisma::hookcost
