/* LEGOLAND portable build -- the four browser-only symbols, for node.
 *
 * PORT-B's `user32.c` calls into `portable/src/browser/ll_canvas.js` (opened
 * canvas, RGB565 -> ImageData, DOM events) and yields with `emscripten_sleep`.
 * Under node there is no canvas, no DOM and no ASYNCIFY, and the `--js-library`
 * is not linked, so those five names are undefined -- which is why
 * `legoland_headless` and `legoland_tests` could not link the shim at all and
 * had to take the generated TRAP stub for `ShowWindow` instead.
 *
 * This file supplies them, and NOTHING else: PORT-B's files are not edited
 * (docs/SCOPE_PORT_WAVE.md rule 3). It is linked into the node executables
 * only, never into `legoland_browser`, which has the real ll_canvas.js.
 *
 * The signatures must match user32.c's declarations exactly -- wasm calls are
 * type-checked and a mismatch becomes a silent trapping stub.
 *
 * `ll_node_frames()` is the useful part: a headless run can say how many
 * frames the game presented, and what the first one looked like, without a
 * canvas. `LL_HOST_TRACE=1` prints the display mode and the first present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ll_node_frames_n;
static int ll_node_w, ll_node_h;

static int ll_node_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("LL_HOST_TRACE");
        on = e && *e && *e != '0';
    }
    return on;
}

/* ll_canvas.js: open a canvas at the mode ddraw.c settled on. */
void ll_js_display_open(int w, int h)
{
    ll_node_w = w;
    ll_node_h = h;
    if (ll_node_trace())
        fprintf(stderr, "NODE display_open %dx%d (no canvas under node)\n", w, h);
}

/* ll_canvas.js: RGB565 -> RGBA into the canvas. Here: count it, and describe
 * the first frame, which is enough to tell "the game drew something" from "the
 * game never got to a present". */
void ll_js_present16(const void* pixels, int w, int h, int pitch)
{
    if (ll_node_frames_n++ == 0 && ll_node_trace()) {
        const unsigned short* p = (const unsigned short*)pixels;
        int i, n = w < 16 ? w : 16;
        unsigned nonzero = 0;
        for (i = 0; i < w * h; i++)
            if (((const unsigned short*)pixels)[i])
                nonzero++;
        fprintf(stderr, "NODE present16 #1 %dx%d pitch %d: %u/%d non-black,"
                        " first row:", w, h, pitch, nonzero, w * h);
        for (i = 0; i < n; i++)
            fprintf(stderr, " %04x", p[i]);
        fprintf(stderr, "\n");
    }
}

/* ll_canvas.js: drain one DOM event. There are none. */
int ll_js_event_next(int* out4)
{
    (void)out4;
    return 0;
}

void ll_js_set_cursor(int visible)
{
    (void)visible;
}

/* PORT-B2's page shows the last MessageBoxA and the answer the shim gave
 * (user32.c decides the answer; this is display only). Under node the text is
 * already on stderr via the trace, so just say what was answered. */
void ll_js_messagebox(const char* text, const char* caption, int answer)
{
    fprintf(stderr, "[MessageBox] %s: %s -> %d\n", caption ? caption : "", text ? text : "", answer);
}

/* PORT-B's ll_host_yield yields the browser's main thread with ASYNCIFY. The
 * node harness is not built with -sASYNCIFY (nothing here needs the browser to
 * paint), so this is the no-op that makes the same shim link. A game loop that
 * really depends on yielding will spin instead -- run it under the harness's
 * wall-clock cap, not unbounded. */
void emscripten_sleep(unsigned int ms)
{
    (void)ms;
}

int ll_node_frames(void)
{
    return ll_node_frames_n;
}

void ll_node_display_size(int* w, int* h)
{
    *w = ll_node_w;
    *h = ll_node_h;
}
