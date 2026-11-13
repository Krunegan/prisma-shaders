// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/util.hpp"

#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace prisma {

namespace fs = std::filesystem;

std::string to_utf8(const fs::path &path)
{
	std::u8string s = path.u8string();
	return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

std::string to_utf8_generic(const fs::path &path)
{
	std::u8string s = path.generic_u8string();
	return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

fs::path utf8_path(std::string_view text)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

std::FILE *open_file(const fs::path &path, const char *mode)
{
#ifdef _WIN32
	std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
	return _wfopen(path.c_str(), wmode.c_str());
#else
	return std::fopen(path.c_str(), mode);
#endif
}

#ifdef _WIN32

static fs::path module_dir(HMODULE mod)
{
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		DWORD n = GetModuleFileNameW(mod, path.data(), (DWORD)path.size());
		if (n < path.size()) {
			path.resize(n);
			break;
		}
		path.resize(path.size() * 2);
	}
	return fs::path(path).parent_path();
}

fs::path core_dir()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&core_dir), &self);
	return module_dir(self);
}

fs::path host_exe_dir()
{
	return module_dir(nullptr);
}

std::string host_version()
{
	std::wstring exe(MAX_PATH, L'\0');
	exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size()));

	DWORD dummy = 0;
	DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
	if (size == 0)
		return {};
	std::vector<char> data(size);
	if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
		return {};

	struct LangCodepage { WORD lang, codepage; } *langs = nullptr;
	UINT len = 0;
	if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &len) && len >= sizeof(*langs)) {
		wchar_t key[64];
		wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", langs[0].lang, langs[0].codepage);
		wchar_t *value = nullptr;
		if (VerQueryValueW(data.data(), key, (void **)&value, &len) && len > 0)
			return to_utf8(fs::path(value));
	}

	VS_FIXEDFILEINFO *fixed = nullptr;
	if (VerQueryValueW(data.data(), L"\\", (void **)&fixed, &len) && fixed) {
		char buf[64];
		wsprintfA(buf, "%u.%u.%u", HIWORD(fixed->dwProductVersionMS), LOWORD(fixed->dwProductVersionMS),
			HIWORD(fixed->dwProductVersionLS));
		return buf;
	}
	return {};
}

#else

fs::path core_dir()
{
	Dl_info info{};
	if (dladdr(reinterpret_cast<void *>(&core_dir), &info) && info.dli_fname)
		return fs::absolute(info.dli_fname).parent_path();
	return fs::current_path();
}

fs::path host_exe_dir()
{
	std::error_code ec;
	fs::path exe = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::current_path() : exe.parent_path();
}

std::string host_version()
{
	return {}; // no version resources on Linux; the log says which binary it is
}

#endif

static const auto g_start = std::chrono::steady_clock::now();

double uptime()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

} // namespace prisma
