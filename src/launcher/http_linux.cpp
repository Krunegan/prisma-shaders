// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
// https_get() on Linux, with the system's libcurl (Luanti itself needs it),
// opened when it is first used: Prisma does not depend on it to start.
#include "launcher/downloads.hpp"
#include "common/version.hpp"

#include <dlfcn.h>

#include <string>
#include <type_traits>
#include <vector>

namespace prisma::launcher::downloads {

// The few parts of curl.h used here, with their stable values
namespace curl {
using Handle = void;
struct Slist;
constexpr int OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_USERAGENT = 10018, OPT_HTTPHEADER = 10023,
			  OPT_WRITEFUNCTION = 20011, OPT_TIMEOUT = 13, OPT_FOLLOWLOCATION = 52, OPT_CONNECTTIMEOUT = 78,
			  OPT_NOSIGNAL = 99, OPT_PROTOCOLS_STR = 10318, OPT_REDIR_PROTOCOLS_STR = 10319;
constexpr int INFO_RESPONSE_CODE = 0x200000 + 2;
} // namespace curl

struct Curl {
	void *lib = nullptr;
	curl::Handle *(*easy_init)() = nullptr;
	int (*easy_setopt)(curl::Handle *, int, ...) = nullptr;
	int (*easy_perform)(curl::Handle *) = nullptr;
	int (*easy_getinfo)(curl::Handle *, int, ...) = nullptr;
	void (*easy_cleanup)(curl::Handle *) = nullptr;
	void (*easy_reset)(curl::Handle *) = nullptr; // optional
	const char *(*easy_strerror)(int) = nullptr;
	curl::Slist *(*slist_append)(curl::Slist *, const char *) = nullptr;
	void (*slist_free_all)(curl::Slist *) = nullptr;
};

static const Curl *load_curl()
{
	static Curl c;
	static bool tried = false;
	if (tried)
		return c.lib ? &c : nullptr;
	tried = true;
	for (const char *name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"})
		if ((c.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
			break;
	if (!c.lib)
		return nullptr;
	auto sym = [&](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(c.lib, name)); };
	sym(c.easy_init, "curl_easy_init");
	sym(c.easy_setopt, "curl_easy_setopt");
	sym(c.easy_perform, "curl_easy_perform");
	sym(c.easy_getinfo, "curl_easy_getinfo");
	sym(c.easy_cleanup, "curl_easy_cleanup");
	sym(c.easy_reset, "curl_easy_reset");
	sym(c.easy_strerror, "curl_easy_strerror");
	sym(c.slist_append, "curl_slist_append");
	sym(c.slist_free_all, "curl_slist_free_all");
	if (!c.easy_init || !c.easy_setopt || !c.easy_perform || !c.easy_getinfo || !c.easy_cleanup ||
		!c.easy_strerror || !c.slist_append || !c.slist_free_all) {
		dlclose(c.lib);
		c.lib = nullptr;
		return nullptr;
	}
	return &c;
}

struct Sink {
	std::string *body;
	std::size_t max;
	bool too_big = false;
};

static std::size_t on_data(char *data, std::size_t size, std::size_t count, void *user)
{
	Sink *s = static_cast<Sink *>(user);
	std::size_t n = size * count;
	if (s->body->size() + n > s->max) {
		s->too_big = true;
		return 0; // curl stops
	}
	s->body->append(data, n);
	return n;
}

// One handle per thread, kept between requests: curl keeps a handle's
// connections alive, so the files of a pack, fetched one after another,
// share one connection and one TLS handshake
struct KeptHandle {
	curl::Handle *h = nullptr;
	~KeptHandle()
	{
		if (const Curl *c = h ? load_curl() : nullptr)
			c->easy_cleanup(h);
	}
};

bool https_get(const std::string &url, const std::vector<std::string> &headers, std::size_t max_bytes,
	std::string &body, std::string &error)
{
	body.clear();
	if (url.rfind("https://", 0) != 0) {
		error = "not an https address: " + url;
		return false;
	}
	const Curl *c = load_curl();
	if (!c) {
		error = "libcurl was not found (install the libcurl4 package)";
		return false;
	}
	static thread_local KeptHandle kept;
	curl::Handle *h = nullptr;
	if (c->easy_reset && kept.h) {
		c->easy_reset(kept.h); // the options go, the connections stay
		h = kept.h;
	} else {
		h = c->easy_init();
		if (c->easy_reset)
			kept.h = h;
	}
	if (!h) {
		error = "curl_easy_init failed";
		return false;
	}
	curl::Slist *list = nullptr;
	for (const std::string &line : headers)
		list = c->slist_append(list, line.c_str());
	Sink sink{&body, max_bytes};
	c->easy_setopt(h, curl::OPT_URL, url.c_str());
	c->easy_setopt(h, curl::OPT_USERAGENT, "Prisma/" PRISMA_VERSION);
	c->easy_setopt(h, curl::OPT_NOSIGNAL, 1L);
	c->easy_setopt(h, curl::OPT_FOLLOWLOCATION, 1L);
	// https only, also after a redirect (older curl ignores these: the URL is https anyway)
	c->easy_setopt(h, curl::OPT_PROTOCOLS_STR, "https");
	c->easy_setopt(h, curl::OPT_REDIR_PROTOCOLS_STR, "https");
	c->easy_setopt(h, curl::OPT_CONNECTTIMEOUT, 15L);
	c->easy_setopt(h, curl::OPT_TIMEOUT, 120L);
	if (list)
		c->easy_setopt(h, curl::OPT_HTTPHEADER, list);
	c->easy_setopt(h, curl::OPT_WRITEFUNCTION, &on_data);
	c->easy_setopt(h, curl::OPT_WRITEDATA, &sink);
	int result = c->easy_perform(h);
	long status = 0;
	c->easy_getinfo(h, curl::INFO_RESPONSE_CODE, &status);
	if (h != kept.h)
		c->easy_cleanup(h);
	if (list)
		c->slist_free_all(list);
	if (sink.too_big) {
		error = "the answer is too big";
		return false;
	}
	if (result != 0) {
		error = c->easy_strerror(result);
		return false;
	}
	if (status != 200) {
		error = "the server answered " + std::to_string(status);
		return false;
	}
	return true;
}

} // namespace prisma::launcher::downloads
