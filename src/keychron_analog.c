/*
 * Keychron HE -> Wooting Analog SDK shim.
 *
 * Drop-in replacement for the Wooting Analog SDK DLL (wooting_analog_sdk.dll /
 * wooting_analog_wrapper.dll): exposes the subset of its C API used by
 * analog-input mods such as AnalogSense, backed by a Keychron hall-effect
 * keyboard polled over its raw HID interface (usage page 0xFF60) with the
 * stock-firmware command AMC_GET_REALTIME_TRAVEL (0xA9 0x30 row col).
 *
 * Wired USB only (stock firmware does not forward raw HID over the 2.4 GHz
 * receiver). Under Proton the device must be exposed with
 *   PROTON_ENABLE_HIDRAW=0x3434/0x0E20
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <wctype.h>

#define EXPORT __declspec(dllexport)

/* ---- Wooting SDK ABI (subset, from wooting-analog-sdk.h) ---------------- */
enum {
    WA_Ok = 1,
    WA_UnInitialized = -2000,
    WA_NoDevices = -1999,
    WA_DeviceDisconnected = -1998,
    WA_Failure = -1997,
    WA_InvalidArgument = -1996,
    WA_NotAvailable = -1992,
};
enum { KC_HID = 0, KC_SC1 = 1, KC_VK = 2, KC_VK_TRANSLATE = 3 };
enum { EV_CONNECTED = 1, EV_DISCONNECTED = 2 };

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char *manufacturer_name;
    char *device_name;
    uint64_t device_id;
    int device_type;   /* 1 = Keyboard */
    int support_level; /* 1 = Basic */
} DeviceInfo_FFI;

typedef void (*device_cb_t)(int, DeviceInfo_FFI *);

/* ---- Key table ----------------------------------------------------------- */
typedef struct {
    uint8_t row, col;   /* Keychron matrix position */
    uint16_t hid;       /* HID usage (keyboard page) */
    uint16_t sc1;       /* PC scan code set 1 */
    uint16_t vk_us;     /* VK on a US layout */
    char name[8];
} key_def_t;

#define MAX_KEYS 16
static key_def_t g_keys[MAX_KEYS] = {
    {2, 2, 0x1A, 0x11, 'W', "W"},
    {3, 1, 0x04, 0x1E, 'A', "A"},
    {3, 2, 0x16, 0x1F, 'S', "S"},
    {3, 3, 0x07, 0x20, 'D', "D"},
};
static int g_nkeys = 4;

/* ---- State --------------------------------------------------------------- */
static CRITICAL_SECTION g_lock;
static volatile LONG g_initialised;
static volatile LONG g_stop;
static HANDLE g_thread;
static int g_keycode_mode = KC_HID;
static device_cb_t g_cb;

static float g_values[MAX_KEYS];
static bool g_reported_nonzero[MAX_KEYS];
static volatile LONG g_connected;
static DeviceInfo_FFI g_info;
static DeviceInfo_FFI *g_info_ptr = &g_info;
static char g_devname[64] = "Keychron HE";

/* config */
static int g_poll_interval_us = 0;   /* extra delay between full sweeps */
static bool g_focus_only = true;
static bool g_log_raw = false;
static uint16_t g_pids[8] = {0x0E20, 0x0E21, 0x0E22};
static int g_npids = 3;

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* ---- Config (KeychronAnalog.ini next to the DLL) ------------------------- */
static void load_config(void) {
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\KeychronAnalog.ini", g_dir);
    g_poll_interval_us = GetPrivateProfileIntW(L"General", L"ExtraPollDelayUs", 0, ini);
    g_focus_only = GetPrivateProfileIntW(L"General", L"PollOnlyWhenFocused", 1, ini) != 0;
    g_log_raw = GetPrivateProfileIntW(L"General", L"LogRaw", 0, ini) != 0;

    wchar_t buf[512];
    if (GetPrivateProfileStringW(L"General", L"ProductIds", L"", buf, 512, ini) > 0) {
        g_npids = 0;
        wchar_t *ctx = NULL;
        for (wchar_t *tok = wcstok(buf, L", ", &ctx); tok && g_npids < 8; tok = wcstok(NULL, L", ", &ctx))
            g_pids[g_npids++] = (uint16_t)wcstoul(tok, NULL, 0);
    }
    /* [Keys] Key1 = row,col,hid,scancode,vk,name  (overrides the WASD default) */
    int n = 0;
    for (int i = 1; i <= MAX_KEYS; i++) {
        wchar_t k[16]; swprintf(k, 16, L"Key%d", i);
        if (GetPrivateProfileStringW(L"Keys", k, L"", buf, 512, ini) == 0) break;
        unsigned r, c, h, s, v; wchar_t nm[8] = L"?";
        if (swscanf(buf, L"%u , %u , %i , %i , %i , %7ls", &r, &c, &h, &s, &v, nm) >= 5) {
            key_def_t kd = {(uint8_t)r, (uint8_t)c, (uint16_t)h, (uint16_t)s, (uint16_t)v, ""};
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, kd.name, sizeof kd.name, NULL, NULL);
            g_keys[n++] = kd;
        }
    }
    if (n > 0) g_nkeys = n;
}

/* ---- HID ------------------------------------------------------------------ */
static HANDLE g_dev = INVALID_HANDLE_VALUE;
static HANDLE g_rd_ev, g_wr_ev;
static uint16_t g_out_len = 33, g_in_len = 33;
static int g_layout = -1; /* 0: flag at [12] (fw 1.3.x), 1: flag at [13] (published source) */

static int g_scan_logs = 3;

static bool is_keychron_path(const wchar_t *path) {
    wchar_t low[512]; size_t i = 0;
    for (; path[i] && i < 511; i++) low[i] = towlower(path[i]);
    low[i] = 0;
    return wcsstr(low, L"vid_3434") != NULL;
}

static HANDLE open_keychron(uint16_t *pid_out) {
    bool verbose = g_scan_logs > 0;
    if (verbose) g_scan_logs--;
    int n_hid = 0, n_kc = 0;
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = {.cbSize = sizeof ifd};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = malloc(need);
        if (!det) continue;
        det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, NULL, NULL)) {
            n_hid++;
            bool kc = is_keychron_path(det->DevicePath);
            if (kc) n_kc++;
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES a = {.Size = sizeof a};
                bool ok = false;
                if (HidD_GetAttributes(h, &a) && a.VendorID == 0x3434) {
                    for (int p = 0; p < g_npids; p++) if (a.ProductID == g_pids[p]) ok = true;
                }
                if (ok) {
                    PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps;
                    ok = false;
                    if (HidD_GetPreparsedData(h, &pp)) {
                        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && verbose)
                            logf_("  candidate %ls usage %04x:%04x", det->DevicePath, caps.UsagePage, caps.Usage);
                        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                            caps.UsagePage == 0xFF60 && caps.Usage == 0x61) {
                            ok = true;
                            g_out_len = caps.OutputReportByteLength ? caps.OutputReportByteLength : 33;
                            g_in_len = caps.InputReportByteLength ? caps.InputReportByteLength : 33;
                        }
                        HidD_FreePreparsedData(pp);
                    }
                }
                if (ok) {
                    found = h; *pid_out = a.ProductID;
                    wchar_t prod[64] = L"";
                    if (HidD_GetProductString(h, prod, sizeof prod))
                        WideCharToMultiByte(CP_UTF8, 0, prod, -1, g_devname, sizeof g_devname, NULL, NULL);
                    logf_("opened %ls (pid %04x, in %u out %u)", det->DevicePath, a.ProductID, g_in_len, g_out_len);
                } else CloseHandle(h);
            } else if (kc && verbose) {
                logf_("  cannot open %ls (error %lu)", det->DevicePath, GetLastError());
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (verbose && found == INVALID_HANDLE_VALUE)
        logf_("scan: %d HID interfaces, %d Keychron (vid 3434), none usable", n_hid, n_kc);
    return found;
}

static void close_dev(void) {
    if (g_dev != INVALID_HANDLE_VALUE) { CancelIo(g_dev); CloseHandle(g_dev); }
    g_dev = INVALID_HANDLE_VALUE;
}

/* Sends [cmd...] and waits for a reply starting with the same two bytes. */
static bool transact(const uint8_t *req, int reqlen, uint8_t *reply /* 32 bytes */, DWORD timeout_ms) {
    uint8_t out[65] = {0}, in[65];
    memcpy(out + 1, req, reqlen);
    OVERLAPPED ow = {.hEvent = g_wr_ev};
    DWORD n;
    if (!WriteFile(g_dev, out, g_out_len, NULL, &ow) && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(g_wr_ev, timeout_ms) != WAIT_OBJECT_0) { CancelIo(g_dev); return false; }
    if (!GetOverlappedResult(g_dev, &ow, &n, FALSE)) return false;

    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        OVERLAPPED orr = {.hEvent = g_rd_ev};
        if (!ReadFile(g_dev, in, g_in_len, NULL, &orr) && GetLastError() != ERROR_IO_PENDING) return false;
        DWORD now = GetTickCount();
        DWORD left = (int)(deadline - now) > 0 ? deadline - now : 0;
        if (WaitForSingleObject(g_rd_ev, left) != WAIT_OBJECT_0) {
            CancelIo(g_dev);
            GetOverlappedResult(g_dev, &orr, &n, TRUE);
            return false;
        }
        if (!GetOverlappedResult(g_dev, &orr, &n, FALSE)) return false;
        /* in[0] is the report id (0) */
        if (n >= 3 && in[1] == req[0] && in[2] == req[1]) {
            memcpy(reply, in + 1, 32);
            return true;
        }
    }
}

/* Returns travel in [0,1] or -1 on error. */
static float read_key(const key_def_t *k) {
    uint8_t req[4] = {0xA9, 0x30, k->row, k->col}, r[32];
    if (!transact(req, 4, r, 100)) return -1.f;
    if (r[2] != 0 || r[3] != k->row || r[4] != k->col) return -1.f;
    int off;
    if (g_layout < 0) {
        if ((r[12] & 0xFE) == 0x10) g_layout = 0;
        else if ((r[13] & 0xFE) == 0x10) g_layout = 1;
        else return -1.f;
        logf_("reply layout %d detected: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              g_layout, r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12]);
    }
    off = g_layout == 0 ? 6 : 7;
    int value = r[off] | r[off + 1] << 8;
    int zero = r[off + 2] | r[off + 3] << 8;
    int full = r[off + 4] | r[off + 5] << 8;
    if (zero == full) return 0.f;
    float t = (float)(zero - value) / (float)(zero - full);
    if (g_log_raw && t > 0.02f) logf_("%s value %d zero %d full %d -> %.3f", k->name, value, zero, full, t);
    return t < 0.f ? 0.f : t > 1.f ? 1.f : t;
}

static bool game_focused(void) {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void set_disconnected(void) {
    close_dev();
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nkeys; i++) g_values[i] = 0.f;
    LeaveCriticalSection(&g_lock);
    if (InterlockedExchange(&g_connected, 0)) {
        logf_("device disconnected");
        if (g_cb) g_cb(EV_DISCONNECTED, &g_info);
    }
}

static DWORD WINAPI poll_thread(LPVOID unused) {
    (void)unused;
    DWORD last_scan = 0;
    unsigned long long sweeps = 0; DWORD rate_t0 = GetTickCount();
    while (!g_stop) {
        if (g_dev == INVALID_HANDLE_VALUE) {
            if (GetTickCount() - last_scan < 2000 && last_scan) { Sleep(100); continue; }
            last_scan = GetTickCount();
            uint16_t pid = 0;
            g_dev = open_keychron(&pid);
            if (g_dev == INVALID_HANDLE_VALUE) continue;
            g_layout = -1;
            if (read_key(&g_keys[0]) < 0.f) {
                logf_("device opened but realtime-travel query failed; check firmware");
                close_dev();
                continue;
            }
            g_info.vendor_id = 0x3434; g_info.product_id = pid;
            g_info.manufacturer_name = "Keychron"; g_info.device_name = g_devname;
            g_info.device_id = 0x3434ULL << 16 | pid; g_info.device_type = 1; g_info.support_level = 1;
            InterlockedExchange(&g_connected, 1);
            logf_("connected: %s", g_devname);
            if (g_cb) g_cb(EV_CONNECTED, &g_info);
        }
        if (g_focus_only && !game_focused()) {
            EnterCriticalSection(&g_lock);
            for (int i = 0; i < g_nkeys; i++) g_values[i] = 0.f;
            LeaveCriticalSection(&g_lock);
            Sleep(50);
            continue;
        }
        for (int i = 0; i < g_nkeys && !g_stop; i++) {
            float v = read_key(&g_keys[i]);
            if (v < 0.f) { set_disconnected(); break; }
            EnterCriticalSection(&g_lock);
            g_values[i] = v;
            LeaveCriticalSection(&g_lock);
        }
        if (++sweeps % 2000 == 0) {
            DWORD el = GetTickCount() - rate_t0;
            logf_("poll rate: %.0f sweeps/s", 2000000.0 / (el ? el : 1));
            rate_t0 = GetTickCount();
        }
        if (g_poll_interval_us > 0) Sleep((g_poll_interval_us + 999) / 1000);
    }
    close_dev();
    return 0;
}

/* ---- Exported SDK API ---------------------------------------------------- */
EXPORT int wooting_analog_initialise(void) {
    if (InterlockedCompareExchange(&g_initialised, 1, 0)) return g_connected ? 1 : 0;
    load_config();
    g_rd_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wr_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    /* give the device a moment so callers that check devices right away see it */
    for (int i = 0; i < 30 && !g_connected; i++) Sleep(10);
    logf_("initialise -> %d device(s)", g_connected ? 1 : 0);
    return g_connected ? 1 : 0;
}

EXPORT bool wooting_analog_is_initialised(void) { return g_initialised != 0; }

EXPORT int wooting_analog_uninitialise(void) {
    if (!InterlockedExchange(&g_initialised, 0)) return WA_UnInitialized;
    g_stop = 1;
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = NULL; }
    g_connected = 0;
    return WA_Ok;
}

EXPORT int wooting_analog_version(void) { return 0; }
EXPORT const char *wooting_analog_version_semver(void) { return "0.8.0"; }
EXPORT bool wooting_analog_using_sys(void) { return false; }

EXPORT int wooting_analog_set_keycode_mode(unsigned int mode) {
    if (!g_initialised) return WA_UnInitialized;
    if (mode > KC_VK_TRANSLATE) return WA_InvalidArgument;
    g_keycode_mode = (int)mode;
    logf_("keycode mode %u", mode);
    return WA_Ok;
}

static uint16_t code_for(const key_def_t *k) {
    switch (g_keycode_mode) {
    case KC_SC1: return k->sc1;
    case KC_VK: return k->vk_us;
    case KC_VK_TRANSLATE: {
        UINT vk = MapVirtualKeyW(k->sc1, MAPVK_VSC_TO_VK_EX);
        return vk ? (uint16_t)vk : k->vk_us;
    }
    default: return k->hid;
    }
}

EXPORT int wooting_analog_read_full_buffer(unsigned short *codes, float *vals, unsigned int len) {
    if (!g_initialised) return WA_UnInitialized;
    if (!g_connected) return WA_NoDevices;
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nkeys && (unsigned)n < len; i++) {
        float v = g_values[i];
        if (v > 0.f || g_reported_nonzero[i]) {
            codes[n] = code_for(&g_keys[i]);
            vals[n] = v;
            n++;
            g_reported_nonzero[i] = v > 0.f;
        }
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

EXPORT int wooting_analog_read_full_buffer_device(unsigned short *codes, float *vals, unsigned int len, uint64_t id) {
    (void)id;
    return wooting_analog_read_full_buffer(codes, vals, len);
}

/* Digital on/off state of a key, used for keys this shim does not poll and
 * whenever the keyboard is not connected, so callers that overwrite a key's
 * value with read_analog (e.g. AnalogSense) keep those keys working. Only
 * meaningful in the virtual-key code modes. */
static float digital_state(unsigned short code) {
    if (g_keycode_mode != KC_VK && g_keycode_mode != KC_VK_TRANSLATE) return 0.f;
    return (GetAsyncKeyState(code) & 0x8000) ? 1.f : 0.f;
}

EXPORT float wooting_analog_read_analog(unsigned short code) {
    if (!g_initialised) return (float)WA_UnInitialized;
    if (g_connected) {
        bool found = false;
        float v = 0.f;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_nkeys; i++)
            if (code_for(&g_keys[i]) == code) { v = g_values[i]; found = true; }
        LeaveCriticalSection(&g_lock);
        if (found) return v;
    }
    return digital_state(code);
}

EXPORT float wooting_analog_read_analog_device(unsigned short code, uint64_t id) {
    (void)id;
    return wooting_analog_read_analog(code);
}

EXPORT int wooting_analog_get_connected_devices_info(DeviceInfo_FFI **buf, unsigned int len) {
    if (!g_initialised) return WA_UnInitialized;
    if (!g_connected || len == 0) return 0;
    buf[0] = g_info_ptr;
    return 1;
}

EXPORT int wooting_analog_set_device_event_cb(device_cb_t cb) {
    if (!g_initialised) return WA_UnInitialized;
    g_cb = cb;
    return WA_Ok;
}

EXPORT int wooting_analog_clear_device_event_cb(void) {
    if (!g_initialised) return WA_UnInitialized;
    g_cb = NULL;
    return WA_Ok;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        wchar_t *s = wcsrchr(g_dir, L'\\');
        if (s) *s = 0;
        wchar_t lp[MAX_PATH];
        swprintf(lp, MAX_PATH, L"%ls\\KeychronAnalog.log", g_dir);
        g_log = _wfopen(lp, L"w");
        logf_("Keychron analog shim loaded");
    } else if (reason == DLL_PROCESS_DETACH && res == NULL) {
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
