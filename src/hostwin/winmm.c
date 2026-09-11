/* LEGOLAND portable build -- the WINMM host shim (scope PORT-B): the
 * millisecond clock, the periodic timers, and the MIDI output that is not here.
 *
 * timeGetTime is the port's second yield point. FlipPrimary (sysmisc.c
 * 0x004661d0) and PresentFlip (blitmisc.c 0x00466080) both enforce their frame
 * floor with a bare spin:
 *
 *     while (timeGetTime() - g_flip_time < 0x1c) ;
 *
 * There is no Sleep in it and the game may not be edited, so the only place a
 * yield can go is inside timeGetTime itself. It yields whenever 4 ms of wall
 * clock has passed since the last yield (ll_host_yield_throttled, user32.c),
 * which turns that 28 ms spin into about seven browser turns and -- more
 * importantly -- bounds EVERY wall-clock spin in the program, including any not
 * yet found. See docs/lanes/scope-port-b.md §1.
 *
 * timeSetEvent callbacks are dispatched from the message pump
 * (ll_host_pump_timers, called by PeekMessageA / WaitMessage), NOT from a JS
 * timer. Under ASYNCIFY the C stack is unwound while a yield is in flight, and
 * calling into wasm then would re-enter a half-unwound program. The pump is
 * game-synchronous, so the callback runs on a stack the game owns.
 *
 * The consequence is that a timer's period is a lower bound, not a guarantee:
 * the pump runs about once per presented frame (the 28 ms floor gives ~35 Hz),
 * so InitMIDIManager's 20 ms TIME_PERIODIC timer (lifecycle.c 0x00480630)
 * actually ticks every ~28 ms. Its callback is MIDITimerTick (0x00480570),
 * which drives MIDI sequence playback; with midiOut* reporting
 * MMSYSERR_NOTSUPPORTED there is nothing audible for the drift to affect.
 *
 * MIDI: every entry returns MMSYSERR_NOTSUPPORTED (8). InitMIDIManager ignores
 * midiOutOpen's result and reports success regardless (docs/runtime/assets.md:
 * "MIDI initialization ... then reports success regardless of those API
 * results"), so the game carries on with a null HMIDIOUT.
 */
#include <string.h>

#include "ll_host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <time.h>
#endif

#define MMSYSERR_NOERROR        0u
#define MMSYSERR_NOTSUPPORTED   8u
#define MMSYSERR_INVALPARAM     11u
#define TIME_ONESHOT            0u
#define TIME_PERIODIC           1u

/* ---- the clock ---------------------------------------------------------- */

static double clock_now_ms(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static double g_epoch_ms;

static unsigned int clock_ticks(void)
{
    double now = clock_now_ms();
    if (g_epoch_ms == 0.0)
        g_epoch_ms = now;
    return (unsigned int)(now - g_epoch_ms);
}

/* THE yield point for the game's spin loops -- see the header comment. */
unsigned int timeGetTime(void)
{
    if (ll_host_beating()) ll_host_beat("winmm.timeGetTime");
    ll_host_yield_throttled(4);
    return clock_ticks();
}

/* timeBeginPeriod / timeEndPeriod: the browser clock has no resolution to
 * request. Not referenced by the current closure, declared for completeness. */
unsigned int timeBeginPeriod(unsigned int period) { (void)period; return MMSYSERR_NOERROR; }
unsigned int timeEndPeriod(unsigned int period) { (void)period; return MMSYSERR_NOERROR; }

/* ---- periodic timers ---------------------------------------------------- */
/* LPTIMECALLBACK: void (CALLBACK*)(UINT id, UINT msg, DWORD_PTR user,
 *                                  DWORD_PTR dw1, DWORD_PTR dw2)
 * -- the shape lifecycle.c declares (MMTimeProc, lifecycle.c:88). */
typedef void (*LLTimeProc)(unsigned int id, unsigned int msg,
                           unsigned long user, unsigned long dw1,
                           unsigned long dw2);

#define LL_TIMERS 8
typedef struct LLTimer {
    int          live;
    unsigned int id;
    unsigned int delay;        /* ms */
    unsigned int periodic;
    LLTimeProc   cb;
    unsigned long user;
    double       due_ms;
} LLTimer;

static LLTimer g_timers[LL_TIMERS];
static unsigned int g_next_timer_id = 1;
static int g_in_timer_dispatch;

unsigned int timeSetEvent(unsigned int delay, unsigned int resolution,
                          void* callback, unsigned long user,
                          unsigned int flags)
{
    int i;
    (void)resolution;
    if (!callback)
        return 0;               /* 0 is timeSetEvent's failure return */
    for (i = 0; i < LL_TIMERS; i++) {
        if (!g_timers[i].live) {
            g_timers[i].live = 1;
            g_timers[i].id = g_next_timer_id++;
            g_timers[i].delay = delay ? delay : 1;
            g_timers[i].periodic = (flags & 1) == TIME_PERIODIC;
            g_timers[i].cb = (LLTimeProc)callback;
            g_timers[i].user = user;
            g_timers[i].due_ms = clock_now_ms() + (double)g_timers[i].delay;
            return g_timers[i].id;
        }
    }
    return 0;
}

unsigned int timeKillEvent(unsigned int id)
{
    int i;
    for (i = 0; i < LL_TIMERS; i++) {
        if (g_timers[i].live && g_timers[i].id == id) {
            g_timers[i].live = 0;
            g_timers[i].cb = 0;
            return MMSYSERR_NOERROR;
        }
    }
    return MMSYSERR_INVALPARAM;
}

/* Called from the message pump, once per pass. Re-entrancy is guarded: a
 * callback that itself pumps messages (none do today) must not recurse. */
void ll_host_pump_timers(void)
{
    double now;
    int i;

    if (g_in_timer_dispatch)
        return;
    g_in_timer_dispatch = 1;
    now = clock_now_ms();
    for (i = 0; i < LL_TIMERS; i++) {
        if (!g_timers[i].live || now < g_timers[i].due_ms)
            continue;
        if (g_timers[i].periodic) {
            /* Do not try to catch up on a backlog: one tick per pass, with the
             * next due time measured from now. A browser tab that was
             * backgrounded for a minute must not fire 3,000 MIDI ticks. */
            g_timers[i].due_ms = now + (double)g_timers[i].delay;
        } else {
            g_timers[i].live = 0;
        }
        if (g_timers[i].cb)
            g_timers[i].cb(g_timers[i].id, 0, g_timers[i].user, 0, 0);
    }
    g_in_timer_dispatch = 0;
}

/* ---- MIDI out: documented failure -------------------------------------- */
unsigned int midiOutOpen(void** out, unsigned int device, unsigned long cb,
                         unsigned long inst, unsigned long flags)
{
    (void)device; (void)cb; (void)inst; (void)flags;
    if (out)
        *out = 0;
    return MMSYSERR_NOTSUPPORTED;
}
unsigned int midiOutClose(void* handle) { (void)handle; return MMSYSERR_NOTSUPPORTED; }
unsigned int midiOutShortMsg(void* handle, unsigned long msg)
{ (void)handle; (void)msg; return MMSYSERR_NOTSUPPORTED; }
unsigned int midiOutReset(void* handle) { (void)handle; return MMSYSERR_NOTSUPPORTED; }
