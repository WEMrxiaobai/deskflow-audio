<!-- SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception -->
# Independent audio review notes — 2026-11-01

## Scope and verification limits

Independent review of the new developer probe, audio engine lifetime/concurrency and bounded-delay behavior, and the DSP ring/jitter interaction. No P0 research was repeated. The initially requested read-only review was followed by explicit parent-agent authorization to add the isolated network-clock module and simulation, then this note. Existing engine/probe/DSP files were not edited by this reviewer. Other agents were concurrently updating tests/statistics and implementing the fixes below.

The existing in-progress CMake/build jobs were not rerun. Only the newly added standalone network-clock simulation was compiled and executed independently. This is not an end-to-end physical latency or real-hardware clock-drift result.

Line locations below refer to the code observed during review and may move as parallel fixes are integrated.

## Findings, ordered by impact

### 1. High: exceptional probe cleanup could destroy a live sink dependency — fixed by parent

Originally `src/apps/deskflow-audio/main.cpp` declared sender, receiver and backend before `UdpLoop udp` (original lines 96–115), then started an encoding worker whose packet sink captured `udp` by reference (126–127). Exceptions after worker startup unwound `udp` first, while the worker could still call `sendPacket()`; `close()` also wrote the socket members concurrently with the worker reading them. This was both a lifetime violation and an unsynchronized socket-member access, not fixed by atomically clearing the device context pointers.

The parent moved UDP ownership before the engine/backend owners. Reverse destruction now stops/joins the backend, destroys the receiver, stops/joins the sender, and only then destroys UDP. The sender sink does not capture the receiver. This satisfies both the device callback and packet sink dependencies on exceptional exit. General guidance: retain this dependency order or use a unified RAII shutdown guard, not pointer nulling alone.

### 2. High: fixed network deadlines eventually lose a valid drifting source — PLL integrated and regression-tested

Originally `JitterBuffer.h` advanced its deadline by a fixed nominal 5/10ms per sequence, and rejected packets at/after their computed deadlines. `AudioEngine.cpp` adjusted only decoded-PCM resampling using playback ring occupancy. That adjustment cannot correct the independent source-media-clock to receiver-monotonic-clock mapping used for compressed-packet admission.

For a source packet period 100ppm longer than nominal, phase error grows by 0.1ms/s. A 5ms initial margin is consumed after approximately 50s; subsequent authenticated packets are continuously late. The opposite frequency error eventually exceeds the eight-packet forward window. Finite ring capacity does not solve this timeline disagreement.

Added `src/lib/audio/dsp/NetworkClock.h` and `src/lib/audio/tests/NetworkClockTests.cpp`, with parent-authorized integration APIs supplied separately in jitter/engine. The decoder-worker-owned PLL observes authenticated, forward-sequence packets before insertion, including packets which might be rejected as late/out-of-window. Duplicate or reordered observations cannot steer it. Its API is:

```cpp
NetworkClock clock(nominalPacketDurationNs, startupDelayNs);
clock.reset(); // at every initial or explicitly trusted anchor
const auto duration = clock.observe(sequence, arrivalNs, jitter.deadlineFor(sequence));
(void)jitter.setPacketDurationNs(duration);
```

The PLL changes future deadline increments, never abruptly moves the currently pending deadline. Observation time steps are forward sequence delta times nominal packet duration, not noisy arrival-time deltas. Phase error is `(arrival + initial margin) - expected deadline`; a 0.5s low-pass filter feeds proportional/integral gains of 0.5/s and 0.1/s². Integral frequency is clamped to ±1000ppm, total correction to ±2000ppm, phase evidence to ±5ms. Gross phase outliers over 40ms and sequence gaps representing over 1s do not update the frequency estimate and require application-level recovery policy.

Ignoring filter lag, `e_dot ≈ source_frequency_error - correction`, with `correction = Kp*e + Ki*integral(e)`. The positive-gain characteristic polynomial `s² + Kp*s + Ki` is stable; the integral estimates persistent frequency mismatch while the proportional term recovers accumulated phase. A frequency estimate alone, with no phase recovery, would preserve previously accumulated timing error.

Arrival times include network path and scheduling variation. `estimatedClockPpm()` is a bounded media-timeline estimate, **not** a measurement of a physical oscillator. Persistent path-delay changes can bias the estimate; gross delay changes still need trusted reanchor. Playback-device occupancy/resampling remains a separate control loop.

### 3. Medium: asynchronous reanchor flush could erase already-published resumed PCM — explicit drop-until-ack fix

Originally `AudioEngine.cpp` set `flushReader=true` at anchor, then immediately allowed new decoded PCM to be published. A delayed device reader subsequently performed `trimTo(0)`, clearing both stale and newly published media. An atomic boolean carries no old/new ring boundary.

The engine agent changed this to a reader-owned flush acknowledgement: while a flush is pending, the writer deliberately discards new PCM, records `reanchorDiscardedFrames`, and publishes no new ring data; the reader trims first and then release-publishes its acknowledgement. This removes accidental clearing of newly published data but intentionally loses resumed PCM while awaiting the next callback. That is a documented recovery tradeoff, not lossless reanchor. A later lossless design could use an absolute ring boundary or generation-tagged frames.

Suggested regression: queue old PCM, reanchor, decode resumed packets before any device callback, then render. Verify old PCM is cleared, pre-ack new PCM is explicitly counted as discarded, and post-ack new PCM survives.

### 4. Medium: 44.1kHz render construction used fractional millisecond frame counts — fixed by parent/engine agent

Originally receiver construction called `PcmFormat::framesForDuration(1)` for smoothing and `framesForDuration(5)` for drift targeting; both demand exact integer frames. A common 44100Hz device therefore threw during construction (44.1 frames/1ms, 220.5 frames/5ms), although the streaming adapter supports 44.1kHz. The probe also unconditionally allocated a discard buffer using `output.framesForDuration(ms)` even in render mode, introducing another 44.1kHz/5ms failure after fixing the engine.

Fixes use explicitly rounded/ceiling integer frame counts for capacities/smoothing, floating-point frame targets for control feedback, and avoid an unused render-mode discard allocation. Keep exact-duration validation for media packet framing (48kHz) separate from approximate device-side smoothing/buffering durations. Representative construction tests should include 44.1kHz and both packet durations.

### 5. Remaining integration gap: no capture callbacks during endpoint silence needs trusted resume

The WASAPI backend contract explicitly allows an idle loopback endpoint to deliver no capture callbacks. CapturePipeline advances media sequence only when PCM actually arrives; the receiver, however, advances its jitter timeline by monotonic deadlines even while synthesizing PLC/zeros.

After real audio, a multi-second no-callback interval, and resumed audio, the sender can continue from its previous media sequence while the receiver has advanced hundreds of media slots. A capture discontinuity only resets codec/adapter state; it does not authorize receiver reanchor. These authenticated resumed packets can remain late indefinitely. The PLL intentionally must not interpret such an outage as oscillator drift.

A complete application needs a **trusted control-plane resume/reanchor** transition based on capture interruption, arranged before resumed media delivery, or recreation of the authorized stream with fresh key/SSRC where required. It must not allow arbitrary UDP to reset a live timeline. This control-plane integration is **not implemented by this review**. The parent integrated one-second no-forward-media timeout, severe decoder-stall and >40ms forward-media phase guards, and the probe now fails clearly on `requiresReanchor` rather than reporting success merely because some packets decoded earlier. Tests cover the silence gap and explicit trusted resume. A late reordered packet is dropped without triggering a forward-clock guard. These detection/fail-fast behaviors do not themselves implement automatic recovery.

### 6. High: packet-boundary occupancy aliases playback phase — normalized callback model integrated, hardware gate remains open

Subsequent extended validation found that the original 5ms pre-write target, 10ms trim and 20ms capacity could clip healthy 10ms callback bursts and force negative correction saturation. Increasing headroom alone is insufficient: the interim 6ms/25ms/30ms candidate with sample-rate-normalized gains still fails some independently simulated source/device phase cases. A 737-case fractional/event diagnostic reproduces both the old failure and candidate counterexamples. Its success exit means reproduction, NOT candidate acceptance. See [the validation record](audio-sharing-p1-validation.md#6-播放水位反馈的历史失败与相位归一化验证).

Subsequently integrated phase-normalized ring + nominal unplayed callback PCM, with a 12ms pre-write total target, single-attempt atomic snapshot and elapsed-time updates once per timely decoder tick. Invalid/underrun/trim/flush snapshots and catch-up samples cannot steer PI. The 1520-case ideal positive matrix and a 30-case real Opus/SRTP/PLL/FIR/ring virtual-clock matrix now pass their steady windows. This is still a nominal callback-time approximation, NOT actual WASAPI padding/IAudioClock. Subsequent 48-case multi-rate/phase/polling and 16-case bounded jitter/receive-quantization matrices pass the same steady gates; these are covering samples, not a full parameter matrix. Deterministic worker/device-thread interleaving now verifies post-decode clock progress and alternating stale-sample recovery, using feedback update/conflict counters rather than an accidentally constant correction. Empty-output flush and loss cooldown/recovery have regressions. Hardware timing, actual multi-packet delivery batches/reordering, delayed callbacks and broader concurrent stress remain open. See the [updated evidence and limitations](audio-sharing-p1-validation.md#6-播放水位反馈的历史失败与相位归一化验证); keep the feature disabled pending physical gates.

The candidate's 25ms trim at 44.1kHz must use explicit ceiling arithmetic, not exact `framesForDuration(25)` (1102.5 frames): throwing inside the noexcept callback terminates the process. This regression was reproduced, corrected and retested in both Release and Debug.

## Standalone network-clock simulation actually executed

Compiled using installed VS 2022 MSVC through `vcvars64.bat`:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /wd4324 /I src\lib ^
  src\lib\audio\tests\NetworkClockTests.cpp ^
  /Fo:build\NetworkClockTests.obj /Fe:build\NetworkClockTests.exe
build\NetworkClockTests.exe
```

`/wd4324` suppresses the intentional cache-line alignment/padding warning in existing SPSC structures; no other warnings were waived. Initial attempts had command quoting and that expected padding-warning issue; the corrected command above compiled and ran successfully.

- 5ms and 10ms packets.
- Source period errors −1000, −500, −100, 0, +100, +500, +1000ppm.
- Each frequency tested with no noise and stationary ±1ms bounded arrival noise: 28 cases total.
- Each case simulated ten minutes; sequence starts at 65500 and crosses the 16-bit sequence boundary.
- Every case: **zero rejected packets and zero jitter losses**.
- Maximum observed expected deadline minus arrival: approximately **7.56ms**, below the test's 10ms bound.
- Noiseless integral estimate error below 0.1ppm; noisy estimate error below 9ppm in the executed deterministic sample.
- Duplicate/reordered observations do not change PLL output; gross timing-gap observations do not steer frequency; trusted reset clears the estimate.

The isolated simulation validates PLL/jitter interaction against deterministic synthetic source clocks. Subsequently the parent integrated the PLL, ran all six Release/Debug CTest suites and real localhost SRTP probes; current results are recorded in [the implementation report](audio-sharing-implementation.md). This still does not validate real network congestion, physical latency, or long-session hardware behavior.
