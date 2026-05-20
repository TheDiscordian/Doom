# CR: Mixer Stable Handles and Autonomous Pump

Status: Draft

## Problem

The mixer API currently exposes a playing sound as an `int voice`, which is a
physical mixer slot index. A slot index is not an ownership token. When a voice
finishes, is stolen, or is reused by another caller, any old app-side reference
to that same integer can still stop or update the new logical sound.

Group-aware allocation reduces the most visible MUSIC-vs-SFX collisions, but it
does not prove ownership:

- stale SFX references can still modify newer SFX in the same slot
- old firmware can report ownership as unknown
- the ended-voice bitmask is also slot-based, so completion events are not tied
  to the logical sound that ended
- app-side guards are duplicated and easy to get subtly wrong

This shows up as random missing or cut-off sounds in ports that keep per-channel
voice references. Doom's door/lift/monster SFX reports are a good example: the
game thinks it is managing a sound it started, but the mixer slot may already
belong to something else.

There is a second timing issue. Any mixer/audio pump work needed to keep audio
buffers full must not depend on render-frame cadence. Long frames, heavy BSP
scenes, or menu stalls can delay pump calls and create note cutoffs, envelope
bursts, or underruns even when the audio hardware itself is capable of smooth
playback.

## Goals

- Make voice ownership stale-safe with an opaque handle that names a logical
  sound, not only a slot.
- Keep existing voice-index APIs working for ABI compatibility.
- Add handle-safe variants for every operation that mutates or queries a
  logical voice.
- Preserve group-aware allocation as policy for priority and stealing.
- Provide handle-based completion events.
- Let the OS/SDK keep audio buffering serviced independently from render-frame
  pacing.
- Keep all hot paths O(1), allocation-free, and safe from timer/IRQ context.

## Non-Goals

- Change the hardware mixer voice count.
- Change SoundFont format, sample conversion, or instrument quality.
- Add reverb, chorus, or new synthesis features.
- Change app-level sound priority rules beyond making ownership exact.
- Remove the legacy `int voice` API in this CR.

## Proposed Design

Add a new opaque mixer handle type:

```c
typedef uint64_t of_mixer_handle_t;

#define OF_MIXER_HANDLE_INVALID ((of_mixer_handle_t)0)
```

The handle is intentionally opaque to callers. The recommended internal layout
is:

```text
bits  7:0   physical mixer voice index
bits 63:8   per-slot generation
```

`generation == 0` is reserved, so handle value 0 is always invalid. A 56-bit
generation is effectively non-wrapping for this device and avoids the stale
collision risk that a compact 16-bit or 24-bit generation would still have in
long sessions.

Each mixer slot stores its current generation. Whenever a slot starts a new
logical sound, the generation is incremented before the new handle is returned.
The generation must skip zero.

A handle is valid only if all of these are true:

1. The extracted voice index is in range.
2. The slot is active.
3. The handle generation equals the slot generation.

All handle-safe mutators must no-op on an invalid or stale handle. Queries must
return inactive, `-1`, or another documented failure value.

Group tags remain useful for allocation and priority policy, but they are no
longer treated as ownership proof.

## Validation Helper

The OS implementation should centralize validation so every handle-safe service
has identical stale-handle behavior:

```c
static mixer_voice_t *mixer_validate_handle(uint64_t handle)
{
    uint32_t voice = (uint32_t)(handle & 0xffu);
    uint64_t generation = handle >> 8;

    if (handle == OF_MIXER_HANDLE_INVALID)
        return NULL;
    if (voice >= mixer_max_voices)
        return NULL;
    if (!mixer_voices[voice].active)
        return NULL;
    if (mixer_voices[voice].generation != generation)
        return NULL;
    return &mixer_voices[voice];
}
```

Allocation/reuse should use a matching helper:

```c
static uint64_t mixer_make_handle(uint32_t voice)
{
    const uint64_t gen_mask = (1ULL << 56) - 1;
    uint64_t generation = (mixer_voices[voice].generation + 1) & gen_mask;
    if (generation == 0)
        generation = 1;
    mixer_voices[voice].generation = generation;
    return (generation << 8) | voice;
}
```

The actual implementation can keep different struct names, but the semantics
must match this exactly.

## Service Table Additions

Append new services after the existing mixer extension block. Do not reorder
or change existing entries.

```c
uint64_t (*mixer_play_h)(const uint8_t *pcm_s16,
                         uint32_t sample_count,
                         uint32_t sample_rate,
                         int priority,
                         int volume);

uint64_t (*mixer_play_8bit_h)(const uint8_t *pcm_s8,
                              uint32_t sample_count,
                              uint32_t sample_rate,
                              int priority,
                              int volume);

uint64_t (*mixer_alloc_for_group_h)(int group,
                                    const uint8_t *pcm_s16,
                                    uint32_t sample_count,
                                    uint32_t sample_rate,
                                    int priority,
                                    int volume);

uint64_t (*mixer_retrigger_h)(uint64_t handle,
                              const uint8_t *pcm_s16,
                              uint32_t sample_count,
                              uint32_t sample_rate,
                              int volume);

void     (*mixer_stop_h)(uint64_t handle);
int      (*mixer_handle_active)(uint64_t handle);
int      (*mixer_handle_group)(uint64_t handle);
int      (*mixer_handle_voice)(uint64_t handle);

void     (*mixer_set_volume_h)(uint64_t handle, int volume);
void     (*mixer_set_pan_h)(uint64_t handle, int pan);
void     (*mixer_set_loop_h)(uint64_t handle,
                             int loop_start,
                             int loop_end);
void     (*mixer_set_rate_h)(uint64_t handle, int sample_rate_hz);
void     (*mixer_set_rate_raw_h)(uint64_t handle, uint32_t rate_fp16);
void     (*mixer_set_vol_lr_h)(uint64_t handle, int vol_l, int vol_r);
void     (*mixer_set_bidi_h)(uint64_t handle, int enable);
int      (*mixer_get_position_h)(uint64_t handle);
void     (*mixer_set_position_h)(uint64_t handle, int sample_offset);
void     (*mixer_set_voice_h)(uint64_t handle,
                              int sample_rate_hz,
                              int vol_l,
                              int vol_r);
void     (*mixer_set_voice_raw_h)(uint64_t handle,
                                  uint32_t rate_fp16,
                                  int vol_l,
                                  int vol_r);
void     (*mixer_set_vol_rate_h)(uint64_t handle, int rate);
void     (*mixer_set_filter_h)(uint64_t handle,
                               int cutoff_q016,
                               int q,
                               int enable);

uint32_t (*mixer_poll_ended_h)(uint64_t *out_handles,
                               uint32_t max_handles);
void     (*mixer_set_end_callback_h)(void (*cb)(uint64_t handle));

int      (*mixer_set_pump_mode)(int mode,
                                uint32_t target_buffer_us,
                                uint32_t max_work_us);
void     (*mixer_get_pump_stats)(void *out_stats,
                                 uint32_t out_stats_size,
                                 int reset);
```

Return `OF_MIXER_HANDLE_INVALID` on allocation failure. `mixer_retrigger_h`
starts a new logical sound in the same physical slot if the input handle is
still valid, increments the slot generation, and returns the new handle. This
prevents an old retrigger owner from accidentally retaining authority over the
new sound forever.

`mixer_handle_voice()` is for diagnostics and low-level interop only. Callers
that need stale safety must not use the returned slot with legacy mutators.

## SDK API

Expose the services through `of_mixer.h`:

```c
typedef uint64_t of_mixer_handle_t;

static inline of_mixer_handle_t
of_mixer_play_h(const uint8_t *pcm_s16,
                uint32_t sample_count,
                uint32_t sample_rate,
                int priority,
                int volume);

static inline of_mixer_handle_t
of_mixer_alloc_for_group_h(int group,
                           const uint8_t *pcm_s16,
                           uint32_t sample_count,
                           uint32_t sample_rate,
                           int priority,
                           int volume);

static inline void of_mixer_stop_h(of_mixer_handle_t handle);
static inline int  of_mixer_handle_active(of_mixer_handle_t handle);
static inline void of_mixer_set_voice_raw_h(of_mixer_handle_t handle,
                                            uint32_t rate_fp16,
                                            int vol_l,
                                            int vol_r);
```

The header should provide wrappers for every service listed above.

Do not silently emulate stable handles with raw voice indexes on old firmware.
If a handle service is absent, the handle wrapper should return
`OF_MIXER_HANDLE_INVALID` or no-op. Ports that need to support old firmware can
explicitly choose their legacy fallback path.

## Mixer Slot Semantics

Allocation must be atomic from the caller's point of view:

1. Choose or steal a slot.
2. Increment that slot's generation.
3. Configure sample address, length, rate, loop, volume, priority, and group.
4. Mark the slot active.
5. Return `generation:index` as the handle.

No callback or poll result should observe a partially configured active voice.
On a single CPU, disabling mixer/timer interrupts across the metadata update
and hardware register sequence is sufficient.

Natural voice end:

- capture the current handle in the ended queue
- mark the slot inactive
- leave generation unchanged until the next logical sound starts

Explicit stop through a valid handle:

- stop the slot if the handle still matches
- mark the slot inactive
- do not enqueue a natural-ended event unless the legacy API already promises
  stop events

`mixer_stop_all()` should invalidate all active handles by stopping the slots
and incrementing each stopped slot generation. This avoids old handles
surviving global teardown/init cycles.

## Completion Events

The existing `mixer_poll_ended()` returns a bitmask of physical slots. Keep it
for legacy callers.

Add a handle-based ended queue:

```c
uint32_t of_mixer_poll_ended_h(of_mixer_handle_t *out_handles,
                               uint32_t max_handles);
```

The OS keeps a fixed-size ring of ended handles. Polling copies and consumes up
to `max_handles` entries and returns the number copied. If `out_handles == NULL`
or `max_handles == 0`, return the number currently pending without consuming.

If the ring overflows, drop the oldest ended events and increment a stats
counter. This keeps ISR behavior bounded and makes overflow visible through
`of_mixer_get_pump_stats()`.

Callbacks use the logical handle:

```c
void of_mixer_set_end_callback_h(void (*cb)(of_mixer_handle_t handle));
```

The callback runs in kernel/timer context and must stay short.

## Autonomous Pump

Add a pump mode:

```c
#define OF_MIXER_PUMP_MANUAL 0
#define OF_MIXER_PUMP_AUTO   1
```

`of_mixer_set_pump_mode(OF_MIXER_PUMP_AUTO, target_buffer_us, max_work_us)`
asks the OS/SDK audio layer to keep the audio FIFO/ring filled to approximately
`target_buffer_us` without requiring per-frame app calls.

Recommended defaults:

```text
target_buffer_us = 32000   // about 32 ms of audio
max_work_us      = 2000    // bound one service burst
```

The implementation should:

- service audio from a timer, audio low-water event, or scheduler hook that is
  independent from render frames
- fill only up to the target buffer level
- cap work per service burst with `max_work_us`
- keep old `of_mixer_pump()` valid as a manual assist or compatibility no-op
  when auto mode is enabled
- expose underruns, max pump gap, max pump work, and budget overruns in stats

The MIDI/sample-voice layer should use the same stable timing source for
envelopes and event dispatch. If MIDI work exceeds the per-burst budget, queued
events may be delayed, but already-buffered audio must not underrun because the
renderer had a long frame.

Suggested stats structure:

```c
typedef struct of_mixer_pump_stats_t {
    uint32_t pump_count;
    uint32_t pump_gap_max_us;
    uint32_t pump_work_max_us;
    uint32_t budget_exceeded;
    uint32_t underruns;
    uint32_t fifo_low_water_samples;
    uint32_t active_voice_peak;
    uint32_t ended_queue_overflows;
} of_mixer_pump_stats_t;
```

`mixer_get_pump_stats(out, sizeof(*out), reset)` copies the fields supported by
the running firmware. Extra fields must default to zero so the struct can grow.

## Compatibility

- Existing `int voice` services remain ABI-stable.
- New SDK wrappers probe for non-NULL service pointers.
- Handle wrappers must not fabricate safe handles on old firmware.
- PC backend should implement the same handle semantics so tests can run on the
  host.
- Existing group APIs stay useful for volume and stealing policy.

## Migration Plan

1. Add OS mixer generation metadata, handle validation helpers, and handle
   services.
2. Add SDK wrappers and PC backend implementations.
3. Migrate `of_smp_voice` from `int mixer_voice` to `of_mixer_handle_t`.
4. Migrate SDL_mixer shims and game ports to store handles per channel.
5. Enable auto pump mode by default for ports that use MIDI/sample playback.
6. Keep legacy group-aware guards only as fallback code for old firmware.

## Testing

### Stale handle cannot stop a newer sound

1. Play a short SFX and keep its handle.
2. Let it end.
3. Force the same physical slot to be reused by another SFX.
4. Call `of_mixer_stop_h(old_handle)`.
5. The newer SFX must keep playing.

Repeat this test with two sounds in the same group. This is the case group tags
cannot solve.

### Stale handle cannot update parameters

After reuse of the same physical slot, calls to `set_vol_lr_h`, `set_rate_h`,
`set_loop_h`, and `set_voice_raw_h` with the old handle must not change the new
voice.

### Completion events identify logical sounds

Play two short sounds that reuse the same physical slot. Poll ended handles and
verify that the returned handles match the logical sounds that ended, not only
the slot number.

### Legacy API compatibility

Existing apps using `of_mixer_play()` and `of_mixer_stop(int voice)` must still
build and behave as before.

### Render stall audio test

With auto pump enabled, start MIDI plus repeating SFX, then block the main
render loop for 100 ms and 250 ms. The timer/audio service should continue to
run while the app is blocked, stats should show no underrun, and note envelopes
must continue at the configured timing cadence. Any budget miss must be visible
in the pump stats instead of being hidden as a random note cutoff.

### Port regression test

In Doom E1M3, repeatedly trigger doors, lifts, and monsters while music is
playing and while the renderer is under load. SFX should not randomly disappear
or cut off because of stale channel references.

## Risks

- 64-bit handle returns add a small ABI cost on RV32. The cost is acceptable
  because allocation is not the per-sample hot path, and mutator validation is
  just index extraction plus one generation compare.
- Auto pump mode can steal CPU time from rendering if `target_buffer_us` or
  `max_work_us` is too aggressive. Defaults should be conservative and stats
  must make overruns visible.
- Apps that silently fall back from handle APIs to raw voice indexes will
  reintroduce stale-handle bugs. The SDK should make this an explicit choice,
  not an automatic behavior.

## Acceptance Criteria

- No handle-safe mutator can affect a reused slot through an old handle.
- Same-group stale references are rejected.
- Handle-ended events survive slot reuse and identify the correct logical
  sound.
- Doom and Duke can migrate their per-channel SFX state to handles without
  app-side ownership heuristics.
- MIDI/sample playback continues smoothly through long render frames when auto
  pump is enabled, within the configured buffer target.
- The legacy voice-index API remains source and ABI compatible.
