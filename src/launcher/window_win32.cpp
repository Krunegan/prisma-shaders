// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The launcher's window on Windows: a Win32 window with a
	OpenGL context where it draws the interface of ui.cpp (Dear ImGui, Win32 and
	OpenGL 3). The file and folder dialogs are Windows' own.
*/
#include "launcher/launcher.hpp"
#include "launcher/ui.hpp"
#include "common/util.hpp"
#include "common/version.hpp"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <GL/gl.h>

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace prisma::launcher {

namespace fs = std::filesystem;

namespace {

Ui *g_ui = nullptr;
float g_scale = 1;

// ---------------------------------------------------------------- what Windows provides

std::string choose_luanti(const std::string &current)
{
	wchar_t file[MAX_PATH * 4] = L"luanti.exe";
	std::wstring dir;
	if (!current.empty())
		dir = utf8_path(current).parent_path().wstring();
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetActiveWindow();
	ofn.lpstrFilter = L"Luanti (luanti.exe)\0luanti.exe;minetest.exe\0Programas (*.exe)\0*.exe\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH * 4;
	ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
	ofn.lpstrTitle = L"Choose the game's luanti.exe (bin folder)";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? to_utf8(fs::path(file)) : std::string();
}

std::string choose_folder(const std::string &current)
{
	std::string chosen;
	IFileOpenDialog *dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		return chosen;
	DWORD options = 0;
	dlg->GetOptions(&options);
	dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dlg->SetTitle(L"Choose the shader packs folder");
	IShellItem *start = nullptr;
	std::wstring dir = utf8_path(current).wstring();
	if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
		dlg->SetFolder(start);
		start->Release();
	}
	IShellItem *item = nullptr;
	if (SUCCEEDED(dlg->Show(GetActiveWindow())) && SUCCEEDED(dlg->GetResult(&item))) {
		wchar_t *path = nullptr;
		if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
			chosen = to_utf8(fs::path(path));
			CoTaskMemFree(path);
		}
		item->Release();
	}
	dlg->Release();
	return chosen;
}

} // namespace

// The executable's version, from its resources ("5.17.0")
std::string luanti_version(const std::string &luanti)
{
	std::wstring exe = utf8_path(luanti).wstring();
	DWORD dummy = 0;
	DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
	if (size == 0)
		return {};
	std::vector<char> data(size);
	if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
		return {};
	// The numbers (5.17.0); the ProductVersion text sometimes shortens them
	UINT len = 0;
	VS_FIXEDFILEINFO *fixed = nullptr;
	if (VerQueryValueW(data.data(), L"\\", (void **)&fixed, &len) && fixed && fixed->dwProductVersionMS)
		return std::to_string(HIWORD(fixed->dwProductVersionMS)) + "." +
			std::to_string(LOWORD(fixed->dwProductVersionMS)) + "." + std::to_string(HIWORD(fixed->dwProductVersionLS));
	struct LangCodepage {
		WORD lang, codepage;
	} *langs = nullptr;
	if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &len) && len >= sizeof(*langs)) {
		wchar_t key[64];
		wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", langs[0].lang, langs[0].codepage);
		wchar_t *value = nullptr;
		if (VerQueryValueW(data.data(), key, (void **)&value, &len) && len > 0)
			return to_utf8(fs::path(value));
	}
	return {};
}

namespace {

void open_folder(const fs::path &folder)
{
	ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- the window

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp))
		return 1;
	switch (msg) {
	case WM_CLOSE:
		if (g_ui)
			g_ui->request_close();
		return 0;
	case WM_GETMINMAXINFO: {
		auto *mm = reinterpret_cast<MINMAXINFO *>(lp);
		mm->ptMinTrackSize.x = (LONG)(760 * g_scale);
		mm->ptMinTrackSize.y = (LONG)(540 * g_scale);
		return 0;
	}
	case WM_DPICHANGED: {
		const RECT *r = reinterpret_cast<RECT *>(lp);
		SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_ACTIVATE:
		// Back from the file manager: a pack may have been added by hand
		if (LOWORD(wp) != WA_INACTIVE && g_ui)
			g_ui->window_focused();
		break; // the default handling still gives the window the focus
	case WM_ERASEBKGND:
		return 1; // OpenGL paints it: no white flash
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

bool create_gl(HDC dc)
{
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	int format = ChoosePixelFormat(dc, &pfd);
	if (!format || !SetPixelFormat(dc, format, &pfd))
		return false;
	HGLRC rc = wglCreateContext(dc);
	if (!rc || !wglMakeCurrent(dc, rc))
		return false;
	using SwapInterval = BOOL(WINAPI *)(int);
	if (auto swap = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT")))
		swap(1);
	return true;
}

} // namespace

WindowResult run_window(const fs::path &prisma_dir, const fs::path &ini, Config &config)
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof wc;
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = window_proc;
	wc.hInstance = instance;
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = L"PrismaLauncher";
	RegisterClassExW(&wc);

	// Sized to the screen it opens on, and centered on it
	POINT origin{0, 0};
	HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{sizeof mi};
	GetMonitorInfoW(monitor, &mi);
	UINT dpi_x = 96, dpi_y = 96;
	using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
	if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
		if (auto get = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
			get(monitor, 0, &dpi_x, &dpi_y);
	g_scale = dpi_x / 96.f;
	const RECT &work = mi.rcWork;
	int w = std::min((int)(920 * g_scale), (int)(work.right - work.left));
	int h = std::min((int)(620 * g_scale), (int)(work.bottom - work.top));
	std::wstring title = L"Prisma " PRISMA_VERSION_W;
	HWND wnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
		work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
	// Dark title bar, like the window (Windows 10 20H1 and later)
	BOOL dark = TRUE;
	DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);

	HDC dc = GetDC(wnd);
	if (!create_gl(dc)) {
		MessageBoxW(nullptr, L"Could not open the configuration window (OpenGL).", L"Prisma", MB_ICONERROR);
		DestroyWindow(wnd);
		return WindowResult::Quit;
	}

	Platform platform;
	platform.choose_luanti = choose_luanti;
	platform.choose_folder = choose_folder;
	platform.luanti_version = luanti_version;
	platform.open_folder = open_folder;
	Ui ui(prisma_dir, ini, config, platform);
	g_ui = &ui;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ui.setup(g_scale);
	ImGui_ImplWin32_InitForOpenGL(wnd);
	ImGui_ImplOpenGL3_Init();
	ShowWindow(wnd, SW_SHOWNORMAL);
	UpdateWindow(wnd);
	SetForegroundWindow(wnd);

	// It draws while something happens and for a few frames after (what is animated
	// on mouse hover); then waits for the next message without using CPU. While
	// work goes on in the background it also wakes up on its own, to show its end.
	bool running = true;
	bool working = false;
	int idle = 0;
	while (running) {
		if (idle >= 4)
			WaitMessage();
		else if (working)
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				running = false;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			idle = 0;
		}
		if (!running)
			break;
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		running = ui.frame();
		ImGui::Render();
		RECT rc;
		GetClientRect(wnd, &rc);
		glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
		glClearColor(20 / 255.f, 22 / 255.f, 27 / 255.f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SwapBuffers(dc);
		idle++;
		working = ui.working();
		if (working)
			idle = 0;
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_ui = nullptr;
	HGLRC rc = wglGetCurrentContext();
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(rc);
	ReleaseDC(wnd, dc);
	DestroyWindow(wnd);
	UnregisterClassW(wc.lpszClassName, instance);
	CoUninitialize();
	return ui.result();
}

} // namespace prisma::launcher
