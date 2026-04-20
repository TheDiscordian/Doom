/* i_timer.c — openfpgaOS timer: 1/35s tics, ms clock. */

#include "of.h"
#include "i_timer.h"
#include "doomtype.h"

#ifdef OF_PC
#include <time.h>
#endif

static unsigned int basetime = 0;

int I_GetTime(void)
{
    unsigned int ticks = of_time_ms();
    if (basetime == 0) basetime = ticks;
    ticks -= basetime;
    return (int)((ticks * TICRATE) / 1000);
}

int I_GetTimeMS(void)
{
    unsigned int ticks = of_time_ms();
    if (basetime == 0) basetime = ticks;
    return (int)(ticks - basetime);
}

void I_Sleep(int ms)
{
    /* TryRunTics() calls I_Sleep(1) in a tight spin-wait when it's
     * throttling to the 35 Hz tic rate — several thousand times a
     * second.  A busy-wait here pegs a core at 100%, causes thermal
     * throttling on laptops/handhelds, and starves the audio thread,
     * all of which show up as visible render judder.  Use a real
     * OS sleep so the scheduler can run other threads. */
    if (ms <= 0) return;
#ifdef OF_PC
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#else
    unsigned int start = of_time_ms();
    while ((int)(of_time_ms() - start) < ms) { /* spin */ }
#endif
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}

void I_InitTimer(void)
{
    basetime = 0;
}
