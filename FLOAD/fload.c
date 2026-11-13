/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Francis C Bullen
 * See the LICENSE file for the full licence text.
 */

/*
 * FLoad for MultiTOS / FreeMiNT (GEM)
 *
 * A GEM take on the X11 "xload" utility:
 *   - runs in a movable, resizable, fullable window
 *   - scrolling CPU graph, user time stacked under system (kernel) time
 *   - 1-minute load average drawn as a horizontal line across the graph
 *   - integer grid lines whenever the load average goes above 1.0
 *   - info line shows CPU % (usr/sys) and the 1/5/15 minute load averages
 *
 * MagiC has no CPU time, load average or process priorities, so there
 * FLoad graphs memory in use instead (title "[MagiC: memory]").
 *
 * Data sources (best available is chosen at runtime, shown in title):
 *   CPU  : u:\kern\stat (FreeMiNT) - with user/sys split       "kern"
 *          idle meter thread, tfork() (MultiTOS, old MiNT)     "meter"
 *   Load : Suptime() (FreeMiNT)                                 "Suptime"
 *          u:\kern\loadavg                                      "kern"
 *          1/5/15 min averages of CPU busy (no kernel load)    "avg"
 *
 * Build: m68k-atari-mint-gcc -O2 -o fload.app fload.c -lgem -lm
 *
 * Usage: fload.app [-i seconds] [-n]
 *        -n  don't start the idle meter (no CPU graph without u:\kern)
 *
 * Keys (window topped): q, Esc or Ctrl+Q quit.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define HIST        1024    /* samples kept (enough for a wide window) */
#define COLW        2       /* pixels per sample column                */
#define FSCALE      2048L   /* fixed point 1.0 (same as MiNT's loadavg) */
#define MIN_W       64      /* minimum work-area size                  */
#define MIN_H       32
#define INIT_W      320     /* initial work-area size                  */
#define INIT_H      100

#ifndef AP_TERM
#define AP_TERM     50
#endif
#ifndef WM_BOTTOMED
#define WM_BOTTOMED 33
#endif
#ifndef WF_BOTTOM
#define WF_BOTTOM   25
#endif

#define WKIND (NAME | CLOSER | FULLER | MOVER | SIZER | INFO)

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static short ap_id, vh, win = -1;
static short ncolours;
static GRECT desk;
static int   fulled;
static GRECT prev_rect;

static char  win_name[64] = " FLoad ";
static char  win_info[96];

/* history, in load units (0..FSCALE == 0..100% of one CPU) */
static short h_usr[HIST], h_sys[HIST];
static int   h_head, h_count;

static long  load[3];          /* fixed point, FSCALE == 1.0          */
static int   have_load;        /* load source available               */

/* values used for the current on-screen picture; when they change the
 * picture can no longer be scrolled and must be redrawn in full */
static int   drawn_scale = -1, drawn_loady = -1, drawn_h = -1;

static long  interval_ms = 1000;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int rect_intersect(const GRECT *a, GRECT *b)
{
    short x1 = a->g_x > b->g_x ? a->g_x : b->g_x;
    short y1 = a->g_y > b->g_y ? a->g_y : b->g_y;
    short x2 = (a->g_x + a->g_w < b->g_x + b->g_w) ? a->g_x + a->g_w : b->g_x + b->g_w;
    short y2 = (a->g_y + a->g_h < b->g_y + b->g_h) ? a->g_y + a->g_h : b->g_y + b->g_h;

    b->g_x = x1; b->g_y = y1;
    b->g_w = x2 - x1; b->g_h = y2 - y1;
    return (b->g_w > 0 && b->g_h > 0);
}

static void get_rect(short what, GRECT *r)
{
    wind_get(win, what, &r->g_x, &r->g_y, &r->g_w, &r->g_h);
}

static void set_clip(const GRECT *r, int on)
{
    short pxy[4];
    pxy[0] = r->g_x;
    pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1;
    pxy[3] = r->g_y + r->g_h - 1;
    vs_clip(vh, on, pxy);
}

/* ------------------------------------------------------------------ */
/* Timing and system info                                              */
/* ------------------------------------------------------------------ */

/* 200 Hz system timer; works on every TOS/MiNT, unlike gettimeofday()
 * whose resolution on old MiNTLib/MiNT can be as coarse as 2 seconds. */
static long read_hz200(void)
{
    return *(volatile long *)0x4baL;
}

static long now_ms(void)
{
    return Supexec(read_hz200) * 5L;
}

static long mint_version;           /* MiNT cookie, 0 if none */
static int  is_magic;               /* MagiC ('MagX' cookie) */

static long find_magx_cookie(void)
{
    long *jar = *(long **)0x5a0L;
    if (jar)
        for (; jar[0]; jar += 2)
            if (jar[0] == 0x4D616758L)   /* 'MagX' */
                return 1;
    return 0;
}

/* memory in use, for MagiC (no CPU or load figures there) */
static long read_phystop(void) { return *(volatile long *)0x42eL; }
static long read_ramtop(void)
{
    if (*(volatile long *)0x5a8L == 0x1357BD13L)
        return *(volatile long *)0x5a4L;
    return 0;
}

static long mem_phys, mem_used;

static long mem_sample(void)            /* returns used fraction (FSCALE) */
{
    long st = Supexec(read_phystop), tt = Supexec(read_ramtop);
    long fst, ftt;

    tt = tt > 0x01000000L ? tt - 0x01000000L : 0;
    fst = (long)Mxalloc(-1L, 0);
    ftt = tt ? (long)Mxalloc(-1L, 1) : 0;
    if (fst < 0) fst = 0;
    if (ftt < 0) ftt = 0;
    mem_phys = st + tt;
    mem_used = mem_phys - fst - ftt;
    if (mem_used < 0) mem_used = 0;
    if (mem_phys < 1024)
        return 0;
    return mem_used / 1024 * FSCALE / (mem_phys / 1024);
}

static long find_mint_cookie(void)
{
    long *jar = *(long **)0x5a0L;
    if (jar) {
        for (; jar[0]; jar += 2)
            if (jar[0] == 0x4D694E54L)   /* 'MiNT' */
                return jar[1];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Data sources                                                        */
/*                                                                     */
/* CPU:  CPU_KERN  u:\kern\stat (FreeMiNT kernfs) - user/sys split      */
/*       CPU_SPIN  idle meter: lowest-priority child that spins and    */
/*                 reports how much CPU it got (MultiTOS / old MiNT)   */
/*       CPU_MEM   MagiC: no CPU figures at all, graph memory in use   */
/* Load: LD_SUPTIME  Suptime() (FreeMiNT)                              */
/*       LD_KERN     u:\kern\loadavg                                   */
/*       LD_AVG      1/5/15 minute averages of measured CPU busy       */
/* ------------------------------------------------------------------ */

enum { CPU_NONE, CPU_KERN, CPU_SPIN, CPU_MEM };
enum { LD_NONE, LD_SUPTIME, LD_KERN, LD_AVG };

static int cpu_src = CPU_NONE, ld_src = LD_NONE;
static int no_spinner;

/* --- u:\kern\stat ---------------------------------------------------
 * FreeMiNT prints "cpu  user nice sys idle".  user/nice/sys are summed
 * process times in milliseconds, but idle is computed as
 * jiffies(200 Hz) - (user+nice+sys), so the four fields always add up
 * to the jiffy counter.  We therefore take elapsed time from that sum
 * (x5 ms) and ignore idle itself.  The per-process sums also drop when
 * a process exits, so deltas are clamped at zero. */
static unsigned long st_prev_u, st_prev_s, st_prev_j;

static int read_kern_stat(unsigned long *u, unsigned long *s, unsigned long *j)
{
    FILE *f;
    char buf[256];
    unsigned long v[4];
    int ok = 0;

    f = fopen("u:\\kern\\stat", "r");
    if (!f)
        return 0;

    while (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, "cpu", 3) == 0 && (buf[3] == ' ' || buf[3] == '\t')) {
            if (sscanf(buf + 3, "%lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3]) == 4) {
                *u = v[0] + v[1];
                *s = v[2];
                *j = v[0] + v[1] + v[2] + v[3];
                ok = 1;
            }
            break;
        }
    }
    fclose(f);
    return ok;
}

/* --- idle meter -----------------------------------------------------
 * A lowest-priority thread (MiNTLib tfork(): Pexec mode 104, shares our
 * memory, own 4K stack) does nothing but increment a counter.  The rate
 * it manages, relative to the rate of the same loop on a free CPU, is
 * the idle fraction.
 *
 * NB: Pfork() must NOT be used for this: on MiNT without MMU-separated
 * memory, parent and child share addresses and the kernel swaps the
 * whole data/bss/heap/stack on every context switch between them, so
 * a spinning child starves the parent. */

extern long tfork(int (*func)(long), long arg);   /* MiNTLib */

#define SPIN_CHUNK  0x4000UL

static volatile unsigned long spin_ctr;
static unsigned long spin_prev;
static long   spin_prev_ms;
static long   spin_pid = -1;
static double spin_max;         /* increments per ms on a free CPU */

static void spin_n(unsigned long n)
{
    while (n--)
        spin_ctr++;
}

static int spin_thread(long arg)
{
    (void)arg;
    Pnice(40);                  /* lowest priority the kernel allows */
    for (;;)
        spin_n(SPIN_CHUNK);
    return 0;
}

static int start_spinner(void)
{
    unsigned long n = SPIN_CHUNK;
    long t0, dt;

    /* calibrate the bare loop: grow the run until it lasts >= 250 ms */
    for (;;) {
        t0 = now_ms();
        spin_n(n);
        dt = now_ms() - t0;
        if (dt >= 250 || n >= 0x40000000UL)
            break;
        n <<= 1;
    }
    spin_max = (double)n / (double)(dt > 0 ? dt : 1);

    spin_ctr = 0;
    spin_prev = 0;
    spin_prev_ms = now_ms();

    spin_pid = tfork(spin_thread, 0L);
    if (spin_pid <= 0) {
        spin_pid = -1;
        return 0;
    }
    return 1;
}

static void stop_spinner(void)
{
    if (spin_pid > 0) {
        Pkill((short)spin_pid, 9);  /* SIGKILL - it shares our memory, */
        Pwait3(0, NULL);            /* so it must be gone before exit  */
        spin_pid = -1;
    }
}

/* returns busy fraction in load units (0..FSCALE) */
static long spin_sample(void)
{
    unsigned long c = spin_ctr;
    long now = now_ms();
    unsigned long dn = c - spin_prev;
    long dt = now - spin_prev_ms;
    double rate, idle;

    spin_prev = c;
    spin_prev_ms = now;
    if (dt <= 0)
        return 0;

    rate = (double)dn / (double)dt;
    if (rate > spin_max)
        spin_max = rate;        /* idle CPU faster than calibrated */
    idle = rate / spin_max;
    return (long)((1.0 - idle) * FSCALE + 0.5);
}

/* --- load average --------------------------------------------------- */
static double avg[3];
static int    avg_valid;
static long   last_sample_ms;

static int read_load(void)
{
    long uptime;
    FILE *f;
    char buf[80];
    unsigned int a[3], b[3];
    int i, ok = 0;

    if (ld_src == LD_SUPTIME || ld_src == LD_NONE) {
        if (Suptime(&uptime, load) == 0) {
            ld_src = LD_SUPTIME;
            return 1;
        }
    }
    if (ld_src == LD_KERN || ld_src == LD_NONE) {
        f = fopen("u:\\kern\\loadavg", "r");
        if (f) {
            if (fgets(buf, sizeof buf, f) &&
                sscanf(buf, "%u.%2u %u.%2u %u.%2u",
                       &a[0], &b[0], &a[1], &b[1], &a[2], &b[2]) == 6) {
                for (i = 0; i < 3; i++)
                    load[i] = (long)a[i] * FSCALE + (long)b[i] * FSCALE / 100;
                ok = 1;
            }
            fclose(f);
        }
        if (ok) {
            ld_src = LD_KERN;
            return 1;
        }
    }
    return 0;
}

/* exponentially damped 1/5/15 minute averages of CPU busy */
static void update_avg(long busy, long dt_ms)
{
    static const double period[3] = { 60.0, 300.0, 900.0 };
    double b = (double)busy / FSCALE;
    int i;

    for (i = 0; i < 3; i++) {
        if (!avg_valid)
            avg[i] = b;
        else {
            double e = exp(-(dt_ms / 1000.0) / period[i]);
            avg[i] = avg[i] * e + b * (1.0 - e);
        }
        load[i] = (long)(avg[i] * FSCALE + 0.5);
    }
    avg_valid = 1;
}

static void init_sources(void)
{
    unsigned long u, s, j;

    mint_version = Supexec(find_mint_cookie);
    is_magic = !mint_version && Supexec(find_magx_cookie);

    if (is_magic) {
        /* MagiC keeps no CPU time and has no load average or process
         * priorities (so no idle meter either): show memory instead */
        cpu_src = CPU_MEM;
        ld_src = LD_NONE;
        last_sample_ms = now_ms();
        strcpy(win_name, " FLoad  [MagiC: memory] ");
        return;
    }

    if (read_kern_stat(&u, &s, &j)) {
        cpu_src = CPU_KERN;
        st_prev_u = u; st_prev_s = s; st_prev_j = j;
    } else if (mint_version && !no_spinner && start_spinner()) {
        cpu_src = CPU_SPIN;
    }

    if (!read_load())
        ld_src = (cpu_src != CPU_NONE) ? LD_AVG : LD_NONE;

    last_sample_ms = now_ms();

    sprintf(win_name, " FLoad  [%s%ld.%02ld  %s/%s] ",
            mint_version ? "MiNT " : "no MiNT",
            mint_version ? (mint_version >> 8) & 0xff : 0L,
            mint_version ? mint_version & 0xff : 0L,
            cpu_src == CPU_KERN ? "kern" : cpu_src == CPU_SPIN ? "meter" : "-",
            ld_src == LD_SUPTIME ? "Suptime" : ld_src == LD_KERN ? "kern"
                                 : ld_src == LD_AVG ? "avg" : "-");
    if (!mint_version)
        strcpy(win_name, " FLoad  [needs MiNT] ");
}

/* Take one sample and push it into the history. */
static void sample(void)
{
    long us = 0, sy = 0, now = now_ms();
    long dt = now - last_sample_ms;

    last_sample_ms = now;

    if (cpu_src == CPU_KERN) {
        unsigned long u, s, j;
        if (read_kern_stat(&u, &s, &j)) {
            long dms = (long)(j - st_prev_j) * 5L;
            long du  = (long)(u - st_prev_u);
            long ds  = (long)(s - st_prev_s);
            if (du < 0) du = 0;
            if (ds < 0) ds = 0;
            if (dms > 0) {
                us = du * FSCALE / dms;
                sy = ds * FSCALE / dms;
            }
            st_prev_u = u; st_prev_s = s; st_prev_j = j;
        }
    } else if (cpu_src == CPU_SPIN) {
        us = spin_sample();     /* no user/sys split available */
    } else if (cpu_src == CPU_MEM) {
        us = mem_sample();      /* MagiC: memory in use */
    }

    if (us < 0) us = 0;
    if (sy < 0) sy = 0;
    if (us > FSCALE) us = FSCALE;
    if (us + sy > FSCALE) sy = FSCALE - us;

    if (ld_src == LD_AVG)
        update_avg(us + sy, dt);
    else if (ld_src != LD_NONE || cpu_src != CPU_MEM)
        have_load = read_load();
    else
        have_load = 0;
    if (ld_src == LD_AVG)
        have_load = 1;
    if (!have_load)
        load[0] = load[1] = load[2] = 0;

    h_usr[h_head] = (short)us;
    h_sys[h_head] = (short)sy;
    h_head = (h_head + 1) % HIST;
    if (h_count < HIST)
        h_count++;

    /* info line */
    {
        int pu = (int)((us * 100L + FSCALE / 2) / FSCALE);
        int ps = (int)((sy * 100L + FSCALE / 2) / FSCALE);
        char ld[48];

        if (have_load)
            sprintf(ld, "%s %ld.%02ld %ld.%02ld %ld.%02ld",
                    ld_src == LD_AVG ? "Avg" : "Load",
                    load[0] / FSCALE, (load[0] % FSCALE) * 100 / FSCALE,
                    load[1] / FSCALE, (load[1] % FSCALE) * 100 / FSCALE,
                    load[2] / FSCALE, (load[2] % FSCALE) * 100 / FSCALE);
        else
            strcpy(ld, "Load n/a");

        if (cpu_src == CPU_MEM) {
            char a[16], b[16];
            long ku = (mem_used + 1023) / 1024, kp = (mem_phys + 1023) / 1024;
            sprintf(a, "%ld.%ldM", ku / 1024, (ku % 1024) * 10 / 1024);
            sprintf(b, "%ld.%ldM", kp / 1024, (kp % 1024) * 10 / 1024);
            sprintf(win_info, " Memory %d%% (%s of %s)  CPU n/a under MagiC",
                    pu, a, b);
        } else if (cpu_src == CPU_KERN)
            sprintf(win_info, " CPU %3d%% (usr %d sys %d)  %s",
                    pu + ps, pu, ps, ld);
        else if (cpu_src == CPU_SPIN)
            sprintf(win_info, " CPU %3d%%  %s", pu, ld);
        else if (have_load)
            sprintf(win_info, " CPU n/a  %s", ld);
        else
            sprintf(win_info, " No CPU or load source on this system");
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static int graph_scale(void)
{
    long s = (load[0] + FSCALE - 1) / FSCALE;
    return s < 1 ? 1 : (int)s;
}

/* pixel height of a value (load units) in a graph of height h */
static int val_h(long v, int scale, int h)
{
    return (int)(v * (h - 1) / ((long)scale * FSCALE));
}

static void fill(short colour, short interior, short style,
                 short x1, short y1, short x2, short y2)
{
    short pxy[4];
    if (x2 < x1 || y2 < y1)
        return;
    vsf_color(vh, colour);
    vsf_interior(vh, interior);
    vsf_style(vh, style);
    pxy[0] = x1; pxy[1] = y1; pxy[2] = x2; pxy[3] = y2;
    vr_recfl(vh, pxy);
}

static void hline(short x1, short x2, short y)
{
    short pxy[4];
    pxy[0] = x1; pxy[1] = y; pxy[2] = x2; pxy[3] = y;
    v_pline(vh, 2, pxy);
}

/* Draw the part of the graph that lies within columns [xa, xb].
 * Order: background, grid, bars, load line.  Drawing the whole range
 * at once gives exactly the same pixels as drawing it column by
 * column, which is what makes blit-scrolling safe. */
static void draw_range(const GRECT *work, short xa, short xb)
{
    short x0 = work->g_x;
    short xr = work->g_x + work->g_w - 1;
    short yb = work->g_y + work->g_h - 1;
    short h  = work->g_h;
    int scale = graph_scale();
    int mono = ncolours < 4;
    int j, k, ncols;

    if (xa < x0) xa = x0;
    if (xb > xr) xb = xr;
    if (xa > xb) return;

    vswr_mode(vh, MD_REPLACE);
    vsf_perimeter(vh, 0);

    /* background */
    fill(G_WHITE, FIS_SOLID, 1, xa, work->g_y, xb, yb);

    /* grid: one line per whole unit of load above 1 */
    vsl_color(vh, G_BLACK);
    vsl_type(vh, SOLID);
    vsl_width(vh, 1);
    for (k = 1; k < scale; k++)
        hline(xa, xb, yb - val_h((long)k * FSCALE, scale, h));

    /* bars: column j = 0 is the newest sample, at the right edge */
    ncols = (work->g_w + COLW - 1) / COLW;
    for (j = 0; j < ncols && j < h_count; j++) {
        short cx2 = xr - j * COLW;
        short cx1 = cx2 - COLW + 1;
        int idx, hu, ht;

        if (cx2 < xa) break;
        if (cx1 > xb) continue;
        if (cx1 < xa) cx1 = xa;
        if (cx2 > xb) cx2 = xb;

        idx = (h_head - 1 - j + HIST) % HIST;
        hu = val_h(h_usr[idx], scale, h);
        ht = val_h((long)h_usr[idx] + h_sys[idx], scale, h);

        if (mono) {
            if (hu > 0) fill(G_BLACK, FIS_PATTERN, 4, cx1, yb - hu + 1, cx2, yb);
            if (ht > hu) fill(G_BLACK, FIS_SOLID, 1, cx1, yb - ht + 1, cx2, yb - hu);
        } else {
            if (hu > 0) fill(G_GREEN, FIS_SOLID, 1, cx1, yb - hu + 1, cx2, yb);
            if (ht > hu) fill(G_RED, FIS_SOLID, 1, cx1, yb - ht + 1, cx2, yb - hu);
        }
    }

    /* load average line */
    if (have_load) {
        short y = yb - val_h(load[0], scale, h);
        if (mono) {
            vswr_mode(vh, MD_XOR);      /* visible over solid black too */
            vsl_color(vh, G_BLACK);
        } else {
            vsl_color(vh, ncolours >= 16 ? G_BLUE : G_BLACK);
        }
        hline(xa, xb, y);
        if (h > 60 && !mono)            /* a bit bolder in colour      */
            hline(xa, xb, y - 1);
        vswr_mode(vh, MD_REPLACE);
    }
}

static void remember_drawn(const GRECT *work)
{
    int scale = graph_scale();
    drawn_scale = scale;
    drawn_h = work->g_h;
    drawn_loady = have_load ? val_h(load[0], scale, work->g_h) : -1;
}

/* Standard GEM redraw through the window's rectangle list. */
static void redraw(const GRECT *area)
{
    GRECT r, work, a;

    if (win < 0) return;
    a = *area;
    if (!rect_intersect(&desk, &a))
        return;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);

    get_rect(WF_WORKXYWH, &work);
    get_rect(WF_FIRSTXYWH, &r);
    while (r.g_w > 0 && r.g_h > 0) {
        if (rect_intersect(&a, &r)) {
            set_clip(&r, 1);
            draw_range(&work, r.g_x, r.g_x + r.g_w - 1);
        }
        get_rect(WF_NEXTXYWH, &r);
    }
    set_clip(&desk, 0);
    remember_drawn(&work);

    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void redraw_all(void)
{
    GRECT work;
    get_rect(WF_WORKXYWH, &work);
    redraw(&work);
}

/* After a new sample: scroll the picture left by one column with a
 * screen-to-screen blit and draw only the new column.  Falls back to
 * a full redraw if the window is partly covered/offscreen or if the
 * scale or load line position has changed. */
static void update_graph(void)
{
    GRECT work, r, r2;
    int scale = graph_scale();
    int loady;

    get_rect(WF_WORKXYWH, &work);
    loady = have_load ? val_h(load[0], scale, work.g_h) : -1;

    if (scale != drawn_scale || loady != drawn_loady ||
        work.g_h != drawn_h || work.g_w <= COLW * 2) {
        redraw_all();
        return;
    }

    wind_update(BEG_UPDATE);
    get_rect(WF_FIRSTXYWH, &r);
    get_rect(WF_NEXTXYWH, &r2);

    if (r.g_x == work.g_x && r.g_y == work.g_y &&
        r.g_w == work.g_w && r.g_h == work.g_h &&
        (r2.g_w == 0 || r2.g_h == 0)) {
        MFDB scr;
        short pxy[8];
        short xr = work.g_x + work.g_w - 1;
        short yb = work.g_y + work.g_h - 1;

        memset(&scr, 0, sizeof scr);     /* fd_addr == 0 -> screen */
        graf_mouse(M_OFF, NULL);
        set_clip(&work, 1);

        pxy[0] = work.g_x + COLW; pxy[1] = work.g_y;
        pxy[2] = xr;              pxy[3] = yb;
        pxy[4] = work.g_x;        pxy[5] = work.g_y;
        pxy[6] = xr - COLW;       pxy[7] = yb;
        vro_cpyfm(vh, S_ONLY, pxy, &scr, &scr);

        draw_range(&work, xr - COLW + 1, xr);

        set_clip(&desk, 0);
        graf_mouse(M_ON, NULL);
        wind_update(END_UPDATE);
    } else {
        wind_update(END_UPDATE);
        redraw_all();                    /* clipped to visible parts */
    }
}

/* ------------------------------------------------------------------ */
/* Window handling                                                     */
/* ------------------------------------------------------------------ */

static void clamp_border(GRECT *b)
{
    GRECT w;
    wind_calc(WC_WORK, WKIND, b->g_x, b->g_y, b->g_w, b->g_h,
              &w.g_x, &w.g_y, &w.g_w, &w.g_h);
    if (w.g_w < MIN_W) w.g_w = MIN_W;
    if (w.g_h < MIN_H) w.g_h = MIN_H;
    wind_calc(WC_BORDER, WKIND, w.g_x, w.g_y, w.g_w, w.g_h,
              &b->g_x, &b->g_y, &b->g_w, &b->g_h);
}

static void set_window_rect(GRECT *b, int force_redraw)
{
    wind_set(win, WF_CURRXYWH, b->g_x, b->g_y, b->g_w, b->g_h);
    if (force_redraw)
        redraw_all();   /* size changed: rescale whole graph */
}

static int open_window(void)
{
    GRECT b;

    wind_calc(WC_BORDER, WKIND,
              desk.g_x + 16, desk.g_y + 16, INIT_W, INIT_H,
              &b.g_x, &b.g_y, &b.g_w, &b.g_h);
    if (b.g_w > desk.g_w) b.g_w = desk.g_w;
    if (b.g_h > desk.g_h) b.g_h = desk.g_h;

    win = wind_create(WKIND, desk.g_x, desk.g_y, desk.g_w, desk.g_h);
    if (win < 0)
        return 0;

    wind_set_str(win, WF_NAME, win_name);
    wind_set_str(win, WF_INFO, win_info);
    wind_open(win, b.g_x, b.g_y, b.g_w, b.g_h);
    prev_rect = b;
    return 1;
}

/* returns 0 to quit */
static int handle_message(short *msg)
{
    GRECT b;

    if (msg[0] == AP_TERM)
        return 0;
    if (msg[3] != win)
        return 1;

    switch (msg[0]) {
    case WM_REDRAW:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        redraw(&b);
        break;
    case WM_TOPPED:
        wind_set(win, WF_TOP, 0, 0, 0, 0);
        break;
    case WM_BOTTOMED:
        wind_set(win, WF_BOTTOM, 0, 0, 0, 0);
        break;
    case WM_CLOSED:
        return 0;
    case WM_MOVED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        set_window_rect(&b, 0);
        fulled = 0;
        break;
    case WM_SIZED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        clamp_border(&b);
        set_window_rect(&b, 1);
        fulled = 0;
        break;
    case WM_FULLED:
        if (fulled) {
            b = prev_rect;
            fulled = 0;
        } else {
            get_rect(WF_CURRXYWH, &prev_rect);
            b = desk;
            fulled = 1;
        }
        set_window_rect(&b, 1);
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    short work_in[11], work_out[57];
    short d, msg[8], ev, key, kstate, mx, my, mb, clicks;
    long next, t;
    int i, running = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            long s = atol(argv[++i]);
            if (s >= 1 && s <= 3600)
                interval_ms = s * 1000L;
        } else if (strcmp(argv[i], "-n") == 0) {
            no_spinner = 1;
        }
    }

    /* before appl_init(), so a forked idle meter never inherits AES state */
    init_sources();
    strcpy(win_info, " Measuring...");

    ap_id = appl_init();
    if (ap_id < 0) {
        stop_spinner();
        return 1;
    }

    menu_register(ap_id, "  FLoad ");
    graf_mouse(ARROW, NULL);

    vh = graf_handle(&d, &d, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[0] = Getrez() + 2;
    work_in[10] = 2;                    /* raster coordinates */
    v_opnvwk(work_in, &vh, work_out);
    if (vh <= 0) {
        form_alert(1, "[3][FLoad: cannot open VDI|workstation.][ Quit ]");
        stop_spinner();
        appl_exit();
        return 1;
    }
    ncolours = work_out[13];

    wind_get(0, WF_WORKXYWH, &desk.g_x, &desk.g_y, &desk.g_w, &desk.g_h);

    if (!open_window()) {
        form_alert(1, "[3][FLoad: no more windows.][ Quit ]");
        stop_spinner();
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    next = now_ms() + interval_ms;

    while (running) {
        t = next - now_ms();
        if (t < 0) t = 0;

        ev = evnt_multi(MU_MESAG | MU_TIMER | MU_KEYBD,
                        0, 0, 0,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, t,
                        &mx, &my, &mb, &kstate, &key, &clicks);

        if (ev & MU_MESAG)
            running = handle_message(msg);

        if (running && (ev & MU_KEYBD)) {
            short c = key & 0xff;
            if (c == 'q' || c == 'Q' || c == 0x1b || c == 0x11)
                running = 0;
        }

        if (running && now_ms() >= next) {
            sample();
            wind_set_str(win, WF_INFO, win_info);
            update_graph();
            next += interval_ms;
            if (next < now_ms())        /* we fell behind: resync */
                next = now_ms() + interval_ms;
        }
    }

    stop_spinner();
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
