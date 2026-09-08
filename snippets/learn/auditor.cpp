// auditor.cpp -- does the conf contradict itself?
//
// Needs an attached client: the offset table is loaded by attach(), so a
// detached run has nothing to audit (and run_snippet refuses anyway).
//
// Two keys on the same class pointing at the same offset means at least one of
// them is wrong. Found the hard way on version-ddf602d9cfe44005:
// Humanoid.UseJumpPower and PlatformStand both read 0xc5, which is not a flag
// at all -- it is inside DisplayName's inline std::string buffer (0xb8..0xc7),
// so it returned a letter of the player's username. Reading it as a bool
// answered "true" every time, for a reason that had nothing to do with jumping.
// The dump published for version-17d504d2c9544583 moved both out (0x1e0 and
// 0x1dc), which is exactly the kind of change this snippet exists to notice.

// ---- known-different-BASE keys, which are not bugs --------------------------
//
// The conf flattens several different base pointers into one Class.Field
// namespace, so keys that are measured from DIFFERENT objects land on the same
// number and look like collisions. Instance is the whole problem: Name is
// (NameContainer + Name), ChildrenEnd is (children container + End), ClassName
// is (ClassDescriptor + ClassName). Four keys at 0x8, six pairs, zero bugs.
//
// SUPPRESSED BY NAME, DELIBERATELY, rather than by a clever rule. A rule
// ("anything whose name implies indirection") would silently swallow a real
// collision the day someone adds a field this list never anticipated. Each
// entry here is a decision somebody wrote down and can be argued with.
//
// The cost of getting this wrong is not a missed row -- it is the whole tool.
// An auditor whose output is more than half known-good noise is one nobody
// reads, and then it misses the row that mattered.
static const char* SAME_OFFSET_OK[] = {
    "Instance.ChildrenEnd", "Instance.ClassName", "Instance.Name", "Instance.This",
};

auto base_differs = [&](const char* a, const char* b) {
    bool ha = false, hb = false;
    for (const char* k : SAME_OFFSET_OK) {
        if (strcmp(a, k) == 0) ha = true;
        if (strcmp(b, k) == 0) hb = true;
    }
    return ha && hb;      // BOTH listed: the pair is explained, not the key
};

int n = fx_off_count();
int hits = 0, muted = 0;
fx_logf("%d keys in the conf", n);

for (int i = 0; i < n; i++) {
    const char* ki = fx_off_key(i);
    if (!ki) continue;

    const char* dot = strchr(ki, '.');
    if (!dot) continue;
    size_t   clen = dot - ki;            // length of the class name
    uint64_t oi   = fx_off(ki);
    if (oi == (uint64_t)-1) continue;    // no value; nothing to collide with

    // j starts at i+1, not 0. Every collision exists twice -- i finds j, then
    // j finds i -- and starting past i reports each pair exactly once.
    for (int j = i + 1; j < n; j++) {
        const char* kj = fx_off_key(j);
        if (!kj) continue;
        const char* dotj = strchr(kj, '.');
        if (!dotj) continue;

        // Length FIRST, then the compare. strncmp(ki, kj, clen) alone would
        // match "Humanoid" against "HumanoidDescription" -- the first 8
        // characters are identical and strncmp only looks at 8.
        if ((size_t)(dotj - kj) != clen) continue;
        if (strncmp(ki, kj, clen) != 0)  continue;
        if (fx_off(kj) != oi)            continue;

        if (base_differs(ki, kj)) { muted++; continue; }

        fx_logf("  0x%-6llx  %s  <->  %s", (unsigned long long)oi, ki, kj);
        hits++;
    }
}

// Both numbers, always. The count alone cannot be told apart from matching that
// silently failed, and the muted count is what stops the suppression list
// quietly growing into "no collisions ever found".
fx_logf("%d colliding pair(s), %d muted as known-different-base", hits, muted);
