/* i_main.c — openfpgaOS entry: boot SDK, then call D_DoomMain. */

#include "config.h"
#include "of.h"
#include "of_caps.h"
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void D_DoomMain(void);

int main(int argc, char **argv)
{
#ifdef OF_PC
    /* Doom ticks at 35 Hz. On a 60 Hz desktop, hard vsync inside
     * SDL_RenderPresent aligns each frame to a 16.67 ms boundary,
     * which effectively quantises our 28.57 ms tic budget to every
     * *other* vsync (= 30 Hz) and makes the game feel choppy.
     * TryRunTics already paces the loop, so disable vsync and let
     * the tic cadence drive the display rate. */
    setenv("OF_NO_VSYNC", "1", 0);
#endif

    /* Boot the SDK. Audio/mixer init is deferred to I_SDL_InitSound
     * (i_sdlsound.c) so the sound module owns the channel count. */
    of_video_init();

#ifndef OF_PC
    {
        const struct of_capabilities *c = of_get_caps();
        printf("heap: base=%08x size=%u (%u KB) sdram=%u MB\n",
               c->heap_base, c->heap_size, c->heap_size / 1024,
               c->sdram_size / (1024*1024));
    }
#endif

    /* openfpgaOS port: the Pocket kernel has no mechanism to pass argv
     * from instance.json, so we inject "-iwad DOOM.WAD" here. The
     * instance JSON binds DOOM.WAD to data slot 3; the kernel file
     * service resolves the filename via that binding. On the desktop
     * build (OF_PC) the user can still override via the real command
     * line -- anything they pass wins because M_CheckParmWithArgs()
     * returns the first match. */
#ifndef OF_PC
    int   injected = 3;
    char *injected_argv[] = { "-iwad", "DOOM.WAD", "-noautoload" };
#else
    int   injected = 0;
    char *injected_argv[] = { 0 };
#endif

    myargc = argc + injected;
    myargv = malloc(myargc * sizeof(char *));
    assert(myargv != NULL);
    for (int i = 0; i < argc; i++)
        myargv[i] = M_StringDuplicate(argv[i]);
    for (int i = 0; i < injected; i++)
        myargv[argc + i] = M_StringDuplicate(injected_argv[i]);

    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

    M_FindResponseFile();
    M_SetExeDir();

    D_DoomMain();
    return 0;
}
