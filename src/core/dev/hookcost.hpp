// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The CPU time Prisma adds to the game, measured
	in developer mode only:

	  * per GL call the game makes: the time spent in Prisma's hook minus the
	    time of the real call (HOOK_COST and REAL in hooks.cpp);
	  * the pack's passes (frame.cpp, run()), wherever they run: inside the
	    hook of the draw that starts the HUD, or before the swap;
	  * the rest of Prisma's work before the swap (the menu), without the
	    swap itself.

	These are CPU times, waits for the GPU included: a pass whose commands
	fill the driver's queue waits there. Measuring costs some 50 ns per GL
	call. Off, each scope costs one test of a global flag.
*/
#include <chrono>
#include <cstdint>
#include <string>

namespace prisma::hookcost {

extern bool g_on;

inline int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One per hook (HOOK_COST in hooks.cpp), to tell which ones cost most
struct Counter {
	const char *name;
	int64_t calls = 0, ns = 0, real_ns = 0;
	explicit Counter(const char *hook_name);
};

void add_call(Counter &c, int64_t ns);
void add_real(int64_t ns);
void add_passes(int64_t ns);
void add_frame_end(int64_t ns);
extern int g_in_call;
extern int64_t g_passes_ns; // since the start: a hook subtracts what ran inside it
extern Counter *g_counter;  // of the hook running now

// The time of a whole hook, real call included
struct CallScope {
	Counter &counter;
	int64_t start = g_on ? now_ns() : 0;
	explicit CallScope(Counter &c) : counter(c)
	{
		g_in_call++;
		g_counter = &c;
	}
	~CallScope()
	{
		g_in_call--;
		if (start)
			add_call(counter, now_ns() - start);
	}
};

// The pack's passes: counted apart, and not as time in the hook they run in
struct PassScope {
	int64_t start = g_on ? now_ns() : 0;
	~PassScope()
	{
		if (start)
			add_passes(now_ns() - start);
	}
};

// The time of the real GL call inside a hook
struct RealScope {
	int64_t start = g_on ? now_ns() : 0;
	~RealScope()
	{
		if (start)
			add_real(now_ns() - start);
	}
};

// Turned on by developer mode
void init(bool on);

// Averages per frame since the last call, for the log line every few
// seconds; "" when off. Starts a new period.
std::string report(int frames);
// The hooks that cost most in that period, from the same numbers
std::string report_top(int frames, int how_many);

// The last report's numbers, for the menu
struct Summary {
	double calls_per_frame = 0;
	double hooks_ms = 0;     // per frame, in the hooks of the game's GL calls
	double passes_ms = 0;    // per frame, the pack's passes
	double frame_end_ms = 0; // per frame, Prisma's other work before the swap
};
const Summary &last();

} // namespace prisma::hookcost
