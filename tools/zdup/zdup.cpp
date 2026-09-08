// zdup -- the presented-frame probe. KEPT, not throwaway: it is the only thing
// in this tree that can say what was actually composed, rather than what this
// process asked for. See README.md beside it before using or trusting it --
// particularly the three traps, which are the difference between a zero that
// means "no fault" and a zero that means "aimed at nothing".
//
// z-walk sampler + DXGI desktop duplication (presented frames) + SendInput
// gesture driver, all stamped with GetTickCount64 % 1e8 to line up with
// eclipse_z.log (run the console with ECLIPSE_ZLOG=1).
//
// Build: g++ -O2 -std=c++17 zdup.cpp -o zdup.exe -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -lole32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

static std::atomic<bool> g_stop{false};
static HWND g_app = 0, g_hud = 0, g_game = 0, g_probe = 0;
static DWORD g_conpid = 0, g_gamepid = 0;
static FILE *g_fz = 0, *g_fp = 0, *g_fg = 0;
static int g_ax = 323, g_ay = 296, g_aw = 1180, g_ah = 780;   // the app's remembered rect moved 2026-09-05
static int g_bx = 1864, g_by = 507, g_bw = 44, g_bh = 66;
static int g_dx = 840, g_dy = 465, g_dw = 240, g_dh = 150;   // docktest, centred

static unsigned long long tick() { return GetTickCount64() % 100000000ull; }
static double qms() {
    static LARGE_INTEGER f{}, s{};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s); }
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - s.QuadPart) * 1000.0 / f.QuadPart;
}
static DWORD pid_of(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof pe }; DWORD out = 0;
    if (Process32FirstW(snap, &pe)) do { if (!_wcsicmp(pe.szExeFile, exe)) { out = pe.th32ProcessID; break; } } while (Process32NextW(snap, &pe));
    CloseHandle(snap); return out;
}
struct Find { DWORD pid; const wchar_t* cls; const wchar_t* title; bool want_cls; HWND best; long area; };
static BOOL CALLBACK find_cb(HWND h, LPARAM lp) {
    Find* f = (Find*)lp; DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p != f->pid) return TRUE;
    wchar_t cls[64] = L""; GetClassNameW(h, cls, 64); wchar_t t[64] = L""; GetWindowTextW(h, t, 64);
    if (f->cls && ((wcscmp(cls, f->cls) == 0) != f->want_cls)) return TRUE;
    if (f->title && wcscmp(t, f->title) != 0) return TRUE;
    // owner not checked: the overlay host is client-owned while a third app is foreground
    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a >= f->area) { f->area = a; f->best = h; }
    return TRUE;
}
static HWND find_win(DWORD pid, const wchar_t* cls, bool want_cls, const wchar_t* title) {
    Find f{ pid, cls, title, want_cls, 0, -1 }; EnumWindows(find_cb, (LPARAM)&f); return f.best;
}
static bool resolve() {
    g_conpid = pid_of(L"eclipse_console.exe"); g_gamepid = pid_of(L"RobloxPlayerBeta.exe");
    if (!g_conpid || !g_gamepid) return false;
    g_game = find_win(g_gamepid, L"WINDOWSCLIENT", true, nullptr);
    g_hud  = find_win(g_conpid, L"EclipseHudHost", true, nullptr);
    g_app  = find_win(g_conpid, L"EclipseHudHost", false, L"Eclipse");
    return g_game && g_hud && g_app;
}
static char fg_class(HWND fg) {
    if (!fg) return '0';
    HWND r = GetAncestor(fg, GA_ROOT);
    if (r == g_app) return 'A'; if (r == g_hud) return 'H'; if (r == g_game) return 'G'; if (r == g_probe) return 'P';
    DWORD p = 0; GetWindowThreadProcessId(fg, &p);
    if (p == g_conpid) return 'c'; if (p == g_gamepid) return 'g'; return 'X';
}
static void zwalk_thread() {
    std::string last; double last_at = qms(); unsigned long long n = 0;
    while (!g_stop) {
        std::string order; bool appvis = IsWindowVisible(g_app) != 0;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (h == g_app && appvis) order += 'A'; else if (h == g_hud) order += 'H'; else if (h == g_game) order += 'G';
            if (order.size() == 3) break;
        }
        char fgc = fg_class(GetForegroundWindow());
        int at = (GetWindowLongW(g_app, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
        int ht = (GetWindowLongW(g_hud, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
        HWND nx = GetWindow(g_app, GW_HWNDNEXT);
        char buf[160];
        snprintf(buf, sizeof buf, "%s fg=%c atop=%d htop=%d nextIsHud=%d", order.c_str(), fgc, at, ht, nx == g_hud ? 1 : 0);
        n++;
        if (buf != last) {
            double now = qms();
            fprintf(g_fz, "t%llu %10.2f  %s   (prev held %.2fms, %llu samples)\n", tick(), now, buf, now - last_at, n);
            fflush(g_fz); last = buf; last_at = now; n = 0;
        }
        if (!IsWindow(g_app) || !IsWindow(g_hud)) { fprintf(g_fz, "t%llu window gone\n", tick()); fflush(g_fz); break; }
    }
}
struct Pt { int x, y; const char* name; };
static std::vector<Pt> g_pts;
static void mark(const char* what) { fprintf(g_fg, "t%llu %10.2f  %s\n", tick(), qms(), what); fflush(g_fg); }

// Sample points derived LIVE (2026-09-05): the app rect from the window while it
// is visible (seeded from overlay_app.txt, client-relative "l t w h", for the
// first travel), the dock and badge from the client's client area every frame.
static void refresh_points(bool log) {
    static bool seeded = false;
    RECT c{}; POINT o{0, 0};
    if (!GetClientRect(g_game, &c) || !ClientToScreen(g_game, &o)) return;
    int vw = c.right, vh = c.bottom;
    if (!seeded) {
        seeded = true;
        std::string p = getenv("APPDATA") ? std::string(getenv("APPDATA")) + "/EclipseConsole/overlay_app.txt" : "";
        FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
        int l, t, w, h;
        if (f && fscanf(f, "%d %d %d %d", &l, &t, &w, &h) == 4) {
            g_ax = o.x + l; g_ay = o.y + t; g_aw = w; g_ah = h;
            if (log) { char b[96]; snprintf(b, sizeof b, "app rect seeded %d,%d %dx%d", g_ax, g_ay, g_aw, g_ah); mark(b); }
        }
        if (f) fclose(f);
    }
    RECT r{};
    if (IsWindowVisible(g_app) && GetWindowRect(g_app, &r) && r.right - r.left > 300) {
        if (r.left != g_ax || r.top != g_ay || r.right - r.left != g_aw) {
            g_ax = r.left; g_ay = r.top; g_aw = r.right - r.left; g_ah = r.bottom - r.top;
            if (log) { char b[96]; snprintf(b, sizeof b, "app rect now %d,%d %dx%d", g_ax, g_ay, g_aw, g_ah); mark(b); }
        }
    }
    int dx = o.x + (vw - 240) / 2, dy = o.y + (vh - 150) / 2;
    int bx = o.x + vw - 12 - 44, by = o.y + vh / 2 - 33;
    if (dx != g_dx || dy != g_dy || bx != g_bx || by != g_by) {
        g_dx = dx; g_dy = dy; g_bx = bx; g_by = by;
        if (log) { char b[96]; snprintf(b, sizeof b, "dock at %d,%d badge at %d,%d (client %dx%d @%d,%d)", dx, dy, bx, by, vw, vh, o.x, o.y); mark(b); }
    }
    if (g_pts.size() >= 8) {
        g_pts[0].x = g_ax + 300; g_pts[0].y = g_ay + 14; g_pts[1].x = g_ax + g_aw - 300; g_pts[1].y = g_ay + 14;
        g_pts[2].x = g_ax + 40; g_pts[2].y = g_ay + 300;
        g_pts[3].x = g_dx + 20; g_pts[3].y = g_dy + 20; g_pts[4].x = g_dx + g_dw - 20; g_pts[4].y = g_dy + g_dh - 20;
        g_pts[5].x = g_bx + g_bw / 2; g_pts[5].y = g_by + g_bh / 2;
    }
}
static void dup_thread() {
    ID3D11Device* dev = 0; ID3D11DeviceContext* ctx = 0; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) { fprintf(g_fp, "no device\n"); return; }
    IDXGIDevice* dxdev = 0; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    IDXGIAdapter* ad = 0; dxdev->GetAdapter(&ad); IDXGIOutput* out = 0; ad->EnumOutputs(0, &out);
    IDXGIOutput1* out1 = 0; out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = (UINT)g_pts.size(); sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stag = 0; dev->CreateTexture2D(&sd, nullptr, &stag);
    IDXGIOutputDuplication* dup = 0; HRESULT hr = out1->DuplicateOutput(dev, &dup);
    fprintf(g_fp, "dup hr=%08lx pts:", (unsigned long)hr);
    for (auto& p : g_pts) fprintf(g_fp, " %s@%d,%d", p.name, p.x, p.y);
    fprintf(g_fp, "\n"); fflush(g_fp);
    if (FAILED(hr)) return;
    while (!g_stop) {
        DXGI_OUTDUPL_FRAME_INFO fi{}; IDXGIResource* res = 0;
        hr = dup->AcquireNextFrame(50, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) { fprintf(g_fp, "t%llu ACCESS_LOST\n", tick()); dup->Release(); dup = 0; Sleep(100); out1->DuplicateOutput(dev, &dup); if (!dup) break; continue; }
        if (FAILED(hr)) { Sleep(20); continue; }
        ID3D11Texture2D* tex = 0; res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (fi.LastPresentTime.QuadPart) {
            refresh_points(true);
            for (size_t i = 0; i < g_pts.size(); i++) {
                D3D11_BOX b{ (UINT)g_pts[i].x, (UINT)g_pts[i].y, 0, (UINT)g_pts[i].x + 1, (UINT)g_pts[i].y + 1, 1 };
                ctx->CopySubresourceRegion(stag, 0, (UINT)i, 0, 0, tex, 0, &b);
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(stag, 0, D3D11_MAP_READ, 0, &m))) {
                const unsigned char* px = (const unsigned char*)m.pData;
                fprintf(g_fp, "t%llu %10.2f acc=%u", tick(), qms(), (unsigned)fi.AccumulatedFrames);
                for (size_t i = 0; i < g_pts.size(); i++) fprintf(g_fp, " %s=%02x%02x%02x", g_pts[i].name, px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0]);
                fprintf(g_fp, "\n"); ctx->Unmap(stag, 0);
            }
        }
        tex->Release(); res->Release(); dup->ReleaseFrame();
    }
    fflush(g_fp);
}
static void move_to(int x, int y) {
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((x * 65535.0) / (GetSystemMetrics(SM_CXSCREEN) - 1)); in.mi.dy = (LONG)((y * 65535.0) / (GetSystemMetrics(SM_CYSCREEN) - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; SendInput(1, &in, sizeof in);
}
static void click(int x, int y, bool right, const char* what) {
    move_to(x, y); Sleep(40); move_to(x, y); Sleep(40);
    char b[96]; snprintf(b, sizeof b, "%s click %s at %d,%d", right ? "RIGHT" : "LEFT", what, x, y); mark(b);
    INPUT in[2]{}; in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof in[0]); Sleep(35); SendInput(1, &in[1], sizeof in[1]);
}
static void key(WORD vk, const char* what) {
    char b[64]; snprintf(b, sizeof b, "KEY %s", what); mark(b);
    INPUT in[2]{}; in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk; in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, 0); in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof in[0]); Sleep(30); SendInput(1, &in[1], sizeof in[1]);
}
static LRESULT CALLBACK probe_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(30, 160, 60)); FillRect(dc, &r, b); DeleteObject(b); EndPaint(h, &ps); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void probe_thread() {
    WNDCLASSW wc{}; wc.lpfnWndProc = probe_wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ZdupThirdApp"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_probe = CreateWindowExW(0, L"ZdupThirdApp", L"zdup third app", WS_OVERLAPPEDWINDOW, 100, 100, 420, 320, nullptr, nullptr, wc.hInstance, nullptr);
    MSG msg; while (!g_stop && GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: zdup <script>\n"); return 1; }
    if (!resolve()) { printf("cannot resolve windows\n"); return 2; }
    printf("app=%p hud=%p game=%p conpid=%lu\n", (void*)g_app, (void*)g_hud, (void*)g_game, g_conpid);
    g_fz = fopen("zdup_z.txt", "w"); g_fp = fopen("zdup_px.txt", "w"); g_fg = fopen("zdup_g.txt", "w");
    // tb1/tb3: app titlebar (121215) vs box (0d0d10). d1/d2: inside the dock card (141418) vs whatever the app paints there.
    g_pts = { { g_ax + 300, g_ay + 14, "tb1" }, { g_ax + g_aw - 300, g_ay + 14, "tb3" }, { g_ax + 40, g_ay + 300, "bd1" },
              { g_dx + 20, g_dy + 20, "d1" }, { g_dx + g_dw - 20, g_dy + g_dh - 20, "d2" },
              { g_bx + g_bw / 2, g_by + g_bh / 2, "badge" }, { 200, 1000, "gm1" }, { 1700, 1000, "gm2" } };
    std::thread tp(probe_thread); while (!g_probe) Sleep(10);
    std::thread tz(zwalk_thread); std::thread td(dup_thread);
    refresh_points(true); Sleep(600); mark("script start");
    std::string sc = argv[1]; size_t i = 0;
    while (i < sc.size()) {
        size_t j = sc.find(',', i); if (j == std::string::npos) j = sc.size();
        std::string t = sc.substr(i, j - i); i = j + 1; if (t.empty()) continue;
        if (t[0] == 'w') Sleep(atoi(t.c_str() + 1));
        else if (t == "ins") key(VK_INSERT, "Insert");
        else if (t == "f5") key(VK_F5, "F5");
        else if (t == "badge") click(g_bx + g_bw / 2, g_by + g_bh / 2, false, "badge");
        else if (t == "third") {
            SetWindowPos(g_probe, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE); Sleep(120);
            click(300, 260, false, "third-app window"); Sleep(150);
            SetWindowPos(g_probe, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            char b[64]; snprintf(b, sizeof b, "third app fg=%c", fg_class(GetForegroundWindow())); mark(b);
        }
        else if (t == "gamefg") {
            // The probe's own window is foreground after "third", so this process may hand foreground on.
            SwitchToThisWindow(g_game, TRUE); Sleep(150);
            char b[64]; snprintf(b, sizeof b, "gamefg fg=%c", fg_class(GetForegroundWindow())); mark(b);
        }
        else if (t == "hidep") { ShowWindow(g_probe, SW_HIDE); mark("probe hidden"); }
        else if (t.rfind("m:", 0) == 0) mark(t.c_str() + 2);
    }
    mark("script end"); Sleep(800); g_stop = true; PostMessageW(g_probe, WM_CLOSE, 0, 0);
    tz.join(); td.join(); tp.join(); fclose(g_fz); fclose(g_fp); fclose(g_fg);
    printf("done\n"); return 0;
}
