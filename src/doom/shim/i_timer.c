/* i_timer.c -- openfpgaOS timer: 1/35s tics, us clock. */

#include "of.h"
#include "i_timer.h"
#include "doomtype.h"

#ifdef OF_PC
#include <time.h>
#endif

static int timer_base_set = 0;
static unsigned int base_us = 0;
static unsigned int last_raw_us = 0;
static uint64_t high_us = 0;
static uint64_t display_vblank_period_us = 16667;
static uint64_t display_last_vblank_raw_us = 0;
static uint32_t display_last_vblank_count = 0;

static uint64_t I_RawTimeUS(void)
{
    unsigned int raw_us = of_time_us();

    if (!timer_base_set)
    {
        timer_base_set = 1;
        base_us = raw_us;
        last_raw_us = raw_us;
    }
    else if (raw_us < last_raw_us)
    {
        high_us += 1ULL << 32;
    }

    last_raw_us = raw_us;
    return high_us + raw_us;
}

uint64_t I_GetTimeUS(void)
{
    return I_RawTimeUS() - base_us;
}

uint64_t I_GetDisplayTimeUS(void)
{
    of_video_timing_t timing;
    uint64_t raw_now;
    uint64_t sample_raw_us;

    raw_now = I_RawTimeUS();
    of_video_get_timing(&timing);

    if (timing.last_vblank_us == 0 || timing.vblank_count == 0)
        return raw_now - base_us;

    if (display_last_vblank_raw_us != 0 &&
        timing.vblank_count != display_last_vblank_count &&
        timing.last_vblank_us > display_last_vblank_raw_us)
    {
        uint32_t count_delta = timing.vblank_count - display_last_vblank_count;
        uint64_t elapsed_us = timing.last_vblank_us - display_last_vblank_raw_us;
        uint64_t period_us = elapsed_us / count_delta;

        if (period_us >= 10000 && period_us <= 25000)
            display_vblank_period_us = period_us;
    }

    display_last_vblank_raw_us = timing.last_vblank_us;
    display_last_vblank_count = timing.vblank_count;

    sample_raw_us = timing.last_vblank_us + display_vblank_period_us;
    if (sample_raw_us <= (uint64_t)base_us)
        return raw_now - base_us;

    return sample_raw_us - base_us;
}

int I_GetTime(void)
{
    return (int)((I_GetTimeUS() * TICRATE) / 1000000ULL);
}

int I_GetTimeMS(void)
{
    return (int)(I_GetTimeUS() / 1000ULL);
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
    timer_base_set = 0;
    base_us = 0;
    last_raw_us = 0;
    high_us = 0;
    display_vblank_period_us = 16667;
    display_last_vblank_raw_us = 0;
    display_last_vblank_count = 0;
}
