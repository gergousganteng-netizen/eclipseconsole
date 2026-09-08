// Stage K -- do string-keyed CONTAINER NODES exist in this client at all?
//
// stage_j searched forward from Instances and found nothing: no slot in the
// first 0x400 bytes pointed at an identifier held at a consistent sub-offset.
// That is a real result, and it says something specific -- if the attribute
// store were a struct with the key inline, stage_j would have seen it.
//
// The likely reason it did not: an MSVC std::map<std::string, V> puts NOTHING
// near the header. The Instance would hold only { _Myhead, _Mysize }, the keys
// living in tree NODES one or more hops away, at addresses that differ per
// instance. No forward scan from the Instance can see that.
//
// So this stage searches for the NODES instead, straight out of the heap, and
// asks a question that comes BEFORE the offset hunt:
//
//     does this place contain string-keyed container nodes whose keys look
//     like a developer wrote them?
//
// If yes, attributes (or something shaped like them) are here and the backwards
// walk to an Instance offset is worth building. If every key we find is engine
// vocabulary, then either this place sets no attributes -- in which case NO
// amount of searching here can succeed, and the hunt needs a place where we
// KNOW one exists -- or the store is not a std::map and this signature is the
// wrong one. Both are worth knowing before writing another search.
//
// THE SIGNATURE. An MSVC _Tree_node is:
//     +0x00 _Left    +0x08 _Parent   +0x10 _Right     (all pointers)
//     +0x18 _Color, _Isnil (2 bytes, then padding)
//     +0x20 _Myval  -- for a map, the pair: key first
// so "three pointers, then a std::string at +0x20" is a specific shape. The
// string decoder is the same three-field test stage_a used, and it is carried
// here with its controls rather than trusted.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

static void L(const char* fmt, ...) {
    char b[700];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

static bool heapish(uint64_t p) {
    return p >= 0x10000ull && p < 0x7ff000000000ull;
}

// Inline (SSO) strings only here, deliberately. A heap-backed string needs a
// second read per candidate and this runs over gigabytes; attribute names are
// short in practice, and restricting to <=15 chars costs recall we can afford
// while keeping the sweep to one pass.
static bool decode_sso(const unsigned char* b, std::string& out) {
    uint64_t size, cap;
    memcpy(&size, b + 16, 8);
    memcpy(&cap,  b + 24, 8);
    if (cap != 15 || size == 0 || size > 15) return false;
    for (uint64_t i = 0; i < size; i++)
        if (b[i] < 0x20 || b[i] > 0x7e) return false;
    if (b[size] != 0) return false;
    out.assign((const char*)b, (size_t)size);
    return true;
}

static bool identifier_like(const std::string& s) {
    if (s.size() < 3) return false;
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// The node test. `key_off` is a parameter so the CONTROL can run the identical
// search at an offset the shape says is wrong and show it finds far less --
// otherwise "we found some strings" proves nothing about the signature.
static bool node_at(const unsigned char* b, size_t key_off, std::string& out) {
    uint64_t l, p, r;
    memcpy(&l, b, 8); memcpy(&p, b + 8, 8); memcpy(&r, b + 16, 8);
    if (!heapish(l) || !heapish(p) || !heapish(r)) return false;
    return decode_sso(b + key_off, out);
}

extern "C" void fx_run() {
    if (!fx_dm()) { L("no DataModel -- attach and join a place first"); return; }

    int nreg = fx_regions(nullptr, nullptr, 0);
    if (nreg <= 0) { L("no regions"); return; }
    std::vector<uint64_t> base(nreg), size(nreg);
    nreg = fx_regions(base.data(), size.data(), nreg);
    uint64_t total = 0;
    for (int i = 0; i < nreg; i++) total += size[i];
    L("%d committed regions, %.2f GB", nreg, total / 1073741824.0);

    // ---- CONTROL: the decoder must accept a known-good string --------------
    // Build one by hand in the shape MSVC uses, so a broken decoder is caught
    // before gigabytes are swept with it.
    {
        unsigned char probe[32] = {0};
        memcpy(probe, "TestAttr", 8);
        uint64_t sz = 8, cap = 15;
        memcpy(probe + 16, &sz, 8);
        memcpy(probe + 24, &cap, 8);
        std::string s;
        bool good = decode_sso(probe, s) && s == "TestAttr";
        // and a known-bad one: same bytes, impossible capacity
        uint64_t bad = 9;
        memcpy(probe + 24, &bad, 8);
        std::string s2;
        bool rejected = !decode_sso(probe, s2);
        L("CONTROL  decoder accepts a valid SSO string: %s   rejects a bad one: %s",
          good ? "yes" : "NO", rejected ? "yes" : "NO");
        if (!good || !rejected) { L("  instrument is broken -- stopping"); return; }
    }

    const size_t CHUNK = 1u << 20;
    std::vector<unsigned char> buf(CHUNK);
    std::unordered_map<std::string, int> keys, ctrl_keys;
    uint64_t swept = 0, nodes = 0, ctrl_nodes = 0;

    for (int i = 0; i < nreg && fx_running(); i++) {
        uint64_t pos = base[i], end = base[i] + size[i];
        while (pos < end && fx_running()) {
            size_t want = (size_t)((end - pos < CHUNK) ? (end - pos) : CHUNK);
            size_t got  = fx_rdblk(pos, buf.data(), want);
            if (got >= 64) {
                for (size_t o = 0; o + 64 <= got; o += 8) {
                    std::string s;
                    // the real signature: key at +0x20
                    if (node_at(buf.data() + o, 0x20, s) && identifier_like(s)) {
                        nodes++;
                        if (keys.size() < 20000) keys[s]++;
                    }
                    // the control: same three pointers, key where the shape
                    // says there is none
                    if (node_at(buf.data() + o, 0x18, s) && identifier_like(s)) {
                        ctrl_nodes++;
                        if (ctrl_keys.size() < 20000) ctrl_keys[s]++;
                    }
                }
                swept += got;
            }
            if (want <= 64) break;
            pos += want - 64;
        }
    }

    L("swept %.2f GB", swept / 1073741824.0);
    L("SIGNATURE  key at +0x20: %llu nodes, %d distinct keys",
      (unsigned long long)nodes, (int)keys.size());
    L("CONTROL    key at +0x18: %llu nodes, %d distinct keys  %s",
      (unsigned long long)ctrl_nodes, (int)ctrl_keys.size(),
      nodes > ctrl_nodes * 2 ? "-> the shape is doing real work"
                             : "-> NO BETTER THAN THE WRONG OFFSET, signature is meaningless");

    std::vector<std::pair<std::string,int>> v(keys.begin(), keys.end());
    std::sort(v.begin(), v.end(), [](const std::pair<std::string,int>& a,
                                     const std::pair<std::string,int>& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    L(" ");
    L("most common string keys found in node-shaped memory:");
    for (int i = 0; i < 60 && i < (int)v.size(); i++)
        L("  %-5d %s", v[i].second, v[i].first.c_str());

    L(" ");
    L("HOW TO READ THIS: engine vocabulary (service names, MemoryCategory_*,");
    L("job names) means these are the client's own maps and tells us nothing");
    L("about attributes. GAME vocabulary -- names that match what this place");
    L("is about -- would mean attributes are here and the backwards walk to an");
    L("Instance offset is worth building.");
}
