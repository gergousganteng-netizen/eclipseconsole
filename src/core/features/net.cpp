// net.cpp -- the one HTTPS GET in the tree, and the two fx_* built on it.
//
// WHY THE HOST DOES THE FETCHING AT ALL. A page cannot. WebView2 pages here
// run from a `file://` or virtual-host origin, and Roblox's endpoints send no
// Access-Control-Allow-Origin, so a fetch() from a snippet's page is
// CORS-blocked -- silently, with no useful error surfaced. Measured against
// thumbnails.roblox.com: 200 with the JSON, and no ACAO header in the reply.
// WinHTTP has no CORS layer to fight, so the request goes here and the page is
// handed the result. (An <img src> to the CDN is fine from the page -- images
// were never CORS-gated. It is only the JSON lookup that has to be host-side.)
//
// TWO EXPORTS, DELIBERATELY OPPOSITE ABOUT BLOCKING:
//
//   fx_http_get     BLOCKS. It runs on the calling snippet's own thread, so it
//                   stalls that snippet and nothing else. Simple and honest;
//                   an async general-purpose GET means handles and polling for
//                   no gain, because a snippet that wants concurrency can make
//                   the call from a thread it owns.
//   fx_avatar_url   NEVER BLOCKS. It is built to be called from inside a
//                   15ms poll loop, where a 500ms round trip is not a slow
//                   frame, it is a broken snippet. First call returns 0 and
//                   queues the id; a later call returns the cached URL.
//
// That split is the whole design. If a future fx_* fetches something, decide
// which of the two shapes it is BEFORE writing it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eclipse_core.h"
#include "fx.h"

namespace {

std::wstring widen_ascii(const char* s) {
    std::wstring w;
    for (; s && *s; s++) w += (wchar_t)(unsigned char)*s;
    return w;
}

}  // namespace

// The shared primitive. ui/client/placename.cpp's http_get is a wrapper over
// this -- one implementation, so a proxy or timeout fix lands everywhere at
// once rather than in whichever copy the fixer happened to open.
bool core_http_get(const char* host, const char* path, std::string& out) {
    out.clear();
    if (!host || !path) return false;
    std::wstring wh = widen_ascii(host), wp = widen_ascii(path);

    HINTERNET sess = WinHttpOpen(L"eclipse-console/1.0",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;
    // Bounded on purpose. The default is minutes, and a snippet blocked in
    // fx_http_get behind a black-holed connection looks exactly like a hang.
    WinHttpSetTimeouts(sess, 5000, 5000, 10000, 10000);

    HINTERNET conn = WinHttpConnect(sess, wh.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", wp.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (req &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        ok = true;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
            out.append(buf.data(), read);
        }
    }
    if (req) WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return ok;
}

FX_API int fx_http_get(const char* host, const char* path, char* buf, size_t n) {
    if (!buf || n == 0) return 0;
    buf[0] = 0;
    std::string body;
    if (!core_http_get(host, path, body)) return 0;
    size_t take = body.size() < n - 1 ? body.size() : n - 1;
    memcpy(buf, body.data(), take);
    buf[take] = 0;
    // The FULL length, not what fitted, so a caller can tell a truncated reply
    // from a complete one and grow its buffer rather than parsing a fragment.
    return (int)body.size();
}

// ---- avatar headshots ------------------------------------------------------
//
// Roblox's thumbnails endpoint answers a BATCH: userIds= takes a comma list,
// so a whole server is one request. That is the entire optimisation -- with it
// a 12-player roster costs 1 request against a measured budget of 120/min
// (x-ratelimit-limit: 120, 120;w=60), so nothing else here is load-bearing for
// rate. Caching is for latency and for not re-asking across restarts.
//
// PENDING IS NOT FAILURE. An avatar Roblox has not rendered yet comes back
// {"state":"Pending"} with no imageUrl, and is ready seconds later. Caching
// that as "no avatar" would leave a player permanently blank -- exactly the
// silent-and-sticky failure this codebase keeps producing. It is retried, with
// a cap so a genuinely dead id stops costing requests.

namespace {

const char*  kThumbHost   = "thumbnails.roblox.com";
const int    kBatchMax    = 100;      // endpoint's own cap on userIds=
const int    kPendingTries= 6;        // ~6 x 700ms before an id is given up on
const double kTtlDays     = 25.0;     // URLs are marked 30DAY-; re-ask before

struct Entry {
    std::string url;
    int64_t     at    = 0;    // unix seconds, for the TTL
    int         tries = 0;    // Pending retries spent
};

std::mutex               g_mx;
std::map<int64_t, Entry> g_cache;
std::set<int64_t>        g_want;      // queued, not yet resolved
bool                     g_worker = false;
bool                     g_loaded = false;
bool                     g_dirty  = false;

int64_t now_secs() { return (int64_t)time(nullptr); }

std::string cache_path() {
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    std::string dir = (n && n < MAX_PATH) ? std::string(appdata) + "\\EclipseConsole"
                                          : ec::self_dir();
    return dir + "\\avatars.txt";
}

// Sibling of settings.conf, same shape of file: one record per line, ordered
// by id so a rewrite is stable. Losing it costs one batch request, so nothing
// here tries very hard.
void cache_load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(cache_path());
    if (!f) return;
    std::string line;
    int64_t cutoff = now_secs() - (int64_t)(kTtlDays * 86400);
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        int64_t id = 0, at = 0;
        std::string url;
        if (!(ls >> id >> at >> url) || id == 0) continue;
        if (at < cutoff) continue;               // expired: re-ask instead
        Entry e; e.url = url; e.at = at;
        g_cache[id] = e;
    }
}

void cache_save_locked() {
    if (!g_dirty) return;
    g_dirty = false;
    std::ofstream f(cache_path(), std::ios::trunc);
    if (!f) return;
    for (const auto& kv : g_cache)
        if (!kv.second.url.empty())
            f << kv.first << " " << kv.second.at << " " << kv.second.url << "\n";
}

// Enough of a JSON reader for one known reply shape, which is the whole
// requirement -- a parser would be more code than the thing it parses:
//   {"data":[{"targetId":1,"state":"Completed","imageUrl":"https://..."}]}
void parse_batch(const std::string& body, std::map<int64_t, std::string>& done,
                 std::set<int64_t>& pending) {
    size_t p = 0;
    while ((p = body.find("\"targetId\"", p)) != std::string::npos) {
        size_t colon = body.find(':', p);
        if (colon == std::string::npos) break;
        int64_t id = _atoi64(body.c_str() + colon + 1);
        // The record ends at the next targetId or at the end -- everything
        // between belongs to this one.
        size_t next = body.find("\"targetId\"", colon);
        std::string rec = body.substr(colon, next == std::string::npos
                                                 ? std::string::npos : next - colon);
        size_t u = rec.find("\"imageUrl\":\"");
        if (u != std::string::npos) {
            size_t s = u + 12, e = rec.find('"', s);
            if (e != std::string::npos && e > s) {
                done[id] = rec.substr(s, e - s);
                p = colon + 1;
                continue;
            }
        }
        if (rec.find("\"state\":\"Pending\"") != std::string::npos) pending.insert(id);
        p = colon + 1;
    }
}

void worker() {
    for (;;) {
        // Coalesce. fx_avatar_url is called per player per loop, so the ids
        // for a whole roster arrive within microseconds of each other; a short
        // wait turns that burst into the ONE request the endpoint is built for.
        Sleep(700);

        std::vector<int64_t> take;
        {
            std::lock_guard<std::mutex> g(g_mx);
            for (int64_t id : g_want) {
                if ((int)take.size() >= kBatchMax) break;
                take.push_back(id);
            }
            if (take.empty()) { g_worker = false; cache_save_locked(); return; }
        }

        std::string ids;
        for (size_t i = 0; i < take.size(); i++) {
            if (i) ids += ",";
            char b[24]; snprintf(b, sizeof b, "%lld", (long long)take[i]);
            ids += b;
        }
        std::string path = "/v1/users/avatar-headshot?userIds=" + ids +
                           "&size=48x48&format=Png&isCircular=true";

        std::string body;
        bool ok = core_http_get(kThumbHost, path.c_str(), body);

        std::map<int64_t, std::string> done;
        std::set<int64_t>              pending;
        if (ok) parse_batch(body, done, pending);

        std::lock_guard<std::mutex> g(g_mx);
        for (const auto& kv : done) {
            Entry e; e.url = kv.second; e.at = now_secs();
            g_cache[kv.first] = e;
            g_want.erase(kv.first);
            g_dirty = true;
        }
        for (int64_t id : take) {
            if (done.count(id)) continue;
            // Pending, or the request failed outright: both are "ask again",
            // and both are capped so a dead id cannot cost requests forever.
            Entry& e = g_cache[id];
            if (++e.tries >= kPendingTries) g_want.erase(id);
        }
        cache_save_locked();
    }
}

}  // namespace

FX_API int fx_avatar_url(int64_t userid, char* buf, size_t n) {
    if (!buf || n == 0) return 0;
    buf[0] = 0;
    if (userid <= 0) return 0;

    std::lock_guard<std::mutex> g(g_mx);
    cache_load();

    auto it = g_cache.find(userid);
    if (it != g_cache.end() && !it->second.url.empty()) {
        const std::string& u = it->second.url;
        if (u.size() >= n) return 0;             // caller's buffer is too small
        memcpy(buf, u.data(), u.size());
        buf[u.size()] = 0;
        return (int)u.size();
    }
    if (it != g_cache.end() && it->second.tries >= kPendingTries) return 0;  // given up

    g_want.insert(userid);
    if (!g_worker) {
        g_worker = true;
        // Detached: it exits by itself once the queue drains, so there is no
        // lifetime to manage and no shutdown ordering to get wrong.
        std::thread(worker).detach();
    }
    return 0;
}
