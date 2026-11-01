<!-- SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception -->
# Native Windows WASAPI backend

`WasapiBackend.h` is preserved without interface changes. This implementation is
standalone C++20; it does not depend on Qt, a codec, the DSP implementation, or a
third-party audio library. It can still be built independently; the parent audio prototype now also integrates it behind a default-OFF CMake option.

## Integration / linking

Compile `WasapiBackend.cpp` on Windows only, with C++20, exceptions enabled and a
Windows 10 SDK containing `IAudioClient3`. Link **ole32.lib, uuid.lib, avrt.lib**.
MSVC example options: `/std:c++20 /EHsc /D_WIN32_WINNT=0x0A00`.
The implementation includes Windows headers privately; the public header remains
platform-header-free. `WasapiPcm.h` is a backend-private adapter, not a DSP API.
No dependencies need downloading. The independent test script discovers an
already-installed VS C++ toolchain with `vswhere`; it also works from an existing
x64 Native Tools Command Prompt.

From the repository root:

```bat
src\lib\audio\windows\tests\build-and-test.cmd
src\lib\audio\windows\tests\build-and-test.cmd --smoke
```

Output goes to the ignored `build/wasapi-backend` directory. The default command
runs deterministic format/conversion/period tests without opening an endpoint.
`--smoke` additionally opens shared render/loopback streams, requests a restart,
checks stop/start and no-device retry, reads (never writes) endpoint volume in a
caller-owned STA, and checks handle counts across create/stop stress. Render
callbacks leave silence. It never selects a microphone, changes the default
endpoint, unmutes the endpoint, or writes volume. Concurrent ambient desktop
playback can, naturally, appear in the loopback callbacks.

## Runtime contract

- Each instance owns one MTA worker. All WASAPI interfaces are activated, used,
  stopped and released on that worker before `CoUninitialize`. Volume helpers
  establish their own apartment when necessary; an existing caller STA is also
  accepted and is never uninitialized by the helper.
- Both directions resolve **default eRender/eConsole**. Capture sets
  `AUDCLNT_STREAMFLAGS_LOOPBACK`; render is always shared, never exclusive.
- Render queries `IAudioClient3::GetSharedModeEnginePeriod`, rounds the minimum
  to a fundamental-period multiple, and tries the shortest legal period. If
  probing, initialization, or event registration fails, it activates a **fresh**
  `IAudioClient` and falls back to ordinary shared/event initialization, then a
  fresh shared timer-driven stream if necessary. Driver/engine policy can
  prevent low-latency periods; no fixed end-to-end latency is promised.
- The documented supported flag for `InitializeSharedAudioStream` is
  `EVENTCALLBACK`, not `LOOPBACK`. Loopback therefore uses legacy shared
  `IAudioClient::Initialize(LOOPBACK | EVENTCALLBACK)`, with shared timer fallback.
  Bounded 1–10 ms wait timeouts drain packets even if a pre-Windows-10-1703
  loopback event is never signalled. Loopback also owns a shared render stream on
  the same IMMDevice, replenished only with `AUDCLNT_BUFFERFLAGS_SILENT` buffers.
  This keeps the Windows audio engine producing capture PCM while applications
  are idle; both clients stop and release together on the same worker.
  OS scheduler timer resolution can make this fallback less precise; this code
  does not change the global timer resolution.
- `GetMixFormat` is interpreted as float32, signed PCM16 or signed PCM32, either
  plain or extensible. Extensible valid bits are respected (including common
  24-valid-bits left-aligned in 32-bit containers); valid-bit padding is cleared
  on render. Packed 24-bit, float64, malformed block alignment/rates/channel
  masks and unsupported formats are rejected with a retry, never reinterpreted.
  Samples are normalized interleaved float, non-finite values become zero, and
  render samples saturate to [-1, 1] before conversion. No sample-rate/channel
  conversion is done by this backend: the callback must obey each generation's
  exact endpoint format.
- Capture drains available packets and propagates silent/discontinuity/timestamp
  metadata. `SILENT` packets are zero-filled **without touching their borrowed
  data pointer**, which is allowed to be null. The silent render stream sustains
  idle capture; timestamps and PCM still come from WASAPI capture packets rather
  than a second application clock or synthesized capture callbacks.
- Scratch storage is allocated before `Start` from the current `GetBufferSize`.
  Format/capacity and scratch are rebuilt for every stream recreation. Unexpected
  packet sizes/padding beyond capacity cause teardown/retry; the audio loop
  never grows storage or writes past it. Generation increases when a newly
  prepared stream is about to run and is retained across stop/start.
- Audio callbacks must not allocate, lock, log, encode, do network I/O, wait, or
  call lifecycle methods. The backend's own active loop does none of those;
  WASAPI calls and bounded kernel waits are the necessary OS operations. All
  capture `GetBuffer` calls are paired with `ReleaseBuffer` before teardown.
  Render callbacks receive a zero-filled buffer.
- MMCSS tries `Pro Audio`, then `Audio`, with HIGH priority. Failure is advisory
  (`SchedulingWarning`), not a reason to lose audio. Registration and reversion
  happen outside the active loop. No time-critical process priority is set.
- Status callbacks occur **only outside the active audio loop**: setup statuses
  precede `Start`; `Running` means prepared/about to call `Start`, not proof of a
  completed hardware start. `Start` failure is subsequently reported as
  `Restarting`/`RetryPending`. No periodic status is emitted during streaming.
  After a runtime failure, `Stop`, MMCSS reversion and stream destruction precede
  restart/retry callbacks. Status callbacks run on the worker and must return
  promptly, never stop/destroy the backend.
- Endpoint notifications signal a restart event only. Default eRender/eConsole
  changes and device add/remove/state changes restart; endpoint device/OEM format
  properties restart, but volume/friendly-name notifications do not. Device
  add/remove/state and format notifications are deliberately conservatively
  coalesced, so an unrelated device's change can also cause a brief restart.
  Notification sinks own a duplicated event handle, no backend pointer, making
  late notifications safe even during worker shutdown.
- Device-open/runtime errors wait up to one second and retry indefinitely, with
  notification/restart events waking the retry immediately. Thus unplugging all
  devices does not terminate the backend. COM/notification setup failure is
  terminal `Failed`; call `stop` before trying `start` again. `start()` only reports
  thread/event startup success, not audio endpoint success.
- `stop()` signals and joins cooperatively; it never detaches or kills a thread.
  Pending retry/audio waits wake on stop. A driver COM call or a user callback
  that blocks cannot be forcibly cancelled, so join has no hard wall-clock bound.
  Lifecycle calls (including `requestRestart` versus start/stop/destruction) must
  be externally serialized as specified by the header. Event handles remain
  valid until re-start/destruction, and WASAPI is released before closing its
  event. Normal stop emits `Stopped`; terminal setup/exception emits `Failed`.
- `readDefaultMasterVolume` commits both outputs only on complete success.
  `setDefaultMasterVolume` rejects NaN/Inf and out-of-range input, changes only
  scalar volume, and never toggles mute. Each call resolves the current default
  render endpoint afresh. Call these helpers only outside real-time callbacks.

## Local verification result

Verified on 2026-11-01 with installed VS 2022 MSVC 19.44.35207, x64, Windows SDK
10.0.26100.0. `/std:c++20 /W4 /WX /permissive-` compilation and the standalone
`--smoke` suite passed with **0 failures**. Both real default-endpoint directions
opened three generations across restart/stop/start; render and loopback each
received 29 callbacks in this run. Volume read returned `S_OK`. Twelve additional
create/restart/stop cycles left the process handle count unchanged (196 → 196).
MSVC `/analyze` also passed; its XML report contains an empty `DEFECTS` element.
These counts are runtime-dependent, not performance guarantees.

Local artifacts (ignored, not source deliverables):

- `build/wasapi-backend/smoke-results.txt`
- `build/wasapi-backend/analysis-results.txt`
- `build/wasapi-backend/WasapiBackend.nativecodeanalysis.xml`

## Verification scope / remaining hardware checks

The deterministic suite covers supported/rejected layouts, malformed extensible
formats, valid-bit conversion/padding, signed full-scale saturation, NaN/Inf,
silent/null packets and period rounding/overflow. The optional smoke suite
exercises actual local endpoint availability rather than mocking WASAPI.

Manual validation still needed on representative Windows hardware:

1. Play an identifiable signal and confirm loopback waveform, channel ordering,
   timestamp/discontinuity flags, silence transitions and measured latency.
2. Switch the default output while active; unplug all endpoints, wait through
   repeated retries, then reconnect. Check changed sample rate, channel count,
   mask and buffer size under the new generation.
3. Sleep/resume and explicitly `requestRestart` from the application's power
   event handler; verify no callbacks occur after `stop` returns.
4. Exercise real PCM16/PCM32 endpoints, low-latency render support/rejection,
   exclusive-device contention and old/no-event loopback drivers. The tests do
   not inject every Windows COM API failure or emulate these driver branches.
5. Validate legitimate volume writes interactively (tests intentionally do not
   change system volume), including muted devices and default-device races.

Protected/DRM content can be absent from loopback, and loopback may include audio
from other Windows sessions; application consent/routing and feedback-loop
prevention belong to the higher-level manager. This backend does not supply a
session/privacy policy or resampling/mixing/encoding/networking.

References used:

- [Loopback recording and pre-1703 event behavior](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [InitializeSharedAudioStream supported flags](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)
- [GetSharedModeEnginePeriod legality constraints](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod)
