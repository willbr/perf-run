/*
 * perf-run.c -- measure time-to-show of the classic Windows Run dialog.
 *
 * Spawns `rundll32.exe shell32.dll,#61` (the Win+R dialog) and times from
 * CreateProcess to the EVENT_OBJECT_SHOW WinEvent for the dialog's HWND.
 * Closes via WM_CLOSE between iterations so the shell's cached state is
 * preserved (matches steady-state launches, not cold-start).
 *
 * Note: EVENT_OBJECT_SHOW fires when ShowWindow is called -- a few ms
 * before pixels actually hit the screen. This matches what most "time to
 * show" numbers measure (including, presumably, Microsoft's 94 ms quote).
 * For true pixels-on-screen timing you would want DWM ETW present events.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DWORD         target_pid;
    HWND          hwnd_seen;
    LARGE_INTEGER t_shown;
    BOOL          shown;
} Iter;

static Iter          g_iter;
static LARGE_INTEGER g_qpc_freq;

static double qpc_to_ms(LARGE_INTEGER start, LARGE_INTEGER end)
{
    double ticks = (double)(end.QuadPart - start.QuadPart);
    return (ticks * 1000.0) / (double)g_qpc_freq.QuadPart;
}

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild,
                                    DWORD dwEventThread, DWORD dwmsEventTime)
{
    (void)hook; (void)idChild; (void)dwEventThread; (void)dwmsEventTime;

    if (event != EVENT_OBJECT_SHOW) return;
    if (idObject != OBJID_WINDOW)   return;
    if (g_iter.shown)               return;
    if (!hwnd)                      return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_iter.target_pid) return;

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;

    char cls[64] = {0};
    GetClassNameA(hwnd, cls, (int)sizeof(cls));
    /* Classic Run dialog is a standard #32770 dialog. */
    if (strcmp(cls, "#32770") != 0) return;

    QueryPerformanceCounter(&g_iter.t_shown);
    g_iter.hwnd_seen = hwnd;
    g_iter.shown     = TRUE;
}

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const double *sorted, size_t n, double p)
{
    if (n == 0) return 0.0;
    double idx  = (p / 100.0) * (double)(n - 1);
    size_t lo   = (size_t)idx;
    size_t hi   = lo + 1 < n ? lo + 1 : lo;
    double frac = idx - (double)lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static void print_histogram(const double *sorted, size_t n)
{
    if (n < 2) return;
    double lo = sorted[0], hi = sorted[n - 1];
    if (hi <= lo) return;

    enum { BINS = 20, BAR = 40 };
    int    bins[BINS] = {0};
    double width      = (hi - lo) / (double)BINS;

    for (size_t i = 0; i < n; i++) {
        int b = (int)((sorted[i] - lo) / width);
        if (b >= BINS) b = BINS - 1;
        if (b <  0)    b = 0;
        bins[b]++;
    }

    int max = 0;
    for (int i = 0; i < BINS; i++) if (bins[i] > max) max = bins[i];
    if (max == 0) return;

    printf("\nHistogram (%d bins, %.1f-%.1f ms):\n", BINS, lo, hi);
    for (int i = 0; i < BINS; i++) {
        double bin_lo = lo + (double)i * width;
        double bin_hi = bin_lo + width;
        int    bar    = (bins[i] * BAR + max / 2) / max;
        printf("  %6.1f-%6.1f ms | ", bin_lo, bin_hi);
        for (int j = 0; j < bar; j++) putchar('#');
        printf(" %d\n", bins[i]);
    }
}

int main(int argc, char **argv)
{
    int warmup        = 5;
    int measure       = 200;
    int settle_ms     = 50;     /* pause after WM_CLOSE before next iter      */
    int per_iter_to   = 5000;   /* per-iteration deadline waiting for show    */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            measure = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc) {
            settle_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: perf-run [--warmup N] [--n N] [--settle MS]\n"
                   "  --warmup N   discard first N iterations (default 5)\n"
                   "  --n N        measured iterations (default 200)\n"
                   "  --settle MS  pause between iterations (default 50)\n");
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (warmup < 0 || measure <= 0) {
        fprintf(stderr, "warmup must be >= 0 and --n must be > 0\n");
        return 2;
    }

    QueryPerformanceFrequency(&g_qpc_freq);

    char rundll32[MAX_PATH];
    UINT slen = GetSystemDirectoryA(rundll32, MAX_PATH);
    if (slen == 0 || slen >= MAX_PATH - 32) {
        fprintf(stderr, "GetSystemDirectory failed (%lu)\n", GetLastError());
        return 1;
    }
    strcat(rundll32, "\\rundll32.exe");

    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" shell32.dll,#61", rundll32);

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        NULL, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) {
        fprintf(stderr, "SetWinEventHook failed (%lu)\n", GetLastError());
        return 1;
    }

    double *samples = (double *)malloc(sizeof(double) * (size_t)measure);
    if (!samples) {
        fprintf(stderr, "out of memory\n");
        UnhookWinEvent(hook);
        return 1;
    }
    int kept  = 0;
    int total = warmup + measure;

    printf("perf-run: %d warmup + %d measured iterations\n", warmup, measure);
    printf("command: %s\n\n", cmdline);

    for (int i = 0; i < total; i++) {
        memset(&g_iter, 0, sizeof(g_iter));

        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);

        /* CreateProcess may modify the command line in place. */
        char cmd_copy[512];
        strncpy(cmd_copy, cmdline, sizeof(cmd_copy));
        cmd_copy[sizeof(cmd_copy) - 1] = 0;

        LARGE_INTEGER t_spawn;
        QueryPerformanceCounter(&t_spawn);

        if (!CreateProcessA(NULL, cmd_copy, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
            free(samples);
            UnhookWinEvent(hook);
            return 1;
        }
        g_iter.target_pid = pi.dwProcessId;

        DWORD deadline = GetTickCount() + (DWORD)per_iter_to;
        while (!g_iter.shown && GetTickCount() < deadline) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_iter.shown) {
                MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
            }
        }

        if (!g_iter.shown) {
            fprintf(stderr, "iter %d: window never appeared, skipping\n", i);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            continue;
        }

        double ms       = qpc_to_ms(t_spawn, g_iter.t_shown);
        BOOL   is_warm  = (i < warmup);
        printf("  %s %3d: %7.2f ms\n", is_warm ? "warm" : "meas", i, ms);
        if (!is_warm) samples[kept++] = ms;

        PostMessageA(g_iter.hwnd_seen, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
            fprintf(stderr, "iter %d: WM_CLOSE timed out, terminating\n", i);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (settle_ms > 0) Sleep((DWORD)settle_ms);
    }

    UnhookWinEvent(hook);

    if (kept == 0) {
        fprintf(stderr, "no measured samples collected\n");
        free(samples);
        return 1;
    }

    qsort(samples, (size_t)kept, sizeof(double), compare_double);

    double sum = 0.0;
    for (int i = 0; i < kept; i++) sum += samples[i];
    double mean = sum / (double)kept;

    printf("\n=== Results (n=%d) ===\n", kept);
    printf("  min  : %7.2f ms\n", samples[0]);
    printf("  p50  : %7.2f ms\n", percentile(samples, (size_t)kept, 50));
    printf("  p90  : %7.2f ms\n", percentile(samples, (size_t)kept, 90));
    printf("  p99  : %7.2f ms\n", percentile(samples, (size_t)kept, 99));
    printf("  max  : %7.2f ms\n", samples[kept - 1]);
    printf("  mean : %7.2f ms\n", mean);

    print_histogram(samples, (size_t)kept);

    free(samples);
    return 0;
}
