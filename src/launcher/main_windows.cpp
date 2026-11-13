// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	prisma.exe — starts Luanti with prisma_core.dll loaded into it.

	Nothing in the game folder is touched: the game is started suspended, the
	core DLL is loaded into it through a remote LoadLibraryW call, and only then
	is the game allowed to run.

	If the active pack fixes some of Luanti's settings (pack.toml, [luanti]),
	the game runs with a copy of the player's minetest.conf that has them, and
	the launcher waits for it to close to copy back whatever else the player
	changed (see luanticonf.hpp).

	Usage:
	    prisma.exe                         shows the configuration window (ui.hpp), unless
	                                       prisma.ini says show_window=0; with Shift held it always does
	    prisma.exe --configure             shows the window
	    prisma.exe <path\luanti.exe>      starts the game right away with that path, and remembers it
	    prisma.exe [<path>] -- <args>     starts it right away; everything after -- goes to Luanti

	It is a window program: there is no console unless it was started from one.
*/
#include "launcher/launcher.hpp"
#include "launcher/luanticonf.hpp"
#include "common/ini.hpp"
#include "common/util.hpp"

#include <windows.h>
#include <commdlg.h>

#include <map>
#include <string>
#include <vector>

using namespace prisma::launcher;
using prisma::to_utf8;
using prisma::utf8_path;

static std::wstring wide(const std::string &utf8)
{
	return utf8_path(utf8).wstring();
}

static std::string utf8(const std::wstring &w)
{
	return to_utf8(std::filesystem::path(w));
}

static bool g_console = false; // started from a console: messages go there

static void say(const std::wstring &text)
{
	std::wstring line = text + L"\n";
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD written = 0;
	if (!WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &written, nullptr)) {
		int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
		std::string utf8(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
		WriteFile(out, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
	}
}

// The launcher's own log (logs\launcher.log): after the game starts the
// console may be gone, so what happens at the end is written here.
static void log_lines(const std::wstring &dir, const std::vector<std::wstring> &lines)
{
	if (lines.empty())
		return;
	CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);
	HANDLE f = CreateFileW((dir + L"\\logs\\launcher.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t stamp[32];
	wsprintfW(stamp, L"[%04u-%02u-%02u %02u:%02u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
	for (const std::wstring &l : lines) {
		std::wstring line = stamp + l + L"\r\n";
		int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
		std::string utf8(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), n, nullptr, nullptr);
		DWORD written = 0;
		WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
	}
	CloseHandle(f);
}

static void say_and_log(const std::wstring &dir, const std::vector<std::wstring> &lines)
{
	for (const std::wstring &l : lines)
		say(l);
	log_lines(dir, lines);
}

static int fail(const std::wstring &text)
{
	if (g_console)
		say(L"ERROR: " + text);
	else
		MessageBoxW(nullptr, text.c_str(), L"Prisma", MB_ICONERROR | MB_OK);
	return 1;
}

static std::wstring last_error_text()
{
	DWORD code = GetLastError();
	wchar_t *msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
	std::wstring s = msg ? msg : L"";
	LocalFree(msg);
	while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r'))
		s.pop_back();
	return s + L" (code " + std::to_wstring(code) + L")";
}

static std::wstring self_dir()
{
	wchar_t path[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
	std::wstring s(path, n);
	return s.substr(0, s.find_last_of(L"\\/"));
}

static std::wstring dir_of(const std::wstring &file)
{
	size_t slash = file.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : file.substr(0, slash);
}

static bool file_exists(const std::wstring &path)
{
	DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ends_with_exe(const std::wstring &s)
{
	return s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0;
}

static std::wstring ask_for_luanti()
{
	wchar_t file[MAX_PATH * 4] = L"luanti.exe";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = L"Luanti (luanti.exe)\0luanti.exe;minetest.exe\0Programas (*.exe)\0*.exe\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH * 4;
	ofn.lpstrTitle = L"Prisma: choose the game's luanti.exe (bin folder)";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

static std::wstring quote(const std::wstring &arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
		return arg;
	std::wstring out = L"\"";
	size_t backslashes = 0;
	for (wchar_t c : arg) {
		if (c == L'\\') {
			backslashes++;
			continue;
		}
		if (c == L'"')
			out.append(backslashes * 2 + 1, L'\\');
		else
			out.append(backslashes, L'\\');
		backslashes = 0;
		out += c;
	}
	out.append(backslashes * 2, L'\\');
	return out + L"\"";
}

static bool inject(HANDLE process, const std::wstring &dll, std::wstring &error)
{
	size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
	void *remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote) {
		error = L"VirtualAllocEx: " + last_error_text();
		return false;
	}
	bool ok = false;
	if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
		error = L"WriteProcessMemory: " + last_error_text();
	} else {
		// kernel32 is mapped at the same address in every process of this
		// boot session, so our LoadLibraryW is theirs too. The remote thread is
		// the first one to run in the suspended process, so it also performs
		// the loader initialisation before calling LoadLibraryW.
		auto load_library = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
		HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
		if (!thread) {
			error = L"CreateRemoteThread: " + last_error_text();
		} else {
			if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
				error = L"Loading the DLL did not finish in time";
			} else {
				DWORD code = 0;
				GetExitCodeThread(thread, &code);
				ok = code != 0;
				if (!ok)
					error = L"LoadLibraryW failed inside the game (antivirus?)";
			}
			CloseHandle(thread);
		}
	}
	VirtualFreeEx(process, remote, 0, MEM_RELEASE);
	return ok;
}

int wmain(int argc, wchar_t **argv)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		g_console = true;
		say(L"");
	}

	const std::wstring here = self_dir();
	const std::wstring dll = here + L"\\prisma_core.dll";
	const std::wstring ini = here + L"\\prisma.ini";
	if (!file_exists(dll))
		return fail(L"Not found: " + dll);

	// ---- arguments
	std::wstring luanti;
	std::vector<std::wstring> passthrough;
	bool configure = false, has_args = false;
	for (int i = 1; i < argc; i++) {
		std::wstring a = argv[i];
		if (a == L"--configure") {
			configure = true;
			continue;
		}
		has_args = true;
		if (a == L"--") {
			for (i++; i < argc; i++)
				passthrough.push_back(argv[i]);
			break;
		}
		if (luanti.empty() && ends_with_exe(a))
			luanti = a;
		else
			passthrough.push_back(a);
	}

	Config config;
	config.load(ini);
	auto configured_arguments = [&config] {
		std::vector<std::wstring> out;
		for (const std::string &a : split_arguments(config.arguments))
			out.push_back(wide(a));
		return out;
	};

	// ---- the configuration window: when started without arguments (unless
	// prisma.ini says show_window=0 and Shift is not held), or if Luanti can't be found
	const std::wstring wanted = luanti.empty() ? wide(config.luanti) : luanti;
	bool missing = wanted.empty() || !file_exists(wanted);
	bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	if (configure || (!has_args && (config.show_window || shift || missing))) {
		if (run_window(here, ini, config) != WindowResult::Play)
			return 0;
		luanti = wide(config.luanti);
		passthrough = configured_arguments();
	} else {
		if (luanti.empty())
			luanti = wide(config.luanti);
		if (!has_args)
			passthrough = configured_arguments();
		if (luanti.empty() || !file_exists(luanti)) {
			if (!luanti.empty())
				say(L"Not found: " + luanti);
			luanti = ask_for_luanti();
			if (luanti.empty())
				return fail(L"No luanti.exe was chosen");
		}
	}
	wchar_t full[MAX_PATH * 4];
	if (GetFullPathNameW(luanti.c_str(), MAX_PATH * 4, full, nullptr))
		luanti = full;
	prisma::ini::set(ini, "prisma", "luanti", utf8(luanti));
	say(L"Game: " + luanti);
	std::string warning = version_warning(luanti_version(utf8(luanti)));
	if (!warning.empty())
		say_and_log(here, {wide(warning)});

	// ---- Luanti's settings fixed by the pack. A previous session that never
	// got to copy back the player's changes (the launcher was killed) is
	// finished first.
	std::vector<std::string> lines; // UTF-8, as luanticonf writes them
	auto wide_lines = [&lines] {
		std::vector<std::wstring> out;
		for (const std::string &l : lines)
			out.push_back(wide(l));
		lines.clear();
		return out;
	};
	prisma::luanticonf::finish(here, lines);
	std::map<std::string, std::string> forced;
	if (config.apply_luanti_settings)
		forced = luanti_settings(read_pack(config.packs_folder(here) / utf8_path(config.pack), config.pack));
	std::vector<std::string> args;
	for (const std::wstring &a : passthrough)
		args.push_back(utf8(a));
	prisma::luanticonf::Session settings = prisma::luanticonf::prepare(here, forced, luanti, args, lines);
	passthrough.clear();
	for (const std::string &a : args)
		passthrough.push_back(wide(a));
	say_and_log(here, wide_lines());

	// ---- start it suspended
	std::wstring cmdline = quote(luanti);
	for (auto &a : passthrough)
		cmdline += L" " + quote(a);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(luanti.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
			dir_of(luanti).c_str(), &si, &pi)) {
		std::wstring why = last_error_text();
		prisma::luanticonf::finish(here, lines); // nothing changed: just tidy up
		return fail(L"Could not start the game: " + why);
	}

	wchar_t event_name[64];
	wsprintfW(event_name, L"Local\\PrismaReady-%lu", pi.dwProcessId);
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, event_name);

	std::wstring error;
	bool injected = inject(pi.hProcess, dll, error);
	bool hooked = injected && ready && WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	if (ready)
		CloseHandle(ready);

	int result = 0;
	if (!injected)
		result = fail(L"Could not load Prisma in the game: " + error +
			L"\nThe game was opened anyway, but without Prisma.");
	else if (!hooked)
		result = fail(L"Prisma loaded but could not hook into OpenGL. Check " + here + L"\\logs\\prisma.log"
			L"\nThe game was opened anyway.");
	else
		say(L"Prisma is active. Log: " + here + L"\\logs\\prisma.log");

	if (settings.active) {
		// Wait for the game to copy back the player's changes (no window: it
		// carries on in the background).
		say(L"When the game closes, the settings you change will be saved to your minetest.conf.");
		WaitForSingleObject(pi.hProcess, INFINITE);
		prisma::luanticonf::finish(here, lines);
		log_lines(here, wide_lines());
	}
	CloseHandle(pi.hProcess);
	return result;
}
