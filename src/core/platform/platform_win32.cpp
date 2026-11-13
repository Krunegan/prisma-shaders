// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/platform.hpp"
#include "core/core.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstring>

namespace prisma::platform {

static const char *dll_name(Library library)
{
	switch (library) {
	case Library::sdl: return "SDL2.dll";
	case Library::gl: return "opengl32.dll";
	}
	return "";
}

// The import descriptor of luanti.exe for `dll`, or null
static IMAGE_IMPORT_DESCRIPTOR *imports_of(HMODULE exe, const char *dll)
{
	auto base = reinterpret_cast<BYTE *>(exe);
	auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
	auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return nullptr;
	for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; d++)
		if (_stricmp(reinterpret_cast<const char *>(base + d->Name), dll) == 0)
			return d;
	return nullptr;
}

// The game's import table slot for `symbol` of `dll`, or null
static void **import_slot(const char *dll, const char *symbol)
{
	HMODULE exe = GetModuleHandleW(nullptr);
	IMAGE_IMPORT_DESCRIPTOR *d = imports_of(exe, dll);
	if (!d)
		return nullptr;
	auto base = reinterpret_cast<BYTE *>(exe);
	auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
	if (d->OriginalFirstThunk) {
		// The names, in a table parallel to the addresses
		auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->OriginalFirstThunk);
		for (int i = 0; names[i].u1.AddressOfData; i++) {
			if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
				continue;
			auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names[i].u1.AddressOfData);
			if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), symbol) == 0)
				return reinterpret_cast<void **>(&slots[i].u1.Function);
		}
		return nullptr;
	}
	// No names left: look for the address the loader put there
	HMODULE module = GetModuleHandleA(dll);
	void *real = module ? reinterpret_cast<void *>(GetProcAddress(module, symbol)) : nullptr;
	if (!real)
		return nullptr;
	for (int i = 0; slots[i].u1.Function; i++)
		if (reinterpret_cast<void *>(slots[i].u1.Function) == real)
			return reinterpret_cast<void **>(&slots[i].u1.Function);
	return nullptr;
}

bool interpose(Interpose *list, int count)
{
	bool ok = true;
	for (int i = 0; i < count; i++) {
		Interpose &e = list[i];
		const char *dll = dll_name(e.library);
		void **slot = import_slot(dll, e.symbol);
		if (!slot) {
			if (e.required) {
				log::error("luanti.exe does not import %s!%s: can't hook it", dll, e.symbol);
				ok = false;
			}
			continue;
		}
		DWORD protect = 0;
		if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protect)) {
			log::error("Can't write the import %s!%s", dll, e.symbol);
			ok = false;
			continue;
		}
		*e.original = *slot;
		*slot = e.detour;
		VirtualProtect(slot, sizeof(void *), protect, &protect);
	}
	return ok;
}

void *library_symbol(Library library, const char *symbol)
{
	HMODULE module = GetModuleHandleA(dll_name(library));
	return module ? reinterpret_cast<void *>(GetProcAddress(module, symbol)) : nullptr;
}

std::filesystem::path game_data_dir()
{
	return host_exe_dir().parent_path(); // luanti.exe lives in <game>/bin
}

unsigned long process_id()
{
	return GetCurrentProcessId();
}

void report_ready(bool ok)
{
	wchar_t name[64];
	wsprintfW(name, L"Local\\PrismaReady-%lu", GetCurrentProcessId());
	if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
		if (ok)
			SetEvent(ev);
		CloseHandle(ev);
	}
}

void open_folder(const std::filesystem::path &folder)
{
	ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace prisma::platform

// prisma_core.dll is loaded into luanti.exe by the launcher while the game
// is still suspended, before it has created any window or GL context.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		prisma::start();
	}
	return TRUE;
}
