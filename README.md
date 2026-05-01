# perf-run

> **Disclaimer:** this repo is AI slop. It was written end-to-end by Claude
> (Opus 4.7) in a single session. The code works and the numbers are real,
> but it has not been carefully reviewed by a human. Trust accordingly.

Measures time-to-show of the classic Windows Run dialog.

## Why

Microsoft announced a rewrite of the Win+R dialog with "performance top-of-mind"
and a quoted **94 ms median time-to-show**. Casey Muratori had thoughts:

> Just want to make sure I'm reading this right: Microsoft rewrote the run
> dialog with performance "top-of-mind", and the best they could manage to do
> when putting up *a single text box* was 10fps?
>
> — [@cmuratori, May 1 2026](https://x.com/cmuratori/status/2050328300745261395)

![Casey Muratori's tweet](images/cmuratori-tweet.png)

This tool measures how fast the *existing* (classic, C/Win32) Run dialog shows,
so the rewrite has a baseline to be embarrassed by.

## Result on this machine

200 iterations after a 5-iteration warmup, classic Run dialog
(`rundll32.exe shell32.dll,#61`):

```
  min  :   40.23 ms
  p50  :   48.53 ms
  p90  :   50.34 ms
  p99  :   52.58 ms
  max  :   54.03 ms
  mean :   48.15 ms
```

**~48 ms p50** — about half the 94 ms quoted for the rewrite.

## What it measures

`CreateProcess` → `EVENT_OBJECT_SHOW` for the dialog HWND, captured via a
global `SetWinEventHook` filtered by spawned PID and the `#32770` window class.
Timestamps come from `QueryPerformanceCounter`. The dialog is closed each
iteration with `WM_CLOSE` (not terminated) so the shell's cached state survives
into the next launch.

`EVENT_OBJECT_SHOW` fires when `ShowWindow` is called — a few ms before pixels
actually hit the screen. This matches what most "time-to-show" numbers measure
(presumably including Microsoft's). For true pixels-on-screen timing you would
want DWM ETW present events.

## Build

Requires `clang` (preferred) or MSVC `cl` on PATH.

```
build.bat
```

## Run

```
perf-run.exe [--warmup N] [--n N] [--settle MS]
```

| flag | default | meaning |
|------|---------|---------|
| `--warmup` | 5 | discard first N iterations (lets shell32 warm up) |
| `--n` | 200 | measured iterations |
| `--settle` | 50 | ms to pause between iterations |

For a cold-cache measurement, use `--warmup 0` immediately after a fresh boot.

## Caveats

- Steady-state, not cold-start. Warmup discards first 5 launches.
- This is the *classic* dialog. Measuring the new XAML one needs Insider
  Experimental + the toggle, with the harness pointed at whatever process
  hosts it.
- `EVENT_OBJECT_SHOW` ≠ first paint. The gap is small but non-zero.
