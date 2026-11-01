<!-- SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception -->
# Endpoint mute ownership and independent watchdog

## Integration

- `EndpointMuteGuard.h/.cpp` are Qt-free. Construct, initialize, call and destroy
  on one COM-initialized control worker, before its `CoUninitialize`.
- `initialize()` resolves default **eRender/eConsole exactly once**;
  `initialize(exactEndpointId)` never resolves the default. `endpointId()` returns
  the bound ID, suitable for binding the associated capture stream.
- `update(false)` acquires mute; `update(true)` / `restore()` release only our
  mute. Call `restore()` explicitly to inspect its HRESULT; destructor retries
  best effort. None of these methods change scalar volume or default devices.
- The original mute state is read once. An originally-muted endpoint is never
  unmuted. If we acquired mute from an originally-unmuted endpoint, toggling
  playback on restores false, and toggling it off can acquire again.
- Any notification with a context other than the guard's unique GUID permanently
  forfeits ownership for that instance, including external volume-only or
  same-value notifications. `S_FALSE` reports ownership forfeited; no repeated
  forcing, reacquisition or restoration can override that external action.
- Register-before-snapshot and a sticky atomic flag avoid clearing external
  notifications during initialization or synchronous own `SetMute`. A permanent
  GUID identifies both synchronous and late own notifications. The callback owns
  its own reference-counted state and never points at the guard. `OnNotify` only
  compares GUIDs and sets a lock-free atomic, with no I/O, locks or COM calls.

WASAPI supplies no atomic compare-and-set. An external change in the final
`GetMute` -> `SetMute` window cannot be made fully atomic: a notification can
prevent subsequent writes, but cannot cancel a call already entered. Also,
Windows need not notify a no-op same-value write; an action for which Windows
emits no notification and leaves no observable state change cannot be detected.

## Parent-crash recovery

Build **deskflow-audio-mute-watchdog.exe** from `MuteWatchdogMain.cpp` and
`EndpointMuteGuard.cpp`, linking `ole32.lib` and `uuid.lib`, and deploy next to the
main executable. Build `MuteWatchdog.cpp` into the parent; it needs only Windows
kernel APIs and no COM/Qt initialization. All sources require C++20.

```cpp
MuteWatchdog watchdog;
HRESULT hr = watchdog.start(exactEndpointId); // initialize child, no mute yet
if (SUCCEEDED(hr))
  hr = watchdog.update(false); // acquire only after transport/capture ready
// Every control tick (e.g. 100 ms):
hr = watchdog.update(localPlayback);
// Before disconnect/stop, observe restoration result:
hr = watchdog.stop();
```

The child owns the actual guard. Parent wrapper uses `CreateProcessW` with an
absolute application path and explicit `STARTUPINFOEX` inherited-handle allowlist:
command/read anonymous pipes and a duplicated actual parent process handle with
only `SYNCHRONIZE` permission. The command line contains only those three numeric
handles. The exact endpoint ID is transmitted through the private pipe, with a
4096-character bound. There are no named objects, shell commands, parent PID
reopens or publicly addressable control IPC. Dynamic changes receive a versioned
HRESULT acknowledgment with a 3-second parent-side timeout. Every tick gets a
fresh response so external ownership loss is reported even if the checkbox did
not change. The guard itself avoids repeated `SetMute` calls.

The helper polls pipe readability while waiting on the inherited parent process
handle, checking parent exit within a normal 10 ms polling interval. Parent exit
or pipe EOF leads to explicit restoration and guard destruction in its initialized
COM apartment. On timeout the parent closes pipes; it never kills the only process
capable of restoration. A temporarily blocked COM driver can delay cleanup.

This protects against **parent process termination**, not every possible failure.
Killing/crashing the helper itself, OS/power loss, endpoint removal or failing
WASAPI calls can still leave mute set. It intentionally makes no unconditional
“crash safe” guarantee and does not persist a mute journal that might later
incorrectly overwrite user changes. A successful HRESULT reports completion;
restoration failure must be surfaced by the caller rather than treated as proof
of unmute.

## Standalone validation (no audio device writes)

Run `tests\build-mute-guard-tests.cmd` from any working directory. It discovers the
installed Visual Studio x64 toolchain, compiles `/std:c++20 /W4 /WX /permissive-`,
runs fake-endpoint tests, builds the helper, and compiles the parent wrapper.
Outputs are under ignored `build/endpoint-mute-guard`.

The deterministic tests cover: original false/true, repeat ticks, checkbox
release/reacquire, external mute/unmute/same-state callbacks, synchronous and late
own callbacks, external notification inside `SetMute`, sticky loss despite late
own events, GetMute/SetMute HRESULT failures, retryable restore failure, external
notification during GetMute, missed-notification observed-state safety, wrong
thread rejection, destructor restoration and callback lifetime after destruction.
They do not open a real endpoint or claim to verify parent-kill hardware recovery.
