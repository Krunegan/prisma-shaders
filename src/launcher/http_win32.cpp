// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
// https_get() on Windows, with WinHTTP (part of the system: nothing to ship)
#include "launcher/downloads.hpp"
#include "common/version.hpp"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace prisma::launcher::downloads {

static std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

static std::string last_error(const char *what)
{
	return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

// One session for the whole program, kept open: WinHTTP keeps its
// connections alive between requests, and finds the proxy once. A session
// per request made every file of a pack a new connection and TLS handshake.
static HINTERNET session()
{
	static HINTERNET s = [] {
		std::wstring agent = widen("Prisma/" PRISMA_VERSION);
		HINTERNET h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (h) {
			WinHttpSetTimeouts(h, 15000, 15000, 30000, 60000);
			// Only TLS 1.2 and newer
			DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
			protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
			WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
		}
		return h;
	}();
	return s;
}

bool https_get(const std::string &url, const std::vector<std::string> &headers, std::size_t max_bytes,
	std::string &body, std::string &error)
{
	body.clear();
	std::wstring wurl = widen(url);
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof parts;
	wchar_t host[256] = {}, path[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = (DWORD)std::size(host);
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = (DWORD)std::size(path);
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
		error = "not an https address: " + url;
		return false;
	}

	HINTERNET shared = session();
	if (!shared) {
		error = last_error("WinHttpOpen");
		return false;
	}
	HINTERNET connection = WinHttpConnect(shared, host, parts.nPort, 0);
	HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
										 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
								   : nullptr;
	auto close = [&] {
		if (request)
			WinHttpCloseHandle(request);
		if (connection)
			WinHttpCloseHandle(connection);
	};
	if (!request) {
		error = last_error("connecting to the server");
		close();
		return false;
	}
	std::wstring extra;
	for (const std::string &h : headers)
		extra += widen(h) + L"\r\n";
	if (!WinHttpSendRequest(request, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
			extra.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request, nullptr)) {
		error = last_error("the request");
		close();
		return false;
	}
	DWORD status = 0, size = sizeof status;
	WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
		&status, &size, WINHTTP_NO_HEADER_INDEX);
	if (status != 200) {
		error = "the server answered " + std::to_string(status);
		close();
		return false;
	}
	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			error = last_error("reading");
			close();
			return false;
		}
		if (available == 0)
			break;
		if (body.size() + available > max_bytes) {
			error = "the answer is too big";
			close();
			return false;
		}
		std::size_t at = body.size();
		body.resize(at + available);
		DWORD read = 0;
		if (!WinHttpReadData(request, body.data() + at, available, &read)) {
			error = last_error("reading");
			close();
			return false;
		}
		body.resize(at + read);
	}
	close();
	return true;
}

} // namespace prisma::launcher::downloads
