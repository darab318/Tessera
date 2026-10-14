// Tessera - a lightweight tiled photo viewer for Windows.
// Direct2D for drawing, Windows Imaging Component (WIC) for decoding/encoding,
// stb_image + qoi as fallbacks for formats Windows has no codec for.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wincodecsdk.h>

#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cwchar>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

// ---------------------------------------------------------------- utilities

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept { if (this != std::addressof(o)) { reset(); p = o.p; o.p = nullptr; } return *this; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** operator&() { reset(); return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

static const wchar_t* kAppName = L"Tessera";
static const wchar_t* kWndClass = L"TesseraViewerWnd";
static const wchar_t* kProgId = L"Tessera.Image";
static const wchar_t* kRegAppKey = L"Software\\Tessera";
static const wchar_t* kMutexName = L"Local\\Tessera.SingleInstance";
static const UINT WM_APP_LOADED = WM_APP + 1;
static const ULONG_PTR kCopyDataMagic = 0x54455353; // 'TESS'

static const wchar_t* kExts[] = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"dib", L"gif", L"tif", L"tiff",
    L"ico", L"cur", L"webp", L"heic", L"heif", L"hif", L"avif", L"jxl", L"jxr", L"wdp", L"hdp",
    L"dds", L"tga", L"psd", L"hdr", L"pic", L"pnm", L"ppm", L"pgm", L"pbm", L"qoi",
    L"dng", L"cr2", L"cr3", L"crw", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"orf", L"rw2",
    L"raf", L"pef", L"srw", L"3fr", L"erf", L"kdc", L"mrw", L"x3f", L"raw"};

static std::wstring LowerExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring e = path.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}
static bool IsImageExt(const std::wstring& e) {
    for (auto* x : kExts) if (e == x) return true;
    return false;
}
static std::wstring FileName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}
static std::wstring DirName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : path.substr(0, s);
}
static std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    return (n && n < sizeof(buf) / sizeof(buf[0])) ? std::wstring(buf, n) : p;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, f);
    _vsnwprintf(buf, 1023, f, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

static std::vector<std::wstring> ListImagesInDir(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (IsImageExt(LowerExt(n))) out.push_back(dir + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(FileName(a).c_str(), FileName(b).c_str()) < 0;
    });
    return out;
}

// ---------------------------------------------------------------- pixels

struct Pixels {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px; // straight-alpha BGRA, top-down
    bool alpha = false;
};

// EXIF-orientation style remap. 6 = rotate 90 CW, 8 = rotate 90 CCW.
static std::unique_ptr<Pixels> Remap(const Pixels& s, int o) {
    auto d = std::make_unique<Pixels>();
    bool swap = o >= 5 && o <= 8;
    d->w = swap ? s.h : s.w;
    d->h = swap ? s.w : s.h;
    d->alpha = s.alpha;
    d->px.resize((size_t)d->w * d->h);
    const size_t W = s.w, H = s.h;
    const uint32_t* sp = s.px.data();
    for (size_t dy = 0; dy < d->h; dy++) {
        uint32_t* row = &d->px[dy * d->w];
        switch (o) {
        case 2: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[dy * W + (W - 1 - dx)]; break;
        case 3: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[(H - 1 - dy) * W + (W - 1 - dx)]; break;
        case 4: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[(H - 1 - dy) * W + dx]; break;
        case 5: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[dx * W + dy]; break;
        case 6: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[(H - 1 - dx) * W + dy]; break;
        case 7: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[(H - 1 - dx) * W + (W - 1 - dy)]; break;
        case 8: for (size_t dx = 0; dx < d->w; dx++) row[dx] = sp[dx * W + (W - 1 - dy)]; break;
        default: memcpy(row, sp + dy * W, d->w * 4); break;
        }
    }
    return d;
}

static bool ScanAlpha(const Pixels& p) {
    for (uint32_t v : p.px) if ((v >> 24) != 0xFF) return true;
    return false;
}

// ---------------------------------------------------------------- decoding

static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& buf) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < 0x7FFFFFFF;
    if (ok) {
        buf.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd == buf.size();
    }
    CloseHandle(f);
    return ok;
}

static bool DecodeWIC(IWICImagingFactory* f, const std::wstring& path, Pixels& out, std::wstring& fmt, GUID& cont,
                      std::wstring& err) {
    Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    dec->GetContainerFormat(&cont);
    Com<IWICBitmapDecoderInfo> info;
    if (SUCCEEDED(dec->GetDecoderInfo(&info))) {
        UINT n = 0;
        info->GetFriendlyName(0, nullptr, &n);
        if (n > 1) {
            std::wstring s(n, 0);
            info->GetFriendlyName(n, &s[0], &n);
            s.resize(wcslen(s.c_str()));
            size_t k = s.find(L" Decoder");
            if (k != std::wstring::npos) s.resize(k);
            fmt = s;
        }
    }
    Com<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) { err = L"The file has no readable image frame."; return false; }

    int orient = 1;
    Com<IWICMetadataQueryReader> q;
    if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) orient = v.uiVal;
        PropVariantClear(&v);
    }
    if (orient < 1 || orient > 8) orient = 1;

    Com<IWICBitmapSource> conv;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, fr, &conv))) {
        err = L"Windows can read this file but can't convert its pixel format.";
        return false;
    }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h) { err = L"The image is empty."; return false; }
    try { out.px.resize((size_t)w * h); } catch (...) { err = L"Not enough memory for this image."; return false; }
    out.w = w; out.h = h;
    // Copy in strips so very large images don't overflow the UINT buffer size.
    UINT rowsPer = std::max<UINT>(1, (UINT)(64u * 1024 * 1024 / ((size_t)w * 4)));
    for (UINT y = 0; y < h; y += rowsPer) {
        UINT rh = std::min(rowsPer, h - y);
        WICRect rc = {0, (INT)y, (INT)w, (INT)rh};
        if (FAILED(conv->CopyPixels(&rc, w * 4, w * 4 * rh, (BYTE*)&out.px[(size_t)y * w]))) {
            err = L"The image data is damaged or incomplete.";
            return false;
        }
    }
    out.alpha = ScanAlpha(out);
    if (orient != 1) {
        try { out = std::move(*Remap(out, orient)); } catch (...) {}
    }
    return true;
}

static bool DecodeStb(const std::vector<uint8_t>& buf, Pixels& out) {
    int w, h, c;
    unsigned char* d = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &c, 4);
    if (!d) return false;
    out.w = w; out.h = h;
    out.px.resize((size_t)w * h);
    for (size_t i = 0; i < out.px.size(); i++) {
        unsigned char* s = d + i * 4;
        out.px[i] = (uint32_t)s[2] | ((uint32_t)s[1] << 8) | ((uint32_t)s[0] << 16) | ((uint32_t)s[3] << 24);
    }
    stbi_image_free(d);
    out.alpha = ScanAlpha(out);
    return true;
}

static bool DecodeQoi(const std::vector<uint8_t>& buf, Pixels& out) {
    qoi_desc desc;
    unsigned char* d = (unsigned char*)qoi_decode(buf.data(), (int)buf.size(), &desc, 4);
    if (!d) return false;
    out.w = desc.width; out.h = desc.height;
    out.px.resize((size_t)out.w * out.h);
    for (size_t i = 0; i < out.px.size(); i++) {
        unsigned char* s = d + i * 4;
        out.px[i] = (uint32_t)s[2] | ((uint32_t)s[1] << 8) | ((uint32_t)s[0] << 16) | ((uint32_t)s[3] << 24);
    }
    free(d);
    out.alpha = ScanAlpha(out);
    return true;
}

static bool DecodeAny(IWICImagingFactory* f, const std::wstring& path, Pixels& out, std::wstring& fmt, GUID& cont,
                      std::wstring& err) {
    cont = GUID{};
    std::wstring ext = LowerExt(path);
    if (ext != L"qoi" && f && DecodeWIC(f, path, out, fmt, cont, err)) return true;
    if (!err.empty()) return false; // WIC recognised the file but it's broken
    std::vector<uint8_t> buf;
    if (!ReadWholeFile(path, buf)) {
        DWORD a = GetFileAttributesW(path.c_str());
        err = a == INVALID_FILE_ATTRIBUTES ? L"The file no longer exists." : L"The file couldn't be read.";
        return false;
    }
    std::wstring up = ext;
    for (auto& c : up) c = (wchar_t)towupper(c);
    if (DecodeQoi(buf, out)) { fmt = L"QOI"; return true; }
    if (DecodeStb(buf, out)) { fmt = up; return true; }
    if (ext == L"heic" || ext == L"heif" || ext == L"hif")
        err = L"HEIC needs Microsoft's free \"HEIF Image Extensions\" (and \"HEVC Video Extensions\") from the Microsoft Store.";
    else if (ext == L"avif")
        err = L"AVIF needs Microsoft's free \"AV1 Video Extension\" from the Microsoft Store.";
    else if (ext == L"jxl")
        err = L"JPEG XL needs Microsoft's \"JPEG XL Image Extension\" from the Microsoft Store.";
    else if (ext == L"webp")
        err = L"WebP needs Microsoft's free \"WebP Image Extensions\" from the Microsoft Store.";
    else if (ext == L"dng" || ext == L"cr2" || ext == L"cr3" || ext == L"nef" || ext == L"arw" || ext == L"orf" ||
             ext == L"rw2" || ext == L"raf" || ext == L"pef" || ext == L"srw" || ext == L"raw")
        err = L"Camera RAW needs Microsoft's free \"Raw Image Extension\" from the Microsoft Store.";
    else
        err = L"This isn't an image format Tessera or Windows can read.";
    return false;
}

// ---------------------------------------------------------------- background loader

struct LoadJob { HWND hwnd; int tileId; int gen; std::wstring path; };
struct LoadResult {
    int tileId = 0, gen = 0;
    std::unique_ptr<Pixels> px;
    std::wstring fmt, err;
    GUID cont{};
};

class Loader {
    std::mutex m;
    std::condition_variable cv;
    std::deque<LoadJob*> q;

  public:
    void Start(int n) {
        for (int i = 0; i < n; i++) {
            std::thread([this] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                Com<IWICImagingFactory> f;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
                for (;;) {
                    LoadJob* j;
                    {
                        std::unique_lock<std::mutex> l(m);
                        cv.wait(l, [&] { return !q.empty(); });
                        j = q.front();
                        q.pop_front();
                    }
                    auto* r = new LoadResult;
                    r->tileId = j->tileId;
                    r->gen = j->gen;
                    r->px = std::make_unique<Pixels>();
                    bool ok = false;
                    try { ok = DecodeAny(f, j->path, *r->px, r->fmt, r->cont, r->err); }
                    catch (...) { r->err = L"Not enough memory for this image."; }
                    if (!ok) r->px.reset();
                    if (!PostMessageW(j->hwnd, WM_APP_LOADED, 0, (LPARAM)r)) delete r;
                    delete j;
                }
            }).detach();
        }
    }
    void Push(LoadJob* j, bool urgent) {
        {
            std::lock_guard<std::mutex> l(m);
            if (urgent) q.push_front(j); else q.push_back(j);
        }
        cv.notify_one();
    }
};


// ---------------------------------------------------------------- app state

struct Chunk { int x, y, w, h; Com<ID2D1Bitmap> bmp; };

struct Tile {
    int id = 0, gen = 0;
    std::wstring path, fmt, err;
    GUID cont{};
    bool loading = false;
    UINT w = 0, h = 0; bool alpha = false;
    std::unique_ptr<Pixels> cpu;            // kept only while needed (edits, uploads)
    std::vector<Chunk> chunks;               // GPU copy, split to fit max texture size
    bool gpuFail = false;
    std::vector<std::unique_ptr<Pixels>> undo;
    int edits = 0;
    bool fit = true; float zoom = 1, cx = 0, cy = 0;
    D2D1_RECT_F cell{};
    std::vector<std::wstring> sibs; int sibIdx = -1; bool sibsLoaded = false;
    bool HasImage() const { return w > 0 && h > 0; }
};

struct CropState {
    bool active = false;
    int tileId = -1;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // image coords
    int drag = 0;                          // 0 none, edge mask L1 R2 T4 B8, 16 move, 32 new
    float ax = 0, ay = 0;                  // anchor / start point (image coords)
    float s0x = 0, s0y = 0, s1x = 0, s1y = 0;
};

struct App {
    HWND hwnd = nullptr;
    float scale = 1;
    std::vector<std::unique_ptr<Tile>> tiles;
    int nextId = 1;
    int focus = -1;
    bool fullscreen = false;
    WINDOWPLACEMENT wpPrev{sizeof(WINDOWPLACEMENT)};
    bool sync = false, grid = true, labels = true, help = false;
    CropState crop;
    int dragMode = 0; // 1 pan, 2 crop, 3 middle pan
    int dragTile = -1;
    POINT lastPt{}, downPt{};
    bool midMoved = false;
    int hoverTile = -1; int hoverX = -1, hoverY = -1;
    int wheelAccum = 0;
    DWORD lastExternalOpen = 0;
    bool cursorHidden = false;
    std::wstring toast; DWORD toastUntil = 0;
    Loader loader;
} g;

// D2D / DWrite / WIC
static ID2D1Factory* g_d2f = nullptr;
static IDWriteFactory* g_dwf = nullptr;
static IWICImagingFactory* g_wic = nullptr;
static Com<ID2D1HwndRenderTarget> g_rt;
static Com<ID2D1DeviceContext> g_dc;
static Com<ID2D1SolidColorBrush> g_brush;
static Com<ID2D1BitmapBrush> g_checker;
static Com<IDWriteTextFormat> g_tfSmall, g_tfStatusL, g_tfStatusR, g_tfCenter, g_tfHelp;

static D2D1_COLOR_F Col(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
static ID2D1SolidColorBrush* B(uint32_t rgb, float a = 1.f) { g_brush->SetColor(Col(rgb, a)); return g_brush; }

static Tile* TileById(int id) {
    for (auto& t : g.tiles) if (t->id == id) return t.get();
    return nullptr;
}
static Tile* Focused() {
    if (g.focus < 0 || g.focus >= (int)g.tiles.size()) return nullptr;
    return g.tiles[g.focus].get();
}
static void Redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }
static void Toast(const std::wstring& s) {
    g.toast = s;
    g.toastUntil = GetTickCount() + 2200;
    SetTimer(g.hwnd, 2, 2300, nullptr);
    Redraw();
}
static float StatusH() { return g.fullscreen ? 0.f : std::round(26 * g.scale); }

// ---------------------------------------------------------------- settings

static void LoadSettings() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Tessera\\Settings", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    auto rd = [&](const wchar_t* n, bool& v) {
        DWORD d, sz = 4;
        if (RegQueryValueExW(k, n, nullptr, nullptr, (BYTE*)&d, &sz) == ERROR_SUCCESS) v = d != 0;
    };
    rd(L"Sync", g.sync); rd(L"Grid", g.grid); rd(L"Labels", g.labels);
    RegCloseKey(k);
}
static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Tessera\\Settings", 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                        nullptr) != ERROR_SUCCESS) return;
    auto wr = [&](const wchar_t* n, bool v) { DWORD d = v; RegSetValueExW(k, n, 0, REG_DWORD, (BYTE*)&d, 4); };
    wr(L"Sync", g.sync); wr(L"Grid", g.grid); wr(L"Labels", g.labels);
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g.fullscreen) wp = g.wpPrev; else GetWindowPlacement(g.hwnd, &wp);
    RegSetValueExW(k, L"Placement", 0, REG_BINARY, (BYTE*)&wp, sizeof(wp));
    RegCloseKey(k);
}
static bool LoadPlacement(WINDOWPLACEMENT& wp) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Tessera\\Settings", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(wp);
    bool ok = RegQueryValueExW(k, L"Placement", nullptr, nullptr, (BYTE*)&wp, &sz) == ERROR_SUCCESS && sz == sizeof(wp);
    RegCloseKey(k);
    return ok;
}

// ---------------------------------------------------------------- default-app registration

static std::wstring ExePath() {
    wchar_t b[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 4);
    return std::wstring(b, n);
}
static void RegStr(const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
static void RegNone(const std::wstring& sub, const wchar_t* name) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_NONE, nullptr, 0);
        RegCloseKey(k);
    }
}
static void RegisterApp() {
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
    std::wstring icon = L"\"" + exe + L"\",0";
    std::wstring pk = std::wstring(L"Software\\Classes\\") + kProgId;
    RegStr(pk, nullptr, std::wstring(L"Image (") + kAppName + L")");
    RegStr(pk, L"FriendlyTypeName", L"Image");
    RegStr(pk + L"\\DefaultIcon", nullptr, icon);
    RegStr(pk + L"\\shell\\open", L"MultiSelectModel", L"Document");
    RegStr(pk + L"\\shell\\open\\command", nullptr, cmd);
    std::wstring ak = L"Software\\Classes\\Applications\\" + FileName(exe);
    RegStr(ak, L"FriendlyAppName", kAppName);
    RegStr(ak + L"\\DefaultIcon", nullptr, icon);
    RegStr(ak + L"\\shell\\open\\command", nullptr, cmd);
    std::wstring ck = std::wstring(kRegAppKey) + L"\\Capabilities";
    RegStr(ck, L"ApplicationName", kAppName);
    RegStr(ck, L"ApplicationDescription", L"Lightweight tiled photo viewer with pixel-exact zoom.");
    RegStr(ck, L"ApplicationIcon", icon);
    for (auto* e : kExts) {
        std::wstring dot = std::wstring(L".") + e;
        RegStr(ck + L"\\FileAssociations", dot.c_str(), kProgId);
        RegNone(L"Software\\Classes\\" + dot + L"\\OpenWithProgids", kProgId);
        RegStr(ak + L"\\SupportedTypes", dot.c_str(), L"");
    }
    RegStr(L"Software\\RegisteredApplications", kAppName, ck);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
static void UnregisterApp() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\Applications\\" + FileName(ExePath())).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(kRegAppKey) + L"\\Capabilities").c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName);
    for (auto* e : kExts)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\.") + e + L"\\OpenWithProgids").c_str(),
                           kProgId);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
static bool IsRegisteredHere() {
    std::wstring want = L"\"" + ExePath() + L"\" \"%1\"";
    wchar_t buf[2048]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command").c_str(),
                     nullptr, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, want.c_str()) == 0;
}
static void OpenDefaultAppsSettings() {
    std::wstring name;
    for (const wchar_t* c = kAppName; *c; c++) { if (*c == L' ') name += L"%20"; else name += *c; }
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", (L"ms-settings:defaultapps?registeredAppUser=" + name).c_str(), nullptr, nullptr,
                               SW_SHOWNORMAL) <= 32)
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- device resources

static void ReleaseDeviceResources() {
    for (auto& t : g.tiles) { t->chunks.clear(); t->gpuFail = false; }
    g_checker.reset();
    g_brush.reset();
    g_dc.reset();
    g_rt.reset();
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    if (FAILED(g_d2f->CreateHwndRenderTarget(
            p, D2D1::HwndRenderTargetProperties(g.hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))),
            &g_rt)))
        return false;
    g_rt->SetDpi(96, 96);
    g_rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&g_dc);
    g_rt->CreateSolidColorBrush(Col(0xFFFFFF), &g_brush);
    // 16x16 checkerboard for transparent images
    uint32_t cb[16 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) cb[y * 16 + x] = (((x / 8) ^ (y / 8)) & 1) ? 0xFF3A3A3A : 0xFF2A2A2A;
    Com<ID2D1Bitmap> cbm;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(16, 16), cb, 64, &bp, &cbm))) {
        D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        g_rt->CreateBitmapBrush(cbm, &bbp, nullptr, &g_checker);
    }
    return true;
}

static void CreateTextFormats() {
    g_tfSmall.reset(); g_tfStatusL.reset(); g_tfStatusR.reset(); g_tfCenter.reset(); g_tfHelp.reset();
    auto mk = [&](Com<IDWriteTextFormat>& f, float size, DWRITE_FONT_WEIGHT wt, DWRITE_TEXT_ALIGNMENT ta,
                  DWRITE_PARAGRAPH_ALIGNMENT pa, bool trim) {
        g_dwf->CreateTextFormat(L"Segoe UI", nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                size * g.scale, L"", &f);
        if (!f) return;
        f->SetTextAlignment(ta);
        f->SetParagraphAlignment(pa);
        if (trim) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            Com<IDWriteInlineObject> sign;
            g_dwf->CreateEllipsisTrimmingSign(f, &sign);
            f->SetTrimming(&tr, sign);
        }
    };
    mk(g_tfSmall, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    mk(g_tfStatusL, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    mk(g_tfStatusR, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    mk(g_tfCenter, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    mk(g_tfHelp, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
}

// Upload CPU pixels into one or more GPU bitmaps.
static void BuildChunks(Tile& t) {
    t.chunks.clear();
    if (!t.cpu) return;
    const Pixels& p = *t.cpu;
    int maxs = (int)std::min<UINT32>(g_rt->GetMaximumBitmapSize(), 16384);
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, p.alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96, 96);
    std::vector<uint32_t> tmp;
    for (int y = 0; y < (int)p.h; y += maxs) {
        for (int x = 0; x < (int)p.w; x += maxs) {
            Chunk c;
            c.x = x; c.y = y;
            c.w = std::min(maxs, (int)p.w - x);
            c.h = std::min(maxs, (int)p.h - y);
            const void* src = &p.px[(size_t)y * p.w + x];
            UINT32 pitch = p.w * 4;
            if (p.alpha) {
                try { tmp.resize((size_t)c.w * c.h); } catch (...) { t.gpuFail = true; t.chunks.clear(); return; }
                for (int yy = 0; yy < c.h; yy++) {
                    const uint32_t* s = &p.px[(size_t)(y + yy) * p.w + x];
                    uint32_t* d = &tmp[(size_t)yy * c.w];
                    for (int xx = 0; xx < c.w; xx++) {
                        uint32_t v = s[xx], a = v >> 24;
                        if (a == 255) { d[xx] = v; continue; }
                        uint32_t b = ((v & 255) * a + 127) / 255, gg = (((v >> 8) & 255) * a + 127) / 255,
                                 r = (((v >> 16) & 255) * a + 127) / 255;
                        d[xx] = b | (gg << 8) | (r << 16) | (a << 24);
                    }
                }
                src = tmp.data();
                pitch = c.w * 4;
            }
            if (FAILED(g_rt->CreateBitmap(D2D1::SizeU(c.w, c.h), src, pitch, &bp, &c.bmp))) {
                t.gpuFail = true;
                t.chunks.clear();
                return;
            }
            t.chunks.push_back(std::move(c));
        }
    }
    // Unedited images don't need a CPU copy once they're on the GPU; re-read from disk on demand.
    if (t.edits == 0 && t.undo.empty()) t.cpu.reset();
}

// ---------------------------------------------------------------- view math

struct View { float z, ox, oy; };

static float CW(const Tile& t) { return t.cell.right - t.cell.left; }
static float CH(const Tile& t) { return t.cell.bottom - t.cell.top; }

static float FitZoom(const Tile& t) {
    if (!t.HasImage()) return 1;
    float s = std::min(CW(t) / t.w, CH(t) / t.h);
    if (s >= 1) return std::floor(s); // upscale only by whole numbers: every pixel stays the same size
    return s > 0 ? s : 1e-3f;
}
static View GetView(const Tile& t) {
    View v;
    float ccx = (t.cell.left + t.cell.right) / 2, ccy = (t.cell.top + t.cell.bottom) / 2;
    float cx, cy;
    if (t.fit) { v.z = FitZoom(t); cx = t.w / 2.f; cy = t.h / 2.f; }
    else { v.z = t.zoom; cx = t.cx; cy = t.cy; }
    v.ox = std::floor(ccx - cx * v.z + 0.5f);
    v.oy = std::floor(ccy - cy * v.z + 0.5f);
    return v;
}

static const float kLadder[] = {0.01f, 0.015f, 0.02f, 0.03f, 0.04f, 0.05f, 0.0625f, 0.08f, 0.1f, 0.125f, 0.15f,
                                0.2f, 0.25f, 0.3333f, 0.5f, 0.6667f, 0.75f, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20,
                                24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};

static float NextZoom(float z, int dir) {
    if (dir > 0) { for (float l : kLadder) if (l > z * 1.001f) return l; return kLadder[sizeof(kLadder) / sizeof(float) - 1]; }
    float best = kLadder[0];
    for (float l : kLadder) if (l < z * 0.999f) best = l;
    return best;
}

static void ClampCenter(Tile& t) {
    t.cx = std::max(0.f, std::min((float)t.w, t.cx));
    t.cy = std::max(0.f, std::min((float)t.h, t.cy));
}

static void SetZoomAt(Tile& t, float nz, float sx, float sy) {
    if (!t.HasImage()) return;
    View v = GetView(t);
    float ix = (sx - v.ox) / v.z, iy = (sy - v.oy) / v.z;
    float ccx = (t.cell.left + t.cell.right) / 2, ccy = (t.cell.top + t.cell.bottom) / 2;
    float fz = FitZoom(t);
    if (nz <= fz * 1.0001f && nz < v.z) { t.fit = true; return; }
    t.fit = false;
    t.zoom = nz;
    t.cx = (ccx - sx) / nz + ix;
    t.cy = (ccy - sy) / nz + iy;
    ClampCenter(t);
}

static void StepZoomAt(Tile& t, int dir, float sx, float sy) {
    if (!t.HasImage()) return;
    View v = GetView(t);
    float nz = NextZoom(v.z, dir);
    float fz = FitZoom(t);
    // Make "fit" a stop on the way in or out.
    if (dir > 0 && v.z < fz * 0.999f && nz > fz) nz = fz;
    SetZoomAt(t, nz, sx, sy);
}

static void SetActualPixels(Tile& t) {
    if (!t.HasImage()) return;
    View v = GetView(t);
    float ccx = (t.cell.left + t.cell.right) / 2, ccy = (t.cell.top + t.cell.bottom) / 2;
    float icx = (ccx - v.ox) / v.z, icy = (ccy - v.oy) / v.z;
    t.fit = false; t.zoom = 1; t.cx = icx; t.cy = icy;
    ClampCenter(t);
}

static bool ScreenToImage(const Tile& t, float sx, float sy, float& ix, float& iy) {
    View v = GetView(t);
    ix = (sx - v.ox) / v.z;
    iy = (sy - v.oy) / v.z;
    return ix >= 0 && iy >= 0 && ix < t.w && iy < t.h;
}

// ---------------------------------------------------------------- layout

static void Layout() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    float W = (float)rc.right, H = (float)rc.bottom - StatusH();
    int n = (int)g.tiles.size();
    if (n == 0 || W < 2 || H < 2) return;
    float gap = g.fullscreen ? 2.f : std::round(3 * g.scale);
    auto aspect = [](const Tile& t) { return t.HasImage() ? (float)t.w / t.h : 1.5f; };

    int bestC = 1; double bestScore = -1;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        if (c > 1 && (c - 1) * r >= n) continue; // fewer columns would give the same rows
        double score = 0;
        float ch = (H - gap * (r - 1)) / r;
        for (int i = 0; i < n; i++) {
            int row = i / c;
            int cnt = row == r - 1 ? n - c * (r - 1) : c;
            float cw = (W - gap * (cnt - 1)) / cnt;
            float a = aspect(*g.tiles[i]);
            float dw = std::min(cw, ch * a), dh = dw / a;
            score += (double)dw * dh;
        }
        if (score > bestScore * 1.0001) { bestScore = score; bestC = c; }
    }
    int c = bestC, r = (n + c - 1) / c;
    float ch = (H - gap * (r - 1)) / r;
    for (int i = 0; i < n; i++) {
        int row = i / c, col = i % c;
        int cnt = row == r - 1 ? n - c * (r - 1) : c;
        float cw = (W - gap * (cnt - 1)) / cnt;
        D2D1_RECT_F cell;
        cell.left = std::round(col * (cw + gap));
        cell.right = std::round(col * (cw + gap) + cw);
        cell.top = std::round(row * (ch + gap));
        cell.bottom = std::round(row * (ch + gap) + ch);
        g.tiles[i]->cell = cell;
    }
}

static int HitTile(float x, float y) {
    for (int i = 0; i < (int)g.tiles.size(); i++) {
        auto& c = g.tiles[i]->cell;
        if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) return i;
    }
    return -1;
}

// ---------------------------------------------------------------- title

static void UpdateTitle() {
    std::wstring t;
    Tile* f = Focused();
    if (g.tiles.empty()) t = kAppName;
    else if (g.tiles.size() == 1 && f) t = (f->edits ? L"• " : L"") + FileName(f->path) + L" — " + kAppName;
    else t = Fmt(L"%d images — %s", (int)g.tiles.size(), kAppName);
    SetWindowTextW(g.hwnd, t.c_str());
}

// ---------------------------------------------------------------- loading

static void StartLoad(Tile& t, bool urgent) {
    t.gen++;
    t.loading = true;
    g.loader.Push(new LoadJob{g.hwnd, t.id, t.gen, t.path}, urgent);
}

static bool EnsureCpu(Tile& t) {
    if (t.cpu) return true;
    if (!t.HasImage()) return false;
    auto p = std::make_unique<Pixels>();
    std::wstring fmt, err; GUID cont;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = false;
    try { ok = DecodeAny(g_wic, t.path, *p, fmt, cont, err); } catch (...) { err = L"Not enough memory."; }
    SetCursor(old);
    if (!ok) { MessageBoxW(g.hwnd, (L"Couldn't re-read the image:\n" + err).c_str(), kAppName, MB_ICONWARNING); return false; }
    if (p->w != t.w || p->h != t.h) { t.w = p->w; t.h = p->h; t.fit = true; t.chunks.clear(); }
    t.alpha = p->alpha;
    t.cpu = std::move(p);
    return true;
}
static void DropCpuIfClean(Tile& t) {
    if (t.edits == 0 && t.undo.empty() && !t.chunks.empty()) t.cpu.reset();
}

static void AddTile(const std::wstring& path) {
    auto t = std::make_unique<Tile>();
    t->id = g.nextId++;
    t->path = path;
    StartLoad(*t, false);
    g.tiles.push_back(std::move(t));
}

static bool SaveTile(Tile& t, bool saveAs);

static bool ConfirmDiscard(Tile& t) {
    if (t.edits == 0) return true;
    int r = MessageBoxW(g.hwnd, (L"Save changes to " + FileName(t.path) + L"?").c_str(), kAppName,
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return SaveTile(t, false);
    return true;
}
static bool ConfirmDiscardAll() {
    for (auto& t : g.tiles) if (!ConfirmDiscard(*t)) return false;
    return true;
}

static void ExpandAndCollect(const std::vector<std::wstring>& in, std::vector<std::wstring>& out) {
    for (auto& p0 : in) {
        if (p0.empty()) continue;
        std::wstring p = FullPath(p0);
        if (IsDir(p)) { auto v = ListImagesInDir(p); out.insert(out.end(), v.begin(), v.end()); }
        else out.push_back(p);
    }
}

static void OpenFiles(const std::vector<std::wstring>& in, bool replace) {
    std::vector<std::wstring> files;
    ExpandAndCollect(in, files);
    if (files.empty()) return;
    if (replace) {
        if (!ConfirmDiscardAll()) return;
        g.crop.active = false;
        g.tiles.clear();
        g.focus = -1;
    }
    int first = (int)g.tiles.size();
    for (auto& f : files) AddTile(f);
    if (g.focus < 0) g.focus = first;
    Layout();
    UpdateTitle();
    Redraw();
}

static void CloseTile(int idx) {
    if (idx < 0 || idx >= (int)g.tiles.size()) return;
    if (!ConfirmDiscard(*g.tiles[idx])) return;
    if (g.crop.active && g.crop.tileId == g.tiles[idx]->id) g.crop.active = false;
    g.tiles.erase(g.tiles.begin() + idx);
    if (g.focus >= (int)g.tiles.size()) g.focus = (int)g.tiles.size() - 1;
    g.hoverTile = -1;
    Layout();
    UpdateTitle();
    Redraw();
}

static void OnLoaded(LoadResult* r) {
    std::unique_ptr<LoadResult> res(r);
    Tile* t = TileById(r->tileId);
    if (!t || t->gen != r->gen) return;
    t->loading = false;
    t->chunks.clear();
    t->gpuFail = false;
    if (r->px) {
        bool sameSize = t->w == r->px->w && t->h == r->px->h;
        t->w = r->px->w; t->h = r->px->h; t->alpha = r->px->alpha;
        t->cpu = std::move(r->px);
        t->fmt = r->fmt; t->cont = r->cont; t->err.clear();
        if (!sameSize) t->fit = true;
    } else {
        t->cpu.reset();
        t->w = t->h = 0;
        t->err = r->err;
        t->fmt.clear();
    }
    Layout();
    UpdateTitle();
    Redraw();
}

// ---------------------------------------------------------------- folder navigation

static void LoadSiblings(Tile& t) {
    t.sibsLoaded = true;
    t.sibs = ListImagesInDir(DirName(t.path));
    t.sibIdx = -1;
    for (int i = 0; i < (int)t.sibs.size(); i++)
        if (_wcsicmp(t.sibs[i].c_str(), t.path.c_str()) == 0) { t.sibIdx = i; break; }
    if (t.sibIdx < 0) {
        t.sibs.push_back(t.path);
        std::sort(t.sibs.begin(), t.sibs.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(FileName(a).c_str(), FileName(b).c_str()) < 0;
        });
        for (int i = 0; i < (int)t.sibs.size(); i++) if (t.sibs[i] == t.path) t.sibIdx = i;
    }
}

static void Navigate(Tile& t, int mode) {
    if (!ConfirmDiscard(t)) return;
    if (!t.sibsLoaded) LoadSiblings(t);
    int n = (int)t.sibs.size();
    if (n <= 1) return;
    int i = t.sibIdx;
    if (mode == -2) i = 0;
    else if (mode == 2) i = n - 1;
    else i = ((i < 0 ? 0 : i) + mode + n) % n;
    if (i == t.sibIdx) return;
    if (g.crop.active && g.crop.tileId == t.id) g.crop.active = false;
    t.sibIdx = i;
    t.path = t.sibs[i];
    t.fit = true;
    t.undo.clear();
    t.edits = 0;
    StartLoad(t, true);
    UpdateTitle();
    Redraw();
}

// ---------------------------------------------------------------- editing

static void PushEdit(Tile& t, std::unique_ptr<Pixels> next) {
    t.undo.push_back(std::move(t.cpu));
    if (t.undo.size() > 4) t.undo.erase(t.undo.begin());
    t.cpu = std::move(next);
    t.w = t.cpu->w; t.h = t.cpu->h; t.alpha = t.cpu->alpha;
    t.chunks.clear();
    t.gpuFail = false;
    t.edits++;
}

static void RotateTile(Tile& t, bool cw) {
    if (!t.HasImage() || t.loading) return;
    if (g.crop.active && g.crop.tileId == t.id) g.crop.active = false;
    if (!EnsureCpu(t)) return;
    std::unique_ptr<Pixels> np;
    try { np = Remap(*t.cpu, cw ? 6 : 8); } catch (...) { MessageBoxW(g.hwnd, L"Not enough memory to rotate.", kAppName, MB_ICONWARNING); return; }
    float W = (float)t.w, H = (float)t.h, ocx = t.cx, ocy = t.cy;
    PushEdit(t, std::move(np));
    if (cw) { t.cx = H - ocy; t.cy = ocx; } else { t.cx = ocy; t.cy = W - ocx; }
    Layout();
    UpdateTitle();
    Redraw();
}

static void UndoTile(Tile& t) {
    if (t.undo.empty()) { Toast(L"Nothing to undo"); return; }
    if (g.crop.active && g.crop.tileId == t.id) g.crop.active = false;
    t.cpu = std::move(t.undo.back());
    t.undo.pop_back();
    t.w = t.cpu->w; t.h = t.cpu->h; t.alpha = t.cpu->alpha;
    t.chunks.clear();
    t.edits--;
    t.fit = true;
    Layout();
    UpdateTitle();
    Redraw();
}

static void BeginCrop(Tile& t) {
    if (!t.HasImage() || t.loading) return;
    g.crop.active = true;
    g.crop.tileId = t.id;
    g.crop.x0 = 0; g.crop.y0 = 0; g.crop.x1 = (float)t.w; g.crop.y1 = (float)t.h;
    g.crop.drag = 0;
    Toast(L"Crop: drag to select • Enter to apply • Esc to cancel • Shift = square");
    Redraw();
}

static void NormalizeCrop(Tile& t) {
    CropState& c = g.crop;
    if (c.x0 > c.x1) std::swap(c.x0, c.x1);
    if (c.y0 > c.y1) std::swap(c.y0, c.y1);
    c.x0 = std::max(0.f, std::min((float)t.w, std::round(c.x0)));
    c.x1 = std::max(0.f, std::min((float)t.w, std::round(c.x1)));
    c.y0 = std::max(0.f, std::min((float)t.h, std::round(c.y0)));
    c.y1 = std::max(0.f, std::min((float)t.h, std::round(c.y1)));
}

static void ApplyCrop() {
    Tile* t = TileById(g.crop.tileId);
    if (!t) { g.crop.active = false; return; }
    NormalizeCrop(*t);
    int x0 = (int)g.crop.x0, y0 = (int)g.crop.y0, x1 = (int)g.crop.x1, y1 = (int)g.crop.y1;
    g.crop.active = false;
    if (x1 - x0 < 1 || y1 - y0 < 1) { Redraw(); return; }
    if (x0 == 0 && y0 == 0 && x1 == (int)t->w && y1 == (int)t->h) { Redraw(); return; }
    if (!EnsureCpu(*t)) return;
    auto np = std::make_unique<Pixels>();
    try {
        np->w = x1 - x0; np->h = y1 - y0;
        np->px.resize((size_t)np->w * np->h);
        for (int y = y0; y < y1; y++)
            memcpy(&np->px[(size_t)(y - y0) * np->w], &t->cpu->px[(size_t)y * t->cpu->w + x0], (size_t)np->w * 4);
        np->alpha = ScanAlpha(*np);
    } catch (...) { MessageBoxW(g.hwnd, L"Not enough memory to crop.", kAppName, MB_ICONWARNING); return; }
    PushEdit(*t, std::move(np));
    t->fit = true;
    Layout();
    UpdateTitle();
    Redraw();
}

// ---------------------------------------------------------------- saving

// Minimal IWICBitmapSource over our pixel buffer (avoids copying the whole image again).
class PixelSource final : public IWICBitmapSource {
    LONG ref = 1;
    const Pixels& p;
  public:
    explicit PixelSource(const Pixels& px) : p(px) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IWICBitmapSource)) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* w, UINT* h) override { *w = p.w; *h = p.h; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* f) override { *f = GUID_WICPixelFormat32bppBGRA; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetResolution(double* x, double* y) override { *x = *y = 96; return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rc, UINT stride, UINT size, BYTE* buf) override {
        WICRect r = rc ? *rc : WICRect{0, 0, (INT)p.w, (INT)p.h};
        if (r.X < 0 || r.Y < 0 || r.X + r.Width > (INT)p.w || r.Y + r.Height > (INT)p.h) return E_INVALIDARG;
        if ((UINT64)stride * (r.Height - 1) + (UINT64)r.Width * 4 > size) return E_INVALIDARG;
        for (INT y = 0; y < r.Height; y++)
            memcpy(buf + (size_t)y * stride, &p.px[(size_t)(r.Y + y) * p.w + r.X], (size_t)r.Width * 4);
        return S_OK;
    }
};

static bool ContainerForExt(const std::wstring& e, GUID& cont) {
    if (e == L"jpg" || e == L"jpeg" || e == L"jpe" || e == L"jfif") { cont = GUID_ContainerFormatJpeg; return true; }
    if (e == L"png") { cont = GUID_ContainerFormatPng; return true; }
    if (e == L"bmp" || e == L"dib") { cont = GUID_ContainerFormatBmp; return true; }
    if (e == L"tif" || e == L"tiff") { cont = GUID_ContainerFormatTiff; return true; }
    return false;
}

static HRESULT Encode(const Pixels& px, const std::wstring& out, GUID cont, const std::wstring& srcPath, bool meta) {
    std::wstring tmp = out + L".tessera-tmp";
    HRESULT hr;
    {
        Com<IWICStream> st;
        if (FAILED(hr = g_wic->CreateStream(&st))) return hr;
        if (FAILED(hr = st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) return hr;
        Com<IWICBitmapEncoder> enc;
        if (FAILED(hr = g_wic->CreateEncoder(cont, nullptr, &enc))) goto fail;
        if (FAILED(hr = enc->Initialize(st, WICBitmapEncoderNoCache))) goto fail;
        {
            Com<IWICBitmapFrameEncode> fe;
            Com<IPropertyBag2> bag;
            if (FAILED(hr = enc->CreateNewFrame(&fe, &bag))) goto fail;
            if (cont == GUID_ContainerFormatJpeg && bag) {
                PROPBAG2 opt{}; opt.pstrName = (LPOLESTR)L"ImageQuality";
                VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.95f;
                bag->Write(1, &opt, &v);
            }
            if (FAILED(hr = fe->Initialize(bag))) goto fail;
            if (FAILED(hr = fe->SetSize(px.w, px.h))) goto fail;
            WICPixelFormatGUID pf = (px.alpha && cont != GUID_ContainerFormatJpeg) ? GUID_WICPixelFormat32bppBGRA
                                                                                  : GUID_WICPixelFormat24bppBGR;
            if (FAILED(hr = fe->SetPixelFormat(&pf))) goto fail;
            if (meta) {
                Com<IWICBitmapDecoder> dec;
                if (SUCCEEDED(g_wic->CreateDecoderFromFilename(srcPath.c_str(), nullptr, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnLoad, &dec))) {
                    Com<IWICBitmapFrameDecode> fr;
                    Com<IWICMetadataBlockReader> br;
                    Com<IWICMetadataBlockWriter> bw;
                    if (SUCCEEDED(dec->GetFrame(0, &fr)) &&
                        SUCCEEDED(fr->QueryInterface(__uuidof(IWICMetadataBlockReader), (void**)&br)) &&
                        SUCCEEDED(fe->QueryInterface(__uuidof(IWICMetadataBlockWriter), (void**)&bw))) {
                        if (FAILED(hr = bw->InitializeFromBlockReader(br))) goto fail;
                        Com<IWICMetadataQueryWriter> qw;
                        if (SUCCEEDED(fe->GetMetadataQueryWriter(&qw))) {
                            PROPVARIANT v; PropVariantInit(&v); v.vt = VT_UI2; v.uiVal = 1; // pixels are already upright
                            qw->SetMetadataByName(L"System.Photo.Orientation", &v);
                        }
                    }
                }
            }
            PixelSource* src = new PixelSource(px);
            Com<IWICBitmapSource> conv;
            if (pf != GUID_WICPixelFormat32bppBGRA) hr = WICConvertBitmapSource(pf, src, &conv);
            else { src->AddRef(); conv.p = src; hr = S_OK; }
            src->Release();
            if (FAILED(hr)) goto fail;
            if (FAILED(hr = fe->WriteSource(conv, nullptr))) goto fail;
            if (FAILED(hr = fe->Commit())) goto fail;
        }
        if (FAILED(hr = enc->Commit())) goto fail;
    }
    if (!MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        DeleteFileW(tmp.c_str());
        return hr;
    }
    return S_OK;
fail:
    DeleteFileW(tmp.c_str());
    return hr;
}

static bool AskSavePath(const Tile& t, std::wstring& out) {
    Com<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC spec[] = {{L"PNG image", L"*.png"}, {L"JPEG image", L"*.jpg;*.jpeg"},
                                {L"Bitmap image", L"*.bmp"}, {L"TIFF image", L"*.tif;*.tiff"}};
    dlg->SetFileTypes(4, spec);
    std::wstring ext = LowerExt(t.path);
    GUID c;
    UINT idx = 1; const wchar_t* def = L"png";
    if (ContainerForExt(ext, c)) {
        if (c == GUID_ContainerFormatJpeg) { idx = 2; def = L"jpg"; }
        else if (c == GUID_ContainerFormatBmp) { idx = 3; def = L"bmp"; }
        else if (c == GUID_ContainerFormatTiff) { idx = 4; def = L"tif"; }
    }
    dlg->SetFileTypeIndex(idx);
    dlg->SetDefaultExtension(def);
    std::wstring name = FileName(t.path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    name += L" (edited)";
    dlg->SetFileName(name.c_str());
    Com<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(DirName(t.path).c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetFolder(folder);
    if (FAILED(dlg->Show(g.hwnd))) return false;
    Com<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    if (!ContainerForExt(LowerExt(out), c)) {
        UINT ti = 1; dlg->GetFileTypeIndex(&ti);
        const wchar_t* exts[] = {L".png", L".jpg", L".bmp", L".tif"};
        out += exts[std::min<UINT>(4, std::max<UINT>(1, ti)) - 1];
    }
    return true;
}

static bool SaveTile(Tile& t, bool saveAs) {
    if (!t.HasImage()) return false;
    std::wstring out = t.path;
    GUID cont;
    bool canOverwrite = ContainerForExt(LowerExt(t.path), cont);
    if (saveAs || !canOverwrite) {
        if (!saveAs && !canOverwrite)
            MessageBoxW(g.hwnd, (L"Tessera can't write " + LowerExt(t.path) + L" files, so it will save a copy instead.").c_str(),
                        kAppName, MB_ICONINFORMATION);
        if (!AskSavePath(t, out)) return false;
        if (!ContainerForExt(LowerExt(out), cont)) return false;
    } else {
        if (t.edits == 0) { Toast(L"No changes to save"); return true; }
        int r = MessageBoxW(g.hwnd,
                            (L"Overwrite the original file?\n\n" + FileName(t.path) +
                             L"\n\nTip: Ctrl+Shift+S saves a copy instead.").c_str(),
                            kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (r != IDYES) return false;
    }
    if (!EnsureCpu(t)) return false;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool sameFormat = t.cont == cont;
    HRESULT hr = Encode(*t.cpu, out, cont, t.path, sameFormat);
    if (FAILED(hr) && sameFormat) hr = Encode(*t.cpu, out, cont, t.path, false);
    SetCursor(old);
    if (FAILED(hr)) {
        wchar_t msg[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0, msg, 511, nullptr);
        MessageBoxW(g.hwnd, (L"Couldn't save the image.\n\n" + std::wstring(msg)).c_str(), kAppName, MB_ICONERROR);
        return false;
    }
    if (out != t.path) {
        t.path = out;
        t.sibsLoaded = false;
        std::wstring e = LowerExt(out);
        for (auto& ch : e) ch = (wchar_t)towupper(ch);
        t.fmt = e == L"JPG" ? L"JPEG" : e;
    }
    t.cont = cont;
    t.edits = 0;
    t.undo.clear();
    DropCpuIfClean(t);
    UpdateTitle();
    Toast(L"Saved " + FileName(out));
    return true;
}

static void CopyToClipboard(Tile& t) {
    if (!t.HasImage() || !EnsureCpu(t)) return;
    const Pixels& p = *t.cpu;
    size_t bytes = (size_t)p.w * p.h * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!hg) { MessageBoxW(g.hwnd, L"Not enough memory to copy.", kAppName, MB_ICONWARNING); return; }
    auto* bi = (BITMAPINFOHEADER*)GlobalLock(hg);
    *bi = {};
    bi->biSize = sizeof(*bi); bi->biWidth = p.w; bi->biHeight = p.h; bi->biPlanes = 1;
    bi->biBitCount = 32; bi->biCompression = BI_RGB; bi->biSizeImage = (DWORD)bytes;
    uint8_t* dst = (uint8_t*)(bi + 1);
    for (UINT y = 0; y < p.h; y++) memcpy(dst + (size_t)y * p.w * 4, &p.px[(size_t)(p.h - 1 - y) * p.w], (size_t)p.w * 4);
    GlobalUnlock(hg);
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hg)) GlobalFree(hg);
        CloseClipboard();
        Toast(L"Copied to clipboard");
    } else GlobalFree(hg);
    DropCpuIfClean(t);
}

static void ShowInExplorer(const Tile& t) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(t.path.c_str());
    if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
}

static void ShowOpenDialog() {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    std::wstring pat;
    for (auto* e : kExts) { if (!pat.empty()) pat += L';'; pat += L"*."; pat += e; }
    COMDLG_FILTERSPEC spec[] = {{L"Images", pat.c_str()}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    DWORD o = 0; dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (Tile* f = Focused()) {
        Com<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(DirName(f->path).c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dlg->SetFolder(folder);
    }
    if (FAILED(dlg->Show(g.hwnd))) return;
    Com<IShellItemArray> arr;
    if (FAILED(dlg->GetResults(&arr))) return;
    DWORD n = 0; arr->GetCount(&n);
    std::vector<std::wstring> files;
    for (DWORD i = 0; i < n; i++) {
        Com<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            files.push_back(p);
            CoTaskMemFree(p);
        }
    }
    OpenFiles(files, false);
}

// ---------------------------------------------------------------- fullscreen

static void SetFullscreen(bool on) {
    if (on == g.fullscreen) return;
    DWORD style = GetWindowLongW(g.hwnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(g.hwnd, &g.wpPrev) ||
            !GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g.fullscreen = true;
        SetWindowLongW(g.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetTimer(g.hwnd, 1, 1500, nullptr);
    } else {
        g.fullscreen = false;
        SetWindowLongW(g.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g.hwnd, &g.wpPrev);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        KillTimer(g.hwnd, 1);
        g.cursorHidden = false;
    }
    Layout();
    Redraw();
}

// ---------------------------------------------------------------- rendering

static void DrawCropOverlay(Tile& t, const View& v) {
    CropState& c = g.crop;
    float ix0 = std::min(c.x0, c.x1), ix1 = std::max(c.x0, c.x1), iy0 = std::min(c.y0, c.y1), iy1 = std::max(c.y0, c.y1);
    D2D1_RECT_F img = {v.ox, v.oy, v.ox + t.w * v.z, v.oy + t.h * v.z};
    D2D1_RECT_F r = {std::floor(v.ox + ix0 * v.z), std::floor(v.oy + iy0 * v.z), std::floor(v.ox + ix1 * v.z),
                     std::floor(v.oy + iy1 * v.z)};
    auto dim = B(0x000000, 0.6f);
    g_rt->FillRectangle(D2D1::RectF(img.left, img.top, img.right, r.top), dim);
    g_rt->FillRectangle(D2D1::RectF(img.left, r.bottom, img.right, img.bottom), dim);
    g_rt->FillRectangle(D2D1::RectF(img.left, r.top, r.left, r.bottom), dim);
    g_rt->FillRectangle(D2D1::RectF(r.right, r.top, img.right, r.bottom), dim);
    // rule-of-thirds guides
    auto thin = B(0xFFFFFF, 0.25f);
    for (int k = 1; k < 3; k++) {
        float x = std::floor(r.left + (r.right - r.left) * k / 3), y = std::floor(r.top + (r.bottom - r.top) * k / 3);
        g_rt->FillRectangle(D2D1::RectF(x, r.top, x + 1, r.bottom), thin);
        g_rt->FillRectangle(D2D1::RectF(r.left, y, r.right, y + 1), thin);
    }
    auto wb = B(0xFFFFFF, 0.95f);
    g_rt->FillRectangle(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.top), wb);
    g_rt->FillRectangle(D2D1::RectF(r.left - 1, r.bottom, r.right + 1, r.bottom + 1), wb);
    g_rt->FillRectangle(D2D1::RectF(r.left - 1, r.top, r.left, r.bottom), wb);
    g_rt->FillRectangle(D2D1::RectF(r.right, r.top, r.right + 1, r.bottom), wb);
    float hs = std::round(4 * g.scale);
    float hx[3] = {r.left, (r.left + r.right) / 2, r.right}, hy[3] = {r.top, (r.top + r.bottom) / 2, r.bottom};
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            if (a == 1 && b == 1) continue;
            D2D1_RECT_F h = {std::round(hx[a] - hs), std::round(hy[b] - hs), std::round(hx[a] + hs), std::round(hy[b] + hs)};
            g_rt->FillRectangle(h, B(0xFFFFFF));
            g_rt->DrawRectangle(h, B(0x000000, 0.6f), 1);
        }
    std::wstring dims = Fmt(L"%d × %d", (int)std::round(ix1 - ix0), (int)std::round(iy1 - iy0));
    float lw = 110 * g.scale, lh = 22 * g.scale;
    float lx = std::max(t.cell.left + 4, std::min(r.left, t.cell.right - lw - 4));
    float ly = r.top - lh - 6 * g.scale;
    if (ly < t.cell.top + 4) ly = r.top + 6 * g.scale;
    D2D1_ROUNDED_RECT rr = {{lx, ly, lx + lw, ly + lh}, 4 * g.scale, 4 * g.scale};
    g_rt->FillRoundedRectangle(&rr, B(0x000000, 0.7f));
    D2D1_RECT_F tr = {lx + 8 * g.scale, ly, lx + lw - 4, ly + lh};
    g_rt->DrawText(dims.c_str(), (UINT32)dims.size(), g_tfSmall, &tr, B(0xFFFFFF));
}

static void DrawTile(Tile& t, int idx) {
    D2D1_RECT_F cell = t.cell;
    g_rt->PushAxisAlignedClip(&cell, D2D1_ANTIALIAS_MODE_ALIASED);
    g_rt->FillRectangle(&cell, B(g.fullscreen ? 0x000000 : 0x1B1B1B));

    if (t.HasImage() && t.chunks.empty() && !t.gpuFail) {
        if (t.cpu) BuildChunks(t);
        else if (!t.loading) StartLoad(t, true); // device was reset: reload in the background
    }

    if (t.HasImage() && !t.chunks.empty()) {
        View v = GetView(t);
        D2D1_RECT_F dest = {v.ox, v.oy, v.ox + t.w * v.z, v.oy + t.h * v.z};
        if (t.alpha && g_checker) {
            D2D1_RECT_F d = {std::max(dest.left, cell.left), std::max(dest.top, cell.top),
                             std::min(dest.right, cell.right), std::min(dest.bottom, cell.bottom)};
            if (d.right > d.left && d.bottom > d.top) g_rt->FillRectangle(&d, g_checker);
        }
        bool nearest = v.z >= 0.999f;
        for (auto& c : t.chunks) {
            float ix0 = std::max<float>(c.x, std::floor((cell.left - v.ox) / v.z));
            float ix1 = std::min<float>(c.x + c.w, std::ceil((cell.right - v.ox) / v.z));
            float iy0 = std::max<float>(c.y, std::floor((cell.top - v.oy) / v.z));
            float iy1 = std::min<float>(c.y + c.h, std::ceil((cell.bottom - v.oy) / v.z));
            if (ix0 >= ix1 || iy0 >= iy1) continue;
            D2D1_RECT_F src = {ix0 - c.x, iy0 - c.y, ix1 - c.x, iy1 - c.y};
            D2D1_RECT_F dst = {v.ox + ix0 * v.z, v.oy + iy0 * v.z, v.ox + ix1 * v.z, v.oy + iy1 * v.z};
            if (g_dc)
                g_dc->DrawBitmap(c.bmp, &dst, 1.f,
                                 nearest ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                 &src, nullptr);
            else
                g_rt->DrawBitmap(c.bmp, &dst, 1.f,
                                 nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                 &src);
        }
        // pixel grid when zoomed in far enough to see individual pixels
        if (g.grid && v.z >= 8) {
            int ix0 = std::max(0, (int)std::floor((cell.left - v.ox) / v.z));
            int ix1 = std::min((int)t.w, (int)std::ceil((cell.right - v.ox) / v.z));
            int iy0 = std::max(0, (int)std::floor((cell.top - v.oy) / v.z));
            int iy1 = std::min((int)t.h, (int)std::ceil((cell.bottom - v.oy) / v.z));
            float top = std::max(cell.top, dest.top), bot = std::min(cell.bottom, dest.bottom);
            float lef = std::max(cell.left, dest.left), rig = std::min(cell.right, dest.right);
            auto gb = B(0x000000, v.z >= 16 ? 0.35f : 0.2f);
            for (int x = ix0; x <= ix1; x++) { float X = v.ox + x * v.z; g_rt->FillRectangle(D2D1::RectF(X, top, X + 1, bot), gb); }
            for (int y = iy0; y <= iy1; y++) { float Y = v.oy + y * v.z; g_rt->FillRectangle(D2D1::RectF(lef, Y, rig, Y + 1), gb); }
        }
        if (g.crop.active && g.crop.tileId == t.id) DrawCropOverlay(t, v);
    } else {
        std::wstring msg;
        if (t.loading) msg = L"Loading…";
        else if (t.gpuFail) msg = L"Not enough video memory to show " + FileName(t.path);
        else if (!t.err.empty()) msg = L"Can't open " + FileName(t.path) + L"\n\n" + t.err;
        D2D1_RECT_F r = {cell.left + 16, cell.top, cell.right - 16, cell.bottom};
        g_rt->DrawText(msg.c_str(), (UINT32)msg.size(), g_tfCenter, &r, B(0x9A9A9A));
    }

    // filename label (windowed, more than one tile)
    if (!g.fullscreen && g.labels && g.tiles.size() > 1) {
        std::wstring lab = (t.edits ? L"• " : L"") + FileName(t.path);
        if (t.HasImage()) lab += Fmt(L"   %d%%", (int)std::round(GetView(t).z * 100));
        float maxw = CW(t) - 16 * g.scale;
        Com<IDWriteTextLayout> lay;
        if (maxw > 40 && SUCCEEDED(g_dwf->CreateTextLayout(lab.c_str(), (UINT32)lab.size(), g_tfSmall, maxw, 20 * g.scale, &lay))) {
            DWRITE_TEXT_METRICS m;
            lay->GetMetrics(&m);
            float pw = std::min(maxw, m.widthIncludingTrailingWhitespace + 16 * g.scale), ph = 22 * g.scale;
            float x = cell.left + 8 * g.scale, y = cell.top + 8 * g.scale;
            D2D1_ROUNDED_RECT rr = {{x, y, x + pw, y + ph}, 4 * g.scale, 4 * g.scale};
            g_rt->FillRoundedRectangle(&rr, B(0x000000, 0.55f));
            D2D1_RECT_F tr = {x + 8 * g.scale, y, x + pw - 6 * g.scale, y + ph};
            g_rt->DrawText(lab.c_str(), (UINT32)lab.size(), g_tfSmall, &tr, B(0xFFFFFF, 0.92f));
        }
    }
    g_rt->PopAxisAlignedClip();

    if (!g.fullscreen && g.tiles.size() > 1 && idx == g.focus) {
        float k = std::max(1.f, std::round(2 * g.scale));
        auto ab = B(0x4FA3FF, 0.9f);
        g_rt->FillRectangle(D2D1::RectF(cell.left, cell.top, cell.right, cell.top + k), ab);
        g_rt->FillRectangle(D2D1::RectF(cell.left, cell.bottom - k, cell.right, cell.bottom), ab);
        g_rt->FillRectangle(D2D1::RectF(cell.left, cell.top, cell.left + k, cell.bottom), ab);
        g_rt->FillRectangle(D2D1::RectF(cell.right - k, cell.top, cell.right, cell.bottom), ab);
    }
}

static void DrawStatus(float W, float H) {
    float sh = StatusH();
    D2D1_RECT_F bar = {0, H - sh, W, H};
    g_rt->FillRectangle(&bar, B(0x202020));
    g_rt->FillRectangle(D2D1::RectF(0, H - sh, W, H - sh + 1), B(0x2E2E2E));
    std::wstring left, right;
    Tile* f = Focused();
    if (f) {
        left = (f->edits ? L"• " : L"") + FileName(f->path);
        if (f->HasImage()) {
            left += Fmt(L"     %u × %u", f->w, f->h);
            if (!f->fmt.empty()) left += L"     " + f->fmt;
            left += Fmt(L"     %d%%", (int)std::round(GetView(*f).z * 100));
        }
        if (f->sibsLoaded && f->sibIdx >= 0 && f->sibs.size() > 1) left += Fmt(L"     %d / %d", f->sibIdx + 1, (int)f->sibs.size());
    }
    if (g.hoverTile >= 0 && g.hoverX >= 0) right += Fmt(L"x %d  y %d", g.hoverX, g.hoverY);
    if (g.tiles.size() > 1) right += Fmt(L"%s%d images", right.empty() ? L"" : L"     ", (int)g.tiles.size());
    if (g.sync) right += L"     Sync";
    right += L"     H for help";
    float pad = 12 * g.scale;
    D2D1_RECT_F rr = {W * 0.6f, H - sh, W - pad, H};
    D2D1_RECT_F lr = {pad, H - sh, W * 0.6f - pad, H};
    g_rt->DrawText(left.c_str(), (UINT32)left.size(), g_tfStatusL, &lr, B(0xD0D0D0));
    g_rt->DrawText(right.c_str(), (UINT32)right.size(), g_tfStatusR, &rr, B(0x8C8C8C));
}

static const wchar_t* kHelp =
    L"Opening\n"
    L"  Ctrl+O\tAdd images (multi-select)\n"
    L"  Drag & drop\tAdd files or whole folders\n"
    L"  ← → / PgUp PgDn\tPrevious / next image in folder\n"
    L"  Home / End\tFirst / last in folder\n"
    L"  Tab\tFocus next tile\n"
    L"  Ctrl+W / middle-click\tClose tile    (Ctrl+Shift+W: close all)\n\n"
    L"Viewing\n"
    L"  Mouse wheel\tZoom tile under cursor\n"
    L"  Drag\tPan\n"
    L"  1\tActual pixels (100%)\n"
    L"  0\tFit\n"
    L"  + / −\tZoom in / out\n"
    L"  S\tSync zoom & pan across all tiles\n"
    L"  G\tPixel grid at high zoom\n"
    L"  I\tFilename labels\n"
    L"  F / F11 / double-click\tFullscreen (Esc to leave)\n\n"
    L"Editing\n"
    L"  R / Shift+R\tRotate right / left\n"
    L"  C\tCrop (Enter applies, Esc cancels)\n"
    L"  Ctrl+Z\tUndo\n"
    L"  Ctrl+S\tSave    (Ctrl+Shift+S: save a copy)\n"
    L"  Ctrl+C\tCopy image\n\n"
    L"Right-click for everything else, including “Make default photo viewer”.";

static void DrawHelp(float W, float H) {
    g_rt->FillRectangle(D2D1::RectF(0, 0, W, H), B(0x000000, 0.55f));
    float pw = std::min(W - 32, 560 * g.scale), ph = std::min(H - 32, 640 * g.scale);
    float x = (W - pw) / 2, y = (H - ph) / 2;
    D2D1_ROUNDED_RECT rr = {{x, y, x + pw, y + ph}, 10 * g.scale, 10 * g.scale};
    g_rt->FillRoundedRectangle(&rr, B(0x242424, 0.97f));
    g_rt->DrawRoundedRectangle(&rr, B(0x3A3A3A), 1);
    Com<IDWriteTextLayout> lay;
    std::wstring s = kHelp;
    if (SUCCEEDED(g_dwf->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_tfHelp, pw - 48 * g.scale, ph - 40 * g.scale, &lay))) {
        lay->SetIncrementalTabStop(190 * g.scale);
        // headings in bold
        const wchar_t* heads[] = {L"Opening", L"Viewing", L"Editing"};
        for (auto* h : heads) {
            size_t p = s.find(h);
            if (p != std::wstring::npos) {
                DWRITE_TEXT_RANGE r{(UINT32)p, (UINT32)wcslen(h)};
                lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
            }
        }
        g_rt->DrawTextLayout(D2D1::Point2F(x + 24 * g.scale, y + 20 * g.scale), lay, B(0xE6E6E6));
    }
}

static void Render() {
    if (!EnsureDevice()) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    float W = (float)rc.right, H = (float)rc.bottom;
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    g_rt->Clear(Col(g.fullscreen ? 0x000000 : 0x111111));
    for (int i = 0; i < (int)g.tiles.size(); i++) DrawTile(*g.tiles[i], i);
    if (g.tiles.empty()) {
        std::wstring s = L"Drop images here, or press Ctrl+O\n\nOpen as many as you like — they tile automatically.";
        D2D1_RECT_F r = {0, 0, W, H - StatusH()};
        g_rt->DrawText(s.c_str(), (UINT32)s.size(), g_tfCenter, &r, B(0x8A8A8A));
    }
    if (!g.fullscreen) DrawStatus(W, H);
    if (!g.toast.empty() && GetTickCount() < g.toastUntil) {
        Com<IDWriteTextLayout> lay;
        if (SUCCEEDED(g_dwf->CreateTextLayout(g.toast.c_str(), (UINT32)g.toast.size(), g_tfSmall, W - 40, 24 * g.scale, &lay))) {
            DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
            float tw = m.widthIncludingTrailingWhitespace + 28 * g.scale, th = 32 * g.scale;
            float x = (W - tw) / 2, y = H - StatusH() - th - 18 * g.scale;
            D2D1_ROUNDED_RECT rr = {{x, y, x + tw, y + th}, th / 2, th / 2};
            g_rt->FillRoundedRectangle(&rr, B(0x000000, 0.78f));
            g_rt->DrawTextLayout(D2D1::Point2F(x + 14 * g.scale, y + (th - m.height) / 2), lay, B(0xFFFFFF));
        }
    }
    if (g.help) DrawHelp(W, H);
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { ReleaseDeviceResources(); Redraw(); }
}

// ---------------------------------------------------------------- crop interaction

static int CropHit(Tile& t, float sx, float sy) {
    View v = GetView(t);
    CropState& c = g.crop;
    float l = v.ox + std::min(c.x0, c.x1) * v.z, r = v.ox + std::max(c.x0, c.x1) * v.z;
    float tp = v.oy + std::min(c.y0, c.y1) * v.z, b = v.oy + std::max(c.y0, c.y1) * v.z;
    float m = 7 * g.scale;
    int mask = 0;
    bool inY = sy >= tp - m && sy <= b + m, inX = sx >= l - m && sx <= r + m;
    if (inY && std::fabs(sx - l) <= m) mask |= 1;
    if (inY && std::fabs(sx - r) <= m) mask |= 2;
    if (inX && std::fabs(sy - tp) <= m) mask |= 4;
    if (inX && std::fabs(sy - b) <= m) mask |= 8;
    if ((mask & 3) == 3) mask &= ~(std::fabs(sx - l) < std::fabs(sx - r) ? 2 : 1);
    if ((mask & 12) == 12) mask &= ~(std::fabs(sy - tp) < std::fabs(sy - b) ? 8 : 4);
    if (mask) return mask;
    if (sx > l && sx < r && sy > tp && sy < b) return 16;
    return 32;
}

static LPCWSTR CursorForCrop(int m) {
    if (m == 16) return IDC_SIZEALL;
    if (m == 32) return IDC_CROSS;
    if (m == (1 | 4) || m == (2 | 8)) return IDC_SIZENWSE;
    if (m == (2 | 4) || m == (1 | 8)) return IDC_SIZENESW;
    if (m & 3) return IDC_SIZEWE;
    if (m & 12) return IDC_SIZENS;
    return IDC_ARROW;
}

static void CropDrag(Tile& t, float sx, float sy, bool shift) {
    View v = GetView(t);
    float ix = std::round((sx - v.ox) / v.z), iy = std::round((sy - v.oy) / v.z);
    ix = std::max(0.f, std::min((float)t.w, ix));
    iy = std::max(0.f, std::min((float)t.h, iy));
    CropState& c = g.crop;
    if (c.drag == 32) {
        float dx = ix - c.ax, dy = iy - c.ay;
        if (shift) {
            float s = std::min(std::fabs(dx), std::fabs(dy));
            dx = dx < 0 ? -s : s; dy = dy < 0 ? -s : s;
        }
        c.x0 = c.ax; c.y0 = c.ay; c.x1 = c.ax + dx; c.y1 = c.ay + dy;
    } else if (c.drag == 16) {
        float dx = ix - c.ax, dy = iy - c.ay;
        float w = c.s1x - c.s0x, h = c.s1y - c.s0y;
        float nx = std::max(0.f, std::min((float)t.w - w, c.s0x + dx));
        float ny = std::max(0.f, std::min((float)t.h - h, c.s0y + dy));
        c.x0 = nx; c.y0 = ny; c.x1 = nx + w; c.y1 = ny + h;
    } else {
        c.x0 = c.s0x; c.y0 = c.s0y; c.x1 = c.s1x; c.y1 = c.s1y;
        if (c.drag & 1) c.x0 = ix;
        if (c.drag & 2) c.x1 = ix;
        if (c.drag & 4) c.y0 = iy;
        if (c.drag & 8) c.y1 = iy;
        if (shift && (c.drag & 3) && (c.drag & 12)) {
            float w = std::fabs(c.x1 - c.x0), h = std::fabs(c.y1 - c.y0), s = std::min(w, h);
            if (c.drag & 1) c.x0 = c.x1 - s; else c.x1 = c.x0 + s;
            if (c.drag & 4) c.y0 = c.y1 - s; else c.y1 = c.y0 + s;
        }
    }
}

// ---------------------------------------------------------------- input

static void OnWheel(float sx, float sy, int steps) {
    int hi = HitTile(sx, sy);
    if (hi < 0) return;
    Tile& src = *g.tiles[hi];
    float rx = sx - (src.cell.left + src.cell.right) / 2, ry = sy - (src.cell.top + src.cell.bottom) / 2;
    int dir = steps > 0 ? 1 : -1;
    for (int s = 0; s < std::abs(steps); s++) {
        if (g.sync) {
            for (auto& t : g.tiles) {
                float px = (t->cell.left + t->cell.right) / 2 + rx, py = (t->cell.top + t->cell.bottom) / 2 + ry;
                StepZoomAt(*t, dir, px, py);
            }
        } else StepZoomAt(src, dir, sx, sy);
    }
    Redraw();
}

static void Pan(Tile& origin, float dx, float dy) {
    auto pan = [&](Tile& t) {
        if (!t.HasImage()) return;
        if (t.fit) return;
        View v = GetView(t);
        t.cx -= dx / v.z; t.cy -= dy / v.z;
        ClampCenter(t);
    };
    if (g.sync) for (auto& t : g.tiles) pan(*t);
    else pan(origin);
}

static void ZoomKey(int dir) {
    if (g.sync) {
        for (auto& t : g.tiles) StepZoomAt(*t, dir, (t->cell.left + t->cell.right) / 2, (t->cell.top + t->cell.bottom) / 2);
    } else if (Tile* t = Focused()) StepZoomAt(*t, dir, (t->cell.left + t->cell.right) / 2, (t->cell.top + t->cell.bottom) / 2);
    Redraw();
}

enum {
    ID_OPEN = 100, ID_CLOSE, ID_CLOSEALL, ID_ROTR, ID_ROTL, ID_CROP, ID_APPLYCROP, ID_UNDO, ID_SAVE, ID_SAVEAS, ID_COPY,
    ID_FIT, ID_ACTUAL, ID_SYNC, ID_GRID, ID_LABELS, ID_FULL, ID_EXPLORER, ID_REGISTER, ID_UNREGISTER, ID_HELP, ID_PREV, ID_NEXT
};

static void DoCommand(int id) {
    Tile* f = Focused();
    switch (id) {
    case ID_OPEN: ShowOpenDialog(); break;
    case ID_CLOSE: CloseTile(g.focus); break;
    case ID_CLOSEALL:
        if (ConfirmDiscardAll()) { g.crop.active = false; g.tiles.clear(); g.focus = -1; UpdateTitle(); Redraw(); }
        break;
    case ID_ROTR: if (f) RotateTile(*f, true); break;
    case ID_ROTL: if (f) RotateTile(*f, false); break;
    case ID_CROP: if (f) BeginCrop(*f); break;
    case ID_APPLYCROP: ApplyCrop(); break;
    case ID_UNDO: if (f) UndoTile(*f); break;
    case ID_SAVE: if (f) SaveTile(*f, false); break;
    case ID_SAVEAS: if (f) SaveTile(*f, true); break;
    case ID_COPY: if (f) CopyToClipboard(*f); break;
    case ID_FIT:
        if (g.sync) for (auto& t : g.tiles) t->fit = true; else if (f) f->fit = true;
        Redraw(); break;
    case ID_ACTUAL:
        if (g.sync) for (auto& t : g.tiles) SetActualPixels(*t); else if (f) SetActualPixels(*f);
        Redraw(); break;
    case ID_SYNC: g.sync = !g.sync; Toast(g.sync ? L"Sync on: zoom and pan move all tiles together" : L"Sync off"); break;
    case ID_GRID: g.grid = !g.grid; Toast(g.grid ? L"Pixel grid on (shows at 800%+)" : L"Pixel grid off"); break;
    case ID_LABELS: g.labels = !g.labels; Redraw(); break;
    case ID_FULL: SetFullscreen(!g.fullscreen); break;
    case ID_EXPLORER: if (f) ShowInExplorer(*f); break;
    case ID_PREV: if (f) Navigate(*f, -1); break;
    case ID_NEXT: if (f) Navigate(*f, 1); break;
    case ID_REGISTER:
        RegisterApp();
        MessageBoxW(g.hwnd,
                    L"Tessera is now registered as a photo viewer.\n\n"
                    L"Windows only lets you choose the default yourself, so Settings will open next. "
                    L"Click “Set default” on the Tessera page (or pick Tessera for each image type).",
                    kAppName, MB_ICONINFORMATION);
        OpenDefaultAppsSettings();
        break;
    case ID_UNREGISTER:
        UnregisterApp();
        MessageBoxW(g.hwnd, L"Tessera's file associations have been removed.", kAppName, MB_ICONINFORMATION);
        break;
    case ID_HELP: g.help = !g.help; Redraw(); break;
    }
}

static void ShowContextMenu(int x, int y) {
    Tile* f = Focused();
    bool img = f && f->HasImage();
    HMENU m = CreatePopupMenu();
    auto add = [&](int id, const wchar_t* s, bool enabled = true, bool checked = false) {
        AppendMenuW(m, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, s);
    };
    auto sep = [&] { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    add(ID_OPEN, L"Add images…\tCtrl+O");
    if (f) {
        add(ID_PREV, L"Previous in folder\t←");
        add(ID_NEXT, L"Next in folder\t→");
    }
    sep();
    if (g.crop.active) add(ID_APPLYCROP, L"Apply crop\tEnter");
    add(ID_ROTR, L"Rotate right\tR", img);
    add(ID_ROTL, L"Rotate left\tShift+R", img);
    add(ID_CROP, L"Crop\tC", img && !g.crop.active);
    add(ID_UNDO, L"Undo\tCtrl+Z", f && !f->undo.empty());
    sep();
    add(ID_SAVE, L"Save\tCtrl+S", img);
    add(ID_SAVEAS, L"Save a copy…\tCtrl+Shift+S", img);
    add(ID_COPY, L"Copy image\tCtrl+C", img);
    add(ID_EXPLORER, L"Show in Explorer", f != nullptr);
    sep();
    add(ID_FIT, L"Fit\t0", img);
    add(ID_ACTUAL, L"Actual pixels\t1", img);
    add(ID_SYNC, L"Sync zoom && pan\tS", true, g.sync);
    add(ID_GRID, L"Pixel grid\tG", true, g.grid);
    add(ID_LABELS, L"Filename labels\tI", true, g.labels);
    add(ID_FULL, L"Fullscreen\tF11", true, g.fullscreen);
    sep();
    add(ID_CLOSE, L"Close image\tCtrl+W", f != nullptr);
    add(ID_CLOSEALL, L"Close all\tCtrl+Shift+W", !g.tiles.empty());
    sep();
    add(ID_REGISTER, L"Make default photo viewer…");
    add(ID_UNREGISTER, L"Remove file associations");
    add(ID_HELP, L"Keyboard shortcuts\tH");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

static void OnKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    Tile* f = Focused();
    if (ctrl) {
        switch (vk) {
        case 'O': DoCommand(ID_OPEN); return;
        case 'S': DoCommand(shift ? ID_SAVEAS : ID_SAVE); return;
        case 'W': DoCommand(shift ? ID_CLOSEALL : ID_CLOSE); return;
        case 'C': DoCommand(ID_COPY); return;
        case 'Z': DoCommand(ID_UNDO); return;
        case '0': case VK_NUMPAD0: DoCommand(ID_FIT); return;
        case '1': case VK_NUMPAD1: DoCommand(ID_ACTUAL); return;
        case VK_OEM_PLUS: case VK_ADD: ZoomKey(1); return;
        case VK_OEM_MINUS: case VK_SUBTRACT: ZoomKey(-1); return;
        }
        return;
    }
    switch (vk) {
    case VK_ESCAPE:
        if (g.help) { g.help = false; Redraw(); }
        else if (g.crop.active) { g.crop.active = false; Redraw(); }
        else if (g.fullscreen) SetFullscreen(false);
        break;
    case VK_RETURN: if (g.crop.active) ApplyCrop(); else SetFullscreen(!g.fullscreen); break;
    case VK_F11: case 'F': SetFullscreen(!g.fullscreen); break;
    case VK_F1: case 'H': DoCommand(ID_HELP); break;
    case 'R': DoCommand(shift ? ID_ROTL : ID_ROTR); break;
    case 'L': DoCommand(ID_ROTL); break;
    case 'C': DoCommand(ID_CROP); break;
    case 'S': DoCommand(ID_SYNC); break;
    case 'G': DoCommand(ID_GRID); break;
    case 'I': DoCommand(ID_LABELS); break;
    case '0': case VK_NUMPAD0: DoCommand(ID_FIT); break;
    case '1': case VK_NUMPAD1: DoCommand(ID_ACTUAL); break;
    case VK_OEM_PLUS: case VK_ADD: ZoomKey(1); break;
    case VK_OEM_MINUS: case VK_SUBTRACT: ZoomKey(-1); break;
    case VK_LEFT: case VK_PRIOR: case VK_BACK: if (f) Navigate(*f, -1); break;
    case VK_RIGHT: case VK_NEXT: case VK_SPACE: if (f) Navigate(*f, 1); break;
    case VK_HOME: if (f) Navigate(*f, -2); break;
    case VK_END: if (f) Navigate(*f, 2); break;
    case VK_TAB:
        if (!g.tiles.empty()) {
            int n = (int)g.tiles.size();
            g.focus = ((g.focus < 0 ? 0 : g.focus) + (shift ? n - 1 : 1)) % n;
            UpdateTitle();
            Redraw();
        }
        break;
    }
}

static void OnDropFiles(HDROP hd) {
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> files;
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(hd, i, nullptr, 0);
        std::wstring s(len, 0);
        DragQueryFileW(hd, i, &s[0], len + 1);
        files.push_back(s);
    }
    DragFinish(hd);
    OpenFiles(files, false);
    SetForegroundWindow(g.hwnd);
}

static void UpdateHover(float x, float y) {
    int hi = HitTile(x, y);
    int ox = g.hoverX, oy = g.hoverY, ot = g.hoverTile;
    g.hoverTile = hi;
    g.hoverX = g.hoverY = -1;
    if (hi >= 0) {
        float ix, iy;
        Tile& t = *g.tiles[hi];
        if (t.HasImage() && ScreenToImage(t, x, y, ix, iy)) { g.hoverX = (int)ix; g.hoverY = (int)iy; }
    }
    if (!g.fullscreen && (ox != g.hoverX || oy != g.hoverY || ot != g.hoverTile)) Redraw();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(std::max(1, (int)LOWORD(lp)), std::max(1, (int)HIWORD(lp))));
        Layout();
        Redraw();
        return 0;
    case WM_DPICHANGED: {
        g.scale = HIWORD(wp) / 96.f;
        CreateTextFormats();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        Redraw();
        return 0;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {320, 240}; return 0;
    case WM_APP_LOADED: OnLoaded((LoadResult*)lp); return 0;
    case WM_COPYDATA: {
        auto* cds = (COPYDATASTRUCT*)lp;
        if (cds->dwData != kCopyDataMagic) return FALSE;
        std::vector<std::wstring> files;
        const wchar_t* p = (const wchar_t*)cds->lpData;
        size_t n = cds->cbData / sizeof(wchar_t);
        std::wstring cur;
        for (size_t i = 0; i < n; i++) {
            if (p[i] == 0) { if (!cur.empty()) files.push_back(cur); cur.clear(); }
            else cur += p[i];
        }
        if (!cur.empty()) files.push_back(cur);
        // Explorer launches one process per selected file; ones arriving together become tiles.
        DWORD now = GetTickCount();
        bool batch = now - g.lastExternalOpen < 1500 && !g.tiles.empty();
        g.lastExternalOpen = now;
        if (!files.empty()) OpenFiles(files, !batch);
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return TRUE;
    }
    case WM_DROPFILES: OnDropFiles((HDROP)wp); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) { SetFullscreen(!g.fullscreen); return 0; }
        if (msg == WM_SYSKEYDOWN) break;
        OnKey(wp);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        g.wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = g.wheelAccum / WHEEL_DELTA;
        g.wheelAccum -= steps * WHEEL_DELTA;
        if (steps) OnWheel((float)pt.x, (float)pt.y, steps);
        UpdateHover((float)pt.x, (float)pt.y);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g.help) { g.help = false; Redraw(); return 0; }
        int hi = HitTile(x, y);
        if (hi < 0) return 0;
        if (g.focus != hi) { g.focus = hi; UpdateTitle(); Redraw(); }
        Tile& t = *g.tiles[hi];
        g.lastPt = {(LONG)x, (LONG)y};
        g.dragTile = t.id;
        if (g.crop.active && g.crop.tileId == t.id) {
            int m = CropHit(t, x, y);
            View v = GetView(t);
            g.crop.drag = m;
            g.crop.ax = std::max(0.f, std::min((float)t.w, std::round((x - v.ox) / v.z)));
            g.crop.ay = std::max(0.f, std::min((float)t.h, std::round((y - v.oy) / v.z)));
            NormalizeCrop(t);
            g.crop.s0x = g.crop.x0; g.crop.s0y = g.crop.y0; g.crop.s1x = g.crop.x1; g.crop.s1y = g.crop.y1;
            g.dragMode = 2;
        } else {
            g.dragMode = 1;
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g.fullscreen) {
            if (g.cursorHidden) { g.cursorHidden = false; SetCursor(LoadCursor(nullptr, IDC_ARROW)); }
            SetTimer(hwnd, 1, 1500, nullptr);
        }
        Tile* t = TileById(g.dragTile);
        if ((g.dragMode == 1 || g.dragMode == 3) && t) {
            Pan(*t, x - g.lastPt.x, y - g.lastPt.y);
            if (std::abs(x - g.downPt.x) + std::abs(y - g.downPt.y) > 4) g.midMoved = true;
            g.lastPt = {(LONG)x, (LONG)y};
            Redraw();
        } else if (g.dragMode == 2 && t) {
            CropDrag(*t, x, y, (wp & MK_SHIFT) != 0);
            Redraw();
        }
        UpdateHover(x, y);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g.dragMode == 2) {
            if (Tile* t = TileById(g.crop.tileId)) {
                NormalizeCrop(*t);
                if (g.crop.x1 - g.crop.x0 < 1 || g.crop.y1 - g.crop.y0 < 1) {
                    g.crop.x0 = g.crop.s0x; g.crop.y0 = g.crop.s0y; g.crop.x1 = g.crop.s1x; g.crop.y1 = g.crop.s1y;
                }
            }
            g.crop.drag = 0;
            Redraw();
        }
        g.dragMode = 0;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        int hi = HitTile(x, y);
        if (g.crop.active && hi >= 0 && g.tiles[hi]->id == g.crop.tileId && CropHit(*g.tiles[hi], x, y) == 16) ApplyCrop();
        else if (!g.crop.active) SetFullscreen(!g.fullscreen);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        int hi = HitTile(x, y);
        if (hi < 0) return 0;
        g.dragTile = g.tiles[hi]->id;
        g.dragMode = 3;
        g.midMoved = false;
        g.lastPt = g.downPt = {(LONG)x, (LONG)y};
        SetCapture(hwnd);
        return 0;
    }
    case WM_MBUTTONUP: {
        bool click = g.dragMode == 3 && !g.midMoved;
        int id = g.dragTile;
        g.dragMode = 0;
        ReleaseCapture();
        if (click) {
            for (int i = 0; i < (int)g.tiles.size(); i++) if (g.tiles[i]->id == id) { CloseTile(i); break; }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        int hi = HitTile(x, y);
        if (hi >= 0 && hi != g.focus) { g.focus = hi; UpdateTitle(); Redraw(); }
        POINT pt = {(LONG)x, (LONG)y};
        ClientToScreen(hwnd, &pt);
        ShowContextMenu(pt.x, pt.y);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (g.fullscreen && g.cursorHidden) { SetCursor(nullptr); return TRUE; }
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            LPCWSTR c = IDC_ARROW;
            if (g.dragMode == 1 || g.dragMode == 3) c = IDC_SIZEALL;
            else if (g.crop.active) {
                Tile* t = TileById(g.crop.tileId);
                int hi = HitTile((float)pt.x, (float)pt.y);
                if (t && hi >= 0 && g.tiles[hi].get() == t) c = CursorForCrop(g.dragMode == 2 ? g.crop.drag : CropHit(*t, (float)pt.x, (float)pt.y));
            }
            SetCursor(LoadCursor(nullptr, c));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wp == 1 && g.fullscreen && g.dragMode == 0) {
            g.cursorHidden = true;
            SetCursor(nullptr);
            KillTimer(hwnd, 1);
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            Redraw();
        }
        return 0;
    case WM_CLOSE:
        if (!ConfirmDiscardAll()) return 0;
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- startup

static void EnableDarkMenus() {
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    auto rgv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW vi{sizeof(vi)};
    if (!rgv || rgv(&vi) != 0 || vi.dwBuildNumber < 17763) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * SetModeFn)(int);
    typedef void(WINAPI * FlushFn)();
    auto setMode = (SetModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2);
    if (flush) flush();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") { RegisterApp(); OpenDefaultAppsSettings(); return 0; }
        if (a == L"--unregister") { UnregisterApp(); return 0; }
        files.push_back(FullPath(a));
    }
    LocalFree(argv);

    // Single instance: hand files to the running window.
    HANDLE mtx = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int i = 0; i < 60 && !other; i++) { other = FindWindowW(kWndClass, nullptr); if (!other) Sleep(50); }
        if (other) {
            std::wstring payload;
            for (auto& f : files) { payload += f; payload += L'\0'; }
            if (payload.empty()) payload.push_back(L'\0');
            COPYDATASTRUCT cds{kCopyDataMagic, (DWORD)(payload.size() * sizeof(wchar_t)), (void*)payload.data()};
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, nullptr);
            return 0;
        }
    }
    (void)mtx;

    EnableDarkMenus();
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&g_d2f)))
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)&g_d2f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dwf);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (!g_d2f || !g_dwf || !g_wic) {
        MessageBoxW(nullptr, L"Direct2D or the Windows Imaging Component isn't available.", kAppName, MB_ICONERROR);
        return 1;
    }
    unsigned hw = std::thread::hardware_concurrency();
    g.loader.Start((int)std::max(1u, std::min(4u, hw ? hw - (hw > 2) : 2)));
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kWndClass, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g.scale = GetDpiForWindow(hwnd) / 96.f;
    CreateTextFormats();
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (LoadPlacement(wp)) {
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) wp.showCmd = SW_SHOWNORMAL;
        if (nShow != SW_SHOWDEFAULT && nShow != SW_SHOWNORMAL) wp.showCmd = nShow;
        SetWindowPlacement(hwnd, &wp);
    } else ShowWindow(hwnd, nShow);
    g.lastExternalOpen = GetTickCount();
    if (!files.empty()) OpenFiles(files, true);
    UpdateTitle();
    // First run (or the exe was moved): register so Windows lists Tessera as a photo viewer.
    if (!IsRegisteredHere()) {
        RegisterApp();
        if (files.empty()) {
            UpdateWindow(hwnd);
            std::wstring msg = std::wstring(kAppName) +
                               L" is installed and now appears in Windows' list of photo viewers.\n\n"
                               L"Make it your default photo viewer now? Settings will open on its page; "
                               L"click \u201CSet default\u201D there.";
            if (MessageBoxW(hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
                OpenDefaultAppsSettings();
        }
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess(0);
}
