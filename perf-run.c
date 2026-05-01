/*
 * perf-run.c -- measure time-to-show of the classic Windows Run dialog.
 *
 * Default mode: spawn `rundll32.exe shell32.dll,#61` (the Win+R dialog) and
 * time from CreateProcess to the EVENT_OBJECT_SHOW WinEvent for the dialog's
 * HWND. This measures the ShowWindow call -- a few ms before pixels actually
 * hit the screen.
 *
 * Optional --presentmon mode: also drives PresentMon as a child process with
 * an ETW session capturing dwm.exe presents (the classic Run dialog is GDI,
 * so DWM presents the frames containing it -- not rundll32 itself). After
 * the run, parses PresentMon's CSV and attributes the first DWM present at
 * or after each iteration's EVENT_OBJECT_SHOW as the "first frame on screen"
 * proxy. PresentMon's QPCTime column is absolute QPC ticks, directly
 * comparable to QueryPerformanceCounter values.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- shared timing state used by the WinEvent callback ---------- */

typedef struct {
    DWORD         target_pid;
    HWND          hwnd_seen;
    LARGE_INTEGER t_shown;
    BOOL          shown;
} Iter;

static Iter          g_iter;
static LARGE_INTEGER g_qpc_freq;

/* ---------- per-iteration record kept across the whole run ------------- */

typedef struct {
    DWORD         pid;
    LARGE_INTEGER t_spawn;
    LARGE_INTEGER t_show;
    BOOL          shown;       /* EVENT_OBJECT_SHOW captured                */
    LARGE_INTEGER t_present;   /* first DWM present at-or-after t_show      */
    BOOL          presented;
    BOOL          warmup;
} Sample;

/* ---------- helpers ---------------------------------------------------- */

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
    if (g_iter.target_pid == 0)     return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_iter.target_pid) return;

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;

    char cls[64] = {0};
    GetClassNameA(hwnd, cls, (int)sizeof(cls));
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

static void report_metric(const char *label, const double *values, int n)
{
    if (n == 0) {
        printf("=== %s: no samples ===\n", label);
        return;
    }
    double *sorted = (double *)malloc(sizeof(double) * (size_t)n);
    if (!sorted) return;
    memcpy(sorted, values, sizeof(double) * (size_t)n);
    qsort(sorted, (size_t)n, sizeof(double), compare_double);

    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += sorted[i];

    printf("\n=== %s (n=%d) ===\n", label, n);
    printf("  min  : %7.2f ms\n", sorted[0]);
    printf("  p50  : %7.2f ms\n", percentile(sorted, (size_t)n, 50));
    printf("  p90  : %7.2f ms\n", percentile(sorted, (size_t)n, 90));
    printf("  p99  : %7.2f ms\n", percentile(sorted, (size_t)n, 99));
    printf("  max  : %7.2f ms\n", sorted[n - 1]);
    printf("  mean : %7.2f ms\n", sum / (double)n);

    print_histogram(sorted, (size_t)n);
    free(sorted);
}

/* ---------- PresentMon integration ------------------------------------- */

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int find_presentmon(const char *override_path, char *out, size_t out_size)
{
    if (override_path && *override_path) {
        if (!file_exists(override_path)) return 0;
        strncpy(out, override_path, out_size);
        out[out_size - 1] = 0;
        return 1;
    }

    const char *env = getenv("PRESENTMON");
    if (env && *env && file_exists(env)) {
        strncpy(out, env, out_size);
        out[out_size - 1] = 0;
        return 1;
    }

    const char *path = getenv("PATH");
    if (!path) return 0;

    char *dup = _strdup(path);
    if (!dup) return 0;

    const char *names[] = {
        "PresentMon.exe", "presentmon.exe",
        "PresentMon-1.10.0-x64.exe", "PresentMon-2.0.0-x64.exe",
        "PresentMon-2.1.0-x64.exe",  "PresentMon-2.2.0-x64.exe",
        NULL
    };

    int found = 0;
    char *ctx = NULL;
    char *tok = strtok_s(dup, ";", &ctx);
    while (tok && !found) {
        for (int i = 0; names[i] && !found; i++) {
            char candidate[MAX_PATH];
            snprintf(candidate, sizeof(candidate), "%s\\%s", tok, names[i]);
            if (file_exists(candidate)) {
                strncpy(out, candidate, out_size);
                out[out_size - 1] = 0;
                found = 1;
            }
        }
        tok = strtok_s(NULL, ";", &ctx);
    }
    free(dup);
    return found;
}

static HANDLE start_presentmon(const char *exe, const char *csv_path,
                               const char *session_name, DWORD *out_pid)
{
    /* Filter to dwm.exe -- the classic Run dialog is GDI, presents come
     * from the compositor, not rundll32. --qpc_time gives an absolute QPC
     * column we can correlate directly with our QueryPerformanceCounter
     * values. --stop_existing_session clears a stale ETW session of the
     * same name (e.g. from a prior crashed run) so startup doesn't fail.
     *
     * Flag style is PresentMon 2.x (double-dash). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" --process_name dwm.exe --no_console_stats --qpc_time "
        "--stop_existing_session --session_name %s --output_file \"%s\"",
        exe, session_name, csv_path);

    char cmd_copy[1024];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy));
    cmd_copy[sizeof(cmd_copy) - 1] = 0;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hnul = CreateFileA("NUL", GENERIC_WRITE,
                              FILE_SHARE_WRITE | FILE_SHARE_READ,
                              &sa, OPEN_EXISTING, 0, NULL);
    if (hnul == INVALID_HANDLE_VALUE) return NULL;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hnul;                          /* silence stats spam */
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE); /* let real errors through */

    /* CREATE_NEW_PROCESS_GROUP lets us send CTRL_BREAK to PresentMon
     * later without affecting ourselves. */
    BOOL ok = CreateProcessA(NULL, cmd_copy, NULL, NULL, TRUE,
                             CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
    CloseHandle(hnul);

    if (!ok) {
        fprintf(stderr, "PresentMon CreateProcess failed (%lu)\n", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    if (out_pid) *out_pid = pi.dwProcessId;
    return pi.hProcess;
}

static void stop_presentmon(HANDLE proc, DWORD group_pid)
{
    if (!proc) return;
    /* PresentMon installs a console handler that flushes the CSV on
     * CTRL_BREAK. CREATE_NEW_PROCESS_GROUP put it in its own group, so
     * this signal targets only PresentMon. */
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, group_pid)) {
        fprintf(stderr, "GenerateConsoleCtrlEvent failed (%lu); terminating\n",
                GetLastError());
        TerminateProcess(proc, 0);
    }
    if (WaitForSingleObject(proc, 10000) == WAIT_TIMEOUT) {
        fprintf(stderr, "PresentMon did not exit within 10s; terminating\n");
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 2000);
    }
    CloseHandle(proc);
}

/* CSV parsing: find columns by header name, then for each iteration
 * record the first row whose QPC >= t_show. Header column lookup is
 * defensive across PresentMon 1.x/2.x naming variants. */

static int find_col(char *const *cols, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (cols[i] && _stricmp(cols[i], name) == 0) return i;
    }
    return -1;
}

static int split_csv_inplace(char *line, char **out, int max_cols)
{
    int    n = 0;
    char  *p = line;
    while (n < max_cols) {
        out[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    /* trim trailing CR/LF on last field */
    if (n > 0) {
        char *last = out[n - 1];
        size_t len = strlen(last);
        while (len > 0 && (last[len - 1] == '\r' || last[len - 1] == '\n')) {
            last[--len] = 0;
        }
    }
    return n;
}

static int correlate_presents(const char *csv_path, Sample *samples, int n_samples)
{
    FILE *f = fopen(csv_path, "rb");
    if (!f) {
        fprintf(stderr, "could not open PresentMon CSV %s (%d)\n", csv_path, errno);
        return 0;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "PresentMon CSV is empty\n");
        fclose(f);
        return 0;
    }

    enum { MAX_COLS = 64 };
    char *header[MAX_COLS];
    int    n_cols = split_csv_inplace(line, header, MAX_COLS);

    int col_app  = find_col(header, n_cols, "Application");
    if (col_app < 0) col_app = find_col(header, n_cols, "ProcessName");

    int col_qpc  = find_col(header, n_cols, "QPCTime");
    if (col_qpc < 0) col_qpc = find_col(header, n_cols, "CPUStartQpc");
    if (col_qpc < 0) col_qpc = find_col(header, n_cols, "CpuStartQpc");

    if (col_qpc < 0 || col_app < 0) {
        fprintf(stderr,
                "PresentMon CSV missing required columns "
                "(need Application/ProcessName and QPCTime/CPUStartQpc)\n");
        fclose(f);
        return 0;
    }

    /* Single forward pass: rows are emitted in increasing QPC order, so
     * for each sample we just look for the first row with QPC >= t_show. */
    int matched = 0;
    int rows    = 0;

    while (fgets(line, sizeof(line), f)) {
        char *fields[MAX_COLS];
        int    nf = split_csv_inplace(line, fields, MAX_COLS);
        if (nf <= col_qpc || nf <= col_app) continue;
        rows++;

        if (_stricmp(fields[col_app], "dwm.exe") != 0) continue;

        LONGLONG qpc = _atoi64(fields[col_qpc]);
        if (qpc <= 0) continue;

        for (int i = 0; i < n_samples; i++) {
            if (!samples[i].shown || samples[i].presented) continue;
            if (qpc >= samples[i].t_show.QuadPart) {
                samples[i].t_present.QuadPart = qpc;
                samples[i].presented          = TRUE;
                matched++;
                break;  /* a single CSV row attributes to at most one sample */
            }
        }
    }

    fclose(f);
    if (rows == 0) {
        fprintf(stderr, "PresentMon CSV had a header but no data rows\n");
    }
    return matched;
}

/* ---------- main ------------------------------------------------------- */

static void usage(void)
{
    printf(
        "usage: perf-run [--warmup N] [--n N] [--settle MS]\n"
        "                [--presentmon PATH]\n"
        "  --warmup N         discard first N iterations (default 5)\n"
        "  --n N              measured iterations (default 200)\n"
        "  --settle MS        pause between iterations (default 50)\n"
        "  --presentmon PATH  use PresentMon to also measure first DWM\n"
        "                     present after the dialog appears (the closest\n"
        "                     proxy for first-pixel-on-screen). Empty PATH or\n"
        "                     setting PRESENTMON env var triggers PATH search.\n"
        "                     PresentMon needs admin (ETW). Get it from\n"
        "                     https://github.com/GameTechDev/PresentMon\n");
}

int main(int argc, char **argv)
{
    int   warmup        = 5;
    int   measure       = 200;
    int   settle_ms     = 50;
    int   per_iter_to   = 5000;
    int   use_presentmon = 0;
    const char *pm_override = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            measure = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc) {
            settle_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--presentmon") == 0) {
            use_presentmon = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                pm_override = argv[++i];
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
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

    /* PresentMon setup ---------------------------------------------------*/
    HANDLE pm_proc      = NULL;
    DWORD  pm_pid       = 0;
    char   pm_csv[MAX_PATH] = {0};
    char   pm_session[64]   = {0};

    if (use_presentmon) {
        char pm_exe[MAX_PATH];
        if (!find_presentmon(pm_override, pm_exe, sizeof(pm_exe))) {
            fprintf(stderr,
                "could not find PresentMon. Pass --presentmon PATH, set PRESENTMON\n"
                "env var, or put PresentMon.exe on PATH. Get it from\n"
                "https://github.com/GameTechDev/PresentMon\n");
            return 1;
        }
        printf("PresentMon: %s\n", pm_exe);

        char tmp_dir[MAX_PATH];
        if (!GetTempPathA(MAX_PATH, tmp_dir)) {
            fprintf(stderr, "GetTempPath failed (%lu)\n", GetLastError());
            return 1;
        }
        snprintf(pm_csv, sizeof(pm_csv), "%sperf-run-%lu-%llu.csv",
                 tmp_dir, GetCurrentProcessId(), (unsigned long long)GetTickCount64());
        snprintf(pm_session, sizeof(pm_session), "perf-run-%lu", GetCurrentProcessId());

        pm_proc = start_presentmon(pm_exe, pm_csv, pm_session, &pm_pid);
        if (!pm_proc) return 1;

        /* Give the ETW session a moment to come up before we start
         * spawning rundll32 instances, so we don't race the first show. */
        Sleep(500);

        /* If PresentMon already died (likely a permissions issue), stop. */
        if (WaitForSingleObject(pm_proc, 0) != WAIT_TIMEOUT) {
            DWORD code = 0;
            GetExitCodeProcess(pm_proc, &code);
            CloseHandle(pm_proc);
            fprintf(stderr,
                "PresentMon exited immediately (code %lu). ETW sessions need\n"
                "either administrative privileges or membership in the\n"
                "\"Performance Log Users\" group. Either:\n"
                "  - Re-run perf-run from an elevated shell, or\n"
                "  - Add yourself to the group (one-time, then no UAC):\n"
                "      net localgroup \"Performance Log Users\" %%USERNAME%% /add\n"
                "    then sign out and back in.\n", code);
            return 1;
        }
    }

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        NULL, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) {
        fprintf(stderr, "SetWinEventHook failed (%lu)\n", GetLastError());
        if (pm_proc) stop_presentmon(pm_proc, pm_pid);
        return 1;
    }

    int total = warmup + measure;
    Sample *samples = (Sample *)calloc((size_t)total, sizeof(Sample));
    if (!samples) {
        fprintf(stderr, "out of memory\n");
        UnhookWinEvent(hook);
        if (pm_proc) stop_presentmon(pm_proc, pm_pid);
        return 1;
    }

    printf("perf-run: %d warmup + %d measured iterations\n", warmup, measure);
    printf("command: %s\n\n", cmdline);

    for (int i = 0; i < total; i++) {
        memset(&g_iter, 0, sizeof(g_iter));
        samples[i].warmup = (i < warmup);

        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);

        char cmd_copy[512];
        strncpy(cmd_copy, cmdline, sizeof(cmd_copy));
        cmd_copy[sizeof(cmd_copy) - 1] = 0;

        QueryPerformanceCounter(&samples[i].t_spawn);

        if (!CreateProcessA(NULL, cmd_copy, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
            free(samples);
            UnhookWinEvent(hook);
            if (pm_proc) stop_presentmon(pm_proc, pm_pid);
            return 1;
        }
        g_iter.target_pid = pi.dwProcessId;
        samples[i].pid    = pi.dwProcessId;

        ULONGLONG deadline = GetTickCount64() + (ULONGLONG)per_iter_to;
        while (!g_iter.shown && GetTickCount64() < deadline) {
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

        samples[i].t_show = g_iter.t_shown;
        samples[i].shown  = TRUE;

        double ms = qpc_to_ms(samples[i].t_spawn, samples[i].t_show);
        printf("  %s %3d: show=%6.2f ms\n",
               samples[i].warmup ? "warm" : "meas", i, ms);

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

    /* Stop PresentMon and parse its CSV --------------------------------*/
    int matched = 0;
    if (pm_proc) {
        printf("\nStopping PresentMon and parsing CSV...\n");
        stop_presentmon(pm_proc, pm_pid);
        /* Small wait for the file to be fully flushed/closed by the OS. */
        Sleep(200);
        matched = correlate_presents(pm_csv, samples, total);
        printf("CSV: %s\n", pm_csv);
        printf("Matched %d/%d iterations to a DWM present.\n", matched, total);
    }

    /* Build per-metric arrays for measured iterations only -------------*/
    double *show_ms    = (double *)malloc(sizeof(double) * (size_t)measure);
    double *present_ms = (double *)malloc(sizeof(double) * (size_t)measure);
    if (!show_ms || !present_ms) {
        fprintf(stderr, "out of memory\n");
        free(show_ms); free(present_ms); free(samples);
        return 1;
    }
    int n_show = 0, n_present = 0;
    for (int i = 0; i < total; i++) {
        if (samples[i].warmup || !samples[i].shown) continue;
        show_ms[n_show++] = qpc_to_ms(samples[i].t_spawn, samples[i].t_show);
        if (samples[i].presented) {
            present_ms[n_present++] =
                qpc_to_ms(samples[i].t_spawn, samples[i].t_present);
        }
    }

    if (n_show == 0) {
        fprintf(stderr, "no measured samples collected\n");
        free(show_ms); free(present_ms); free(samples);
        return 1;
    }

    report_metric("CreateProcess -> EVENT_OBJECT_SHOW", show_ms, n_show);
    if (use_presentmon) {
        report_metric("CreateProcess -> first DWM present (>= show)",
                      present_ms, n_present);
        if (n_present == 0) {
            fprintf(stderr,
                "\nNo DWM presents matched. Possible causes:\n"
                "  - PresentMon ran without admin (no ETW session created)\n"
                "  - dwm.exe column name differs in your PresentMon version\n"
                "  - QPC column is named differently (we look for QPCTime,\n"
                "    CPUStartQpc, CpuStartQpc)\n"
                "Inspect %s to debug.\n", pm_csv);
        } else {
            /* Only delete the temp CSV on a clean run. */
            DeleteFileA(pm_csv);
        }
    }

    free(show_ms);
    free(present_ms);
    free(samples);
    return 0;
}
