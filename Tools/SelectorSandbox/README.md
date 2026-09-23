# Selector AppContainer feasibility probe

This is a diagnostic prototype, not a Shipping selector or release gate. It
leaves the game's existing browser path unchanged. The sources build against
the CEF 128 SDK already installed with Unreal 5.8; build outputs belong under
ignored `test-results/p1/selector-sandbox-build/`.

On 2026-09-23, the standalone CEF probe loaded the local MapLibre selector
with tiles disabled outside AppContainer and reported `cef_webgl=1` and three
paint callbacks. The same executable launched in a no-network-capability
AppContainer exited with Windows status `0x80000003`. Its container-local CEF
log recorded `FATAL:platform_channel.cc(76) ... Access is denied. (0x5)`.
The `about:blank` control, with the cache moved into the container profile,
failed with the same status and fatal log line. CEF therefore did not reach
the page or WebGL proof inside the proposed containment boundary.
The captured blank-page Chromium log is under ignored
`test-results/p1/selector-sandbox-evidence/blank-appcontainer-chrome-debug.log`.

The separate AppContainer socket probe confirmed that the child ran with an
AppContainer token, but its nonblocking connection timed out (`exit=25`);
that result alone does not prove OS network denial. No packaged selector or
long-duration egress test passed. Do not infer release compliance from this
prototype. The per-user probe profile can be removed with the executable's
`--cleanup` option after preserving the result.
