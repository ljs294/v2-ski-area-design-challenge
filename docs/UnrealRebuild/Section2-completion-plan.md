# Section 2 completion plan — original P1 on Medium terrain

Status: **not accepted** (audit dated 2026-09-23). This is an execution plan, not a
claim that the current worktree or any older package is release-ready. Section 2
ends only when the same frozen Shipping build passes the security, Medium-product,
live-source, visual, performance, offline-reopen, and owner-review gates below.

## Audit findings

1. **Selector security is the first blocking gate.** The unchanged Shipping
   selector audit in `test-results/p1/runs/20260923T161841.669597Z-71afeabb`
   failed on a process-attributed CEF-helper TCP endpoint outside the OSM tile
   allow-list. The isolated `about:blank`, no-tile, and full-map runs under
   `test-results/p1/selector-diagnostic/11a4a3e1-a568-42de-a700-420bd335a846`
   reproduced it without MapLibre tiles. Fresh browser profiles separately
   recorded `accounts.google.com`; the TCP audit does not itself resolve hostnames.
   CEF-exposed request handlers, identity/DNS preferences, and browser/child
   resolver-rule experiments did not establish an egress boundary. A subsequent
   packaged proxy/native-tile experiment loaded two actual OSM tiles but still
   contacted a Comcast DNS-over-HTTPS endpoint after about six seconds
   (`test-results/p1/runs/20260923T185139.038595Z-135966a4`). A helper-wrapper
   probe broke CEF child startup; the original helper was restored. None of
   these experimental paths remains in the production selector.
2. **Receipts do not describe one candidate release.** The latest Passing focused
   P1/UI tests, Shipping package, and older Medium smoke have different source
   digests. The last Shipping UI-layout and performance receipts failed; the
   Shipping visual receipt is absent. The Medium smoke passed on a synthetic
   fixture, not on a live mountain. The release freeze is stale for the dirty
   worktree. Treat all of these as diagnostic history, not Section 2 acceptance.
3. **The product path still needs integrated proof.** Worktree code adds an
   analytical WorldCover class COG, required CoverEcology, surrounding elevation,
   a cumulative edit sidecar, normal reopen, and viewer controls. It is not yet
   proven that live transforms/validity/provenance survive installation and
   offline reopen, that edits restore through the normal UI, or that all
   canonical/render/query revisions remain aligned after failures and LOD changes.
   Current contour generation samples a coarsened whole heightfield and pairs
   marching-square crossings by encounter order; ambiguous saddles, nodata
   boundaries, streaming cost, and canonical-triangle alignment need tests and,
   where necessary, correction.
4. **Qualification is incomplete.** Current visual capture uses a synthetic
   package only. The performance scenario is synthetic and its latest receipt
   failed renderer-path proof. It does not establish the required cold/warm real
   10 km envelope. No post-cook Crystal and Mount Washington Medium pair, complete
   network-denied reopen, or owner acceptance is recorded for a single frozen
   build.

## Execution sequence

### A. Make the selector's network boundary enforceable

Keep the embedded local MapLibre selector, bounds-only token/generation bridge,
existing shell, and zero-unapproved-CEF-endpoint Shipping rule. Do not add Google
to the allow-list or reclassify a CEF background connection as a map-tile request.

1. Preserve the dirty worktree and inventory its ownership. Reproduce the
   `about:blank` failure in an isolated package while recording the CEF process
   tree, effective browser/child command lines, proxy configuration, endpoint,
   and timing. Capture no cookies, URLs with query strings, bodies, or credentials.
2. The application-level CEF proxy feasibility proof **failed**: it allowed
   map rendering while the helper later bypassed it through secure DNS. Do not
   repeat this approach without a demonstrated process-level enforcement
   mechanism. The next proof must contain the browser and every CEF child for
   longer than the observed late connection, include deliberate forbidden-request
   and map-tile controls, and pass the unchanged process-attributed TCP audit.
3. Only if that proof passes, implement a project-local selector adapter. Route CEF through
   a loopback-only, default-deny tile service; validate exact OSM `z/x/y.png`
   requests, bounds, redirects, method, response size, and content type in native
   code. The native provider may fetch only approved tiles; the browser receives
   no general Internet route. Install policy before creating CEF, and fail closed
   if the proxy/service cannot start. The owner approved a project-local copy or
   patch of Epic's integration for this purpose; do not edit the installed engine
   or change global settings.
4. If step 2 fails, do **not** keep trying feature flags. The next candidate is a
   project-owned browser process under OS-enforced per-process network isolation,
   with native tile delivery and no Internet capability. First prove packaged
   WebGL, local assets, input, IPC, child-process containment, and zero external
   endpoints. The owner approved a project-owned internal helper process, not a
   second user-facing application. If it cannot satisfy the existing audit, stop and present
   the evidence and architecture choices; do not weaken the gate silently.
   The first bounded helper proof on 2026-09-23 stopped here: standalone CEF
   rendered MapLibre/WebGL, but CEF in a no-network-capability AppContainer
   terminated on a Chromium `platform_channel.cc(76)` access-denied fatal,
   including with `about:blank` and a container-local cache. The separate
   socket probe timed out and did not establish network denial. See
   `Tools/SelectorSandbox/README.md`. No packaged containment proof exists.
5. With an enforcement candidate, test selecting, retry, Change Selection,
   navigation, redirect, download, auth prompt, popup, close, and terrain entry.
   Require real WebGL and selection, denied prohibited actions, stale callbacks
   unable to install, complete asynchronous browser teardown before gameplay,
   and zero unapproved CEF endpoints in the unchanged process-attributed Shipping
   audit. Run the same check after offline reopen to prove no browser profile or
   browser-owned connection exists there.

### B. Close the Medium implementation gaps

1. Complete focused tests for requested/returned bounds, native/delivered
   spacing, datum, analytical WorldCover categorical transform and validity,
   required-source activation, and explicit NAIP/vector absence reasons. Test
   uneven and nodata-heavy live-shaped rasters. Surrounding elevation must be
   labeled context and must never change canonical core queries. Replace any
   approximate geodetic placement with the domain's WGS84 ECEF-to-ENU transform.
2. Make contour and cover-display derivation bounded per tile or visible region.
   Resolve saddle cases deterministically against the canonical cell split,
   preserve nodata gaps, share seam endpoints, and assert segment/CPU budgets.
   Avoid constructing a full-detail whole-mountain overlay to prove a streamed
   renderer.
3. Exercise the scratch edit through the visible right-side action at the last
   valid probe. Publish the cumulative edit and active-edit reference atomically
   only after render/query readiness; stale or failed work must preserve the
   previous ready revision. Normal **Open last installed terrain** must restore
   the edit without a command-line ID. Verify base hashes are unchanged.
4. Finish and test all presentation and input modes: full, 95th-percentile
   steepest quadrant, and probe camera; presentation/elevation/slope/cover/LOD/
   topology; three lights; Auto/fixed LOD; labeled 1×/2×/4× scale; unobstructed
   camera framing; panel scrolling and pointer isolation; batched overlays and
   exactly 3,000 markers without per-marker Actors.

### C. Freeze, qualify, and release one candidate

1. Freeze one source digest after the fixes. Run exact named focused selector,
   cover/transform, contour, edit/reference, query, camera/input, UI, TIFF,
   acquisition, TerrainCore, and P1 tests. Run full Unreal automation because
   package activation, renderer, browser, and release contracts cross modules.
2. Build Shipping once from that digest. Run selector, UI-layout,
   geotiff/acquisition, terraincore, and Medium import→probe→edit→LOD/mode→close→
   separate-process offline-reopen smokes. Receipts must bind executable hash,
   package IDs/hashes, fixture or live identity, actual metadata, revisions, and
   network audit. Do not let a synthetic success satisfy a live-source gate.
3. In the built executable, prepare fresh Crystal Mountain 2 km Medium and Mount
   Washington, NH 10 km Medium. Reopen both offline with acquisition denied;
   verify cover validity, source provenance, spacing/datum, edit persistence,
   query alignment, browser absence, and unchanged immutable base hashes.
4. Capture each real package's full and 95th-percentile steepest-quadrant views
   under all three lights, plus elevation/slope/cover/LOD/topology and probe views
   at 2560×1440, 100% internal resolution, 1× scale; capture the ultrawide UI
   proof at 2560×1080. Keep synthetic and real receipts separate and have the
   owner judge the actual editable packaged terrain.
5. Measure cold and warm runs on the declared Low and reference machines using
   the real 10 km package with visible rendered tiles. Enforce p95 33.3/20 ms,
   p99 50/33.3 ms, ordinary maximum gap 250 ms, cancellation acknowledgment
   250 ms, and reopen 30 s. Record all >50/100/250 ms gaps, first-use costs,
   hardware, RHI, resolution, memory, cache state, and preparation duration.
6. Obtain fresh non-author Sol High review of the frozen diff and receipts; fix
   critical/high findings. Update the requirement matrix row by row, assemble
   the self-contained tester bundle only from fresh passing receipts, then seek
   owner Section 2 acceptance. Staging/committing follows the current repository
   rule: only the user stages and commits.

## Decision and stop rules

- The proxy and OS-isolation approaches are **candidates**, not proven fixes.
  Time-box each at its packaged negative-control and TCP-audit proof before
  investing in a complete browser adapter.
- The selector audit remains zero unapproved CEF hosts/endpoints; local bundle
  and exact map tiles are the only allowed selector resources. No global Windows,
  Unreal, or Codex configuration change, no engine fork, no replacement picker,
  and no silent allow-list expansion.
- An older passing fixture, Editor screenshot, or mixed-digest receipt cannot
  close a Shipping/live/owner gate. If a required live source or hardware lane is
  unavailable, report that acceptance as open instead of substituting a fixture.
- P1A verified lidar and P2A cover refinement remain outside Section 2.

## Sources for the proposed enforcement probes

- [CEF proxy configuration and process callbacks](https://chromiumembedded.github.io/cef/general_usage)
- [Microsoft AppContainer network isolation](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)
