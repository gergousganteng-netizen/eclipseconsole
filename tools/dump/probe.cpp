// Feed the round-trip test the DataModel the console already resolves. If it
// says "not an Instance" for that, the test is wrong, not the search.
#include <cstdarg>
#include <cstdio>
#include <cstring>

static void L(const char* fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fx_log(b);
}

extern "C" void fx_run() {
    uint64_t dm = fx_dm();
    L("dm = %llx", (unsigned long long)dm);

    uint64_t cd = fx_off("Instance.ClassDescriptor");
    uint64_t cn = fx_off("Instance.ClassName");
    uint64_t cs = fx_off("Instance.ChildrenStart");
    uint64_t ce = fx_off("Instance.ChildrenEnd");
    uint64_t pa = fx_off("Instance.Parent");
    L("conf: desc +%llx  name +%llx  children +%llx  end +%llx  parent +%llx",
      (unsigned long long)cd, (unsigned long long)cn, (unsigned long long)cs,
      (unsigned long long)ce, (unsigned long long)pa);

    uint64_t desc = fx_rd(dm + cd);
    L("desc          = %llx", (unsigned long long)desc);
    L("desc + name   = %llx", (unsigned long long)(desc + cn));

    char s[32] = {};
    fx_rdblk(desc + cn, s, 16);
    uint64_t len = fx_rd(desc + cn + 0x10), cap = fx_rd(desc + cn + 0x18);
    L("string bytes  = \"%s\"  len %llu  cap %llu", s,
      (unsigned long long)len, (unsigned long long)cap);

    uint64_t cont  = fx_rd(dm + cs);
    uint64_t begin = fx_rd(cont);
    uint64_t end   = fx_rd(cont + ce);
    L("container %llx  begin %llx  end %llx  span %llu  /0x10 = %llu",
      (unsigned long long)cont, (unsigned long long)begin,
      (unsigned long long)end, (unsigned long long)(end - begin),
      (unsigned long long)((end - begin) / 0x10));

    for (int i = 0; i < 4; i++) {
        uint64_t e = fx_rd(begin + (uint64_t)i * 0x10);
        L("  child[%d] = %llx   parent field = %llx   %s", i,
          (unsigned long long)e, (unsigned long long)fx_rd(e + pa),
          fx_rd(e + pa) == dm ? "points back" : "DOES NOT point back");
    }
    L("dm parent = %llx", (unsigned long long)fx_rd(dm + pa));
}
