/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Francis C Bullen
 * See the LICENSE file for the full licence text.
 */

/*
 * FPulse - CPU and memory VU meters for MultiTOS (MiNT 1.04/1.08) and
 * FreeMiNT
 *
 * One resizable window showing CPU and memory, in one of two views
 * (View menu):
 *
 *   Bars       (default) two vertical segmented bars in square housings,
 *              CPU in blue with kernel (system) time in red at the bottom,
 *              memory in green, each with its percentage.
 *   VU Meters  two analogue VU meters, side by side (or stacked when the
 *              window is taller than it is wide); the CPU meter has a thin
 *              red second needle for kernel time.
 *
 *
 *   CPU      busy time of all processes over the last sample; the MiNT
 *            root process (pid 0) runs when nothing else does and is
 *            counted as idle.  Kernel/user split shown under the meter.
 *   Memory   physical RAM in use = ST + TT RAM minus the largest free
 *            block of each (all GEMDOS reports on MultiTOS).
 *
 * Each meter has a heading with its percentage, a scale from 0 to 100
 * with a red zone from 80, and a needle.
 *
 * MagiC has u:\proc but no per-process CPU time (Fcntl(PPROCADDR) is not
 * supported).  There the CPU figures come from a statistical sampler on
 * the 200 Hz timer (magsamp.h, shared with TaskMan and FLoad); if the
 * timer cannot be sampled the CPU meter shows "n/a".
 *
 * Data comes from u:\proc (MultiTOS and FreeMiNT, no u:\kern needed):
 * Fcntl(PPROCADDR) gives each process's kernel record, whose system and
 * user times are read in supervisor mode.
 *
 * Menu bar: FPULSE (About, desk accessories), File (Quit), View (Bars,
 * VU Meters, Hide Window Bars, Hide AES), Rate (sample interval 0.5 -
 * 20 s, as in TaskMan).
 *
 * Hide AES (on by default) counts the AES process (AESSYS on MultiTOS,
 * or XaAES, N.AES, MyAES) and the ALERT accessory as idle, so the CPU
 * meter shows what the programs are using rather than polling.
 *
 * The window opens in the bottom right corner of the screen.  With Hide
 * Window Bars on (the default), it shows only the meters until clicked;
 * clicking it brings back the title bar and gadgets so it can be moved
 * or resized, and they hide again when another window takes the focus.
 * Edges touching the screen edge stay there, so it keeps its corner.
 *
 * Colours follow the system palette: light theme when colour 0 is light,
 * dark otherwise.  In palette modes the palette is never changed; every
 * colour is the closest existing one that contrasts with its background.
 *
 * Build: m68k-atari-mint-gcc -O2 -o fpulse.app fpulse.c -lgem -lm
 *
 * Keys: Ctrl+Q quit.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include "../magsamp.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAXPROC     128
#define PROCREC     40          /* bytes of the process record we read */

#ifndef PPROCADDR
#define PPROCADDR   0x5001
#endif
#ifndef PBASEADDR
#define PBASEADDR   0x5002
#endif
#ifndef AP_TERM
#define AP_TERM     50
#endif
#ifndef WM_BOTTOMED
#define WM_BOTTOMED 33
#endif
#ifndef WF_BOTTOM
#define WF_BOTTOM   25
#endif
#ifndef MN_SELECTED
#define MN_SELECTED 10
#endif
#ifndef OF_LASTOB
#define OF_LASTOB   0x0020
#endif
#ifndef OS_DISABLED
#define OS_DISABLED 0x0008
#endif

/* offsets into the process record, measured from p->magic */
#define OFF_PID     8
#define OFF_SYSTIME 32
#define OFF_USRTIME 36

#define WKIND_FULL    (NAME | CLOSER | FULLER | MOVER | SIZER)
#define WKIND_BARE    0         /* no title bar or gadgets: just the meters */
#ifndef WM_UNTOPPED
#define WM_UNTOPPED   30        /* AES 4.0+: our window lost the top      */
#endif

#define SWEEP       50.0        /* needle swings +-50 degrees */
#define REDZONE     800         /* red from 80 % */

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static short ap_id, vh, win = -1, wchar, hchar;
static GRECT desk, prev_rect;
static int   fulled;
static char  win_name[] = " FPulse ";

static long  sample_ms = 5000L;          /* default: every 5 seconds */

enum { MODE_BARS, MODE_VU, NMODE };
static int   mode = MODE_BARS;          /* bars are the default view */
static int   hidebars = 1;              /* View > Hide Window Bars (default) */
static short mode_w[NMODE], mode_h[NMODE];  /* last work size per view */
static int   bars_out;                  /* ...but shown while focused        */

static short wkind(void) { return hidebars && !bars_out ? WKIND_BARE : WKIND_FULL; }
static long  last_sample_ms;

typedef struct {
    short pid;
    long  addr;
    unsigned long sys, usr, psys, pusr;
    int   aes;                  /* part of the AES (View > Hide AES)  */
    int   probed;               /* u:\proc file queried already       */
    long  bp;                   /* basepage (MagiC sampler)           */
    unsigned long ms_prev;      /* MagiC: sampler ticks last time     */
    int   have_prev, seen;
    unsigned char raw[PROCREC];
} PROC;

static PROC  procs[MAXPROC];
static int   nproc;
static int   hide_aes = 1;              /* View > Hide AES (default on) */

/* The AES runs as an ordinary MiNT process: AESSYS (MultiTOS), XaAES,
 * N.AES or MyAES, matched by name prefix.  ALERT (ALERT.ACC, which polls
 * constantly) is filtered with it, by exact name only. */
static int is_aes(const char *name, int len)
{
    static const char *pre[] = { "AESSYS", "XAAES", "N_AES", "NAES", "MYAES" };
    char up[16];
    int i, j;

    if (len > 15) len = 15;
    for (i = 0; i < len; i++)
        up[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    up[len] = 0;
    if (strcmp(up, "ALERT") == 0)
        return 1;
    for (i = 0; i < (int)(sizeof pre / sizeof pre[0]); i++) {
        for (j = 0; pre[i][j] && up[j] == pre[i][j]; j++)
            ;
        if (!pre[i][j])
            return 1;
    }
    return 0;
}

static short cpu_usr, cpu_sys;          /* permille */
static short mem_pm;                    /* permille of physical RAM used */
static long  mem_phys, mem_free;
static int   have_data;
static int   cpu_avail = 1;             /* kernel reports CPU time per process */
static int   is_magic;                  /* MagiC: no per-process CPU time      */
static unsigned long ms_prev_total, ms_prev_stop;
static int   ms_primed;
static volatile int quit_req;           /* SIGTERM etc.: leave cleanly         */

static int bp_alive(unsigned long bp);

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

enum { C_BG, C_FACE, C_INK, C_HEAD, C_DIM, C_RED, C_NEEDLE, C_FRAME,
       C_HOUSE, C_OFF, C_CPU, C_MEM, C_SYS, NPEN };
enum { THEME_DARK, THEME_LIGHT, NTHEME };

static int   theme;
static short pen[NPEN];

static const unsigned char theme_rgb[NTHEME][NPEN][3] = {
  { /* dark */
    {  30,  30,  33 },  /* C_BG      window background       */
    {  14,  14,  16 },  /* C_FACE    meter face              */
    { 230, 230, 230 },  /* C_INK     scale and face text     */
    { 240, 240, 240 },  /* C_HEAD    headings                */
    { 150, 150, 156 },  /* C_DIM     detail line             */
    { 222,  48,  40 },  /* C_RED     red zone                */
    { 255, 176,   0 },  /* C_NEEDLE  amber needle            */
    {  90,  90,  96 },  /* C_FRAME   face border             */
    {  40,  42,  46 },  /* C_HOUSE   bar housing             */
    {  62,  74,  90 },  /* C_OFF     unlit segment           */
    { 100, 180, 255 },  /* C_CPU     CPU bar (blue)          */
    {  90, 220, 120 },  /* C_MEM     memory bar (green)      */
    { 230,  56,  48 },  /* C_SYS     CPU kernel time (red)   */
  },
  { /* light */
    { 236, 236, 238 },  /* C_BG      */
    { 252, 244, 214 },  /* C_FACE    cream, like a real VU   */
    {  20,  20,  20 },  /* C_INK     */
    {   0,   0,   0 },  /* C_HEAD    */
    {  90,  90,  96 },  /* C_DIM     */
    { 204,  32,  24 },  /* C_RED     */
    {  16,  16,  16 },  /* C_NEEDLE  */
    { 120, 120, 126 },  /* C_FRAME   */
    {  44,  46,  50 },  /* C_HOUSE   dark housing in both themes */
    {  62,  74,  90 },  /* C_OFF     */
    { 100, 180, 255 },  /* C_CPU     */
    {  90, 220, 120 },  /* C_MEM     */
    { 220,  40,  32 },  /* C_SYS     */
  },
};

static short ncolours_g;
static short pal[256][3];
static int   npal;

static long lum_of(const short *c)
{
    return (299L * c[0] + 587L * c[1] + 114L * c[2]) / 1000;
}

static long dist_to(const short *c, const unsigned char *rgb)
{
    long dr = c[0] - rgb[0] * 1000L / 255;
    long dg = c[1] - rgb[1] * 1000L / 255;
    long db = c[2] - rgb[2] * 1000L / 255;
    return (3 * dr * dr + 4 * dg * dg + 2 * db * db) / 16;
}

static int same_colour(int a, int b)
{
    return pal[a][0] == pal[b][0] && pal[a][1] == pal[b][1] && pal[a][2] == pal[b][2];
}

static int is_grey(const short *c)
{
    short mx = c[0], mn = c[0];
    if (c[1] > mx) mx = c[1];
    if (c[1] < mn) mn = c[1];
    if (c[2] > mx) mx = c[2];
    if (c[2] < mn) mn = c[2];
    return mx - mn < 150;
}

static int want_grey(const unsigned char *w)
{
    short c[3];
    c[0] = w[0] * 1000 / 255; c[1] = w[1] * 1000 / 255; c[2] = w[2] * 1000 / 255;
    return is_grey(c);
}

/* nearest palette entry that differs in brightness by at least minlum
 * from every (already mapped) role in `against'; with `avoid' >= 0 it
 * must look different from that pen but stay within dlimit of it */
static int pick_pass(const unsigned char *want, const int *against, long minlum,
                     int avoid, long dlimit, int greys_only, int *found)
{
    long best = 0x7fffffffL, bestc = -1;
    int j, k, bj = -1, cj = 0;

    for (j = 0; j < npal; j++) {
        long minc = 100000L;
        if (greys_only > 0 && !is_grey(pal[j]))
            continue;                   /* greys only */
        if (greys_only < 0 && is_grey(pal[j]))
            continue;                   /* colours only */
        if (avoid >= 0) {
            long dl = lum_of(pal[j]) - lum_of(pal[avoid]);
            if (same_colour(j, avoid) || (dl < 0 ? -dl : dl) > dlimit)
                continue;
        }
        for (k = 0; against && against[k] >= 0; k++) {
            long dl = lum_of(pal[j]) - lum_of(pal[pen[against[k]]]);
            if (dl < 0) dl = -dl;
            if (dl < minc) minc = dl;
        }
        if (minc >= minlum) {
            long d = dist_to(pal[j], want);
            if (d < best) { best = d; bj = j; }
        }
        if (minc > bestc) { bestc = minc; cj = j; }
    }
    *found = (bj >= 0);
    if (bj >= 0) return bj;
    return avoid >= 0 ? -1 : cj;
}

/* grey roles try the palette's greys first, coloured roles (bars, red
 * zone, needle) its colours first, so a blue bar never turns grey just
 * because a grey happens to be a little closer */
static int pick_pen(const unsigned char *want, const int *against, long minlum,
                    int avoid, long dlimit)
{
    int found = 0, j;
    if (want_grey(want)) {
        j = pick_pass(want, against, minlum, avoid, dlimit, 1, &found);
        if (found)
            return j;
        if (avoid >= 0)
            return -1;
    } else {
        j = pick_pass(want, against, minlum, avoid, dlimit, -1, &found);
        if (found)
            return j;
    }
    return pick_pass(want, against, minlum, avoid, dlimit, 0, &found);
}

static void map_bg(int role, int other, long dlimit)
{
    const unsigned char *want = theme_rgb[theme][role];
    int j = pick_pen(want, NULL, 0, -1, 0);
    if (other >= 0 && same_colour(j, pen[other])) {
        int k = pick_pen(want, NULL, 0, pen[other], dlimit);
        if (k >= 0) j = k;
    }
    pen[role] = (short)j;
}

static void map_fg(int role, const int *against, long minlum)
{
    pen[role] = (short)pick_pen(theme_rgb[theme][role], against, minlum, -1, 0);
}

static void setup_colours(void)
{
    static const int on_face[]   = { C_FACE, -1 };
    static const int on_bg[]     = { C_BG, -1 };
    static const int on_needle[] = { C_FACE, -1 };   /* the face matters most */
    static const int on_frame[]  = { C_BG, C_FACE, -1 };
    short ext[57], rgb[3];
    int i;

    vq_extnd(vh, 1, ext);

    if (ext[4] >= 15 && ext[5] == 0 && ncolours_g >= 16 + NPEN) {
        for (i = 0; i < NPEN; i++) {        /* true colour: exact pens */
            rgb[0] = (short)(theme_rgb[theme][i][0] * 1000L / 255);
            rgb[1] = (short)(theme_rgb[theme][i][1] * 1000L / 255);
            rgb[2] = (short)(theme_rgb[theme][i][2] * 1000L / 255);
            vs_color(vh, 16 + i, rgb);
            pen[i] = 16 + i;
        }
        return;
    }

    npal = ncolours_g > 256 ? 256 : ncolours_g;
    if (npal < 2) npal = 2;
    for (i = 0; i < npal; i++)
        vq_color(vh, i, 1, pal[i]);

    map_bg(C_BG, -1, 0);
    map_bg(C_FACE, C_BG, 600);          /* the face should stand out */
    map_fg(C_INK, on_face, 450);
    map_fg(C_HEAD, on_bg, 450);
    map_fg(C_DIM, on_bg, 250);
    map_fg(C_RED, on_face, 150);
    map_fg(C_NEEDLE, on_needle, 150);
    map_fg(C_FRAME, on_frame, 80);

    /* bars: unlit segments get their own shade only if one is close to
     * the housing; lit colours must stand out from both, and CPU and
     * memory should differ from each other */
    {
        static const int on_bar[] = { C_HOUSE, C_OFF, -1 };
        int k;
        map_bg(C_HOUSE, -1, 0);
        map_bg(C_OFF, C_HOUSE, 600);
        map_fg(C_CPU, on_bar, 150);
        k = pick_pen(theme_rgb[theme][C_SYS], on_bar, 120, pen[C_CPU], 1000);
        if (k >= 0)
            pen[C_SYS] = (short)k;      /* kernel time: must differ from user */
        else
            map_fg(C_SYS, on_bar, 120);
        k = pick_pen(theme_rgb[theme][C_MEM], on_bar, 150, pen[C_CPU], 1000);
        if (k >= 0)
            pen[C_MEM] = (short)k;
        else
            map_fg(C_MEM, on_bar, 150);

        /* memory should differ from both CPU colours (e.g. ST medium:
         * green user, red kernel, white memory) */
        if (same_colour(pen[C_MEM], pen[C_SYS]) || same_colour(pen[C_MEM], pen[C_CPU])) {
            long best = 0x7fffffffL, lh = lum_of(pal[pen[C_HOUSE]]);
            int j;
            for (j = 0; j < npal; j++) {
                long dl = lum_of(pal[j]) - lh, d;
                if (same_colour(j, pen[C_SYS]) || same_colour(j, pen[C_CPU]) ||
                    same_colour(j, pen[C_OFF]) || (dl < 0 ? -dl : dl) < 150)
                    continue;
                d = dist_to(pal[j], theme_rgb[theme][C_MEM]);
                if (d < best) { best = d; pen[C_MEM] = (short)j; }
            }
        }
    }
}

static int system_theme(void)
{
    short c[3];
    vq_color(vh, 0, 1, c);
    return lum_of(c) > 500 ? THEME_LIGHT : THEME_DARK;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static short g16(const unsigned char *p)
{
    return (short)((p[0] << 8) | p[1]);
}

static unsigned long g32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static long read_hz200(void)
{
    return *(volatile long *)0x4baL;
}

static long now_ms(void)
{
    return Supexec(read_hz200) * 5L;
}

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

static void set_clip(const GRECT *r, int on)
{
    short pxy[4];
    pxy[0] = r->g_x;
    pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1;
    pxy[3] = r->g_y + r->g_h - 1;
    vs_clip(vh, on, pxy);
}

static void fmt_mem(long bytes, char *out)
{
    long k = (bytes + 1023) / 1024;
    if (k < 1024)
        sprintf(out, "%ldK", k);
    else
        sprintf(out, "%ld.%ldM", k / 1024, (k % 1024) * 10 / 1024);
}

static void fmt_pct(int pm, char *out)
{
    if (pm < 0) pm = 0;
    sprintf(out, "%d%%", (pm + 5) / 10);
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static void box(int c, short x1, short y1, short x2, short y2)
{
    short pxy[4];
    if (x2 < x1 || y2 < y1)
        return;
    vswr_mode(vh, MD_REPLACE);
    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, pen[c]);
    pxy[0] = x1; pxy[1] = y1; pxy[2] = x2; pxy[3] = y2;
    vr_recfl(vh, pxy);
}

static void frame(int c, const GRECT *r)
{
    short pxy[10];
    vswr_mode(vh, MD_REPLACE);
    vsl_width(vh, 1);
    vsl_color(vh, pen[c]);
    pxy[0] = r->g_x;              pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1; pxy[3] = r->g_y;
    pxy[4] = r->g_x + r->g_w - 1; pxy[5] = r->g_y + r->g_h - 1;
    pxy[6] = r->g_x;              pxy[7] = r->g_y + r->g_h - 1;
    pxy[8] = r->g_x;              pxy[9] = r->g_y;
    v_pline(vh, 5, pxy);
}

/* text centred on cx, top at y */
static void ctext(int c, short cx, short y, const char *s, int bold)
{
    short n = (short)strlen(s);
    vswr_mode(vh, MD_TRANS);
    vst_color(vh, pen[c]);
    vst_effects(vh, bold ? 1 : 0);
    v_gtext(vh, cx - (n * wchar + (bold ? 1 : 0)) / 2, y, (char *)s);
    vst_effects(vh, 0);
    vswr_mode(vh, MD_REPLACE);
}

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */

static PROC *find_proc(short pid)
{
    int i;
    for (i = 0; i < nproc; i++)
        if (procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

static long read_procs_super(void)
{
    int i;
    for (i = 0; i < nproc; i++)
        if (procs[i].addr)
            memcpy(procs[i].raw, (const void *)procs[i].addr, PROCREC);
    return 0;
}

static long read_phystop(void) { return *(volatile long *)0x42eL; }

static long magx_cookie(void)
{
    long *jar = *(long **)0x5a0L;
    if (jar)
        for (; jar[0]; jar += 2)
            if (jar[0] == 0x4D616758L)  /* 'MagX' */
                return 1;
    return 0;
}
static long read_ramtop(void)
{
    if (*(volatile long *)0x5a8L == 0x1357BD13L)
        return *(volatile long *)0x5a4L;
    return 0;
}

static int scan_procs(void)
{
    long dh;
    char buf[64], path[48];
    int i, j;

    for (i = 0; i < nproc; i++)
        procs[i].seen = 0;

    dh = Dopendir("u:\\proc", 0);
    if (dh < 0)
        return 0;

    while (Dreaddir(sizeof buf, dh, buf) == 0) {
        char *nm = buf + 4, *dot;
        short pid;
        PROC *p;

        if (nm[0] == '.')
            continue;
        dot = strrchr(nm, '.');
        if (!dot || dot == nm)
            continue;
        pid = (short)atoi(dot + 1);

        p = find_proc(pid);
        if (!p) {
            if (nproc >= MAXPROC)
                continue;
            p = &procs[nproc++];
            memset(p, 0, sizeof *p);
            p->pid = pid;
        }
        p->seen = 1;
        p->aes = is_aes(nm, (int)(dot - nm));

        if (!p->probed) {
            long fd;
            p->probed = 1;
            sprintf(path, "u:\\proc\\%s", nm);
            fd = Fopen(path, 0);
            if (fd >= 0) {
                long a = 0, bp = 0;
                if (Fcntl((short)fd, (long)&a, PPROCADDR) == 0)
                    p->addr = a;
                if (Fcntl((short)fd, (long)&bp, PBASEADDR) == 0)
                    p->bp = bp;
                Fclose((short)fd);
            }
            p->have_prev = 0;
        }
    }
    Dclosedir(dh);

    for (i = j = 0; i < nproc; i++)     /* drop exited processes */
        if (procs[i].seen) {
            if (i != j) procs[j] = procs[i];
            j++;
        }
    nproc = j;
    return 1;
}

static int bp_alive(unsigned long bp)
{
    int i;
    for (i = 0; i < nproc; i++)
        if ((unsigned long)procs[i].bp == bp)
            return 1;
    return 0;
}

static void sample(void)
{
    long now = now_ms(), dt = now - last_sample_ms;
    unsigned long sum_all = 0, busy = 0, ksys = 0, total;
    long st, tt, fst, ftt, used;
    int i, ok;

    last_sample_ms = now;
    ok = scan_procs();
    Supexec(read_procs_super);

    cpu_avail = 0;
    for (i = 0; i < nproc; i++) {
        PROC *p = &procs[i];
        if (p->addr && g16(p->raw + OFF_PID) != p->pid) {
            p->addr = 0;                /* stale: look it up again */
            p->probed = 0;
            p->have_prev = 0;
        }
        if (!p->addr)                   /* MagiC: no process record */
            continue;
        cpu_avail = 1;
        p->sys = g32(p->raw + OFF_SYSTIME);
        p->usr = g32(p->raw + OFF_USRTIME);
        if (p->have_prev) {
            long ds = (long)(p->sys - p->psys);
            long du = (long)(p->usr - p->pusr);
            if (ds < 0) ds = 0;
            if (du < 0) du = 0;
            sum_all += ds + du;
            /* pid 0 = idle; with Hide AES the AES counts as idle too */
            if (p->pid != 0 && !(hide_aes && p->aes)) {
                busy += ds + du;
                ksys += ds;
            }
        }
        p->psys = p->sys;
        p->pusr = p->usr;
        p->have_prev = 1;
    }

    total = sum_all;
    if (dt > 0 && (unsigned long)dt > total)
        total = (unsigned long)dt;
    if (total == 0)
        total = 1;
    cpu_sys = (short)(ksys * 1000UL / total);
    cpu_usr = (short)((busy - ksys) * 1000UL / total);
    if (cpu_sys > 1000) cpu_sys = 1000;
    if (cpu_usr + cpu_sys > 1000) cpu_usr = 1000 - cpu_sys;

    /* MagiC: no kernel accounting, use the shared timer sampler.
     * user = ticks in listed processes (the AES counts as idle with
     * Hide AES), idle = nothing to run (MCPU or FMON), kernel = the rest */
    if (!cpu_avail && ms) {
        unsigned long tot, stp, td, sd;
        unsigned long matched = 0, user = 0, other;

        ms_read(&tot, &stp);                /* all ticks, idle ticks */
        td = tot - ms_prev_total;
        sd = stp - ms_prev_stop;

        for (i = 0; i < nproc; i++) {
            PROC *p = &procs[i];
            unsigned long cur = ms_count((unsigned long)p->bp, p->pid), d;
            d = cur >= p->ms_prev ? cur - p->ms_prev : cur;
            if (!ms_primed)
                d = 0;
            p->ms_prev = cur;
            matched += d;
            if (!(hide_aes && p->aes))
                user += d;
        }
        ms_prev_total = tot;
        ms_prev_stop = stp;
        if (ms_primed && td > 0) {
            other = td > sd + matched ? td - sd - matched : 0;
            cpu_usr = (short)(user * 1000UL / td);
            cpu_sys = (short)(other * 1000UL / td);
            if (cpu_usr > 1000) cpu_usr = 1000;
            if (cpu_usr + cpu_sys > 1000) cpu_sys = 1000 - cpu_usr;
            cpu_avail = 1;
        }
        ms_primed = 1;
        ms_tidy(bp_alive);
    }

    /* memory */
    st = Supexec(read_phystop);
    tt = Supexec(read_ramtop);
    tt = tt > 0x01000000L ? tt - 0x01000000L : 0;
    fst = (long)Mxalloc(-1L, 0);
    ftt = tt ? (long)Mxalloc(-1L, 1) : 0;
    if (fst < 0) fst = 0;
    if (ftt < 0) ftt = 0;
    mem_phys = st + tt;
    mem_free = fst + ftt;
    used = mem_phys - mem_free;
    if (used < 0) used = 0;
    mem_pm = mem_phys >= 1024 ? (short)(used / 1024 * 1000L / (mem_phys / 1024)) : 0;
    if (mem_pm > 1000) mem_pm = 1000;

    have_data = ok;
}

/* ------------------------------------------------------------------ */
/* VU meter                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    GRECT cell;                 /* the meter's share of the window   */
    GRECT face;                 /* the meter face                    */
    short px, py;               /* needle pivot (below the face)     */
    short r;                    /* scale radius                      */
} METER;

static void meter_layout(const GRECT *cell, METER *m)
{
    short headh = hchar + 8, deth = hchar + 6, margin = 6;
    short avail, fw, fh, rw, rh, top;
    double s = sin(SWEEP * M_PI / 180.0);

    m->cell = *cell;
    fw = cell->g_w - 2 * margin;
    if (fw < 16) fw = 16;
    avail = cell->g_h - headh - deth;
    if (avail < 16) avail = 16;

    /* Like a real VU meter: the pivot sits below the face, a quarter of
     * the radius under its bottom edge, and the face is just tall enough
     * for the arc and its labels.  So the face height follows from the
     * radius:  fh = R + labels + top pad - R/4.
     * Width limit: the "0" and "100" labels must fit at the arc ends. */
    rw = (short)((fw / 2 - 4 - 3 * wchar / 2) / s) - (6 + hchar / 2);
    rh = (short)((avail - hchar - 14) * 4 / 3);
    m->r = rw < rh ? rw : rh;
    if (m->r < 12) m->r = 12;

    fh = m->r - m->r / 4 + hchar + 14;
    if (fh > avail) fh = avail;

    /* centre the face between the heading and the detail line */
    top = cell->g_y + headh + (avail - fh) / 2;
    m->face.g_x = cell->g_x + (cell->g_w - fw) / 2;
    m->face.g_y = top;
    m->face.g_w = fw;
    m->face.g_h = fh;

    m->px = m->face.g_x + fw / 2;
    m->py = top + hchar + 14 + m->r;    /* labels + pad above the arc */
}

static void polar(const METER *m, int pm, short rad, short *x, short *y)
{
    double a = (-SWEEP + pm * (2.0 * SWEEP) / 1000.0) * M_PI / 180.0;
    *x = m->px + (short)floor(rad * sin(a) + 0.5);
    *y = m->py - (short)floor(rad * cos(a) + 0.5);
}

static void draw_meter(const METER *m, const GRECT *clip, const char *title,
                       int pm, int red_pm, const char *detail, const char *shortdet,
                       const char *facelabel)
{
    (void)facelabel;            /* the heading already names the meter */
    GRECT fc, r;
    short pxy[64], x, y, x2, y2;
    char head[32], pct[8];
    int i, n;

    /* background, heading and detail line */
    r = m->cell;
    if (rect_intersect(clip, &r)) {
        box(C_BG, r.g_x, r.g_y, r.g_x + r.g_w - 1, r.g_y + r.g_h - 1);
        fmt_pct(pm, pct);
        if (pm < 0)
            strcpy(pct, "n/a");         /* kernel has no CPU figures */
        sprintf(head, have_data ? "%s %s" : "%s --", title, pct);
        ctext(C_HEAD, m->cell.g_x + m->cell.g_w / 2, m->face.g_y - hchar - 6, head, 1);
        if ((short)strlen(detail) * wchar > m->cell.g_w - 8)
            detail = shortdet;          /* narrow: compact form */
        if ((short)strlen(detail) * wchar <= m->cell.g_w - 4)
            ctext(C_DIM, m->cell.g_x + m->cell.g_w / 2,
                  m->face.g_y + m->face.g_h + 4, detail, 0);
    }

    /* face: everything below is clipped to it */
    fc = m->face;
    if (!rect_intersect(clip, &fc))
        return;
    set_clip(&fc, 1);
    box(C_FACE, m->face.g_x, m->face.g_y,
        m->face.g_x + m->face.g_w - 1, m->face.g_y + m->face.g_h - 1);

    /* red zone: a band along the outer edge of the scale */
    n = 0;
    for (i = REDZONE; i <= 1000 && n < 28; i += 25) {
        polar(m, i, m->r, &x, &y);
        pxy[n++] = x; pxy[n++] = y;
    }
    for (i = 1000; i >= REDZONE && n < 60; i -= 25) {
        polar(m, i, m->r - 5, &x, &y);
        pxy[n++] = x; pxy[n++] = y;
    }
    vswr_mode(vh, MD_REPLACE);
    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, pen[C_RED]);
    vsf_perimeter(vh, 0);
    v_fillarea(vh, n / 2, pxy);

    /* scale arc */
    vsl_color(vh, pen[C_INK]);
    vsl_width(vh, 1);
    n = 0;
    for (i = 0; i <= 1000; i += 40) {
        polar(m, i, m->r, &x, &y);
        pxy[n++] = x; pxy[n++] = y;
    }
    v_pline(vh, n / 2, pxy);

    /* ticks every 10 %, labels every 20 % (every 50 % when small) */
    for (i = 0; i <= 1000; i += 100) {
        int major = (i % 200) == 0;
        polar(m, i, m->r + (major ? 4 : 2), &x, &y);
        polar(m, i, m->r - (major ? 7 : 4), &x2, &y2);
        pxy[0] = x; pxy[1] = y; pxy[2] = x2; pxy[3] = y2;
        v_pline(vh, 2, pxy);
        if (major && (m->r >= 6 * wchar || i % 500 == 0)) {
            char lab[4];
            sprintf(lab, "%d", i / 10);
            polar(m, i, m->r + 6 + hchar / 2, &x, &y);
            ctext(i >= REDZONE ? C_RED : C_INK, x, y - hchar / 2, lab, 0);
        }
    }


    /* second, thin needle for kernel time (CPU meter only) */
    if (have_data && pm >= 0 && red_pm >= 0) {
        polar(m, red_pm, m->r - 2, &x, &y);
        vsl_color(vh, pen[C_RED]);      /* red that contrasts with the face */
        vsl_width(vh, 1);
        pxy[0] = m->px; pxy[1] = m->py; pxy[2] = x; pxy[3] = y;
        v_pline(vh, 2, pxy);
    }

    /* needle */
    if (have_data && pm >= 0) {
        polar(m, pm, m->r + 2, &x, &y);
        vsl_color(vh, pen[C_NEEDLE]);
        vsl_width(vh, m->r >= 40 ? 3 : 1);
        pxy[0] = m->px; pxy[1] = m->py; pxy[2] = x; pxy[3] = y;
        v_pline(vh, 2, pxy);
        vsl_width(vh, 1);
    }

    frame(C_FRAME, &m->face);
    set_clip(clip, 1);
}

/* ------------------------------------------------------------------ */
/* Bar view                                                            */
/* ------------------------------------------------------------------ */

/* one bar: housing column hx..hx+hw-1, from hy, hh high */
static void draw_bar(short hx, short hw, short hy, short hh, const char *title,
                     int pm, int red_pm, int colour, const char *legend)
{
    short pad, seg, gap, y, cx = hx + hw / 2;
    int nseg, lit, lit_red, k;
    char pct[8];

    /* square housing */
    box(C_HOUSE, hx, hy, hx + hw - 1, hy + hh - 1);

    /* fine segments, filled from the bottom */
    pad = hw >= 6 * wchar ? 4 : 3;
    seg = hchar >= 16 ? 3 : 2;
    gap = 1;
    nseg = (hh - 2 * pad + gap) / (seg + gap);
    if (nseg < 1) nseg = 1;
    lit = pm > 0 ? (pm * nseg + 500) / 1000 : 0;
    if (pm > 0 && lit == 0) lit = 1;    /* show that something is happening */
    lit_red = (red_pm * nseg + 500) / 1000;
    if (red_pm > 0 && lit_red == 0) lit_red = 1;
    if (lit_red > lit) lit_red = lit;

    /* kernel (system) time at the bottom in red, user time above it */
    for (k = 0; k < nseg; k++) {
        int c = k < lit_red ? C_SYS : k < lit ? colour : C_OFF;
        y = hy + hh - pad - 1 - k * (seg + gap);
        box(c, hx + pad, y - seg + 1, hx + hw - pad - 1, y);
    }

    /* heading above, percentage below */
    ctext(C_HEAD, cx, hy - hchar - 6, title, 0);
    fmt_pct(pm, pct);
    if (pm < 0)
        strcpy(pct, "n/a");             /* kernel has no CPU figures */
    ctext(C_HEAD, cx, hy + hh + 5, have_data ? pct : "--", 1);
    if (legend && have_data)
        ctext(C_DIM, cx, hy + hh + hchar + 7, legend, 0);
}

/* the two bars side by side, close together, centred in the window */
static void draw_bars(const GRECT *work, const GRECT *clip)
{
    short headh = hchar + 8, foot = hchar + 10;
    short hw, gap, x0, hy, hh;
    GRECT r = *work;

    if (rect_intersect(clip, &r))
        box(C_BG, r.g_x, r.g_y, r.g_x + r.g_w - 1, r.g_y + r.g_h - 1);

    /* the bars fill the window: half a character of margin at each side
     * and one character between them */
    gap = wchar;
    hw = (work->g_w - wchar - gap) / 2;
    if (hw < 3 * wchar) hw = 3 * wchar;
    x0 = work->g_x + (work->g_w - (2 * hw + gap)) / 2;
    hy = work->g_y + headh;
    hh = work->g_h - headh - foot;
    if (hh < 16) hh = 16;

    /* the red part of the CPU bar is kernel time, the blue part user time */
    if (cpu_avail)
        draw_bar(x0, hw, hy, hh, "CPU", cpu_usr + cpu_sys, cpu_sys, C_CPU, NULL);
    else
        draw_bar(x0, hw, hy, hh, "CPU", -1, 0, C_CPU, NULL);
    draw_bar(x0 + hw + gap, hw, hy, hh, "Memory", mem_pm, 0, C_MEM, NULL);
}

/* split the work area into the two meter cells */
static void cells(const GRECT *w, GRECT *a, GRECT *b)
{
    *a = *w;
    *b = *w;
    if (w->g_w >= w->g_h) {             /* side by side */
        a->g_w = w->g_w / 2;
        b->g_x = w->g_x + a->g_w;
        b->g_w = w->g_w - a->g_w;
    } else {                            /* stacked */
        a->g_h = w->g_h / 2;
        b->g_y = w->g_y + a->g_h;
        b->g_h = w->g_h - a->g_h;
    }
}

static void draw_all(const GRECT *work, const GRECT *clip)
{
    GRECT ca, cb;
    METER ma, mb;
    char d1[40], d2[40], s1[24], s2[24], a[16], b[16];

    cells(work, &ca, &cb);
    if (mode == MODE_BARS)
        draw_bars(work, clip);
    else {
    meter_layout(&ca, &ma);
    meter_layout(&cb, &mb);

    sprintf(d1, "usr %d%%  sys %d%%", (cpu_usr + 5) / 10, (cpu_sys + 5) / 10);
    fmt_mem(mem_phys - mem_free > 0 ? mem_phys - mem_free : 0, a);
    fmt_mem(mem_phys, b);
    sprintf(d2, "%s of %s", a, b);
    sprintf(s1, "u%d s%d", (cpu_usr + 5) / 10, (cpu_sys + 5) / 10);
    sprintf(s2, "%s/%s", a, b);

    if (!cpu_avail) {
        strcpy(d1, is_magic ? "not reported by MagiC" : "not reported");
        strcpy(s1, "n/a");
    }
    draw_meter(&ma, clip, "CPU", cpu_avail ? cpu_usr + cpu_sys : -1, cpu_sys,
               d1, s1, "CPU %");
    draw_meter(&mb, clip, "Memory", mem_pm, -1, d2, s2, "MEM %");
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void get_rect(short what, GRECT *r)
{
    wind_get(win, what, &r->g_x, &r->g_y, &r->g_w, &r->g_h);
}

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
            draw_all(&work, &r);
        }
        get_rect(WF_NEXTXYWH, &r);
    }
    set_clip(&desk, 0);
    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void redraw_all(void)
{
    GRECT work;
    if (win < 0) return;
    get_rect(WF_WORKXYWH, &work);
    redraw(&work);
}

static void min_work(short *w, short *h)
{
    *w = mode == MODE_BARS ? 14 * wchar : 24 * wchar;
    *h = mode == MODE_BARS ? 5 * hchar : 7 * hchar;
}

static void clamp_border(GRECT *b)
{
    GRECT w;
    short mw, mh;
    wind_calc(WC_WORK, wkind(), b->g_x, b->g_y, b->g_w, b->g_h,
              &w.g_x, &w.g_y, &w.g_w, &w.g_h);
    min_work(&mw, &mh);
    if (w.g_w < mw) w.g_w = mw;
    if (w.g_h < mh) w.g_h = mh;
    wind_calc(WC_BORDER, wkind(), w.g_x, w.g_y, w.g_w, w.g_h,
              &b->g_x, &b->g_y, &b->g_w, &b->g_h);
}

/* keep a border rect on the desk */
static void fit_desk(GRECT *b)
{
    if (b->g_x + b->g_w > desk.g_x + desk.g_w) b->g_x = desk.g_x + desk.g_w - b->g_w;
    if (b->g_y + b->g_h > desk.g_y + desk.g_h) b->g_y = desk.g_y + desk.g_h - b->g_h;
    if (b->g_x < desk.g_x) b->g_x = desk.g_x;
    if (b->g_y < desk.g_y) b->g_y = desk.g_y;
}

/* create and open the window with the current kind; `work' is the work
 * area to keep, `old' the previous border (edges that touched the screen
 * edge stay there, so a window in the corner stays in the corner) */
static int create_window(const GRECT *work, const GRECT *old)
{
    GRECT b;

    wind_calc(WC_BORDER, wkind(), work->g_x, work->g_y, work->g_w, work->g_h,
              &b.g_x, &b.g_y, &b.g_w, &b.g_h);
    clamp_border(&b);
    if (old) {
        /* within a few pixels of a screen edge: keep that edge exactly
         * where it was (e.g. the 1 pixel gap on the right) */
        short orr = old->g_x + old->g_w, ob = old->g_y + old->g_h;
        if (orr >= desk.g_x + desk.g_w - 4)
            b.g_x = orr - b.g_w;                        /* right edge */
        else if (old->g_x <= desk.g_x + 3)
            b.g_x = old->g_x;                           /* left edge */
        if (ob >= desk.g_y + desk.g_h - 4)
            b.g_y = ob - b.g_h;                         /* bottom edge */
        else if (old->g_y <= desk.g_y + 3)
            b.g_y = old->g_y;                           /* top edge */
    }
    fit_desk(&b);

    win = wind_create(wkind(), desk.g_x, desk.g_y, desk.g_w, desk.g_h);
    if (win < 0)
        return 0;
    if (wkind() & NAME)
        wind_set_str(win, WF_NAME, win_name);
    wind_open(win, b.g_x, b.g_y, b.g_w, b.g_h);
    prev_rect = b;
    fulled = 0;
    return 1;
}

/* first window: narrow for the bars, in the bottom right corner of the
 * screen, one pixel in from the right edge */
static int open_window(void)
{
    GRECT w, b;

    w.g_w = 15 * wchar;
    w.g_h = 13 * hchar / 2;
    if (w.g_w > desk.g_w - 32) w.g_w = desk.g_w - 32;
    if (w.g_h > desk.g_h - 48) w.g_h = desk.g_h - 48;
    w.g_x = desk.g_x;
    w.g_y = desk.g_y;
    wind_calc(WC_BORDER, wkind(), w.g_x, w.g_y, w.g_w, w.g_h,
              &b.g_x, &b.g_y, &b.g_w, &b.g_h);
    clamp_border(&b);
    b.g_x = desk.g_x + desk.g_w - b.g_w - 1; /* 1 pixel in from the right */
    b.g_y = desk.g_y + desk.g_h - b.g_h;     /* on the bottom edge */
    return create_window(&w, &b);
}

/* the AES cannot change a window's gadgets, so showing or hiding the
 * bars means re-creating the window around the same meters */
static void rebuild_window(int stay_on_top)
{
    GRECT work, old;
    short h;

    if (win < 0)
        return;
    get_rect(WF_WORKXYWH, &work);
    get_rect(WF_CURRXYWH, &old);
    h = win;
    wind_close(h);
    wind_delete(h);
    win = -1;
    if (!create_window(&work, &old)) {
        form_alert(1, "[3][FPulse: no more windows.][ OK ]");
        return;
    }
    if (!stay_on_top)
        wind_set(win, WF_BOTTOM, 0, 0, 0, 0);   /* give the top back */
}

static int is_top(void)
{
    short top = -1, d;
    wind_get(0, WF_TOP, &top, &d, &d, &d);
    return top == win;
}

/* Hide Window Bars: bars appear when the window is clicked ... */
static void show_bars(void)
{
    if (hidebars && !bars_out) {
        bars_out = 1;
        rebuild_window(1);
    }
}

/* ... and go again when another window takes the focus */
static void hide_bars_if_unfocused(void)
{
    if (hidebars && bars_out && win >= 0 && !is_top()) {
        bars_out = 0;
        rebuild_window(0);
    }
}

static void set_hidebars(int on)
{
    if (on == hidebars)
        return;
    hidebars = on;
    bars_out = 0;
    rebuild_window(1);
}

/* give the window a new work size; edges near a screen edge stay where
 * they are (so the window keeps its corner), otherwise the top left does */
static void resize_work(short ww, short wh)
{
    GRECT b, w, n;
    short d;

    if (win < 0) return;
    get_rect(WF_CURRXYWH, &b);
    get_rect(WF_WORKXYWH, &w);
    wind_calc(WC_BORDER, wkind(), w.g_x, w.g_y, ww, wh,
              &d, &d, &n.g_w, &n.g_h);
    n.g_x = b.g_x;
    n.g_y = b.g_y;
    clamp_border(&n);
    if (b.g_x + b.g_w >= desk.g_x + desk.g_w - 4)
        n.g_x = b.g_x + b.g_w - n.g_w;          /* keep the right edge */
    if (b.g_y + b.g_h >= desk.g_y + desk.g_h - 4)
        n.g_y = b.g_y + b.g_h - n.g_h;          /* keep the bottom edge */
    fit_desk(&n);
    wind_set(win, WF_CURRXYWH, n.g_x, n.g_y, n.g_w, n.g_h);
}

/* switch view, remembering each view's window size */
static void set_mode(int m)
{
    GRECT w;

    if (m == mode)
        return;
    if (win >= 0) {
        get_rect(WF_WORKXYWH, &w);
        mode_w[mode] = w.g_w;
        mode_h[mode] = w.g_h;
    }
    mode = m;
    if (!mode_w[m]) {                   /* first time: a size that suits it */
        mode_w[m] = m == MODE_BARS ? 15 * wchar : 40 * wchar;
        mode_h[m] = m == MODE_BARS ? 13 * hchar / 2 : 9 * hchar;
    }
    resize_work(mode_w[m], mode_h[m]);
}

/* ------------------------------------------------------------------ */
/* Menu bar (built in code)                                            */
/* ------------------------------------------------------------------ */

#define MAXOBJ  40
#define NRATE   7
#define NTITLE  4

static OBJECT mtree[MAXOBJ];
static int    nobj;
static short  mi_about = -1, mi_quit = -1, mi_rate[NRATE], mi_mode[NMODE];
static const long rate_ms[NRATE] = { 500L, 1000L, 2000L, 5000L, 10000L, 15000L, 20000L };

static char ms_about[] = "  About FPulse...    ";
static char ms_sep[]   = "---------------------";
static char ms_acc[6][22];
static char ms_quit[]  = "  Quit       ^Q ";
static char ms_rate[NRATE][16] = {
    "  0.5 seconds ", "  1 second    ", "  2 seconds   ", "  5 seconds   ",
    "  10 seconds  ", "  15 seconds  ", "  20 seconds  "
};
static char ms_mode[NMODE][20] = { "  Bars             ", "  VU Meters        " };
static char ms_vsep[]    = "-------------------";
static char ms_hide[]    = "  Hide Window Bars ";
static char ms_haes[]    = "  Hide AES         ";
static short mi_haes = -1;
static short mi_hide = -1;
static char mt_title[NTITLE][10] = { " FPULSE ", " File ", " View ", " Rate " };

static int new_obj(int parent, unsigned short type, short x, short y, short w, short h)
{
    OBJECT *o = &mtree[nobj];

    memset(o, 0, sizeof *o);
    o->ob_next = -1;
    o->ob_head = o->ob_tail = -1;
    o->ob_type = type;
    o->ob_x = x; o->ob_y = y; o->ob_width = w; o->ob_height = h;
    if (parent >= 0) {
        OBJECT *p = &mtree[parent];
        if (p->ob_head < 0)
            p->ob_head = (short)nobj;
        else
            mtree[p->ob_tail].ob_next = (short)nobj;
        p->ob_tail = (short)nobj;
        o->ob_next = (short)parent;
    }
    return nobj++;
}

static void build_menu(short scr_w, short scr_h)
{
    char *lists[NTITLE][8];
    int counts[NTITLE];
    short cols = scr_w / wchar, rows = scr_h / hchar, tx[NTITLE], x = 0;
    int root, bar, active, screen, box_, i, k;

    for (i = 0; i < 6; i++)
        sprintf(ms_acc[i], "  Desk Accessory %d  ", i + 1);
    lists[0][0] = ms_about;
    lists[0][1] = ms_sep;
    for (i = 0; i < 6; i++)
        lists[0][2 + i] = ms_acc[i];
    counts[0] = 8;
    lists[1][0] = ms_quit;
    counts[1] = 1;
    for (i = 0; i < NMODE; i++)
        lists[2][i] = ms_mode[i];
    lists[2][NMODE] = ms_vsep;
    lists[2][NMODE + 1] = ms_hide;
    lists[2][NMODE + 2] = ms_haes;
    counts[2] = NMODE + 3;
    for (i = 0; i < NRATE; i++)
        lists[3][i] = ms_rate[i];
    counts[3] = NRATE;

    nobj = 0;
    root   = new_obj(-1, G_IBOX, 0, 0, cols, rows);
    bar    = new_obj(root, G_BOX, 0, 0, cols, 0x0201);
    mtree[bar].ob_spec.index = 0x00001100L;
    active = new_obj(bar, G_IBOX, 2, 0, 0, 0x0301);
    for (i = 0; i < NTITLE; i++) {
        short w = (short)strlen(mt_title[i]);
        int t;
        tx[i] = x;
        t = new_obj(active, G_TITLE, x, 0, w, 0x0301);
        mtree[t].ob_spec.free_string = mt_title[i];
        x += w;
    }
    mtree[active].ob_width = x;
    screen = new_obj(root, G_IBOX, 0, 0x0301, cols, 19);

    for (k = 0; k < NTITLE; k++) {
        short bw = 0, bx;
        for (i = 0; i < counts[k]; i++)
            if ((short)strlen(lists[k][i]) > bw)
                bw = (short)strlen(lists[k][i]);
        bx = 2 + tx[k];
        if (bx + bw > cols) bx = cols - bw;
        box_ = new_obj(screen, G_BOX, bx, 0, bw, counts[k]);
        mtree[box_].ob_spec.index = 0x00FF1100L;
        for (i = 0; i < counts[k]; i++) {
            int o = new_obj(box_, G_STRING, 0, i, bw, 1);
            mtree[o].ob_spec.free_string = lists[k][i];
            if (lists[k][i] == ms_sep || lists[k][i] == ms_vsep)
                mtree[o].ob_state |= OS_DISABLED;
            if (k == 0 && i == 0) mi_about = (short)o;
            if (k == 1 && i == 0) mi_quit = (short)o;
            if (k == 2 && i < NMODE) mi_mode[i] = (short)o;
            if (lists[k][i] == ms_hide) mi_hide = (short)o;
            if (lists[k][i] == ms_haes) mi_haes = (short)o;
            if (k == 3) mi_rate[i] = (short)o;
        }
    }
    mtree[nobj - 1].ob_flags |= OF_LASTOB;

    for (i = 0; i < nobj; i++)
        rsrc_obfix(mtree, i);
}

static void sync_menu(void)
{
    int i;
    for (i = 0; i < NRATE; i++)
        menu_icheck(mtree, mi_rate[i], rate_ms[i] == sample_ms);
    for (i = 0; i < NMODE; i++)
        menu_icheck(mtree, mi_mode[i], mode == i);
    menu_icheck(mtree, mi_hide, hidebars);
    menu_icheck(mtree, mi_haes, hide_aes);
}

/* returns 0 to quit */
static int menu_select(short title, short item)
{
    int i, quit = 0;

    if (item == mi_about)
        form_alert(1, "[1][FPulse|CPU and memory VU meters|for MultiTOS and FreeMiNT.|"
                      "Copyright (c) 2026|Francis C Bullen - MIT licence][ OK ]");
    else if (item == mi_quit)
        quit = 1;
    else {
        for (i = 0; i < NRATE; i++)
            if (item == mi_rate[i]) {
                sample_ms = rate_ms[i];
                sync_menu();
            }
        for (i = 0; i < NMODE; i++)
            if (item == mi_mode[i] && mode != i) {
                set_mode(i);
                sync_menu();
                redraw_all();
            }
        if (item == mi_haes) {
            hide_aes = !hide_aes;       /* takes effect from the next sample */
            sync_menu();
        }
        if (item == mi_hide) {
            set_hidebars(!hidebars);
            sync_menu();
        }
    }
    menu_tnormal(mtree, title, 1);
    return !quit;
}

/* returns 0 to quit */
static int handle_message(short *msg)
{
    GRECT b;

    if (msg[0] == AP_TERM)
        return 0;
    if (msg[0] == MN_SELECTED)
        return menu_select(msg[3], msg[4]);
    if (msg[3] != win)
        return 1;

    switch (msg[0]) {
    case WM_REDRAW:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        redraw(&b);
        break;
    case WM_TOPPED:
        wind_set(win, WF_TOP, 0, 0, 0, 0);
        show_bars();                    /* clicked: bring the bars back */
        break;
    case WM_UNTOPPED:
        hide_bars_if_unfocused();
        break;
    case WM_BOTTOMED:
        wind_set(win, WF_BOTTOM, 0, 0, 0, 0);
        break;
    case WM_CLOSED:
        return 0;
    case WM_MOVED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        wind_set(win, WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        fulled = 0;
        break;
    case WM_SIZED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        clamp_border(&b);
        wind_set(win, WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        fulled = 0;
        redraw_all();               /* layout depends on the size */
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
        wind_set(win, WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        redraw_all();
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void on_quit_signal(int sig)
{
    (void)sig;
    quit_req = 1;
}

int main(void)
{
    short work_in[11], work_out[57];
    short d, msg[8], ev, key, kstate, mx, my, mb, clicks;
    long next, t;
    int i, running = 1;

    ap_id = appl_init();
    if (ap_id < 0)
        return 1;
    menu_register(ap_id, "  FPULSE ");     /* desk menu entry */
    graf_mouse(ARROW, NULL);

    vh = graf_handle(&wchar, &hchar, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[0] = Getrez() + 2;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    if (vh <= 0) {
        form_alert(1, "[3][FPulse: cannot open VDI|workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    ncolours_g = work_out[13];
    theme = system_theme();
    setup_colours();
    vst_alignment(vh, 0, 5, &d, &d);        /* left, top */
    vsf_perimeter(vh, 0);
    vsl_type(vh, SOLID);
    vsl_width(vh, 1);

    wind_get(0, WF_WORKXYWH, &desk.g_x, &desk.g_y, &desk.g_w, &desk.g_h);

    is_magic = (int)Supexec(magx_cookie);
    if (is_magic)
        ms_attach();                        /* CPU sampler (shared) */
    signal(SIGTERM, on_quit_signal);
    signal(SIGINT, on_quit_signal);
    signal(SIGHUP, on_quit_signal);
    last_sample_ms = now_ms();
    sample();                               /* baseline process times */
    if (!have_data) {
        form_alert(1, "[3][FPulse needs MultiTOS,|FreeMiNT or MagiC:|u:\\proc not found.][ Quit ]");
        ms_detach();
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    build_menu(work_out[0] + 1, work_out[1] + 1);
    menu_bar(mtree, 1);
    sync_menu();

    if (!open_window()) {
        form_alert(1, "[3][FPulse: no more windows.][ Quit ]");
        menu_bar(mtree, 0);
        ms_detach();
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    /* first real reading after a short interval, then at the set rate */
    next = now_ms() + 500L;

    while (running) {
        t = next - now_ms();
        if (t < 0) t = 0;
        if (hidebars && bars_out && t > 300)
            t = 300;                        /* watch for losing the focus */

        ev = evnt_multi(MU_MESAG | MU_TIMER | MU_KEYBD | MU_BUTTON,
                        1, 1, 1,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, t,
                        &mx, &my, &mb, &kstate, &key, &clicks);

        if (ev & MU_MESAG)
            running = handle_message(msg);
        if (quit_req)
            running = 0;

        if (running && (ev & MU_KEYBD) && (key & 0xff) == 0x11)
            running = 0;                    /* Ctrl+Q */

        /* bare window clicked while already on top: show the bars */
        if (running && (ev & MU_BUTTON) && hidebars && !bars_out &&
            win >= 0 && wind_find(mx, my) == win)
            show_bars();

        /* not every AES reports losing the top, so check as well */
        if (running)
            hide_bars_if_unfocused();

        if (running && now_ms() >= next) {
            sample();
            redraw_all();
            next += sample_ms;
            if (next < now_ms() || next > now_ms() + sample_ms)
                next = now_ms() + sample_ms;
        }
    }

    wind_close(win);
    wind_delete(win);
    menu_bar(mtree, 0);
    ms_detach();
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
