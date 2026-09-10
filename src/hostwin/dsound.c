/* LEGOLAND portable build -- the DirectSound host shim (scope PORT-B).
 *
 * There is no audio yet, so DirectSoundCreate reports failure and the game runs
 * silent. That is a SAFE failure, and the chain that makes it safe was checked
 * in the recovered sources before this was written:
 *
 *   audio4.c 0x00492130 InitSoundSampleSystem
 *       if (DirectSoundCreate(0, &g_dsound, 0) != 0) goto fail;
 *       ...
 *   fail: g_dsound = 0; g_samples_ready = 0; return 0;
 *
 *   lifecycle.c 0x004964f0 InitSoundSystem
 *       ok = InitSoundSampleSystem(g_snd_hwnd);
 *       if (!ok) return ok;                 -- music system never starts
 *
 *   gamemain.c 0x00459520 RunGame
 *       InitSoundSystem();                  -- result IGNORED
 *
 * So a failed create leaves g_dsound null and g_samples_ready 0, and RunGame
 * carries on. Every later sample entry point is guarded by g_samples_ready
 * (audio3.c's playable start, audio4.c's PlayNarrationFile, audio5.c's
 * ClearSampleSource / RefreshSampleVolumes, sysmisc.c's UpdateSampleSource) and
 * returns 0 with the null pointer untouched, which is why this is a documented
 * failure rather than a stub object. docs/runtime/assets.md's rules section:
 * "Playable start requires ready system, nonnull instance and nonnull
 * definition."
 *
 * The one thing to watch: KillSoundSampleSystem (lifecycle.c 0x00492c20)
 * dereferences g_dsound -- but only after `if (!g_samples_ready) return 0;`,
 * which is the branch this failure takes.
 *
 * When audio does arrive it belongs here as a real IDirectSound over
 * Web Audio / SDL3 audio, with the IDirectSoundBuffer vtable input2.c:160
 * declares (QueryInterface/AddRef/Release/GetCaps/GetCurrentPosition/GetFormat/
 * GetVolume/GetPan/GetFrequency/GetStatus/Initialize/Lock/Play/
 * SetCurrentPosition/...).
 */
#include "ll_host.h"

#define DSERR_NODRIVER  ((long)0x88780078)   /* no sound driver is available */

long DirectSoundCreate(void* guid, void** out, void* outer)
{
    (void)guid; (void)outer;
    if (out)
        *out = 0;
    return DSERR_NODRIVER;
}
