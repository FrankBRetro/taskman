/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Francis C Bullen
 * See the LICENSE file for the full licence text.
 */

/*
 * TaskMan - a GEM task manager for MultiTOS (MiNT 1.04/1.08) and FreeMiNT
 *
 * Three resizable windows, styled after a modern activity monitor:
 *
 *   Task Manager   two tabs:
 *                    CPU    - name, PID, PPID, nice, state, %CPU, CPU time,
 *                             memory
 *                    Memory - name, PID, memory, % of RAM, text/data/BSS
 *                             sizes, ST or TT RAM, memory protection mode
 *                  click a header (or use the Sort menu) to sort, click
 *                  again to reverse; click a row to select, double-click
 *                  to set its nice level; toolbar: Stop (SIGTERM),
 *                  Kill (SIGKILL), Nice...; footer with memory and CPU.
 *   CPU            single-CPU segmented meter (kernel portion in red).
 *   CPU History    scrolling LED-style graph, kernel (system) time in red
 *                  at the bottom, user time in green on top.
 *   Memory         segmented meter of physical RAM in use.
 *   Memory History scrolling graph, RAM in use (blue) under free (green).
 *
 *   The four meter/graph windows can be iconified (AES 4.1, e.g. MultiTOS):
 *   the icon shows a live miniature of the window and its title shows the
 *   current figure ("CPU 42%", "Mem 28%"), both updated every sample.
 *
 * Data source (works on MultiTOS, no u:\kern needed):
 *   u:\proc            one entry "NAME.PID" per process
 *   Fxattr()           size = memory used by the process
 *   Fcntl(PPROCADDR)   address of the kernel's process record; its first
 *                      fields (pid, ppid, pri, wait_q, systime, usrtime,
 *                      ... curpri) have kept the same layout since MiNT 1.08.
 *                      They are read in supervisor mode so memory protection
 *                      does not get in the way.
 *   CPU % = change in each process's sys+usr time over the sample period.
 *   The MiNT root process (pid 0) runs when nothing else does, so it is
 *   counted as idle.
 *   View > Hide AES removes the AES (AESSYS, XaAES, N.AES, MyAES) and the
 *   ALERT accessory from the list and leaves their CPU time and memory out
 *   of the totals and graphs, so they show what everything else is using.
 *
 * Colours: light and dark themes (toolbar switch or View menu); the start-up
 * theme follows the system palette (light when colour 0 is light).  In
 * true-colour modes the exact theme colours are set on private pens.  In
 * palette modes (2-256 colours) TaskMan adapts to the system palette as it
 * is, never changing it: each colour is the closest existing one that still
 * contrasts with whatever it is drawn on, so text never disappears.
 *
 * Build: m68k-atari-mint-gcc -O2 -o taskman.app taskman.c -lgem
 *
 * Menu bar: TaskMan (About, desk accessories), File (Quit), Process (Stop, Kill, Set Nice),
 * View (show/hide each window, Show All, Hide AES, Light/Dark theme,
 * CPU/Memory tab), Sort (by name,
 * memory or CPU; ascending/descending), Rate (sample interval 0.5 - 20 s).  Closing a window hides
 * it; reopen it from the View menu.
 *
 * Keys: Ctrl+Q quit, Up/Down move selection, Delete = Stop, Tab = switch tab.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAXTASK     128
#define HIST        512
#define GCOLW       4           /* history: 3 px LED + 1 px gap */
#define PROCREC     68          /* bytes of the process record we read */

#ifndef PPROCADDR
#define PPROCADDR   0x5001      /* ('P'<<8)|1 */
#endif
#ifndef PBASEADDR
#define PBASEADDR   0x5002
#endif
#ifndef PGETFLAGS
#define PGETFLAGS   0x5005
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
#ifndef SMALLER
#define SMALLER         0x4000  /* iconifier gadget (AES 4.1)      */
#endif
#ifndef WM_ICONIFY
#define WM_ICONIFY      34
#define WM_UNICONIFY    35
#define WM_ALLICONIFY   36
#endif
#ifndef WF_ICONIFY
#define WF_ICONIFY      26
#define WF_UNICONIFY    27
#endif
#ifndef WM_ARROWED
#define WM_ARROWED  24
#endif
#ifndef WM_VSLID
#define WM_VSLID    26
#endif
#ifndef WF_VSLIDE
#define WF_VSLIDE   9
#endif
#ifndef WF_VSLSIZE
#define WF_VSLSIZE  16
#endif
#ifndef UPARROW
#define UPARROW     0x0040
#define DNARROW     0x0080
#define VSLIDE      0x0100
#endif
#define WA_UPPAGE   0
#define WA_DNPAGE   1
#define WA_UPLINE   2
#define WA_DNLINE   3

/* offsets into the process record, measured from p->magic */
#define OFF_PID     8
#define OFF_PPID    10
#define OFF_PRI     24
#define OFF_WAITQ   26
#define OFF_SYSTIME 32
#define OFF_USRTIME 36
#define OFF_CURPRI  66

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

enum {
    C_BG, C_ROWALT, C_HDR, C_PANEL, C_TEXT, C_DIM, C_SEL, C_SELTXT,
    C_GRID, C_BTN, C_ACCENT, C_USR, C_SYS, C_BLACK, C_METON, C_METOFF,
    NPEN
};

/* C_BLACK is the graph/meter background (black in the dark theme) */
enum { THEME_DARK, THEME_LIGHT, NTHEME };
static int theme = THEME_DARK;

static const unsigned char theme_rgb[NTHEME][NPEN][3] = {
  { /* dark */
    {  30,  30,  33 },  /* C_BG      window background      */
    {  42,  42,  46 },  /* C_ROWALT  alternate row          */
    {  56,  56,  62 },  /* C_HDR     column header          */
    {  44,  44,  48 },  /* C_PANEL   toolbar / footer       */
    { 236, 236, 236 },  /* C_TEXT                           */
    { 150, 150, 156 },  /* C_DIM     secondary text         */
    {  38,  92, 204 },  /* C_SEL     selected row           */
    { 255, 255, 255 },  /* C_SELTXT                         */
    {  78,  78,  84 },  /* C_GRID    separators             */
    {  74,  74,  80 },  /* C_BTN     buttons                */
    { 255, 196,   0 },  /* C_ACCENT  sort arrow             */
    {  64, 204,  64 },  /* C_USR     user time (green)      */
    { 222,  48,  40 },  /* C_SYS     kernel time (red)      */
    {   0,   0,   0 },  /* C_BLACK   graph background       */
    {  72, 144, 244 },  /* C_METON   meter lit (blue)       */
    {  24,  34,  54 },  /* C_METOFF  meter unlit            */
  },
  { /* light */
    { 255, 255, 255 },  /* C_BG      */
    { 242, 242, 246 },  /* C_ROWALT  */
    { 226, 226, 230 },  /* C_HDR     */
    { 236, 236, 238 },  /* C_PANEL   */
    {   0,   0,   0 },  /* C_TEXT    */
    { 100, 100, 106 },  /* C_DIM     */
    {  38,  92, 204 },  /* C_SEL     */
    { 255, 255, 255 },  /* C_SELTXT  */
    { 186, 186, 192 },  /* C_GRID    */
    { 212, 212, 218 },  /* C_BTN     */
    { 200, 110,   0 },  /* C_ACCENT  */
    {   0, 150,   0 },  /* C_USR     */
    { 204,  32,  24 },  /* C_SYS     */
    { 255, 255, 255 },  /* C_BLACK   graph background */
    {  38,  92, 204 },  /* C_METON   */
    { 212, 222, 238 },  /* C_METOFF  */
  },
};

static short pen[NPEN];
static int   sel_xor;           /* selection colour == background: XOR it */

/* ------------------------------------------------------------------ */
/* Process data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    short pid, ppid, pri, curpri, state;
    char  name[16];
    long  addr;                 /* &p->magic, 0 if unknown            */
    long  mem;                  /* bytes                              */
    unsigned long sys, usr;     /* ms                                 */
    unsigned long psys, pusr;   /* previous sample                    */
    int   have_prev;
    short cpu;                  /* permille of the sample period      */
    short nice;                 /* -pri: -20 highest .. 20 lowest     */
    int   seen;
    unsigned char raw[PROCREC];
    /* memory detail, from the basepage and program flags */
    long  bp;                   /* basepage address, 0 if none        */
    long  mflags;               /* prgflags (protection in bits 4-7)  */
    int   have_flags;
    int   bp_state;             /* 0 unread, 1 ok, -1 unusable        */
    long  tlen, dlen, blen, tpa;
    unsigned char bpraw[32];
} TASK;

static TASK  tasks[MAXTASK];
static int   ntask;
static short order[MAXTASK];
static int   proc_ok = 1;
static int   nrows;             /* rows in the list (after filtering)  */
static int   hide_aes;          /* View > Hide AES                     */

/* the AES (and its desktop thread) runs as an ordinary MiNT process;
 * recognise it by name: MultiTOS AESSYS, XaAES, N.AES, MyAES (prefix),
 * plus the ALERT accessory, which polls constantly (exact name) */
static int is_aes(const char *name)
{
    static const char *aes_names[] = { "AESSYS", "XAAES", "N_AES", "NAES", "MYAES" };
    int i, j;

    for (j = 0; "ALERT"[j]; j++) {
        char c = name[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != "ALERT"[j]) break;
    }
    if (!"ALERT"[j] && name[j] == 0)
        return 1;               /* exact match */

    for (i = 0; i < (int)(sizeof aes_names / sizeof aes_names[0]); i++) {
        const char *a = aes_names[i];
        for (j = 0; a[j]; j++) {
            char c = name[j];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c != a[j]) break;
        }
        if (!a[j])
            return 1;           /* prefix match */
    }
    return 0;
}

static int hidden(const char *name)
{
    return hide_aes && is_aes(name);
}

static short cpu_usr, cpu_sys;  /* permille                           */
static short h_usr[HIST], h_sys[HIST];
static short h_mused[HIST], h_mfree[HIST];  /* permille of physical RAM */
static short mem_used_pm;
static int   h_head, h_count;

static long  mem_st, mem_tt, mem_tasks, free_st, free_tt;
static long  last_sample_ms;
static long  sample_ms = 1000L;
static short my_pid;

static const char *state_name[] = {
    "Run", "Ready", "Wait", "IO", "Zombie", "TSR", "Stop", "Select"
};

/* ------------------------------------------------------------------ */
/* UI state                                                            */
/* ------------------------------------------------------------------ */

enum { W_TASKS, W_CPU, W_HIST, W_MEM, W_MHIST, NWIN };

typedef struct {
    short h;
    short kind;
    char  title[32];
    GRECT prev;
    int   fulled;
    int   iconified;
    GRECT normal;               /* window rect before iconifying      */
    char  ititle[16];           /* live title while iconified         */
} WIN;

static WIN wins[NWIN] = {
    { -1, NAME|CLOSER|FULLER|MOVER|SIZER|UPARROW|DNARROW|VSLIDE, " Task Manager ", {0,0,0,0}, 0, 0, {0,0,0,0}, "" },
    { -1, NAME|CLOSER|FULLER|MOVER|SIZER, " CPU ", {0,0,0,0}, 0, 0, {0,0,0,0}, "" },
    { -1, NAME|CLOSER|FULLER|MOVER|SIZER, " CPU History - Kernel ", {0,0,0,0}, 0, 0, {0,0,0,0}, "" },
    { -1, NAME|CLOSER|FULLER|MOVER|SIZER, " Memory ", {0,0,0,0}, 0, 0, {0,0,0,0}, "" },
    { -1, NAME|CLOSER|FULLER|MOVER|SIZER, " Memory History ", {0,0,0,0}, 0, 0, {0,0,0,0}, "" },
};

static int is_graph(int i) { return i == W_HIST || i == W_MHIST; }
static int can_iconify;         /* AES supports WF_ICONIFY            */
static int iconifiable(int i) { return i != W_TASKS; }

static short vh, ap_id, wchar, hchar;
static GRECT desk;

enum {
    K_NAME, K_PID, K_PPID, K_NICE, K_STATE, K_CPU, K_TIME, K_MEM,
    K_PCT, K_TEXT, K_DATA, K_BSS, K_REGION, K_PROT, NKEY
};

typedef struct { const char *title; short chars; short right; short key; } COLDEF;

#define NCOLMAX 9
enum { TAB_CPU, TAB_MEM, NTAB };
static const char *tab_label[NTAB] = { "CPU", "Memory" };
static const char *theme_label[2] = { "Dark", "Light" };

static const COLDEF cpu_cols[] = {
    { "Process",  10, 0, K_NAME  },
    { "PID",       5, 1, K_PID   },
    { "PPID",      5, 1, K_PPID  },
    { "Nice",      4, 1, K_NICE  },
    { "State",     7, 0, K_STATE },
    { "% CPU",     7, 1, K_CPU   },
    { "CPU Time",  9, 1, K_TIME  },
    { "Memory",    9, 1, K_MEM   },
};
static const COLDEF mem_cols[] = {
    { "Process",  10, 0, K_NAME   },
    { "PID",       5, 1, K_PID    },
    { "Memory",    9, 1, K_MEM    },
    { "% RAM",     6, 1, K_PCT    },
    { "Text",      8, 1, K_TEXT   },
    { "Data",      8, 1, K_DATA   },
    { "BSS",       8, 1, K_BSS    },
    { "RAM",       3, 0, K_REGION },
    { "Protect",   8, 0, K_PROT   },
};
static const COLDEF *tab_cols[NTAB] = { cpu_cols, mem_cols };
static const int tab_ncol[NTAB] = {
    sizeof cpu_cols / sizeof cpu_cols[0], sizeof mem_cols / sizeof mem_cols[0]
};

static int   cur_tab = TAB_CPU;
static int   sort_key[NTAB]  = { K_CPU, K_MEM };
static int   sort_desc[NTAB] = { 1, 1 };
static short sel_pid = -1;
static int   top_row;
static int   gdirty[NWIN] = { 1, 1, 1, 1, 1 };  /* graph needs a full redraw */
static short last_slpos = -1, last_slsize = -1;

enum { B_STOP, B_KILL, B_NICE, NBTN };
static const char *btn_label[NBTN] = { "Stop", "Kill", "Nice..." };

static const char *prot_name[] = { "Private", "Global", "Super", "Readable" };

static int default_desc(int key)
{
    return key == K_CPU || key == K_TIME || key == K_MEM || key == K_PCT ||
           key == K_TEXT || key == K_DATA || key == K_BSS;
}

/* ------------------------------------------------------------------ */
/* Low level helpers                                                   */
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

static int in_rect(short x, short y, const GRECT *r)
{
    return x >= r->g_x && y >= r->g_y &&
           x < r->g_x + r->g_w && y < r->g_y + r->g_h;
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

static void rbox(int c, const GRECT *r)
{
    box(c, r->g_x, r->g_y, r->g_x + r->g_w - 1, r->g_y + r->g_h - 1);
}

static void xorbox(short x1, short y1, short x2, short y2)
{
    short pxy[4];
    vswr_mode(vh, MD_XOR);
    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, G_BLACK);
    pxy[0] = x1; pxy[1] = y1; pxy[2] = x2; pxy[3] = y2;
    vr_recfl(vh, pxy);
    vswr_mode(vh, MD_REPLACE);
}

static void line(int c, short x1, short y1, short x2, short y2)
{
    short pxy[4];
    vswr_mode(vh, MD_REPLACE);
    vsl_color(vh, pen[c]);
    pxy[0] = x1; pxy[1] = y1; pxy[2] = x2; pxy[3] = y2;
    v_pline(vh, 2, pxy);
}

static void frame(int c, const GRECT *r)
{
    short pxy[10];
    vswr_mode(vh, MD_REPLACE);
    vsl_color(vh, pen[c]);
    pxy[0] = r->g_x;                  pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1;     pxy[3] = r->g_y;
    pxy[4] = r->g_x + r->g_w - 1;     pxy[5] = r->g_y + r->g_h - 1;
    pxy[6] = r->g_x;                  pxy[7] = r->g_y + r->g_h - 1;
    pxy[8] = r->g_x;                  pxy[9] = r->g_y;
    v_pline(vh, 5, pxy);
}

/* text, top-left aligned, clipped to maxchars (0 = no limit) */
static void txt(int c, short x, short y, const char *s, int maxchars)
{
    char buf[80];
    int n = (int)strlen(s);

    if (maxchars > 0 && n > maxchars)
        n = maxchars;
    if (n <= 0)
        return;
    if (n > (int)sizeof buf - 1)
        n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    vswr_mode(vh, MD_TRANS);
    vst_color(vh, pen[c]);
    v_gtext(vh, x, y, buf);
    vswr_mode(vh, MD_REPLACE);
}

static void txt_right(int c, short xr, short y, const char *s, int maxchars)
{
    int n = (int)strlen(s);
    if (maxchars > 0 && n > maxchars)
        n = maxchars;
    txt(c, xr - n * wchar, y, s, maxchars);
}

static void fmt_mem(long bytes, char *out)
{
    long k = (bytes + 1023) / 1024;
    if (k < 10000)
        sprintf(out, "%ldK", k);
    else
        sprintf(out, "%ld.%ldM", k / 1024, (k % 1024) * 10 / 1024);
}

static void fmt_time(unsigned long ms, char *out)
{
    unsigned long s = ms / 1000;
    if (s < 3600)
        sprintf(out, "%lu:%02lu.%lu", s / 60, s % 60, (ms % 1000) / 100);
    else
        sprintf(out, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

static void fmt_pm(int pm, char *out)
{
    if (pm < 0) pm = 0;
    sprintf(out, "%d.%d%%", pm / 10, pm % 10);
}

/* ------------------------------------------------------------------ */
/* Colour setup                                                        */
/* ------------------------------------------------------------------ */

static short ncolours_g;
static short pal[256][3];       /* system palette, 0..1000 per gun   */
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

/* Nearest palette entry to `want' whose brightness differs by at least
 * `minlum' from every role in `against' (already mapped, -1 ends the
 * list).  If `avoid' >= 0 the entry must look different from that pen
 * but stay within `dlimit' brightness of it.  Without a candidate that
 * meets the contrast rule, the most contrasting entry is used. */
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

static int pick_pen_pass(const unsigned char *want, const int *against, long minlum,
                         int avoid, long dlimit, int greys_only, int *found)
{
    long best = 0x7fffffffL, bestc = -1;
    int j, k, bj = -1, cj = 0;

    for (j = 0; j < npal; j++) {
        long minc = 100000L;
        if (greys_only && !is_grey(pal[j]))
            continue;
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

/* grey roles try the palette's greys first (black/white count as grey),
 * so stripes and buttons don't turn red or yellow on 4/8-colour screens */
static int pick_pen(const unsigned char *want, const int *against, long minlum,
                    int avoid, long dlimit)
{
    int found = 0, j;
    if (want_grey(want)) {
        j = pick_pen_pass(want, against, minlum, avoid, dlimit, 1, &found);
        if (found)
            return j;
        if (avoid >= 0)
            return -1;          /* no grey alternative: keep it the same */
    }
    return pick_pen_pass(want, against, minlum, avoid, dlimit, 0, &found);
}

/* background role, preferring to look different from `other' */
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
    static const int on_text[] = { C_BG, C_ROWALT, C_PANEL, C_BTN, C_HDR, C_BLACK, -1 };
    /* secondary text must read clearly on rows, headers and panels; on a
     * button it is the "disabled" look, so it may be fainter there */
    static const int on_dim[]  = { C_BG, C_ROWALT, C_PANEL, C_HDR, C_BLACK, -1 };
    static const int on_dimb[] = { C_BG, C_ROWALT, C_PANEL, C_HDR, C_BLACK, C_BTN, -1 };
    static const int on_sel[]  = { C_SEL, -1 };
    static const int on_grid[] = { C_BG, C_PANEL, C_HDR, -1 };
    static const int on_hdr[]  = { C_HDR, -1 };
    static const int on_usr[]  = { C_BLACK, C_BG, C_ROWALT, C_PANEL, -1 };
    static const int on_sys[]  = { C_BLACK, C_BG, C_PANEL, -1 };
    static const int on_met[]  = { C_BLACK, C_BG, -1 };
    short ext[57], rgb[3];
    int i;

    vq_extnd(vh, 1, ext);

    if (ext[4] >= 15 && ext[5] == 0 && ncolours_g >= 16 + NPEN) {
        /* true colour: our own pens, exact colours */
        for (i = 0; i < NPEN; i++) {
            rgb[0] = (short)(theme_rgb[theme][i][0] * 1000L / 255);
            rgb[1] = (short)(theme_rgb[theme][i][1] * 1000L / 255);
            rgb[2] = (short)(theme_rgb[theme][i][2] * 1000L / 255);
            vs_color(vh, 16 + i, rgb);
            pen[i] = 16 + i;
        }
        sel_xor = 0;
        return;
    }

    /* palette modes: use the system palette as it is, never change it,
     * and make sure every text colour stands out from what it is drawn on */
    npal = ncolours_g > 256 ? 256 : ncolours_g;
    if (npal < 2) npal = 2;
    for (i = 0; i < npal; i++)
        vq_color(vh, i, 1, pal[i]);

    map_bg(C_BG, -1, 0);
    /* stripes only when the palette has a shade close to the background;
     * a heavy stripe (e.g. light grey on white) makes text hard to read */
    map_bg(C_ROWALT, C_BG, 150);
    map_bg(C_HDR, -1, 0);
    map_bg(C_PANEL, -1, 0);
    map_bg(C_BTN, C_PANEL, 350);
    map_bg(C_BLACK, -1, 0);
    map_bg(C_SEL, C_BG, 1000);

    map_fg(C_TEXT,   on_text, 450);
    map_fg(C_DIM,    on_dim,  300);
    {
        long d = lum_of(pal[pen[C_DIM]]) - lum_of(pal[pen[C_BTN]]);
        if ((d < 0 ? -d : d) < 150)     /* would vanish on buttons */
            map_fg(C_DIM, on_dimb, 200);
    }
    map_fg(C_SELTXT, on_sel,  400);
    map_fg(C_GRID,   on_grid,  80);
    map_fg(C_ACCENT, on_hdr,  250);
    map_fg(C_USR,    on_usr,  120);
    map_fg(C_SYS,    on_sys,  150);
    map_fg(C_METON,  on_met,  100);

    /* unlit meter: its own shade if there is one, never the lit colour */
    map_bg(C_METOFF, C_BLACK, 400);
    if (same_colour(pen[C_METOFF], pen[C_METON]))
        pen[C_METOFF] = pen[C_BLACK];

    sel_xor = same_colour(pen[C_SEL], pen[C_BG]);
}

/* light theme if the system's colour 0 (GEM's window/desk white) is light */
static int system_theme(void)
{
    short c[3];
    vq_color(vh, 0, 1, c);
    return lum_of(c) > 500 ? THEME_LIGHT : THEME_DARK;
}

/* ------------------------------------------------------------------ */
/* Process sampling                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned short mode;
    long           index;
    unsigned short dev, rdev, nlink, uid, gid;
    long           size, blksize, nblocks;
    unsigned short mtime, mdate, atime, adate, ctime, cdate;
    unsigned short attr, res2;
    long           res3[2];
} __attribute__((packed)) MYXATTR;

static TASK *find_task(short pid)
{
    int i;
    for (i = 0; i < ntask; i++)
        if (tasks[i].pid == pid)
            return &tasks[i];
    return NULL;
}

static long read_procs_super(void)
{
    int i;
    for (i = 0; i < ntask; i++) {
        if (tasks[i].addr)
            memcpy(tasks[i].raw, (const void *)tasks[i].addr, PROCREC);
        /* basepage: read once per process (it does not change) */
        if (tasks[i].bp_state == 0 && tasks[i].bp && !(tasks[i].bp & 1))
            memcpy(tasks[i].bpraw, (const void *)tasks[i].bp, 32);
    }
    return 0;
}

/* basepage: p_lowtpa, p_hitpa, p_tbase, p_tlen, p_dbase, p_dlen,
 * p_bbase, p_blen */
static void parse_basepage(TASK *t)
{
    unsigned long low, hi, tl, dl, bl;

    if (t->bp_state != 0)
        return;
    if (!t->bp || (t->bp & 1)) {
        t->bp_state = -1;
        return;
    }
    low = g32(t->bpraw);
    hi  = g32(t->bpraw + 4);
    tl  = g32(t->bpraw + 12);
    dl  = g32(t->bpraw + 20);
    bl  = g32(t->bpraw + 28);
    if (low != (unsigned long)t->bp || hi <= low ||
        tl > hi - low || dl > hi - low || bl > hi - low) {
        t->bp_state = -1;           /* not a normal program basepage */
        return;
    }
    t->tpa  = (long)(hi - low);
    t->tlen = (long)tl;
    t->dlen = (long)dl;
    t->blen = (long)bl;
    t->bp_state = 1;
}

static void scan_procs(void)
{
    long dh;
    char buf[64], path[48];
    int i, j;

    for (i = 0; i < ntask; i++)
        tasks[i].seen = 0;

    dh = Dopendir("u:\\proc", 0);
    if (dh < 0) {
        proc_ok = 0;
        ntask = 0;
        return;
    }
    proc_ok = 1;

    while (Dreaddir(sizeof buf, dh, buf) == 0) {
        char *nm = buf + 4, *dot;
        short pid;
        int len;
        TASK *t;
        MYXATTR xa;

        if (nm[0] == '.')
            continue;
        dot = strrchr(nm, '.');
        if (!dot || dot == nm)
            continue;
        pid = (short)atoi(dot + 1);
        len = (int)(dot - nm);
        if (len > 15) len = 15;

        t = find_task(pid);
        if (t && (strncmp(t->name, nm, len) != 0 || t->name[len] != 0))
            t->addr = 0, t->have_prev = 0;      /* pid re-used */

        if (!t) {
            if (ntask >= MAXTASK)
                continue;
            t = &tasks[ntask++];
            memset(t, 0, sizeof *t);
            t->pid = pid;
        }
        memcpy(t->name, nm, len);
        t->name[len] = 0;
        t->seen = 1;

        sprintf(path, "u:\\proc\\%s", nm);

        if (!t->addr) {
            long fd = Fopen(path, 0);
            if (fd >= 0) {
                long a = 0, bp = 0, fl = 0;
                if (Fcntl((short)fd, (long)&a, PPROCADDR) == 0)
                    t->addr = a;
                if (Fcntl((short)fd, (long)&bp, PBASEADDR) == 0)
                    t->bp = bp;
                if (Fcntl((short)fd, (long)&fl, PGETFLAGS) == 0) {
                    t->mflags = fl;
                    t->have_flags = 1;
                }
                t->bp_state = 0;
                Fclose((short)fd);
            }
        }
        if (Fxattr(0, path, &xa) == 0)
            t->mem = xa.size;
    }
    Dclosedir(dh);

    /* drop exited processes */
    for (i = j = 0; i < ntask; i++)
        if (tasks[i].seen) {
            if (i != j) tasks[j] = tasks[i];
            j++;
        }
    ntask = j;
}

static long read_phystop(void) { return *(volatile long *)0x42eL; }
static long read_ramtop(void)
{
    if (*(volatile long *)0x5a8L == 0x1357BD13L)
        return *(volatile long *)0x5a4L;
    return 0;
}

static void sample(void)
{
    long now = now_ms(), dt = now - last_sample_ms;
    unsigned long sum_all = 0, busy = 0, ksys = 0, total;
    int i;

    last_sample_ms = now;
    scan_procs();
    Supexec(read_procs_super);

    for (i = 0; i < ntask; i++) {
        TASK *t = &tasks[i];
        if (!t->addr || g16(t->raw + OFF_PID) != t->pid) {
            t->addr = 0;                /* stale: re-read address next time */
            t->have_prev = 0;
            t->cpu = 0;
            continue;
        }
        t->ppid   = g16(t->raw + OFF_PPID);
        t->pri    = g16(t->raw + OFF_PRI);
        t->state  = g16(t->raw + OFF_WAITQ);
        t->curpri = g16(t->raw + OFF_CURPRI);
        t->nice   = -t->pri;
        parse_basepage(t);
        t->sys    = g32(t->raw + OFF_SYSTIME);
        t->usr    = g32(t->raw + OFF_USRTIME);

        if (t->have_prev) {
            long ds = (long)(t->sys - t->psys);
            long du = (long)(t->usr - t->pusr);
            if (ds < 0) ds = 0;
            if (du < 0) du = 0;
            sum_all += ds + du;
            if (t->pid != 0 && !hidden(t->name)) {
                busy += ds + du;
                ksys += ds;
            }
        }
    }

    total = sum_all;
    if (dt > 0 && (unsigned long)dt > total)
        total = (unsigned long)dt;
    if (total == 0)
        total = 1;

    for (i = 0; i < ntask; i++) {
        TASK *t = &tasks[i];
        if (t->addr && t->have_prev) {
            long d = (long)(t->sys - t->psys) + (long)(t->usr - t->pusr);
            if (d < 0) d = 0;
            t->cpu = (short)(d * 1000L / (long)total);
        } else
            t->cpu = 0;
        if (t->addr) {
            t->psys = t->sys;
            t->pusr = t->usr;
            t->have_prev = 1;
        }
    }

    cpu_sys = (short)(ksys * 1000UL / total);
    cpu_usr = (short)((busy - ksys) * 1000UL / total);
    if (cpu_sys > 1000) cpu_sys = 1000;
    if (cpu_usr + cpu_sys > 1000) cpu_usr = 1000 - cpu_sys;

    h_usr[h_head] = cpu_usr;
    h_sys[h_head] = cpu_sys;
    h_head = (h_head + 1) % HIST;
    if (h_count < HIST)
        h_count++;

    /* memory */
    mem_st = Supexec(read_phystop);
    mem_tt = Supexec(read_ramtop);
    if (mem_tt > 0x01000000L)
        mem_tt -= 0x01000000L;
    else
        mem_tt = 0;
    free_st = (long)Mxalloc(-1L, 0);
    free_tt = mem_tt ? (long)Mxalloc(-1L, 1) : 0;
    if (free_st < 0) free_st = 0;
    if (free_tt < 0) free_tt = 0;
    mem_tasks = 0;
    for (i = 0; i < ntask; i++)
        if (!hidden(tasks[i].name))
            mem_tasks += tasks[i].mem;

    /* memory in use = physical - free (GEMDOS reports the largest free
     * block of ST and of TT RAM, so fragmentation counts as used) */
    {
        long phys = mem_st + mem_tt, used = phys - free_st - free_tt;
        int idx = (h_head - 1 + HIST) % HIST;
        if (used < 0) used = 0;
        mem_used_pm = phys >= 1024 ? (short)(used / 1024 * 1000L / (phys / 1024)) : 0;
        if (mem_used_pm > 1000) mem_used_pm = 1000;
        h_mused[idx] = mem_used_pm;
        h_mfree[idx] = 1000 - mem_used_pm;
    }
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

static long key_value(const TASK *t, int key)
{
    switch (key) {
    case K_PID:    return t->pid;
    case K_PPID:   return t->ppid;
    case K_NICE:   return t->nice;
    case K_STATE:  return t->state;
    case K_CPU:    return t->cpu;
    case K_MEM:
    case K_PCT:    return t->mem;
    case K_TEXT:   return t->bp_state == 1 ? t->tlen : -1;
    case K_DATA:   return t->bp_state == 1 ? t->dlen : -1;
    case K_BSS:    return t->bp_state == 1 ? t->blen : -1;
    case K_REGION: return t->bp ? (t->bp >= 0x01000000L ? 1 : 0) : -1;
    case K_PROT:   return t->have_flags ? (t->mflags >> 4) & 0x0f : -1;
    }
    return 0;
}

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb || !ca)
            return ca - cb;
    }
}

static int cmp_task(const void *a, const void *b)
{
    const TASK *x = &tasks[*(const short *)a];
    const TASK *y = &tasks[*(const short *)b];
    int key = sort_key[cur_tab];
    long r;

    if (key == K_NAME) {
        r = name_cmp(x->name, y->name);
    } else if (key == K_TIME) {
        unsigned long tx = x->sys + x->usr, ty = y->sys + y->usr;
        r = tx < ty ? -1 : tx > ty ? 1 : 0;
    } else {
        long vx = key_value(x, key), vy = key_value(y, key);
        r = vx < vy ? -1 : vx > vy ? 1 : 0;
    }
    if (r == 0)
        r = x->pid - y->pid;
    if (sort_desc[cur_tab])
        r = -r;
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

static void sort_tasks(void)
{
    int i;
    nrows = 0;
    for (i = 0; i < ntask; i++)
        if (!hidden(tasks[i].name))
            order[nrows++] = (short)i;
    qsort(order, nrows, sizeof order[0], cmp_task);
}

static int sel_index(void)
{
    int i;
    for (i = 0; i < nrows; i++)
        if (tasks[order[i]].pid == sel_pid)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Task window                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    GRECT tb, hdr, list, foot;
    GRECT btn[NBTN];
    GRECT tab[NTAB];
    GRECT thm[NTHEME];          /* Light | Dark switch (g_w 0: hidden) */
    short colx[NCOLMAX + 1];    /* column left edges, colx[ncol] = right */
    int   ncol;
    short rowh;
    int   vis;
} TLAYOUT;

static void tasks_layout(const GRECT *w, TLAYOUT *L)
{
    const COLDEF *cols = tab_cols[cur_tab];
    short y = w->g_y, h = w->g_h, x, tw;
    short tbh = hchar + 12, hdrh = hchar + 4, footh = 4 * (hchar + 2) + 12;
    int i, other = 0;

    L->rowh = hchar + 3;
    L->ncol = tab_ncol[cur_tab];

    L->tb.g_x = w->g_x; L->tb.g_y = y; L->tb.g_w = w->g_w; L->tb.g_h = tbh;
    y += tbh; h -= tbh;

    if (h < hdrh + footh + 3 * L->rowh)
        footh = 0;
    L->foot.g_x = w->g_x; L->foot.g_w = w->g_w; L->foot.g_h = footh;
    L->foot.g_y = w->g_y + w->g_h - footh;
    h -= footh;

    L->hdr.g_x = w->g_x; L->hdr.g_y = y; L->hdr.g_w = w->g_w; L->hdr.g_h = hdrh;
    y += hdrh; h -= hdrh;

    L->list.g_x = w->g_x; L->list.g_y = y; L->list.g_w = w->g_w;
    L->list.g_h = h > 0 ? h : 0;
    L->vis = L->list.g_h / L->rowh;

    /* action buttons on the left */
    x = w->g_x + 6;
    for (i = 0; i < NBTN; i++) {
        L->btn[i].g_x = x;
        L->btn[i].g_y = L->tb.g_y + 4;
        L->btn[i].g_w = (short)((strlen(btn_label[i]) + 2) * wchar);
        L->btn[i].g_h = hchar + 4;
        x += L->btn[i].g_w + 4;
    }

    /* segmented tab control: centred if there is room, else after buttons */
    tw = 0;
    for (i = 0; i < NTAB; i++)
        tw += (short)((strlen(tab_label[i]) + 4) * wchar);
    {
        short tx = w->g_x + (w->g_w - tw) / 2;
        if (tx < x + 2 * wchar)
            tx = x + 2 * wchar;
        /* Light | Dark switch at the right; tabs move left to make room */
        short sw = 0, sx;
        for (i = 0; i < NTHEME; i++)
            sw += (short)((strlen(theme_label[i]) + 2) * wchar);
        sx = w->g_x + w->g_w - sw - 6;
        if (tx + tw + 2 * wchar > sx)
            tx = x + 2 * wchar;
        for (i = 0; i < NTAB; i++) {
            L->tab[i].g_x = tx;
            L->tab[i].g_y = L->tb.g_y + 4;
            L->tab[i].g_w = (short)((strlen(tab_label[i]) + 4) * wchar);
            L->tab[i].g_h = hchar + 4;
            tx += L->tab[i].g_w;
        }
        /* order on screen: Light, Dark */
        for (i = 0; i < NTHEME; i++) {
            int th = NTHEME - 1 - i;
            GRECT *r = &L->thm[th];
            r->g_x = sx;
            r->g_y = L->tb.g_y + 4;
            r->g_w = (short)((strlen(theme_label[th]) + 2) * wchar);
            r->g_h = hchar + 4;
            sx += r->g_w;
        }
        if (L->thm[THEME_LIGHT].g_x < tx + 2 * wchar)
            for (i = 0; i < NTHEME; i++)
                L->thm[i].g_w = 0;          /* no room: use the View menu */
    }

    /* columns: the name column gets whatever is left */
    for (i = 1; i < L->ncol; i++)
        other += cols[i].chars + 1;
    {
        short namew = w->g_w / wchar - other - 1;
        if (namew < cols[0].chars) namew = cols[0].chars;
        x = w->g_x + wchar / 2;
        L->colx[0] = x;
        x += (namew + 1) * wchar;
        for (i = 1; i < L->ncol; i++) {
            L->colx[i] = x;
            x += (cols[i].chars + 1) * wchar;
        }
        L->colx[L->ncol] = x;
    }
    /* leave out columns that would be cut off at the right edge */
    while (L->ncol > 2 && L->colx[L->ncol] - wchar > w->g_x + w->g_w - 1)
        L->ncol--;
}

static int btn_enabled(int b)
{
    (void)b;
    return sel_pid > 0 && sel_pid != my_pid && find_task(sel_pid) != NULL;
}

static void draw_toolbar(const TLAYOUT *L)
{
    int i;

    rbox(C_PANEL, &L->tb);
    line(C_GRID, L->tb.g_x, L->tb.g_y + L->tb.g_h - 1,
         L->tb.g_x + L->tb.g_w - 1, L->tb.g_y + L->tb.g_h - 1);

    for (i = 0; i < NBTN; i++) {
        const GRECT *b = &L->btn[i];
        int on = btn_enabled(i);
        rbox(C_BTN, b);
        frame(on ? C_DIM : C_GRID, b);
        txt(on ? C_TEXT : C_DIM, b->g_x + wchar, b->g_y + 2, btn_label[i], 0);
    }

    for (i = 0; i < NTAB; i++) {
        const GRECT *t = &L->tab[i];
        int act = (i == cur_tab);
        rbox(act ? (sel_xor ? C_BTN : C_SEL) : C_BTN, t);
        frame(C_DIM, t);
        txt(act ? C_SELTXT : C_DIM, t->g_x + 2 * wchar, t->g_y + 2, tab_label[i], 0);
        if (act && sel_xor)
            xorbox(t->g_x + 1, t->g_y + 1, t->g_x + t->g_w - 2, t->g_y + t->g_h - 2);
    }

    for (i = 0; i < NTHEME; i++) {
        const GRECT *t = &L->thm[i];
        int act = (i == theme);
        if (t->g_w <= 0)
            continue;
        rbox(act ? (sel_xor ? C_BTN : C_SEL) : C_BTN, t);
        frame(C_DIM, t);
        txt(act ? C_SELTXT : C_DIM, t->g_x + wchar, t->g_y + 2, theme_label[i], 0);
        if (act && sel_xor)
            xorbox(t->g_x + 1, t->g_y + 1, t->g_x + t->g_w - 2, t->g_y + t->g_h - 2);
    }
}

static void draw_header(const TLAYOUT *L)
{
    const COLDEF *cols = tab_cols[cur_tab];
    int i;
    short y = L->hdr.g_y + 2;

    rbox(C_HDR, &L->hdr);
    for (i = 0; i < L->ncol; i++) {
        short x1 = L->colx[i], x2 = L->colx[i + 1] - wchar;
        int sorted = (cols[i].key == sort_key[cur_tab]);
        int c = sorted ? C_TEXT : C_DIM;
        short tl = (short)strlen(cols[i].title);

        if (cols[i].right)
            txt_right(c, x2, y, cols[i].title, 0);
        else
            txt(c, x1, y, cols[i].title, 0);

        if (sorted) {
            char a[2];
            a[0] = sort_desc[cur_tab] ? 2 : 1;  /* Atari font: down/up arrows */
            a[1] = 0;
            if (cols[i].right)
                txt(C_ACCENT, x2 - (tl + 1) * wchar, y, a, 0);
            else
                txt(C_ACCENT, x1 + (tl + 1) * wchar, y, a, 0);
        }
        if (i > 0)
            line(C_GRID, x1 - wchar / 2, L->hdr.g_y + 2,
                 x1 - wchar / 2, L->hdr.g_y + L->hdr.g_h - 3);
    }
    line(C_GRID, L->hdr.g_x, L->hdr.g_y + L->hdr.g_h - 1,
         L->hdr.g_x + L->hdr.g_w - 1, L->hdr.g_y + L->hdr.g_h - 1);
}

/* text for one cell; may change the colour */
static void cell_text(const TASK *t, int key, char *s, int *c, int plain)
{
    long phys = mem_st + mem_tt;

    switch (key) {
    case K_NAME:  strcpy(s, t->name); break;
    case K_PID:   sprintf(s, "%d", t->pid); break;
    case K_PPID:  sprintf(s, "%d", t->ppid); break;
    case K_NICE:
        sprintf(s, "%d", t->nice);
        if (plain && t->nice != 0) *c = t->nice < 0 ? C_ACCENT : C_DIM;
        break;
    case K_STATE:
        strcpy(s, (t->state >= 0 && t->state < 8) ? state_name[t->state] : "?");
        if (plain) *c = (t->state == 0 || t->state == 1) ? C_USR : C_DIM;
        break;
    case K_CPU:
        fmt_pm(t->cpu, s);
        if (plain && t->cpu >= 500) *c = C_SYS;
        break;
    case K_TIME:  fmt_time(t->sys + t->usr, s); break;
    case K_MEM:   fmt_mem(t->mem, s); break;
    case K_PCT:
        fmt_pm(phys > 0 ? (int)(t->mem / 1024 * 1000L / (phys / 1024)) : 0, s);
        break;
    case K_TEXT:  if (t->bp_state == 1) fmt_mem(t->tlen, s); else strcpy(s, "-"); break;
    case K_DATA:  if (t->bp_state == 1) fmt_mem(t->dlen, s); else strcpy(s, "-"); break;
    case K_BSS:   if (t->bp_state == 1) fmt_mem(t->blen, s); else strcpy(s, "-"); break;
    case K_REGION:
        strcpy(s, !t->bp ? "-" : t->bp >= 0x01000000L ? "TT" : "ST");
        if (plain && t->bp >= 0x01000000L) *c = C_METON;
        break;
    case K_PROT:
        if (t->have_flags)
            strcpy(s, prot_name[((t->mflags >> 4) & 0x0f) & 3]);
        else
            strcpy(s, "-");
        if (plain) *c = C_DIM;
        break;
    default: s[0] = 0;
    }
    if (!t->addr && (key == K_PPID || key == K_NICE || key == K_STATE ||
                     key == K_CPU || key == K_TIME))
        strcpy(s, "-");
}

static void draw_rows(const TLAYOUT *L, const GRECT *clip)
{
    const COLDEF *cols = tab_cols[cur_tab];
    int r;
    short xr = L->list.g_x + L->list.g_w - 1;

    for (r = 0; r <= L->vis; r++) {
        int idx = top_row + r;
        short y1 = L->list.g_y + r * L->rowh;
        short y2 = y1 + L->rowh - 1;
        GRECT rr;

        if (y1 >= L->list.g_y + L->list.g_h)
            break;
        if (y2 >= L->list.g_y + L->list.g_h)
            y2 = L->list.g_y + L->list.g_h - 1;

        rr.g_x = L->list.g_x; rr.g_y = y1; rr.g_w = L->list.g_w; rr.g_h = y2 - y1 + 1;
        if (!rect_intersect(clip, &rr))
            continue;

        if (idx >= nrows) {
            box(C_BG, L->list.g_x, y1, xr, y2);
            continue;
        }

        {
            TASK *t = &tasks[order[idx]];
            int sel = (t->pid == sel_pid);
            int plain = !sel || sel_xor;
            int bg = !plain ? C_SEL : (idx & 1) ? C_ROWALT : C_BG;
            int fg = !plain ? C_SELTXT : C_TEXT;
            short ty = y1 + 1;
            char s[24];
            int i;

            box(bg, L->list.g_x, y1, xr, y2);

            for (i = 0; i < L->ncol; i++) {
                short x1 = L->colx[i], x2 = L->colx[i + 1] - wchar;
                int w = (x2 - x1) / wchar + 1;
                int c = fg;

                cell_text(t, cols[i].key, s, &c, plain);
                if (cols[i].right)
                    txt_right(c, x2, ty, s, w);
                else
                    txt(c, x1, ty, s, w);
            }
            if (sel && sel_xor)
                xorbox(L->list.g_x, y1, xr, y2);
        }
    }
}

/* horizontal bar: part of total, with label */
static void meter_bar(short x, short y, short xr, short yb, long part, long total, int c)
{
    GRECT fr;
    short w = xr - x - 1, pw;

    if (w < 4) return;
    pw = total > 0 ? (short)(part / 1024 * w / (total / 1024 > 0 ? total / 1024 : 1)) : 0;
    if (pw > w) pw = w;
    box(C_BLACK, x, y, xr, yb);
    if (pw > 0) box(c, x + 1, y + 1, x + pw, yb - 1);
    fr.g_x = x; fr.g_y = y; fr.g_w = xr - x + 1; fr.g_h = yb - y + 1;
    frame(C_GRID, &fr);
}

static void draw_footer(const TLAYOUT *L)
{
    const GRECT *f = &L->foot;
    short lh = hchar + 2, y = f->g_y + 6, x1 = f->g_x + wchar, x2;
    char a[24], b[24], s[64];
    long phys = mem_st + mem_tt;

    if (f->g_h <= 0)
        return;

    rbox(C_PANEL, f);
    line(C_GRID, f->g_x, f->g_y, f->g_x + f->g_w - 1, f->g_y);

    /* memory block (both tabs) */
    txt(C_DIM, x1, y, "MEMORY", 0);
    fmt_mem(phys, a);
    sprintf(s, "Physical:  %s", a);
    txt(C_TEXT, x1, y + lh, s, 0);
    fmt_mem(mem_tasks, a);
    sprintf(s, "Tasks:     %s", a);
    txt(C_TEXT, x1, y + 2 * lh, s, 0);
    fmt_mem(free_st, a);
    fmt_mem(free_tt, b);
    if (mem_tt)
        sprintf(s, "Free ST %s  TT %s", a, b);
    else
        sprintf(s, "Free:      %s", a);
    txt(C_DIM, x1, y + 3 * lh, s, 0);

    x2 = f->g_x + 26 * wchar;
    if (x2 + 16 * wchar >= f->g_x + f->g_w)
        return;
    line(C_GRID, x2 - wchar, f->g_y + 4, x2 - wchar, f->g_y + f->g_h - 5);

    if (cur_tab == TAB_MEM) {
        /* RAM usage bars by region (tasks by where their program lives) */
        long st_used = 0, tt_used = 0;
        short bx = x2 + 9 * wchar, bxr = f->g_x + f->g_w - wchar - 1;
        int i;

        for (i = 0; i < ntask; i++) {
            if (hidden(tasks[i].name)) continue;
            if (tasks[i].bp >= 0x01000000L) tt_used += tasks[i].mem;
            else st_used += tasks[i].mem;
        }
        txt(C_DIM, x2, y, "RAM USED BY TASKS", 0);
        fmt_pm(phys > 0 ? (int)(mem_tasks / 1024 * 1000L / (phys / 1024)) : 0, a);
        sprintf(s, "Total %s", a);
        txt(C_TEXT, x2, y + lh, s, 0);
        txt(C_DIM, x2, y + 2 * lh, "ST RAM", 0);
        meter_bar(bx, y + 2 * lh, bxr, y + 3 * lh - 3, st_used, mem_st, C_USR);
        if (mem_tt) {
            txt(C_DIM, x2, y + 3 * lh, "TT RAM", 0);
            meter_bar(bx, y + 3 * lh, bxr, y + 4 * lh - 3, tt_used, mem_tt, C_METON);
        }
        return;
    }

    /* CPU block */
    sprintf(s, hide_aes ? "CPU  (%d tasks, no AES)" : "CPU  (%d tasks)", nrows);
    txt(C_DIM, x2, y, s, 0);
    fmt_pm(cpu_usr, a);
    sprintf(s, "User:   %s", a);
    txt(C_USR, x2, y + lh, s, 0);
    fmt_pm(cpu_sys, a);
    sprintf(s, "System: %s", a);
    txt(C_SYS, x2, y + 2 * lh, s, 0);
    fmt_pm(1000 - cpu_usr - cpu_sys, a);
    sprintf(s, "Idle:   %s", a);
    txt(C_DIM, x2, y + 3 * lh, s, 0);

    /* usage bar */
    {
        short bx = x2 + 17 * wchar, bxr = f->g_x + f->g_w - wchar - 1;
        short by = y + lh, byb = y + 4 * lh - 4;
        if (bxr - bx > 4 * wchar) {
            short bw = bxr - bx - 1;
            short ws = (short)((long)cpu_sys * bw / 1000);
            short wu = (short)((long)(cpu_sys + cpu_usr) * bw / 1000);
            GRECT fr;
            box(C_BLACK, bx, by, bxr, byb);
            box(C_SYS, bx + 1, by + 1, bx + ws, byb - 1);
            box(C_USR, bx + 1 + ws, by + 1, bx + wu, byb - 1);
            fr.g_x = bx; fr.g_y = by; fr.g_w = bxr - bx + 1; fr.g_h = byb - by + 1;
            frame(C_GRID, &fr);
            txt(C_DIM, bx, y, "LOAD", 0);
        }
    }
}

static void draw_tasks(const GRECT *work, const GRECT *clip)
{
    TLAYOUT L;
    GRECT r;

    tasks_layout(work, &L);

    r = L.tb;   if (rect_intersect(clip, &r)) draw_toolbar(&L);
    r = L.hdr;  if (rect_intersect(clip, &r)) draw_header(&L);
    r = L.list; if (L.list.g_h > 0 && rect_intersect(clip, &r)) draw_rows(&L, &r);
    r = L.foot; if (L.foot.g_h > 0 && rect_intersect(clip, &r)) draw_footer(&L);
}

/* ------------------------------------------------------------------ */
/* CPU meter window                                                    */
/* ------------------------------------------------------------------ */

static void draw_meter(const GRECT *w, int red_pm, int total_pm)
{
    short labh = w->g_h >= 3 * hchar + 12 ? hchar + 6 : 0;
    short mx1 = w->g_x + 6, mx2 = w->g_x + w->g_w - 7;
    short my1 = w->g_y + 6, my2 = w->g_y + w->g_h - labh - 1;
    int nseg, k, lit_sys, lit_tot;
    char s[16];

    rbox(C_BLACK, w);

    if (mx2 - mx1 > 24) {           /* keep the meter column-shaped */
        short mid = (mx1 + mx2) / 2, half = (mx2 - mx1) / 2;
        if (half > 3 * wchar) half = 3 * wchar;
        mx1 = mid - half; mx2 = mid + half;
    }

    nseg = (my2 - my1 + 1) / 4;
    lit_sys = (red_pm * nseg + 500) / 1000;
    lit_tot = (total_pm * nseg + 500) / 1000;

    for (k = 0; k < nseg; k++) {
        short y2 = my2 - k * 4, y1 = y2 - 2;
        int c = k < lit_sys ? C_SYS : k < lit_tot ? C_METON : C_METOFF;
        box(c, mx1, y1, mx2, y2);
    }

    if (labh) {
        fmt_pm(total_pm, s);
        {
            short tw = (short)strlen(s) * wchar;
            txt(C_TEXT, w->g_x + (w->g_w - tw) / 2, w->g_y + w->g_h - labh + 2, s, 0);
        }
    }
}

static void draw_cpu(const GRECT *w)
{
    draw_meter(w, cpu_sys, cpu_sys + cpu_usr);
}

static void draw_mem(const GRECT *w)
{
    draw_meter(w, 0, mem_used_pm);
}

/* ------------------------------------------------------------------ */
/* CPU history window                                                  */
/* ------------------------------------------------------------------ */

static void hist_layout(const GRECT *w, GRECT *lab, GRECT *g)
{
    short lh = hchar + 6;
    *lab = *w;
    lab->g_h = lh;
    *g = *w;
    g->g_y += lh;
    g->g_h -= lh + 2;
    if (g->g_h < 1) g->g_h = 1;
}

static void draw_hist_label(int win, const GRECT *lab)
{
    char a[16], s[32];
    short x = lab->g_x + wchar;

    rbox(C_BLACK, lab);
    if (win == W_MHIST) {
        long phys = mem_st + mem_tt;
        long used = phys - free_st - free_tt;
        if (used < 0) used = 0;
        fmt_mem(used, a);
        sprintf(s, "Used %s", a);
        txt(C_METON, x, lab->g_y + 3, s, 0);
        x += ((short)strlen(s) + 3) * wchar;
        fmt_mem(free_st + free_tt, a);
        sprintf(s, "Free %s", a);
        txt(C_USR, x, lab->g_y + 3, s, 0);
        return;
    }
    fmt_pm(cpu_usr, a);
    sprintf(s, "User %s", a);
    txt(C_USR, x, lab->g_y + 3, s, 0);
    x += ((short)strlen(s) + 3) * wchar;
    fmt_pm(cpu_sys, a);
    sprintf(s, "Kernel %s", a);
    txt(C_SYS, x, lab->g_y + 3, s, 0);
    if (hide_aes) {
        x += ((short)strlen(s) + 3) * wchar;
        if (x + 9 * wchar < lab->g_x + lab->g_w)
            txt(C_DIM, x, lab->g_y + 3, "(no AES)", 0);
    }
}

/* draws columns [xa, xb] of the graph area; identical pixels whether
 * drawn all at once or column by column, so blit-scrolling is safe */
static void draw_hist_range(int win, const GRECT *g, short xa, short xb)
{
    const short *lo = win == W_MHIST ? h_mused : h_sys;   /* bottom part */
    const short *hi = win == W_MHIST ? h_mfree : h_usr;   /* stacked on top */
    int clo = win == W_MHIST ? C_METON : C_SYS;
    int chi = C_USR;
    short xr = g->g_x + g->g_w - 1, yb = g->g_y + g->g_h - 1;
    short xe = xr - ((xr + 1) % GCOLW);     /* gap pixel of newest column */
    short y;
    int j;

    if (xa < g->g_x) xa = g->g_x;
    if (xb > xr) xb = xr;
    if (xa > xb) return;

    box(C_BLACK, xa, g->g_y, xb, yb);

    for (j = 0; j < h_count; j++) {
        short gap = xe - j * GCOLW;
        short c1 = gap - GCOLW + 1, c2 = gap - 1;
        int idx, hs, ht;

        if (gap < xa) break;
        if (c1 > xb) continue;
        if (c1 < xa) c1 = xa;
        if (c2 > xb) c2 = xb;
        if (c2 < c1) continue;

        idx = (h_head - 1 - j + HIST) % HIST;
        hs = (int)((long)lo[idx] * g->g_h / 1000);
        ht = (int)((long)(lo[idx] + hi[idx]) * g->g_h / 1000);
        if (hs > 0) box(clo, c1, yb - hs + 1, c2, yb);
        if (ht > hs) box(chi, c1, yb - ht + 1, c2, yb - hs);
    }

    /* LED rows: black line on every 4th screen line */
    for (y = g->g_y; y <= yb; y++)
        if ((y & 3) == 3)
            line(C_BLACK, xa, y, xb, y);
}

static void draw_hist(int win, const GRECT *w, const GRECT *clip)
{
    GRECT lab, g, r;

    hist_layout(w, &lab, &g);
    r = lab;
    if (rect_intersect(clip, &r)) draw_hist_label(win, &lab);
    r = g;
    if (rect_intersect(clip, &r)) draw_hist_range(win, &g, r.g_x, r.g_x + r.g_w - 1);
    /* strip under the graph */
    box(C_BLACK, w->g_x, g.g_y + g.g_h, w->g_x + w->g_w - 1, w->g_y + w->g_h - 1);
}

/* ------------------------------------------------------------------ */
/* Window plumbing                                                     */
/* ------------------------------------------------------------------ */

static void get_rect(short h, short what, GRECT *r)
{
    wind_get(h, what, &r->g_x, &r->g_y, &r->g_w, &r->g_h);
}

static int win_index(short h)
{
    int i;
    for (i = 0; i < NWIN; i++)
        if (wins[i].h == h && h >= 0)
            return i;
    return -1;
}

/* iconified: a miniature of the window, redrawn every sample */
static void draw_icon(int i, const GRECT *work)
{
    GRECT g;

    switch (i) {
    case W_CPU:
        draw_meter(work, cpu_sys, cpu_sys + cpu_usr);
        break;
    case W_MEM:
        draw_meter(work, 0, mem_used_pm);
        break;
    case W_HIST:
    case W_MHIST:
        g = *work;              /* the whole icon is graph, no label */
        g.g_x += 2; g.g_w -= 4;
        g.g_y += 2; g.g_h -= 4;
        rbox(C_BLACK, work);
        if (g.g_w > GCOLW && g.g_h > 2)
            draw_hist_range(i, &g, g.g_x, g.g_x + g.g_w - 1);
        break;
    }
}

static void draw_win(int i, const GRECT *work, const GRECT *clip)
{
    if (wins[i].iconified) {
        draw_icon(i, work);
        return;
    }
    switch (i) {
    case W_TASKS: draw_tasks(work, clip); break;
    case W_CPU:   draw_cpu(work); break;
    case W_HIST:  draw_hist(W_HIST, work, clip); break;
    case W_MEM:   draw_mem(work); break;
    case W_MHIST: draw_hist(W_MHIST, work, clip); break;
    }
}

static void redraw(int i, const GRECT *area)
{
    GRECT r, work, a;
    short h = wins[i].h;

    if (h < 0) return;
    a = *area;
    if (!rect_intersect(&desk, &a))
        return;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);
    get_rect(h, WF_WORKXYWH, &work);
    get_rect(h, WF_FIRSTXYWH, &r);
    while (r.g_w > 0 && r.g_h > 0) {
        if (rect_intersect(&a, &r)) {
            set_clip(&r, 1);
            draw_win(i, &work, &r);
        }
        get_rect(h, WF_NEXTXYWH, &r);
    }
    set_clip(&desk, 0);
    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void redraw_all(int i)
{
    GRECT work;
    if (wins[i].h < 0) return;
    get_rect(wins[i].h, WF_WORKXYWH, &work);
    redraw(i, &work);
}

/* history: redraw the label, scroll the graph by one column with a blit
 * when the window is fully visible, else redraw it through the rect list */
static void graph_update(int win)
{
    short h = wins[win].h;
    GRECT work, lab, g, r, r2;

    if (h < 0) return;
    get_rect(h, WF_WORKXYWH, &work);
    if (wins[win].iconified) {
        redraw(win, &work);
        return;
    }
    hist_layout(&work, &lab, &g);
    redraw(win, &lab);

    if (gdirty[win]) {
        redraw(win, &work);
        gdirty[win] = 0;
        return;
    }

    wind_update(BEG_UPDATE);
    get_rect(h, WF_FIRSTXYWH, &r);
    get_rect(h, WF_NEXTXYWH, &r2);
    if (r.g_x == work.g_x && r.g_y == work.g_y && r.g_w == work.g_w &&
        r.g_h == work.g_h && (r2.g_w == 0 || r2.g_h == 0) && g.g_w > 2 * GCOLW) {
        MFDB scr;
        short pxy[8];
        short xr = g.g_x + g.g_w - 1, yb = g.g_y + g.g_h - 1;
        short xe = xr - ((xr + 1) % GCOLW);

        memset(&scr, 0, sizeof scr);
        graf_mouse(M_OFF, NULL);
        set_clip(&g, 1);
        pxy[0] = g.g_x + GCOLW; pxy[1] = g.g_y; pxy[2] = xe;         pxy[3] = yb;
        pxy[4] = g.g_x;         pxy[5] = g.g_y; pxy[6] = xe - GCOLW; pxy[7] = yb;
        vro_cpyfm(vh, S_ONLY, pxy, &scr, &scr);
        draw_hist_range(win, &g, xe - GCOLW + 1, xr);
        set_clip(&desk, 0);
        graf_mouse(M_ON, NULL);
        wind_update(END_UPDATE);
    } else {
        wind_update(END_UPDATE);
        redraw(win, &g);
    }
}

static void update_slider(void)
{
    short h = wins[W_TASKS].h, pos, size;
    GRECT work;
    TLAYOUT L;
    int maxtop;

    if (h < 0) return;
    get_rect(h, WF_WORKXYWH, &work);
    tasks_layout(&work, &L);

    maxtop = nrows - L.vis;
    if (maxtop < 0) maxtop = 0;
    if (top_row > maxtop) top_row = maxtop;
    if (top_row < 0) top_row = 0;

    size = (nrows <= L.vis || nrows == 0) ? 1000 : (short)((long)L.vis * 1000 / nrows);
    pos = maxtop ? (short)((long)top_row * 1000 / maxtop) : 0;
    if (size != last_slsize) {
        wind_set(h, WF_VSLSIZE, size, 0, 0, 0);
        last_slsize = size;
    }
    if (pos != last_slpos) {
        wind_set(h, WF_VSLIDE, pos, 0, 0, 0);
        last_slpos = pos;
    }
}

static void min_work(int i, short *w, short *h)
{
    switch (i) {
    case W_TASKS: *w = 40 * wchar; *h = 6 * (hchar + 3) + 2 * hchar + 16; break;
    case W_CPU:
    case W_MEM:   *w = 6 * wchar;  *h = 4 * hchar; break;
    default:      *w = 16 * wchar; *h = 4 * hchar; break;
    }
}

static void clamp_border(int i, GRECT *b)
{
    GRECT w;
    short mw, mh;

    wind_calc(WC_WORK, wins[i].kind, b->g_x, b->g_y, b->g_w, b->g_h,
              &w.g_x, &w.g_y, &w.g_w, &w.g_h);
    min_work(i, &mw, &mh);
    if (w.g_w < mw) w.g_w = mw;
    if (w.g_h < mh) w.g_h = mh;
    wind_calc(WC_BORDER, wins[i].kind, w.g_x, w.g_y, w.g_w, w.g_h,
              &b->g_x, &b->g_y, &b->g_w, &b->g_h);
}

static void default_rect(int i, GRECT *b)
{
    short dx = desk.g_x, dy = desk.g_y, dw = desk.g_w, dh = desk.g_h;
    short lw = dw * 2 / 3;

    switch (i) {
    case W_TASKS:
        b->g_x = dx + 2; b->g_y = dy + 2; b->g_w = lw - 4; b->g_h = dh * 3 / 4;
        break;
    case W_HIST:                    /* graphs: a quarter of the screen high */
    case W_MHIST:
        b->g_x = dx + lw; b->g_w = dw - lw - 2; b->g_h = dh / 4;
        b->g_y = dy + 2 + (i == W_MHIST ? dh / 4 + 4 : 0);
        if (b->g_w < 20 * wchar) {  /* narrow screen: side by side, below */
            b->g_w = dw / 2 - 3;
            b->g_x = dx + 2 + (i == W_MHIST ? dw / 2 : 0);
            b->g_y = dy + dh * 3 / 4 + 4;
            b->g_h = (dh - (b->g_y - dy) - 2) / 2;
        }
        break;
    case W_CPU:                     /* meters: CPU at the right, Memory left of it */
    case W_MEM:
        b->g_w = 12 * wchar; b->g_h = (dh / 2 - 8) / 2;
        b->g_x = dx + dw - b->g_w - 2 - (i == W_MEM ? b->g_w + 4 : 0);
        b->g_y = dy + dh / 2 + 4;
        break;
    }
    clamp_border(i, b);
    if (b->g_x + b->g_w > dx + dw) b->g_x = dx + dw - b->g_w;
    if (b->g_y + b->g_h > dy + dh) b->g_y = dy + dh - b->g_h;
    if (b->g_x < dx) b->g_x = dx;
    if (b->g_y < dy) b->g_y = dy;
}

static void sync_menu(void);

/* title of an iconified window: its current figure, e.g. "CPU 42%" */
static void update_icon_title(int i)
{
    char t[16];
    int pm = (i == W_MEM || i == W_MHIST) ? mem_used_pm : cpu_sys + cpu_usr;

    if (wins[i].h < 0 || !wins[i].iconified)
        return;
    sprintf(t, "%s %d%%", (i == W_MEM || i == W_MHIST) ? "Mem" : "CPU", (pm + 5) / 10);
    if (strcmp(t, wins[i].ititle) != 0) {
        strcpy(wins[i].ititle, t);
        wind_set_str(wins[i].h, WF_NAME, wins[i].ititle);
    }
}

static int open_win(int i)
{
    WIN *w = &wins[i];
    GRECT b;

    if (w->h >= 0) {
        wind_set(w->h, WF_TOP, 0, 0, 0, 0);
        return 1;
    }
    if (can_iconify && iconifiable(i))
        w->kind |= SMALLER;
    w->h = wind_create(w->kind, desk.g_x, desk.g_y, desk.g_w, desk.g_h);
    if (w->h < 0) {
        form_alert(1, "[3][TaskMan: no more windows.][ OK ]");
        return 0;
    }
    wind_set_str(w->h, WF_NAME, w->title);
    if (w->prev.g_w == 0)
        default_rect(i, &w->prev);
    b = w->prev;
    w->fulled = 0;
    if (i == W_TASKS) {
        last_slpos = last_slsize = -1;
    }
    gdirty[i] = 1;
    wind_open(w->h, b.g_x, b.g_y, b.g_w, b.g_h);
    if (i == W_TASKS)
        update_slider();
    sync_menu();
    return 1;
}

static void close_win(int i)
{
    WIN *w = &wins[i];
    if (w->h < 0) return;
    if (w->iconified)
        w->prev = w->normal;    /* reopen as a normal window */
    else if (!w->fulled)
        get_rect(w->h, WF_CURRXYWH, &w->prev);
    w->iconified = 0;
    wind_close(w->h);
    wind_delete(w->h);
    w->h = -1;
    sync_menu();
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

#ifndef OF_SELECTABLE
#define OF_SELECTABLE 0x0001
#define OF_DEFAULT    0x0002
#define OF_EXIT       0x0004
#endif
#ifndef OF_TOUCHEXIT
#define OF_TOUCHEXIT  0x0040
#endif
#ifndef OF_LASTOB
#define OF_LASTOB     0x0020
#endif
#ifndef OS_SELECTED
#define OS_SELECTED   0x0001
#endif
#ifndef OS_OUTLINED
#define OS_OUTLINED   0x0010
#endif
#ifndef OS_DISABLED
#define OS_DISABLED   0x0008
#endif
#ifndef FMD_START
#define FMD_START     0
#define FMD_FINISH    3
#endif

static void sync_menu(void);

static void do_signal(int sig)
{
    TASK *t = find_task(sel_pid);
    char msg[160];

    if (!t || t->pid <= 0 || t->pid == my_pid)
        return;
    sprintf(msg, "[2][%s '%s' (PID %d)?|%s][Cancel|%s]",
            sig == 9 ? "Kill" : "Stop", t->name, t->pid,
            sig == 9 ? "It gets no chance to clean up." : "Sends SIGTERM.",
            sig == 9 ? "Kill" : "Stop");
    if (form_alert(1, msg) == 2)
        if (Pkill(t->pid, sig) < 0)
            form_alert(1, "[3][Could not signal|that process.][ OK ]");
}

/* --- nice dialog (object tree built in code) ------------------------ */

enum { D_ROOT, D_TITLE, D_PROC, D_LABEL, D_VAL, D_M5, D_M1, D_P1, D_P5,
       D_HELP, D_OK, D_CANCEL, D_NOBJ };

static OBJECT dtree[D_NOBJ];
static char   d_title[]  = "Set Nice Level";
static char   d_proc[40];
static char   d_label[]  = "Nice:";
static char   d_val[8];
static char   d_m5[] = "-5", d_m1[] = "-1", d_p1[] = "+1", d_p5[] = "+5";
static char   d_help[]   = "-20 = highest priority, 20 = lowest";
static char   d_ok[]     = "OK";
static char   d_cancel[] = "Cancel";

static void dobj(int i, int parent, unsigned short type, char *str,
                 short x, short y, short w, short h, unsigned short flags)
{
    OBJECT *o = &dtree[i];
    memset(o, 0, sizeof *o);
    o->ob_next = o->ob_head = o->ob_tail = -1;
    o->ob_type = type;
    o->ob_flags = flags;
    o->ob_x = x; o->ob_y = y; o->ob_width = w; o->ob_height = h;
    if (str)
        o->ob_spec.free_string = str;
    if (parent >= 0) {
        OBJECT *p = &dtree[parent];
        if (p->ob_head < 0) p->ob_head = (short)i;
        else dtree[p->ob_tail].ob_next = (short)i;
        p->ob_tail = (short)i;
        o->ob_next = (short)parent;
    }
}

static void build_nice_dialog(void)
{
    int i;

    dobj(D_ROOT,   -1,     G_BOX,    NULL,     0, 0, 42, 10, 0);
    dtree[D_ROOT].ob_spec.index = 0x00021100L;
    dtree[D_ROOT].ob_state = OS_OUTLINED;
    dobj(D_TITLE,  D_ROOT, G_STRING, d_title,  2, 1, 14, 1, 0);
    dobj(D_PROC,   D_ROOT, G_STRING, d_proc,   2, 2, 36, 1, 0);
    dobj(D_LABEL,  D_ROOT, G_STRING, d_label,  2, 4, 5, 1, 0);
    dobj(D_VAL,    D_ROOT, G_STRING, d_val,    8, 4, 4, 1, 0);
    dobj(D_M5,     D_ROOT, G_BUTTON, d_m5,    14, 4, 5, 1, OF_TOUCHEXIT);
    dobj(D_M1,     D_ROOT, G_BUTTON, d_m1,    20, 4, 5, 1, OF_TOUCHEXIT);
    dobj(D_P1,     D_ROOT, G_BUTTON, d_p1,    26, 4, 5, 1, OF_TOUCHEXIT);
    dobj(D_P5,     D_ROOT, G_BUTTON, d_p5,    32, 4, 5, 1, OF_TOUCHEXIT);
    dobj(D_HELP,   D_ROOT, G_STRING, d_help,   2, 6, 38, 1, 0);
    /* own row with a blank line above: default buttons draw a thick
     * border outside their box */
    dobj(D_OK,     D_ROOT, G_BUTTON, d_ok,    20, 8, 9, 1, OF_SELECTABLE | OF_DEFAULT | OF_EXIT);
    dobj(D_CANCEL, D_ROOT, G_BUTTON, d_cancel,31, 8, 9, 1, OF_SELECTABLE | OF_EXIT | OF_LASTOB);
    for (i = 0; i < D_NOBJ; i++)
        rsrc_obfix(dtree, i);
}

static void do_nice_dialog(void)
{
    TASK *t = find_task(sel_pid);
    short x, y, w, h, ox, oy, r;
    int val, old;

    if (!t || !t->addr || t->pid <= 0)
        return;
    build_nice_dialog();
    old = val = t->nice;
    sprintf(d_proc, "%.15s (PID %d), now %d", t->name, t->pid, old);
    sprintf(d_val, "%+3d", val);

    form_center(dtree, &x, &y, &w, &h);
    wind_update(BEG_UPDATE);
    form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
    objc_draw(dtree, D_ROOT, 8, x, y, w, h);

    for (;;) {
        r = form_do(dtree, 0) & 0x7fff;
        if (r == D_M5) val -= 5;
        else if (r == D_M1) val -= 1;
        else if (r == D_P1) val += 1;
        else if (r == D_P5) val += 5;
        else break;
        if (val < -20) val = -20;
        if (val > 20) val = 20;
        sprintf(d_val, "%+3d", val);
        objc_offset(dtree, D_VAL, &ox, &oy);
        objc_draw(dtree, D_ROOT, 8, ox, oy, dtree[D_VAL].ob_width, dtree[D_VAL].ob_height);
    }
    dtree[r].ob_state &= ~OS_SELECTED;
    form_dial(FMD_FINISH, 0, 0, 0, 0, x, y, w, h);
    wind_update(END_UPDATE);

    if (r == D_OK && val != old) {
        /* Prenice adds its argument to the nice value (nice = -pri) */
        if (Prenice(t->pid, (short)(val - old)) < 0)
            form_alert(1, "[3][Could not change the|nice level of that process.][ OK ]");
        else {
            t->nice = (short)val;
            t->pri = (short)-val;
        }
        redraw_all(W_TASKS);
    }
}

/* --- list helpers --------------------------------------------------- */

static void refresh_tasks(void)
{
    sort_tasks();
    update_slider();
    redraw_all(W_TASKS);
}

static void set_sort(int key, int desc)
{
    sort_key[cur_tab] = key;
    sort_desc[cur_tab] = desc;
    refresh_tasks();
    sync_menu();
}

static void set_theme(int th)
{
    int i;
    if (th == theme) return;
    theme = th;
    setup_colours();
    for (i = 0; i < NWIN; i++) {
        gdirty[i] = 1;
        redraw_all(i);
    }
    sync_menu();
}

static void set_tab(int tab)
{
    if (tab == cur_tab) return;
    cur_tab = tab;
    refresh_tasks();
    sync_menu();
}

static void ensure_visible(void)
{
    GRECT work;
    TLAYOUT L;
    int si = sel_index();

    if (si < 0 || wins[W_TASKS].h < 0) return;
    get_rect(wins[W_TASKS].h, WF_WORKXYWH, &work);
    tasks_layout(&work, &L);
    if (si < top_row) top_row = si;
    if (L.vis > 0 && si >= top_row + L.vis) top_row = si - L.vis + 1;
}

static void tasks_click(short mx, short my, short clicks)
{
    GRECT work;
    TLAYOUT L;
    int i;

    get_rect(wins[W_TASKS].h, WF_WORKXYWH, &work);
    tasks_layout(&work, &L);

    if (in_rect(mx, my, &L.tb)) {
        for (i = 0; i < NBTN; i++) {
            if (!in_rect(mx, my, &L.btn[i]) || !btn_enabled(i))
                continue;
            switch (i) {
            case B_STOP: do_signal(15); break;
            case B_KILL: do_signal(9); break;
            case B_NICE: do_nice_dialog(); break;
            }
        }
        for (i = 0; i < NTAB; i++)
            if (in_rect(mx, my, &L.tab[i]))
                set_tab(i);
        for (i = 0; i < NTHEME; i++)
            if (L.thm[i].g_w > 0 && in_rect(mx, my, &L.thm[i]))
                set_theme(i);
        return;
    }
    if (in_rect(mx, my, &L.hdr)) {
        const COLDEF *cols = tab_cols[cur_tab];
        for (i = L.ncol - 1; i >= 0; i--)
            if (mx >= L.colx[i] - wchar / 2) {
                int key = cols[i].key;
                if (sort_key[cur_tab] == key)
                    set_sort(key, !sort_desc[cur_tab]);
                else
                    set_sort(key, default_desc(key));
                break;
            }
        return;
    }
    if (in_rect(mx, my, &L.list)) {
        int idx = top_row + (my - L.list.g_y) / L.rowh;
        sel_pid = (idx < nrows) ? tasks[order[idx]].pid : -1;
        redraw_all(W_TASKS);
        sync_menu();
        if (clicks >= 2 && btn_enabled(B_NICE))
            do_nice_dialog();
    }
}

static void tasks_key(short key)
{
    short scan = (key >> 8) & 0xff;
    int si = sel_index();

    if (scan == 0x48 || scan == 0x50) {         /* up / down */
        if (nrows == 0) return;
        if (si < 0) si = 0;
        else if (scan == 0x48 && si > 0) si--;
        else if (scan == 0x50 && si < nrows - 1) si++;
        sel_pid = tasks[order[si]].pid;
        ensure_visible();
        update_slider();
        redraw_all(W_TASKS);
        sync_menu();
    } else if (scan == 0x53) {                  /* Delete */
        if (btn_enabled(B_STOP))
            do_signal(15);
    } else if (scan == 0x0f) {                  /* Tab: switch tab */
        set_tab((cur_tab + 1) % NTAB);
    }
}

/* ------------------------------------------------------------------ */
/* Menu bar (built in code, no resource file needed)                   */
/* ------------------------------------------------------------------ */

#ifndef MN_SELECTED
#define MN_SELECTED 10
#endif

#define MAXOBJ  64
#define NRATE   7
#define NTITLE  6
#define SEP     ((char *)1)         /* separator marker in item lists */

static OBJECT mtree[MAXOBJ];
static int    nobj;
static int    menu_on;
static const long rate_ms[NRATE] = { 500L, 1000L, 2000L, 5000L, 10000L, 15000L, 20000L };

/* item strings live in static storage: the AES may write desk
 * accessory names into the Desk menu entries */
static char ms_about[]  = "  About TaskMan...  ";
static char ms_acc[6][21];
static char ms_quit[]   = "  Quit        ^Q ";
static char ms_stop[]   = "  Stop (SIGTERM)  Del ";
static char ms_kill[]   = "  Kill (SIGKILL)      ";
static char ms_nice[]   = "  Set Nice...         ";
static char ms_vwin[NWIN][20] = { "  Task Manager   ", "  CPU Meter      ", "  CPU History    ",
                                   "  Memory Meter   ", "  Memory History " };
static char ms_vall[]   = "  Show All       ";
static char ms_vaes[]   = "  Hide AES       ";
static char ms_vthm[NTHEME][20] = { "  Dark Theme     ", "  Light Theme    " };
static char ms_vtab[NTAB][20] = { "  CPU Tab        ", "  Memory Tab     " };
static char ms_sname[]  = "  By Name      ";
static char ms_smem[]   = "  By Memory    ";
static char ms_scpu[]   = "  By CPU       ";
static char ms_sasc[]   = "  Ascending    ";
static char ms_sdesc[]  = "  Descending   ";
static char ms_rate[NRATE][16] = {
    "  0.5 seconds ", "  1 second    ", "  2 seconds   ",
    "  5 seconds   ", "  10 seconds  ", "  15 seconds  ", "  20 seconds  "
};
static char ms_sep[16][40];
static char mt_title[NTITLE][10] = { " TaskMan ", " File ", " Process ", " View ", " Sort ", " Rate " };

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

/* object index of a menu item, found by its string */
static short mi(const char *str)
{
    int i;
    for (i = 0; i < nobj; i++)
        if (mtree[i].ob_type == G_STRING && mtree[i].ob_spec.free_string == str)
            return (short)i;
    return -1;
}

static void build_menu(short scr_w, short scr_h)
{
    char *lists[NTITLE][16];
    int counts[NTITLE];
    short cols = scr_w / wchar, rows = scr_h / hchar;
    short tx[NTITLE], x = 0;
    int root, bar, active, screen, box, i, k, nsep = 0;

    for (i = 0; i < 6; i++)
        sprintf(ms_acc[i], "  Desk Accessory %d ", i + 1);

    k = 0;
    lists[0][k++] = ms_about; lists[0][k++] = SEP;
    for (i = 0; i < 6; i++) lists[0][k++] = ms_acc[i];
    counts[0] = k;
    lists[1][0] = ms_quit; counts[1] = 1;
    k = 0;
    lists[2][k++] = ms_stop; lists[2][k++] = ms_kill; lists[2][k++] = SEP;
    lists[2][k++] = ms_nice; counts[2] = k;
    k = 0;
    for (i = 0; i < NWIN; i++) lists[3][k++] = ms_vwin[i];
    lists[3][k++] = SEP; lists[3][k++] = ms_vall; lists[3][k++] = SEP;
    lists[3][k++] = ms_vaes; lists[3][k++] = SEP;
    lists[3][k++] = ms_vthm[THEME_LIGHT]; lists[3][k++] = ms_vthm[THEME_DARK];
    lists[3][k++] = SEP;
    for (i = 0; i < NTAB; i++) lists[3][k++] = ms_vtab[i];
    counts[3] = k;
    k = 0;
    lists[4][k++] = ms_sname; lists[4][k++] = ms_smem; lists[4][k++] = ms_scpu;
    lists[4][k++] = SEP; lists[4][k++] = ms_sasc; lists[4][k++] = ms_sdesc;
    counts[4] = k;
    for (i = 0; i < NRATE; i++) lists[5][i] = ms_rate[i];
    counts[5] = NRATE;

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

    /* one drop-down box per title, in title order */
    for (k = 0; k < NTITLE; k++) {
        short bw = 0, bx;
        for (i = 0; i < counts[k]; i++)
            if (lists[k][i] != SEP && (short)strlen(lists[k][i]) > bw)
                bw = (short)strlen(lists[k][i]);
        bx = 2 + tx[k];
        if (bx + bw > cols) bx = cols - bw;
        box = new_obj(screen, G_BOX, bx, 0, bw, counts[k]);
        mtree[box].ob_spec.index = 0x00FF1100L;

        for (i = 0; i < counts[k]; i++) {
            char *str = lists[k][i];
            int o, sep = (str == SEP);
            if (sep) {
                str = ms_sep[nsep++];
                memset(str, '-', bw);
                str[bw] = 0;
            }
            o = new_obj(box, G_STRING, 0, i, bw, 1);
            mtree[o].ob_spec.free_string = str;
            if (sep)
                mtree[o].ob_state |= OS_DISABLED;
        }
    }
    mtree[nobj - 1].ob_flags |= OF_LASTOB;

    for (i = 0; i < nobj; i++)
        rsrc_obfix(mtree, i);
}

static void sync_menu(void)
{
    int i, en;
    if (!menu_on)
        return;
    for (i = 0; i < NWIN; i++)
        menu_icheck(mtree, mi(ms_vwin[i]), wins[i].h >= 0);
    for (i = 0; i < NTAB; i++)
        menu_icheck(mtree, mi(ms_vtab[i]), cur_tab == i);
    for (i = 0; i < NRATE; i++)
        menu_icheck(mtree, mi(ms_rate[i]), rate_ms[i] == sample_ms);
    menu_icheck(mtree, mi(ms_vaes), hide_aes);
    for (i = 0; i < NTHEME; i++)
        menu_icheck(mtree, mi(ms_vthm[i]), theme == i);
    menu_icheck(mtree, mi(ms_sname), sort_key[cur_tab] == K_NAME);
    menu_icheck(mtree, mi(ms_smem),  sort_key[cur_tab] == K_MEM);
    menu_icheck(mtree, mi(ms_scpu),  sort_key[cur_tab] == K_CPU);
    menu_icheck(mtree, mi(ms_sasc),  !sort_desc[cur_tab]);
    menu_icheck(mtree, mi(ms_sdesc), sort_desc[cur_tab]);
    en = btn_enabled(B_STOP);
    menu_ienable(mtree, mi(ms_stop), en);
    menu_ienable(mtree, mi(ms_kill), en);
    menu_ienable(mtree, mi(ms_nice), en);
}

/* returns 0 to quit */
static int menu_select(short title, short item)
{
    int i, quit = 0;

    if (item == mi(ms_about)) {
        form_alert(1, "[1][TaskMan|GEM task manager for MultiTOS|"
                      "and FreeMiNT.|CPU, kernel time, memory and|"
                      "processes from u:\\proc.][ OK ]");
    } else if (item == mi(ms_quit)) {
        quit = 1;
    } else if (item == mi(ms_stop)) {
        do_signal(15);
    } else if (item == mi(ms_kill)) {
        do_signal(9);
    } else if (item == mi(ms_nice)) {
        do_nice_dialog();
    } else if (item == mi(ms_vaes)) {
        TASK *st = find_task(sel_pid);
        hide_aes = !hide_aes;
        if (st && hidden(st->name))
            sel_pid = -1;
        refresh_tasks();
        sync_menu();
    } else if (item == mi(ms_vall)) {
        for (i = 0; i < NWIN; i++)
            open_win(i);
    } else if (item == mi(ms_sname)) {
        set_sort(K_NAME, sort_desc[cur_tab]);
    } else if (item == mi(ms_smem)) {
        set_sort(K_MEM, sort_desc[cur_tab]);
    } else if (item == mi(ms_scpu)) {
        set_sort(K_CPU, sort_desc[cur_tab]);
    } else if (item == mi(ms_sasc)) {
        set_sort(sort_key[cur_tab], 0);
    } else if (item == mi(ms_sdesc)) {
        set_sort(sort_key[cur_tab], 1);
    } else {
        for (i = 0; i < NWIN; i++)
            if (item == mi(ms_vwin[i])) {
                if (wins[i].h >= 0) close_win(i);
                else open_win(i);
            }
        for (i = 0; i < NTAB; i++)
            if (item == mi(ms_vtab[i])) {
                if (wins[W_TASKS].h < 0) open_win(W_TASKS);
                set_tab(i);
            }
        for (i = 0; i < NTHEME; i++)
            if (item == mi(ms_vthm[i]))
                set_theme(i);
        for (i = 0; i < NRATE; i++)
            if (item == mi(ms_rate[i])) {
                sample_ms = rate_ms[i];
                sync_menu();
            }
    }
    menu_tnormal(mtree, title, 1);
    return !quit;
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

static int handle_message(short *msg)
{
    int i;
    GRECT b;

    if (msg[0] == AP_TERM)
        return 0;
    if (msg[0] == MN_SELECTED)
        return menu_select(msg[3], msg[4]);

    i = win_index(msg[3]);
    if (i < 0)
        return 1;

    switch (msg[0]) {
    case WM_REDRAW:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        redraw(i, &b);
        break;
    case WM_ICONIFY:
    case WM_ALLICONIFY:             /* treated as iconifying this window */
        if (!iconifiable(i) || wins[i].iconified)
            break;
        if (wins[i].fulled)
            wins[i].normal = wins[i].prev, wins[i].fulled = 0;
        else
            get_rect(msg[3], WF_CURRXYWH, &wins[i].normal);
        wins[i].iconified = 1;
        wins[i].ititle[0] = 0;
        wind_set(msg[3], WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
        update_icon_title(i);
        break;
    case WM_UNICONIFY:
        if (!wins[i].iconified)
            break;
        wins[i].iconified = 0;
        wind_set(msg[3], WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
        wind_set_str(msg[3], WF_NAME, wins[i].title);
        gdirty[i] = 1;
        break;
    case WM_TOPPED:
        wind_set(msg[3], WF_TOP, 0, 0, 0, 0);
        break;
    case WM_BOTTOMED:
        wind_set(msg[3], WF_BOTTOM, 0, 0, 0, 0);
        break;
    case WM_CLOSED:                 /* hide; View menu brings it back */
        close_win(i);
        break;
    case WM_MOVED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        wind_set(msg[3], WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        wins[i].fulled = 0;
        if (is_graph(i)) {          /* LED grid is screen-aligned */
            gdirty[i] = 1;
            graph_update(i);
        }
        break;
    case WM_SIZED:
        b.g_x = msg[4]; b.g_y = msg[5]; b.g_w = msg[6]; b.g_h = msg[7];
        clamp_border(i, &b);
        wind_set(msg[3], WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        wins[i].fulled = 0;
        if (i == W_TASKS) update_slider();
        if (is_graph(i)) gdirty[i] = 1;
        redraw_all(i);
        break;
    case WM_FULLED:
        if (wins[i].fulled) {
            b = wins[i].prev;
            wins[i].fulled = 0;
        } else {
            get_rect(msg[3], WF_CURRXYWH, &wins[i].prev);
            b = desk;
            wins[i].fulled = 1;
        }
        wind_set(msg[3], WF_CURRXYWH, b.g_x, b.g_y, b.g_w, b.g_h);
        if (i == W_TASKS) update_slider();
        if (is_graph(i)) gdirty[i] = 1;
        redraw_all(i);
        break;
    case WM_ARROWED:
        if (i == W_TASKS) {
            GRECT work;
            TLAYOUT L;
            get_rect(msg[3], WF_WORKXYWH, &work);
            tasks_layout(&work, &L);
            switch (msg[4]) {
            case WA_UPPAGE: top_row -= L.vis; break;
            case WA_DNPAGE: top_row += L.vis; break;
            case WA_UPLINE: top_row--; break;
            case WA_DNLINE: top_row++; break;
            }
            update_slider();
            redraw_all(W_TASKS);
        }
        break;
    case WM_VSLID:
        if (i == W_TASKS) {
            GRECT work;
            TLAYOUT L;
            int maxtop;
            get_rect(msg[3], WF_WORKXYWH, &work);
            tasks_layout(&work, &L);
            maxtop = nrows - L.vis;
            if (maxtop < 0) maxtop = 0;
            top_row = (int)((long)msg[4] * maxtop / 1000);
            update_slider();
            redraw_all(W_TASKS);
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    short work_in[11], work_out[57];
    short d, msg[8], ev, key, kstate, mx, my, mb, clicks;
    long next, t;
    int i, running = 1;

    ap_id = appl_init();
    if (ap_id < 0)
        return 1;
    menu_register(ap_id, "  TaskMan ");
    graf_mouse(ARROW, NULL);

    vh = graf_handle(&wchar, &hchar, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[0] = Getrez() + 2;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    if (vh <= 0) {
        form_alert(1, "[3][TaskMan: cannot open VDI|workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    ncolours_g = work_out[13];
    theme = system_theme();             /* follow the system palette */
    setup_colours();
    vst_alignment(vh, 0, 5, &d, &d);        /* left, top */
    vsf_perimeter(vh, 0);
    vsl_type(vh, SOLID);
    vsl_width(vh, 1);

    wind_get(0, WF_WORKXYWH, &desk.g_x, &desk.g_y, &desk.g_w, &desk.g_h);
    my_pid = (short)Pgetpid();

    last_sample_ms = now_ms();
    sample();                               /* baseline times */
    sort_tasks();

    if (!proc_ok) {
        form_alert(1, "[3][TaskMan needs MiNT|(MultiTOS or FreeMiNT):|u:\\proc not found.][ Quit ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    {
        short scr_w = work_out[0] + 1, scr_h = work_out[1] + 1;
        build_menu(scr_w, scr_h);
        menu_bar(mtree, 1);
        menu_on = 1;
    }

    /* iconify needs AES 4.1 (MultiTOS) or an AES that says so */
    if (aes_global[0] >= 0x0400 || appl_find("?AGI") >= 0) {
        short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
        if (appl_getinfo(11, &o1, &o2, &o3, &o4) && (o1 & 0x0080))
            can_iconify = 1;
    }

    if (!open_win(W_TASKS)) {
        menu_bar(mtree, 0);
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }
    open_win(W_HIST);
    open_win(W_MHIST);
    open_win(W_MEM);
    open_win(W_CPU);

    next = now_ms() + sample_ms;

    while (running) {
        t = next - now_ms();
        if (t < 0) t = 0;

        ev = evnt_multi(MU_MESAG | MU_TIMER | MU_KEYBD | MU_BUTTON,
                        2, 1, 1,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, t,
                        &mx, &my, &mb, &kstate, &key, &clicks);

        if (ev & MU_MESAG)
            running = handle_message(msg);

        if (running && (ev & MU_BUTTON)) {
            short h = wind_find(mx, my);
            if (h >= 0 && h == wins[W_TASKS].h) {
                short top, dd;
                GRECT work;
                wind_get(0, WF_TOP, &top, &dd, &dd, &dd);
                get_rect(h, WF_WORKXYWH, &work);
                if (top == h && in_rect(mx, my, &work))
                    tasks_click(mx, my, clicks);
            }
        }

        if (running && (ev & MU_KEYBD)) {
            short c = key & 0xff;
            if (c == 0x11)                          /* Ctrl+Q */
                running = 0;
            else
                tasks_key(key);
        }

        if (running && now_ms() >= next) {
            sample();
            sort_tasks();
            if (sel_pid >= 0 && !find_task(sel_pid)) {
                sel_pid = -1;
                sync_menu();
            }
            update_slider();
            redraw_all(W_TASKS);
            redraw_all(W_CPU);
            redraw_all(W_MEM);
            graph_update(W_HIST);
            graph_update(W_MHIST);
            for (i = W_CPU; i < NWIN; i++)
                update_icon_title(i);
            next += sample_ms;
            if (next < now_ms() || next > now_ms() + sample_ms)
                next = now_ms() + sample_ms;
        }
    }

    menu_on = 0;
    for (i = NWIN - 1; i >= 0; i--)
        close_win(i);
    menu_bar(mtree, 0);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
