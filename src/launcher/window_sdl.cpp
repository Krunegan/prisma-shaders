// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The launcher's window on Linux: SDL2 with an OpenGL context where
	it draws the interface of ui.cpp (Dear ImGui, SDL2 and OpenGL 3 backends). The
	file and folder dialogs are the desktop's own (zenity or kdialog)
	if they are installed.
*/
#include "launcher/launcher.hpp"
#include "launcher/ui.hpp"
#include "common/util.hpp"
#include "common/version.hpp"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char **environ;

namespace prisma::launcher {

namespace fs = std::filesystem;

namespace {

// Runs a program (no shell) and returns what it writes; false if it
// could not run or ended with an error
bool run(const std::vector<std::string> &argv, std::string &out)
{
	int pipe_fd[2];
	if (pipe(pipe_fd) != 0)
		return false;
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], 1);
	posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", 0, 0);
	std::vector<char *> args;
	for (const std::string &a : argv)
		args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(nullptr);
	pid_t pid = 0;
	int err = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_fd[1]);
	if (err != 0) {
		close(pipe_fd[0]);
		return false;
	}
	out.clear();
	char buf[4096];
	ssize_t n;
	while ((n = read(pipe_fd[0], buf, sizeof buf)) > 0)
		out.append(buf, (size_t)n);
	close(pipe_fd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
		out.pop_back();
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool have(const char *program)
{
	std::string out;
	return run({"sh", "-c", std::string("command -v ") + program}, out) && !out.empty();
}

std::string choose_luanti(const std::string &current)
{
	std::string out;
	if (have("zenity")) {
		std::vector<std::string> argv = {"zenity", "--file-selection", "--title=Choose the Luanti executable"};
		if (!current.empty())
			argv.push_back("--filename=" + current);
		return run(argv, out) ? out : "";
	}
	if (have("kdialog"))
		return run({"kdialog", "--getopenfilename", current.empty() ? "/usr/games" : current}, out) ? out : "";
	// No desktop dialogs: the one in the PATH
	return find_luanti();
}

std::string choose_folder(const std::string &current)
{
	std::string out;
	if (have("zenity"))
		return run({"zenity", "--file-selection", "--directory", "--title=Choose the shader packs folder",
				   "--filename=" + current + "/"}, out) ? out : "";
	if (have("kdialog"))
		return run({"kdialog", "--getexistingdirectory", current}, out) ? out : "";
	return "";
}

} // namespace

// "luanti --version" starts with "Luanti 5.17.0 (Linux)"
std::string luanti_version(const std::string &luanti)
{
	std::string out;
	run({luanti, "--version"}, out);
	std::string first = out.substr(0, out.find('\n'));
	size_t space = first.find(' ');
	if (space == std::string::npos || (!first.starts_with("Luanti") && !first.starts_with("Minetest")))
		return "";
	std::string v = first.substr(space + 1);
	return v.substr(0, v.find(' '));
}

namespace {

void open_folder(const fs::path &folder)
{
	std::string out;
	run({"xdg-open", folder.string()}, out);
}

} // namespace

WindowResult run_window(const fs::path &prisma_dir, const fs::path &ini, Config &config)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::fprintf(stderr, "Can't open the window: %s\n", SDL_GetError());
		return WindowResult::Quit;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	std::string title = std::string("Prisma ") + PRISMA_VERSION;
	SDL_Window *window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 920, 620,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
	if (!gl) {
		std::fprintf(stderr, "Can't open the window: %s\n", SDL_GetError());
		if (window)
			SDL_DestroyWindow(window);
		SDL_Quit();
		return WindowResult::Quit;
	}
	SDL_SetWindowMinimumSize(window, 760, 540);
	SDL_GL_MakeCurrent(window, gl);
	SDL_GL_SetSwapInterval(1);

	Platform platform;
	platform.choose_luanti = choose_luanti;
	platform.choose_folder = choose_folder;
	platform.luanti_version = luanti_version;
	platform.open_folder = open_folder;
	Ui ui(prisma_dir, ini, config, platform);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ui.setup(1);
	ImGui_ImplSDL2_InitForOpenGL(window, gl);
	ImGui_ImplOpenGL3_Init("#version 150");

	// For unattended tests: PRISMA_WINDOW_CAPTURE=<file.ppm> saves
	// the window after a few frames and closes it
	const char *capture = std::getenv("PRISMA_WINDOW_CAPTURE");
	int frames = 0;

	// It draws while something happens and for a few frames after; then waits for the
	// next event without using CPU. While work goes on in the background it also
	// wakes up on its own, to show its end.
	bool running = true;
	bool working = false;
	int idle = 0;
	while (running) {
		SDL_Event e;
		bool got = idle >= 4 && !capture ? SDL_WaitEvent(&e) :
			working && !capture ? SDL_WaitEventTimeout(&e, 33) : SDL_PollEvent(&e);
		while (got) {
			ImGui_ImplSDL2_ProcessEvent(&e);
			if (e.type == SDL_QUIT ||
					(e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
						e.window.windowID == SDL_GetWindowID(window)))
				ui.request_close();
			// Back from the file manager: a pack may have been added by hand
			if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				ui.window_focused();
			idle = 0;
			got = SDL_PollEvent(&e);
		}
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();
		running = ui.frame();
		ImGui::Render();
		int w = 0, h = 0;
		SDL_GL_GetDrawableSize(window, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(20 / 255.f, 22 / 255.f, 27 / 255.f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		if (capture && ++frames == 10) {
			std::vector<unsigned char> pixels((size_t)w * h * 3);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
			if (std::FILE *f = std::fopen(capture, "wb")) {
				std::fprintf(f, "P6\n%d %d\n255\n", w, h);
				for (int y = h - 1; y >= 0; y--)
					std::fwrite(pixels.data() + (size_t)y * w * 3, 1, (size_t)w * 3, f);
				std::fclose(f);
			}
			running = false;
		}
		SDL_GL_SwapWindow(window);
		idle++;
		working = ui.working();
		if (working)
			idle = 0;
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_GL_DeleteContext(gl);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return ui.result();
}

} // namespace prisma::launcher
