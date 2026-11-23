/* Loads wooting_analog_sdk.dll from the exe's folder and prints values. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { uint16_t vid, pid; char *man, *name; uint64_t id; int type, level; } DevInfo;

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 15;
    HMODULE m = LoadLibraryW(L"wooting_analog_sdk.dll");
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    int (*init)(void) = (void *)GetProcAddress(m, "wooting_analog_initialise");
    int (*mode)(unsigned) = (void *)GetProcAddress(m, "wooting_analog_set_keycode_mode");
    int (*devs)(DevInfo **, unsigned) = (void *)GetProcAddress(m, "wooting_analog_get_connected_devices_info");
    int (*rfb)(unsigned short *, float *, unsigned) = (void *)GetProcAddress(m, "wooting_analog_read_full_buffer");
    int r = init();
    printf("initialise -> %d\n", r);
    printf("keycode mode -> %d\n", mode(3));
    for (int i = 0; i < 30; i++) {
        DevInfo *d[4]; int n = devs(d, 4);
        if (n > 0) { printf("device: %04x:%04x %s / %s\n", d[0]->vid, d[0]->pid, d[0]->man, d[0]->name); break; }
        Sleep(100);
    }
    float maxv[256] = {0};
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        unsigned short c[16]; float v[16];
        int n = rfb(c, v, 16);
        for (int i = 0; i < n; i++) if (c[i] < 256 && v[i] > maxv[c[i]]) maxv[c[i]] = v[i];
        if (n > 0) {
            printf("\r");
            for (int i = 0; i < n; i++) printf("%c=%.2f  ", c[i] < 128 ? c[i] : '?', v[i]);
            printf("          ");
            fflush(stdout);
        }
        Sleep(20);
    }
    printf("\nmax:");
    for (int i = 0; i < 256; i++) if (maxv[i] > 0) printf(" %c=%.3f", i, maxv[i]);
    printf("\n");
    return 0;
}
