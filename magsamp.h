/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Francis C Bullen
 * See the LICENSE file for the full licence text.
 */

/*
 * magsamp.h - statistical CPU sampling for MagiC
 *
 * MagiC keeps no CPU time per process (no PPROCADDR, Prusage
 * unsupported).  CPU use is measured by sampling, 200 times a second,
 * which process is running.  Two sources, the same method (agreed with
 * MSHELL, see NOTE-MCPU.md):
 *
 *  1. MCPU.PRG (MSHELL, resident): publishes its counts in the cookie
 *     'MCPU' (struct mcpu, layout copied below).  Used whenever present,
 *     then no handler of ours runs at all.
 *  2. Otherwise our own sampler 'FMON' on Timer C (vector $114, XBRA),
 *     installed while a monitor runs.
 *
 * Both: a tick is idle when the AES ready list is empty (*act_appl == 0,
 * from Dcntl(KER_GETINFO)); that works on MagicOnLinux, whose idle loop
 * makes a host call instead of STOP.  FMON also counts a tick taken in a
 * STOP instruction (word before the PC is $4E72) as idle.  Otherwise the
 * tick belongs to the running process, act_pd (falls back to the OS
 * header's _run if KER_GETINFO is missing).
 *
 * Robustness:
 *  - The FMON handler is position independent and is copied into a block
 *    allocated with Mxalloc "don't free" (bit 14), so if a monitor is
 *    killed the timer never points into freed memory.
 *  - TaskMan, FPulse and FLoad share one sampler: the first attaches it,
 *    others find it in the XBRA chain and add to its reference count;
 *    the last to detach unhooks and frees it.  If something hooked the
 *    timer after us without XBRA, the sampler stays (harmless) and is
 *    reused next time.
 *  - The interrupt handler only reads memory and increments counters.
 *
 * Only used on MagiC ('MagX' cookie); MiNT keeps real per-process times.
 */

#ifndef MAGSAMP_H
#define MAGSAMP_H

#define MS_SLOTS     128
#define MS_ID        0x464D4F4EL     /* 'FMON' */
#define MS_MAGIC     0x464D7632L     /* 'FMv2': data layout version */

/* MSHELL's MCPU.PRG (mcpu.h version 1.0), read through its cookie */
#define MCPU_COOKIE  0x4D435055L     /* 'MCPU' */
#define MCPU_VERSION 0x0100

struct mcpu_slot {
    long pd;                         /* basepage, 0 = free slot         */
    short pid;                       /* process id (p_procid)           */
    short res;
    unsigned long ticks;             /* ticks this process was running  */
    unsigned long last;              /* total when it last ran          */
};

struct mcpu {
    long magic;                      /* MCPU_COOKIE                     */
    short version;                   /* MCPU_VERSION                    */
    short nslots;
    short hz;                        /* samples per second (200)        */
    short res;
    unsigned long total;             /* all samples taken               */
    unsigned long idle;              /* samples with nothing running    */
    long *act_pd;
    long *act_appl;
    struct mcpu_slot slot[1];        /* nslots of them                  */
};

/* data block, as laid out in the template below */
typedef struct {
    volatile unsigned long total;    /* every tick                      */
    volatile unsigned long idle;     /* nothing to run, or STOP         */
    volatile unsigned long full;     /* slot table full                 */
    volatile unsigned long run;      /* address of act_pd (or _run)     */
    volatile unsigned long refcnt;   /* attached monitors               */
    volatile unsigned long magic;    /* MS_MAGIC                        */
    volatile unsigned long nobp;     /* no current basepage (also idle) */
    volatile unsigned long appl;     /* address of act_appl, 0 if none  */
    struct { volatile unsigned long bp, cnt; } slot[MS_SLOTS];
} MS_DATA;

#if defined(__m68k__) || defined(__mc68000__)

/* Template: XBRA header, handler, data.  Copied as one block. */
__asm__(
"	.text\n"
"	.even\n"
"ms_tpl_start:\n"
"	.ascii	\"XBRA\"\n"
"	.ascii	\"FMON\"\n"
"ms_tpl_old:\n"
"	.long	0\n"
"ms_tpl_handler:\n"
"	movem.l	%d0-%d1/%a0-%a1,-(%sp)\n"
"	lea	ms_tpl_data(%pc),%a1\n"
"	addq.l	#1,(%a1)\n"
"	move.l	28(%a1),%d0\n"
"	beq.s	7f\n"
"	move.l	%d0,%a0\n"
"	tst.l	(%a0)\n"
"	beq.s	8f\n"
"7:	move.l	18(%sp),%a0\n"
"	cmp.w	#0x4e72,-4(%a0)\n"
"	bne.s	1f\n"
"8:	addq.l	#1,4(%a1)\n"
"	bra.s	4f\n"
"1:	move.l	12(%a1),%a0\n"
"	move.l	(%a0),%d0\n"
"	bne.s	6f\n"
"	addq.l	#1,24(%a1)\n"
"	bra.s	8b\n"
"6:	lea	32(%a1),%a0\n"
"	move.w	#127,%d1\n"
"2:	cmp.l	(%a0),%d0\n"
"	beq.s	3f\n"
"	tst.l	(%a0)\n"
"	beq.s	5f\n"
"	addq.l	#8,%a0\n"
"	dbra	%d1,2b\n"
"	addq.l	#1,8(%a1)\n"
"	bra.s	4f\n"
"5:	move.l	%d0,(%a0)\n"
"3:	addq.l	#1,4(%a0)\n"
"4:	movem.l	(%sp)+,%d0-%d1/%a0-%a1\n"
"	move.l	ms_tpl_old(%pc),-(%sp)\n"
"	rts\n"
"	.even\n"
"ms_tpl_data:\n"
"	.space	32+128*8\n"
"ms_tpl_end:\n"
);

extern char ms_tpl_start[]   __asm__("ms_tpl_start");
extern char ms_tpl_handler[] __asm__("ms_tpl_handler");
extern char ms_tpl_data[]    __asm__("ms_tpl_data");
extern char ms_tpl_end[]     __asm__("ms_tpl_end");

#else   /* not a 68000 target: only so the C logic can be tested on a host */

static char ms_tpl_start[12 + 64 + sizeof(MS_DATA)];
#define ms_tpl_handler  (ms_tpl_start + 12)
#define ms_tpl_data     (ms_tpl_start + 12 + 64)
#define ms_tpl_end      (ms_tpl_start + sizeof ms_tpl_start)

#endif

static char    *ms_blk;             /* our block (copied template)     */
static MS_DATA *ms_fmon;            /* FMON data, NULL if not attached */
static struct mcpu *ms_mcpu;        /* MCPU.PRG's data, if installed   */
static int      ms;                 /* a sampler is available          */
static long     ms_kernel;          /* MX_KERNEL from KER_GETINFO      */
static long     ms_newblk;          /* block to install, from Mxalloc  */
static int      ms_free_it;

#define MS_OFF_HANDLER  (ms_tpl_handler - ms_tpl_start)
#define MS_OFF_DATA     (ms_tpl_data - ms_tpl_start)
#define MS_SIZE         (ms_tpl_end - ms_tpl_start)
#define MS_VEC          ((volatile long *)0x114L)

/* look for an existing sampler in the Timer C XBRA chain */
static long ms_find_super(void)
{
    long p = *MS_VEC;
    int guard = 64;

    while (p && !(p & 1) && guard--) {
        if (*(long *)(p - 12) != 0x58425241L)       /* 'XBRA' */
            break;
        if (*(long *)(p - 8) == MS_ID) {
            MS_DATA *d = (MS_DATA *)(p - 12 + MS_OFF_DATA);
            if (d->magic != MS_MAGIC)
                return 0;                           /* other layout */
            d->refcnt++;
            ms_blk = (char *)(p - 12);
            ms_fmon = d;
            return 1;
        }
        p = *(long *)(p - 4);
    }
    return 0;
}

static long ms_install_super(void)
{
    char *b = (char *)ms_newblk;
    MS_DATA *d = (MS_DATA *)(b + MS_OFF_DATA);
    long sysbase = *(long *)0x4f2L;
    long hdr = *(long *)(sysbase + 8);              /* os_beg */
    long i;

    for (i = 0; i < MS_SIZE; i++)
        b[i] = ms_tpl_start[i];
    d->run = *(long *)(hdr + 0x28);                 /* _run: &act_pd */
    d->appl = 0;
    if (ms_kernel > 0 && !(ms_kernel & 1)) {
        long apd = *(long *)(ms_kernel + 14);       /* MX_KERNEL.act_pd   */
        long aap = *(long *)(ms_kernel + 18);       /* MX_KERNEL.act_appl */
        if (apd && !(apd & 1)) d->run = apd;
        if (aap && !(aap & 1)) d->appl = aap;
    }
    d->magic = MS_MAGIC;
    d->refcnt = 1;
    *(long *)(b + 8) = *MS_VEC;                     /* XBRA old vector */
    *MS_VEC = (long)(b + MS_OFF_HANDLER);
    ms_blk = b;
    ms_fmon = d;
    return 1;
}

static long ms_find_mcpu(void)
{
    long *jar = *(long **)0x5a0L;
    if (jar)
        for (; jar[0]; jar += 2)
            if (jar[0] == MCPU_COOKIE) {
                struct mcpu *m = (struct mcpu *)jar[1];
                if (m && !((long)m & 1) && m->magic == MCPU_COOKIE &&
                    (m->version >> 8) == (MCPU_VERSION >> 8) && m->nslots > 0)
                    ms_mcpu = m;
                return 0;
            }
    return 0;
}

/* attach to the shared sampler, installing it if needed */
static int ms_attach(void)
{
    if (ms)
        return 1;
    Supexec(ms_find_mcpu);                          /* MSHELL's MCPU.PRG */
    if (ms_mcpu)
        return ms = 1;
    if (Supexec(ms_find_super))
        return ms = 1;
    ms_kernel = Dcntl(0x0100, 0L, 0L);              /* KER_GETINFO */
    if (ms_kernel <= 0)
        ms_kernel = Dcntl(0x6d00, 0L, 0L);          /* MX_KER_GETINFO */
    ms_newblk = (long)Mxalloc(MS_SIZE, 0x4000 | 3);  /* don't free */
    if (ms_newblk <= 0)
        return 0;
    return ms = (int)Supexec(ms_install_super);
}

static long ms_detach_super(void)
{
    long h, p;
    int guard = 64;

    ms_free_it = 0;
    if (!ms_fmon || ms_fmon->refcnt == 0)
        return 0;
    if (--ms_fmon->refcnt)
        return 0;                                   /* others still use it */

    h = (long)(ms_blk + MS_OFF_HANDLER);
    if (*MS_VEC == h) {
        *MS_VEC = *(long *)(ms_blk + 8);
        ms_free_it = 1;
        return 0;
    }
    p = *MS_VEC;                                    /* someone hooked after us */
    while (p && !(p & 1) && guard--) {
        if (*(long *)(p - 12) != 0x58425241L)
            return 0;                               /* can't unlink: keep it */
        if (*(long *)(p - 4) == h) {
            *(long *)(p - 4) = *(long *)(ms_blk + 8);
            ms_free_it = 1;
            return 0;
        }
        p = *(long *)(p - 4);
    }
    return 0;
}

static void ms_detach(void)
{
    if (ms_fmon) {                                  /* MCPU: nothing to do */
        Supexec(ms_detach_super);
        if (ms_free_it)
            Mfree(ms_blk);
    }
    ms = 0;
    ms_fmon = 0;
    ms_mcpu = 0;
    ms_blk = 0;
}

/* all ticks so far, and the idle ones */
static void ms_read(unsigned long *total, unsigned long *idle)
{
    if (ms_mcpu) {
        *total = ms_mcpu->total;
        *idle = ms_mcpu->idle;
    } else if (ms_fmon) {
        *total = ms_fmon->total;
        *idle = ms_fmon->idle;
    } else
        *total = *idle = 0;
}

/* ticks counted so far for one process (by basepage, or pid for MCPU) */
static unsigned long ms_count(unsigned long bp, short pid)
{
    unsigned long n = 0;
    int i;
    if (ms_mcpu) {
        for (i = 0; i < ms_mcpu->nslots; i++) {
            struct mcpu_slot *s = &ms_mcpu->slot[i];
            if (s->pd && ((bp && (unsigned long)s->pd == bp) ||
                          (!bp && s->pid == pid)))
                return s->ticks;
        }
        return 0;
    }
    if (!ms_fmon || !bp)
        return 0;
    for (i = 0; i < MS_SLOTS; i++)
        if (ms_fmon->slot[i].bp == bp)
            n += ms_fmon->slot[i].cnt;
    return n;
}

/* free slots of basepages that no longer exist, once the table is
 * three quarters full; `alive' says whether a basepage still exists */
static void ms_tidy(int (*alive)(unsigned long bp))
{
    int i, used = 0;
    if (!ms_fmon)                   /* MCPU recycles its own slots */
        return;
    for (i = 0; i < MS_SLOTS; i++)
        if (ms_fmon->slot[i].bp)
            used++;
    if (used < MS_SLOTS * 3 / 4)
        return;
    for (i = 0; i < MS_SLOTS; i++) {
        unsigned long bp = ms_fmon->slot[i].bp;
        if (bp && !alive(bp)) {
            ms_fmon->slot[i].cnt = 0;
            ms_fmon->slot[i].bp = 0;
        }
    }
}

#endif /* MAGSAMP_H */
