# P1 requirement and acceptance matrix

This matrix is the authoritative accounting record for the original P1 scope. It
distinguishes code presence from verified acceptance. `PASS` requires the named
test and packaged receipt from one frozen source digest; `OWNER` additionally
requires owner visual or workflow acceptance. A blank or `OPEN` row is not made
complete by another row passing.

The current Section 0 remediation must be accepted and committed by the owner
before TerrainCore v2, P1A, or P2A implementation starts. The user alone stages
and commits section boundaries.

| ID | Requirement | Implementation | Deterministic test | Packaged evidence | Status |
| --- | --- | --- | --- | --- | --- |
| S0-01 | Tiled and stripped float32 GeoTIFF decoding, bounded metadata and nodata parsing | `SkiPreparation/GeoTiffDecoder` | `p1.py tiff` exact three-test manifest | Shipping `geotiff-regression` | Section 0 gate pending final frozen run |
| S0-02 | Bounded HTTP timeouts, classified retry, High 2x2 acquisition planning and cancellation | `TerrainAcquisition` and native provider | `p1.py acquisition` exact four-test manifest | Shipping `acquisition-regression` drives the production retry, stitch and activation-fence paths | Section 0 gate pending final frozen run |
| S0-03 | No stale progress, journal ownership, or stale atomic activation | operation lease, provider and package store | Acquisition retry/activation tests | Shipping activation-fence regression | Section 0 gate pending final frozen run |
| S0-04 | Actionable redacted diagnostics, interruption journal and tester access | native diagnostics plus application JSONL log | `ProviderDiagnostics` in full P1 automation | abnormal packaged runs copy bounded isolated crash context; tester has `OPEN DIAGNOSTICS.bat` | Section 0 gate pending final frozen run |
| S0-05 | Responsive recovery UI and terrain-input isolation | existing P1 right-side shell and terrain view controller | `p1.py ui` supported viewport set | Shipping `ui-layout` | Section 0 gate pending final frozen run and owner review |
| S0-06 | One immutable source identity across focused gates, cook, Shipping regressions and release | `p1.py` freeze and release assembler | exact automation-name parser tooling tests | receipt/package file hashes and source digest checked at assembly | Section 0 gate pending final release run |
| P1.1-01 | Portable immutable schema-1 package with hostile-input validation | schema-1 domain manifest/package store | `PackageAndProtocol`; domain tests | packaged import/reopen | Implemented; integrated frozen evidence OPEN |
| P1.1-02 | Editable Dynamic Mesh terrain tiles with shared samples, skirts and normal halo | `SkiTerrainRuntime` | runtime automation coverage is partial | packaged import/edit receipt | Implemented; owner editability acceptance OPEN |
| P1.1-03 | Full/half/quarter LOD with adjacent difference no greater than one | runtime terrain actor | LOD unit/automation coverage is partial | visual/performance receipt | Implemented; seam and performance acceptance OPEN |
| P1.1-04 | Canonical heightfield ray query uses the renderer cell split and revision | domain/runtime query path | domain query tests | packaged pick/query-alignment receipt | Implemented; integrated evidence OPEN |
| P1.1-05 | Bounded asynchronous scratch mutation, stale-result rejection and readiness coherence | runtime mutation path | mutation tests | packaged edit/reopen receipt | Implemented; persistence/reopen acceptance OPEN |
| P1.2-01 | Slope/altitude/cover presentation and three lighting presets on the same terrain | P1 generated assets and presentation controller | asset recipe and presentation automation | 2560x1440 visual captures | Implemented; final captures and OWNER review OPEN |
| P1.2-02 | Batched thin overlays and 3,000 guest markers without per-marker actors/controllers | runtime overlay batches | explicit count/ownership test required | packaged visual/performance receipt | OPEN |
| P1.3-01 | One responsive selector/progress/view/status/node-view shell | `SkiP1Widget` | UI focused gate | Shipping UI-layout | Implemented; final frozen evidence OPEN |
| P1.3-02 | Single-owner pointer, focus, Escape, camera and drag state | widget and terrain view controller | UI input tests are partial | packaged interaction workflow | Implemented; complete interaction acceptance OPEN |
| P1.4-01 | Local MapLibre bundle with allow-listed navigation, popup denial and token/generation validation | P1 selector and browser wrapper | selector protocol tests | packaged selector smoke | Implemented; final CEF/WebGL qualification OPEN |
| P1.4-02 | Native 2-10 km validation; JavaScript cannot submit paths, commands, credentials or terrain arrays | domain selector protocol | `PackageAndProtocol` | packaged selector smoke | Implemented; final frozen evidence OPEN |
| P1.4-03 | Browser binding/resource teardown before terrain gameplay | presentation browser lifecycle | explicit cleanup test required | packaged selector-to-terrain workflow | OPEN |
| P1.5-01 | Required core/surround elevation and analytical WorldCover; optional NAIP/vector with explicit reasons | native provider/derivation | provider tests are partial | live preparation receipts | OPEN: current RGB WorldCover route must be replaced by class-value COG in P1 Section 2 |
| P1.5-02 | Maximum four network, two elevation-provider requests and two decode jobs with generation-fenced cancellation | process-wide acquisition/decode permits and operation lease | acquisition gate exercises overlapping operations, in-flight cancellation and decode caps | Shipping acquisition regression records the caps and cancellation latency; live cancellation receipt remains required | Implemented; final frozen and live evidence OPEN |
| P1.5-03 | Contours and compact cover-display geometry | derivation/runtime presentation | deterministic geometry tests required | packaged diagnostic views | OPEN |
| P1.6-01 | Sibling staging, complete validation and atomic content-addressed activation | package store | package/activation tests | packaged import and offline reopen | Implemented for schema 1; final frozen evidence OPEN |
| P1.6-02 | Canonical manifest, SHA-256 assets, dimensions/bounds/datum/provenance/licenses and resource limits | domain manifest/package store | malicious manifest and overflow tests | packaged receipt | Implemented for schema 1; coverage audit OPEN |
| P1.6-03 | Base package immutable; scratch edits never overwrite it | package/edit separation | mutation immutability test required | packaged edit then offline reopen | OPEN |
| P1.7-01 | Same packaged implementation through display, pick, mutation, LOD, query alignment and close | runtime and harness | full P1 automation | packaged import/edit receipt | OPEN |
| P1.7-02 | Synthetic, Crystal 2 km and reference 10 km datasets remain separately identified | fixtures/live qualification | fixture hashes and provider checks | separate receipts/captures | Synthetic present; live reference evidence OPEN |
| P1.7-03 | Full and steepest-quadrant views under three lights plus elevation/slope/cover/LOD/topology diagnostics | viewer and visual harness | camera/mode automation | tokened screenshots and SHA-256 receipts | Implemented in part; final evidence and OWNER review OPEN |
| P1.7-04 | Declared frame, gap, cancellation and reopen performance bounds | performance harness | deterministic controls | cold/warm packaged performance receipt | OPEN |
| P1.8-01 | Shipping build prepares after cook, activates, exits, loses network and reopens without development dependencies | provider/package/runtime | full automation | post-cook live prepare and network-denied reopen | OPEN |
| P1.8-02 | Tester bundle is self-contained with direct launch and diagnostics access | release assembler | package-tree/source-freeze validation | assembled folder and ZIP | Section 0 final release gate pending |
| P1-MCP-01 | Unreal MCP remains editor-only, loopback-only, selected toolsets only and absent from Shipping | project descriptor and local `.codex` config | static project checks | packaged module/listener assertion | Implemented; final Shipping assertion OPEN |

## Evidence locations

- Per-invocation reports and complete logs: `test-results/p1/runs/<invocation>/`.
- Latest command receipts: `test-results/p1/<command>.json` and
  `test-results/p1/smoke-Shipping-<scenario>.json`.
- Release-wide source identity: `test-results/p1/release-freeze.json`.
- Packaged tree identity: `test-results/p1/package-Shipping.json`.
- Tester output: a source/package-versioned `release/MountainPlanner-P1-Windows-*/`
  folder and adjacent ZIP and
  SHA-256 file.

Section 0 may be marked accepted only after the final freeze, focused gates, full
automation, Shipping cook, all three Shipping regressions, release assembly, a
fresh non-author Sol High review, and explicit owner acceptance. No row for later
P1, P1A, or P2A is closed by that acceptance.
