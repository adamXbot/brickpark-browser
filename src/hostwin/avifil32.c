/* LEGOLAND portable build -- the AVIFIL32 host shim (scope PORT-B): Video for
 * Windows' AVIFile API, reporting "this file will not open", honestly.
 *
 * WHAT THIS IS AND IS NOT.  The game's FMV and its on-screen advisor are both
 * played through AVIFile (movie.c, movie2.c, advisor.c, screens3.c), and the
 * clips are Indeo 5 (Ir50_32.dll -- RunGame even LoadLibraryA's it,
 * gamemain.c:398).  There is no Indeo 5 decoder in this port and writing one is
 * not in any lane's scope.  So this file is not a decoder and does not pretend
 * to be one: it is the sixteen entry points the import table names
 * (portable/tools/win32_imports.txt), each returning the result a real AVIFile
 * returns when the file cannot be opened, so that every path the game has for
 * "no movie" runs -- instead of the generated trap, which printed a line and
 * handed the caller a zero it never computed.
 *
 *     Before this file:  TRAP AVIFIL32.dll AVIFileInit  from advisor.c, movie.c
 *                        TRAP AVIFIL32.dll AVIFileOpenA ...   (six of them)
 *     After:             the page runs with no traps at all.
 *
 * THE ONE DECISION: AVIFileOpenA FAILS.  Everything else follows from it, and
 * the alternative -- succeeding with a synthetic zero-length stream -- was
 * rejected after reading both callers.  The failure is the path the game is
 * BUILT for; the synthetic stream is a path it has never taken:
 *
 *   movie.c 0x00476460 OpenMovie
 *       if (AVIFileOpenA(&pfile, path, 0, 0) != 0) {
 *           if (g_avi_open_count == 0) AVIFileExit();
 *           return 0;                        <-- the whole of it
 *       }
 *     and uimisc2.c 0x004771f0 PlayMovie, its only caller, answers a null
 *     movie by trying the second prefix (g_res_path) and then `return 1` --
 *     WITHOUT entering the player: no PauseAllSamples, no StopMusic, no
 *     PushRenderingStatusAndUnlockVideoSurface, no RunMovie, no CloseMovie.
 *     So the intro, lmi.avi, the three park movies (uimisc3.c), the level-end
 *     movie (gameframe.c:517) and the report movie (uimisc.c:734) each cost one
 *     DebugPrintf and nothing else.  A SYNTHETIC stream would instead take the
 *     game into RunMovie with frames == 0, which unlocks and relocks the video
 *     surface around a loop that never runs, ducks and restores the whole audio
 *     stack, and spins until all three mouse buttons are up -- a great deal of
 *     machinery to arrive at the same blank screen.  It also divides by
 *     si.dwScale (`fps = si.dwRate / si.dwScale`, movie.c:663) in a struct the
 *     shim would have to fill in, in the ILP32 layout, from a header nobody has
 *     checked against the original.
 *
 *   advisor.c 0x00443bd0 LoadAdvisorMovie is the same shape, and the advisor
 *     degrades to "no advisor animation" with nothing else touched, because the
 *     one place an advisor frame is drawn is guarded:
 *
 *       screens3.c 0x00443e30 RenderAdvisorIcon
 *           if (g_vidanim) {              /. 0x00665f5c == advisor.c's
 *               dib = AVIStreamGetFrame(g_vidanim->pgf, g_vid_frame);
 *                                          g_advisor_clip ./
 *               BltAdvisor(dib, p->x, p->y);
 *               ... g_gfx_point.x < p->x + dib->width ...
 *
 *     With all six LoadAdvisorMovie calls returning 0, InitAdvisorMovies leaves
 *     g_ad_blink..g_ad_wobble null, StartAdvisorClip stores null into
 *     g_advisor_clip, and that `if (g_vidanim)` is false for the rest of the
 *     run.  AVIStreamGetFrame is therefore never called, and -- the part that
 *     matters -- `BltAdvisor(dib, ...)` and `dib->width` never see a null DIB.
 *     KillAdvisorMovies guards every FreeAdvisorClip with its own `if`, so
 *     teardown is clean too.  MEASURED: the advisor panel draws nothing and the
 *     front end runs on.
 *
 * POINTER-BLIND, AND WHY THAT IS LOAD-BEARING, NOT TIDINESS.  No entry point
 * here dereferences a PAVIFILE, PAVISTREAM or PGETFRAME, or writes through an
 * out parameter it was not handed by a call that succeeded.  It cannot: the
 * game reaches two of these functions with a pointer read out of a NULL struct.
 *
 *   advisor.c 0x00443dc0 StartAdvisorClip(clip) is called as
 *   `StartAdvisorClip(g_ad_blink)` at the end of InitAdvisorMovies with
 *   g_ad_blink == 0.  Its first statement is `if (clip->stop)` -- an unguarded
 *   dereference, three lines above an `if (clip)` that shows the author knew it
 *   could be null.  As shipped that is an access violation on Windows the
 *   moment an advisor AVI is missing; here it reads offset 0x20 of address 0,
 *   which wasm permits, and clang then uses the dereference to prove clip !=
 *   null and FOLDS AWAY the later `if (clip)` -- so
 *   `AVIStreamGetFrameOpen(clip->video, &g_advisor_bmi)` is called
 *   unconditionally with whatever word sits at address 0x1c.  That is the
 *   observed behaviour of the -O2 browser build and it is why
 *   AVIStreamGetFrameOpen returns 0 without looking at its argument.  Recorded
 *   as a game-side finding in docs/lanes/scope-port-b6.md; LEGOLAND/*.c is
 *   read-only for this lane.
 *
 *   movie.c OpenMovie leaves `pfile` UNINITIALISED when AVIFileOpenA fails (it
 *   only writes *ppfile on success, as the real AVIFile does), and on the
 *   no-video path calls `AVIFileRelease(pfile)` with that garbage.  Note that
 *   the no-video path is unreachable from a FAILED open -- OpenMovie returns
 *   first -- but AVIFileRelease is still in the trap list because the caller
 *   that does reach it is the same function on a different rung.  Either way
 *   the shim must not follow the pointer.
 *
 * RESULT CODES.  MAKE_AVIERR(n) is MAKE_SCODE(SEVERITY_ERROR, FACILITY_ITF,
 * 0x4000 + n), i.e. 0x8004_4000 + n, so AVIERR_FILEOPEN (111) is 0x8004406F and
 * AVIERR_BADHANDLE (108) is 0x8004406C.  Those are the two this file uses;
 * AVIFileInit/AVIFileExit and the AddRef/Release pair return counts, not
 * HRESULTs, and the game reads AVIFileRelease's and AVIStreamRelease's results
 * nowhere (unlike the DirectSound buffer Release, which audiomisc.c:51 compares
 * to 0 -- see dsound.c).
 *
 * WHAT WOULD HAVE TO CHANGE FOR REAL VIDEO.  A decoder (Indeo 5 or a
 * transcode of the 26 AD_*.avi plus the FMV set to something a browser can
 * decode), AVIFileOpenA returning a real object, AVIStreamInfoA filling an
 * AVISTREAMINFOA in the ILP32 layout movie.c:123 documents, and
 * AVIStreamGetFrame returning a 16-bpp BITMAPINFOHEADER + pixels at +0x28
 * (blitmisc.c:67) matching g_movie_bmi / g_advisor_bmi (112x96x16 for the
 * advisor, advisor.c InitAdvisorBmi).  At that point the audio half needs
 * msacm32.c to become real as well -- see the header of that file.
 */
#include "ll_host.h"

/* MAKE_AVIERR(n) == 0x80044000 + n. */
#define AVIERR_BADHANDLE   0x8004406Cl   /* MAKE_AVIERR(108) */
#define AVIERR_FILEOPEN    0x8004406Fl   /* MAKE_AVIERR(111) */
#define AVIERR_NODATA      0x80044073l   /* MAKE_AVIERR(115) */

/* AVIFileInit/AVIFileExit bracket the library. The game reference-counts them
 * itself in g_avi_open_count (movie.c:225) and calls Init only on the 0 -> 1
 * edge, so these see one balanced pair per failed open. Counted here too, and
 * traced once, purely so the lane notes can say the bracketing is balanced. */
static int avi_init_depth;
static int avi_init_total;

void AVIFileInit(void)
{
    if (avi_init_depth++ == 0 && avi_init_total++ == 0)
        ll_host_trace("AVIFileInit: no AVIFile library in this port;"
                      " every open will report AVIERR_FILEOPEN");
}

void AVIFileExit(void)
{
    if (avi_init_depth > 0)
        avi_init_depth--;
}

/* The one that matters. *ppfile is deliberately NOT written: a real AVIFile
 * leaves it alone on failure, and both callers test the HRESULT before reading
 * it (movie.c:648, advisor.c:144). */
long AVIFileOpenA(void** ppfile, const char* name, unsigned int mode,
                  const void* handler)
{
    (void)ppfile;
    (void)mode;
    (void)handler;
    ll_host_trace("AVIFileOpenA(\"%s\"): AVIERR_FILEOPEN (no AVI decoder)",
                  name ? name : "(null)");
    return AVIERR_FILEOPEN;
}

/* Never reached from a failed open (OpenMovie returns first), but the import
 * exists and a future decoder would reach it. Leaves *pfi alone for the reason
 * in the header; both callers pre-clear the only field they read
 * (`fi.dwStreams = 0;` movie.c:653, advisor.c:150) and neither checks the
 * result, so an untouched buffer reads as "no streams". */
long AVIFileInfoA(void* pfile, void* pfi, long size)
{
    (void)pfile;
    (void)pfi;
    (void)size;
    return AVIERR_BADHANDLE;
}

long AVIFileGetStream(void* pfile, void** ppstream, unsigned long fcc,
                      long lParam)
{
    (void)pfile;
    (void)ppstream;
    (void)fcc;
    (void)lParam;
    return AVIERR_BADHANDLE;
}

/* A reference count, not an HRESULT. Nothing in the game reads it. */
unsigned long AVIFileRelease(void* pfile)
{
    (void)pfile;
    return 0;
}

long AVIStreamInfoA(void* pavi, void* psi, long size)
{
    (void)pavi;
    (void)psi;
    (void)size;
    return AVIERR_BADHANDLE;
}

unsigned long AVIStreamAddRef(void* pavi)
{
    (void)pavi;
    return 1;
}

unsigned long AVIStreamRelease(void* pavi)
{
    (void)pavi;
    return 0;
}

/* Reached with a garbage pointer -- see the header. Returns a null PGETFRAME,
 * which is what a real AVIStreamGetFrameOpen returns when it has no
 * decompressor for the stream, and which the callers store straight into
 * clip->getframe / mv->getframe (both then guarded by `if (...->getframe)`
 * before AVIStreamGetFrameClose; advisor.c:282, movie.c:561). */
void* AVIStreamGetFrameOpen(void* pavi, const void* wanted)
{
    (void)pavi;
    (void)wanted;
    return 0;
}

/* movie.c:776 treats a null return as "the decompressor gave up" and aborts
 * the movie (`mv->getframe = 0; return 0;`); screens3.c:2197 is behind the
 * `if (g_vidanim)` guard and so is unreachable here. */
void* AVIStreamGetFrame(void* pgf, long pos)
{
    (void)pgf;
    (void)pos;
    return 0;
}

long AVIStreamGetFrameClose(void* pgf)
{
    (void)pgf;
    return 0;
}

/* ---- the audio side of a stream (movie2.c) -------------------------------
 *
 * All four are behind `mv->audio`, which only StartMovieAudio reads and which
 * OpenMovie only ever sets from a stream walk that cannot happen. Kept real
 * (not traps) because the import table names them and because a zero-length,
 * zero-start stream is the consistent answer to go with a failed open:
 * StartMovieAudio's `end = AVIStreamStart(mv->audio) + AVIStreamLength(...)`
 * (movie2.c:268) is then 0, and its caller plays nothing. */
long AVIStreamStart(void* pavi)
{
    (void)pavi;
    return 0;
}

long AVIStreamLength(void* pavi)
{
    (void)pavi;
    return 0;
}

long AVIStreamRead(void* pavi, long start, long samples, void* buf,
                   long buflen, long* bytes, long* nsamples)
{
    (void)pavi;
    (void)start;
    (void)samples;
    (void)buf;
    (void)buflen;
    /* The two out counts ARE written when the pointers are non-null: movie2.c
     * hands them stack addresses, and "0 bytes, 0 samples" is what says the
     * read produced nothing. */
    if (bytes)
        *bytes = 0;
    if (nsamples)
        *nsamples = 0;
    return AVIERR_NODATA;
}

/* movie2.c:312 calls this with fmt == 0 to SIZE the format block, then again
 * with a buffer. `*size = 0` on the sizing pass is what makes the second call
 * ask for nothing; the caller checks the HRESULT of neither, but it does test
 * the wave format tag afterwards, which an untouched buffer leaves as whatever
 * the record already held -- all zero in the image. */
long AVIStreamReadFormat(void* pavi, long pos, void* fmt, long* size)
{
    (void)pavi;
    (void)pos;
    (void)fmt;
    if (size)
        *size = 0;
    return AVIERR_BADHANDLE;
}
