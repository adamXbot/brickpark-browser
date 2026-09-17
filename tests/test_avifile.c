/* avifile -- the AVIFIL32 shim serves the advisor's decoded frames.  PARK-2.
 *
 * The in-game advisor window at game (522, 378) is a hole in InterfaceBG.lls
 * that only RenderAdvisorIcon (screens3.c) ever paints, from
 * `dib = AVIStreamGetFrame(...)`; with every AVI open failing it was never
 * painted, and the cursor and bubble help drawn over it stayed on screen
 * (docs/lanes/scope-port-b11.md §4). src/hostwin/avifil32.c now opens
 * a clip whose frames tools/advisor_frames.py decoded at build time.
 *
 * What this pins, with synthetic frames files the test writes itself (so no
 * gamedata/, and nothing lands in the page's own advisor/ directory -- ctest
 * runs it in build-wasm/test-avifile):
 *
 *   - the ILP32 layouts LoadAdvisorMovie reads, at the offsets advisor.c
 *     declares: AVIFILEINFOA.dwStreams, AVISTREAMINFOA fccType / dwScale /
 *     dwRate / dwLength / rcFrame -- read here by byte offset, not through the
 *     shim's own structs;
 *   - the DIB BltAdvisor (blitmisc.c) takes on trust: a BITMAPINFOHEADER, then
 *     the stored 16-bpp rows at +0x28, the frame index clamped;
 *   - the open rules: a Windows path and any case find the clip, the FMV (no
 *     frames file) and a short or foreign file do not, and a failed open leaves
 *     *ppfile alone;
 *   - that no handle the shim did not issue is ever followed -- movie.c passes
 *     AVIFileRelease an uninitialised PAVIFILE -- including a PAVIFILE handed
 *     to a PAVISTREAM entry point and a PGETFRAME after its close.
 *
 * wasm32 only (tests.cmake registers it ILP32-only): the native driver links
 * the closure's trapping host stubs, not legoland_hostwin. */
#include "ll_tests.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

/* The import names as advisor.c and movie.c call them (the shim's own
 * spellings; __stdcall is empty in the portable build). */
extern void          AVIFileInit(void);
extern void          AVIFileExit(void);
extern long          AVIFileOpenA(void** ppfile, const char* name,
                                  unsigned int mode, const void* handler);
extern long          AVIFileInfoA(void* pfile, void* pfi, long size);
extern long          AVIFileGetStream(void* pfile, void** ppstream,
                                      unsigned long fcc, long lParam);
extern unsigned long AVIFileRelease(void* pfile);
extern long          AVIStreamInfoA(void* pavi, void* psi, long size);
extern unsigned long AVIStreamAddRef(void* pavi);
extern unsigned long AVIStreamRelease(void* pavi);
extern void*         AVIStreamGetFrameOpen(void* pavi, const void* wanted);
extern void*         AVIStreamGetFrame(void* pgf, long pos);
extern long          AVIStreamGetFrameClose(void* pgf);

#define AVIERR_BADHANDLE 0x8004406Cul
#define AVIERR_FILEOPEN  0x8004406Ful
#define AVIERR_NODATA    0x80044073ul
#define OF_WRITE         0x0001u

enum { W = 5, H = 3, FRAMES = 4, RATE = 15 };

/* Every stored word unique: frame, stored row, column. */
static uint16_t word_at(int frame, int row, int x)
{
    return (uint16_t)(frame << 12 | row << 8 | x << 4 | 0x5);
}

static void put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static uint32_t get32(const unsigned char* p, int off)
{
    return (uint32_t)p[off] | (uint32_t)p[off + 1] << 8
         | (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;
}

static uint16_t get16(const unsigned char* p, int off)
{
    return (uint16_t)(p[off] | p[off + 1] << 8);
}

/* advisor_frames.py's format: "LLV1", u16 w, u16 h, u32 frames, u32 rate,
 * u32 scale, then the frames. `stored` frames are written whatever `frames`
 * claims, which is how the short file is made. */
static int write_clip(const char* path, const char* magic, int frames, int stored)
{
    unsigned char hdr[0x14];
    FILE* f = fopen(path, "wb");
    int fr, row, x;

    if (!f)
        return 0;
    memcpy(hdr, magic, 4);
    hdr[4] = W; hdr[5] = 0;
    hdr[6] = H; hdr[7] = 0;
    put32(hdr + 0x08, (uint32_t)frames);
    put32(hdr + 0x0c, RATE);
    put32(hdr + 0x10, 1);
    fwrite(hdr, 1, sizeof hdr, f);
    for (fr = 0; fr < stored; fr++)
        for (row = 0; row < H; row++)
            for (x = 0; x < W; x++) {
                uint16_t v = word_at(fr, row, x);
                fputc(v & 0xff, f);
                fputc(v >> 8, f);
            }
    fclose(f);
    return 1;
}

/* The words of `dib` that are not frame `frame`'s. */
static int frame_mismatches(const unsigned char* dib, int frame)
{
    int row, x, bad = 0;
    for (row = 0; row < H; row++)
        for (x = 0; x < W; x++)
            if (get16(dib, 0x28 + (row * W + x) * 2) != word_at(frame, row, x))
                bad++;
    return bad;
}

void test_avifile(void)
{
    unsigned char fi[0x6c + 1], si[0x8c], small[0x11];
    unsigned char bmi[0x28], bmi24[0x28], bmi_wide[0x28];
    int           not_ours;
    void*         pf = 0, *pf2 = 0, *ps = 0, *ps_none = (void*)&not_ours, *pgf, *pgf0;
    void*         sentinel = (void*)&not_ours;
    unsigned char* dib;

    mkdir("advisor", 0777);
    if (!write_clip("advisor/tst_blink.llv", "LLV1", FRAMES, FRAMES)
        || !write_clip("advisor/tst_short.llv", "LLV1", FRAMES, 2)
        || !write_clip("advisor/tst_foreign.llv", "RIFF", FRAMES, FRAMES)) {
        ll_fail("setup", "cannot write advisor/*.llv in the working directory");
        return;
    }
    AVIFileInit();

    /* ---- opening --------------------------------------------------------- */
    LL_CHECK_INT("open: Windows path, mixed case",
                 AVIFileOpenA(&pf, "D:\\LEGOLAND\\TST_Blink.AVI", 0, 0), 0);
    LL_CHECK_TRUE("open: PAVIFILE written", pf != 0);
    LL_CHECK_INT("open again: same name", AVIFileOpenA(&pf2, "tst_blink.avi", 0, 0), 0);
    LL_CHECK_TRUE("open again: the loaded clip, not a second copy", pf2 == pf);

    pf2 = sentinel;
    LL_CHECK_HEX("open: no frames file (the FMV)",
                 (unsigned long)AVIFileOpenA(&pf2, "Intro.avi", 0, 0), AVIERR_FILEOPEN);
    LL_CHECK_TRUE("open: failure leaves *ppfile alone", pf2 == sentinel);
    LL_CHECK_HEX("open: frames file shorter than its header",
                 (unsigned long)AVIFileOpenA(&pf2, "tst_short.avi", 0, 0), AVIERR_FILEOPEN);
    LL_CHECK_HEX("open: not an LLV1 file",
                 (unsigned long)AVIFileOpenA(&pf2, "tst_foreign.avi", 0, 0), AVIERR_FILEOPEN);
    LL_CHECK_HEX("open: for writing",
                 (unsigned long)AVIFileOpenA(&pf2, "tst_blink.avi", OF_WRITE, 0), AVIERR_FILEOPEN);

    /* ---- AVIFILEINFOA, 0x6c, as LoadAdvisorMovie reads it ------------------ */
    memset(fi, 0xaa, sizeof fi);
    LL_CHECK_INT("AVIFileInfoA", AVIFileInfoA(pf, fi, 0x6c), 0);
    LL_CHECK_INT("AVIFILEINFOA +0x0c dwStreams", get32(fi, 0x0c), 1);
    LL_CHECK_INT("AVIFILEINFOA +0x14 dwWidth", get32(fi, 0x14), W);
    LL_CHECK_INT("AVIFILEINFOA +0x18 dwHeight", get32(fi, 0x18), H);
    LL_CHECK_INT("AVIFILEINFOA +0x24 dwLength", get32(fi, 0x24), FRAMES);
    LL_CHECK_INT("AVIFILEINFOA: nothing past 0x6c", fi[0x6c], 0xaa);
    memset(small, 0xaa, sizeof small);
    AVIFileInfoA(pf, small, 0x10);
    LL_CHECK_INT("AVIFileInfoA: writes no more than `size`", small[0x10], 0xaa);

    /* ---- the stream ------------------------------------------------------- */
    LL_CHECK_INT("AVIFileGetStream 0", AVIFileGetStream(pf, &ps, 0, 0), 0);
    LL_CHECK_TRUE("AVIFileGetStream: PAVISTREAM written", ps != 0);
    LL_CHECK_HEX("AVIFileGetStream 1: there is no second stream",
                 (unsigned long)AVIFileGetStream(pf, &ps_none, 0, 1), AVIERR_NODATA);
    LL_CHECK_TRUE("AVIFileGetStream 1: *ppstream cleared", ps_none == 0);

    memset(si, 0xaa, sizeof si);
    LL_CHECK_INT("AVIStreamInfoA", AVIStreamInfoA(ps, si, 0x8c), 0);
    LL_CHECK_HEX("AVISTREAMINFOA +0x00 fccType 'vids'", get32(si, 0x00), 0x73646976u);
    LL_CHECK_INT("AVISTREAMINFOA +0x14 dwScale", get32(si, 0x14), 1);
    LL_CHECK_INT("AVISTREAMINFOA +0x18 dwRate", get32(si, 0x18), RATE);
    LL_CHECK_INT("AVISTREAMINFOA +0x20 dwLength", get32(si, 0x20), FRAMES);
    LL_CHECK_INT("AVISTREAMINFOA +0x34 rcFrame.left", get32(si, 0x34), 0);
    LL_CHECK_INT("AVISTREAMINFOA +0x38 rcFrame.top", get32(si, 0x38), 0);
    LL_CHECK_INT("AVISTREAMINFOA +0x3c rcFrame.right", get32(si, 0x3c), W);
    LL_CHECK_INT("AVISTREAMINFOA +0x40 rcFrame.bottom", get32(si, 0x40), H);
    LL_CHECK_INT("AVIStreamAddRef counts", AVIStreamAddRef(ps), 2);
    LL_CHECK_INT("AVIStreamRelease counts", AVIStreamRelease(ps), 1);

    /* ---- the GETFRAME: InitAdvisorBmi's format, and nothing else ----------- */
    memset(bmi, 0, sizeof bmi);
    put32(bmi + 0x00, 0x28);
    put32(bmi + 0x04, W);
    put32(bmi + 0x08, H);
    bmi[0x0c] = 1;
    bmi[0x0e] = 16;
    memcpy(bmi24, bmi, sizeof bmi);
    bmi24[0x0e] = 24;
    memcpy(bmi_wide, bmi, sizeof bmi);
    put32(bmi_wide + 0x04, W + 1);
    LL_CHECK_TRUE("GetFrameOpen: 24 bpp is not on offer",
                  AVIStreamGetFrameOpen(ps, bmi24) == 0);
    LL_CHECK_TRUE("GetFrameOpen: another size is not on offer",
                  AVIStreamGetFrameOpen(ps, bmi_wide) == 0);
    pgf0 = AVIStreamGetFrameOpen(ps, 0);
    LL_CHECK_TRUE("GetFrameOpen: no preference", pgf0 != 0);
    AVIStreamGetFrameClose(pgf0);
    pgf = AVIStreamGetFrameOpen(ps, bmi);
    LL_CHECK_TRUE("GetFrameOpen: 16 bpp BI_RGB at the clip's size", pgf != 0);
    if (!pgf)
        return;

    dib = (unsigned char*)AVIStreamGetFrame(pgf, 2);
    LL_CHECK_TRUE("GetFrame 2: a DIB", dib != 0);
    if (!dib)
        return;
    LL_CHECK_INT("DIB +0x00 biSize", get32(dib, 0x00), 0x28);
    LL_CHECK_INT("DIB +0x04 biWidth (BltAdvisor, the hit test)", get32(dib, 0x04), W);
    LL_CHECK_INT("DIB +0x08 biHeight, positive: bottom-up", get32(dib, 0x08), H);
    LL_CHECK_INT("DIB +0x0e biBitCount", get16(dib, 0x0e), 16);
    LL_CHECK_INT("DIB +0x10 biCompression BI_RGB", get32(dib, 0x10), 0);
    LL_CHECK_INT("DIB +0x14 biSizeImage", get32(dib, 0x14), W * H * 2);
    LL_CHECK_INT("GetFrame 2: rows at +0x28 as stored", frame_mismatches(dib, 2), 0);
    dib = (unsigned char*)AVIStreamGetFrame(pgf, 3);
    LL_CHECK_INT("GetFrame 3", dib ? frame_mismatches(dib, 3) : -1, 0);
    dib = (unsigned char*)AVIStreamGetFrame(pgf, 99);
    LL_CHECK_INT("GetFrame past the end: the last frame", dib ? frame_mismatches(dib, FRAMES - 1) : -1, 0);
    dib = (unsigned char*)AVIStreamGetFrame(pgf, -5);
    LL_CHECK_INT("GetFrame before the start: frame 0", dib ? frame_mismatches(dib, 0) : -1, 0);

    /* ---- nothing unknown is followed ------------------------------------- */
    LL_CHECK_HEX("AVIFileInfoA: a handle never issued",
                 (unsigned long)AVIFileInfoA(sentinel, fi, 0x6c), AVIERR_BADHANDLE);
    LL_CHECK_HEX("AVIStreamInfoA: a PAVIFILE is not a PAVISTREAM",
                 (unsigned long)AVIStreamInfoA(pf, si, 0x8c), AVIERR_BADHANDLE);
    LL_CHECK_TRUE("GetFrameOpen: a PAVIFILE is not a PAVISTREAM",
                  AVIStreamGetFrameOpen(pf, bmi) == 0);
    LL_CHECK_TRUE("GetFrame: a handle never issued", AVIStreamGetFrame(sentinel, 0) == 0);
    LL_CHECK_INT("AVIFileRelease: a handle never issued", AVIFileRelease(sentinel), 0);
    LL_CHECK_INT("GetFrameClose", AVIStreamGetFrameClose(pgf), 0);
    LL_CHECK_TRUE("GetFrame after its close", AVIStreamGetFrame(pgf, 0) == 0);
    LL_CHECK_INT("GetFrameClose twice", AVIStreamGetFrameClose(pgf), 0);

    AVIStreamRelease(ps);
    AVIFileRelease(pf);
    AVIFileRelease(pf);
    AVIFileExit();
    remove("advisor/tst_blink.llv");
    remove("advisor/tst_short.llv");
    remove("advisor/tst_foreign.llv");
}
