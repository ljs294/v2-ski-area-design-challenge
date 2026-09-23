# P1 / P1A / P2A implementation handoff

Status as of 2026-09-22 (local). This is a handoff, not an acceptance receipt or authorization to relax the approved plan. Read `AGENTS.md`, `docs/UnrealRebuild/P1-runbook.md`, and the owner's **Remaining P1, P1A Lidar Terrain, and P2A Terrain Cover Plan** before changing code. Treat the older v0.9 pack and the separately attached lidar/cover proposals as reference material where they conflict with that plan. Do not change TypeScript saves, schema-1 packages, global settings, or the approved selector architecture.

## What is actually finished

| Section | State | Evidence |
| --- | --- | --- |
| 0 — Mount Washington / GeoTIFF / acquisition / UI remediation baseline | Committed | `de7413dee3e011529c6289ce631c782a58873b1e` |
| 1 — TerrainCore v2 foundation | Committed | `7eea78441cd15a415101aa832a4a618819e140c0`; focused and packaged TerrainCore gates passed at that checkpoint |
| 2 — Complete original P1 on Medium | Substantial **uncommitted** work; not accepted | 33 modified tracked files and 10 new files at handoff. Do not describe this as a finished release. |
| 3 — P1A verified 1 m lidar | Not implemented | Current old `high` is resampled; it is not verified lidar High. New selector deliberately withholds verified High. |
| 4 — P2A accurate terrain cover | Not implemented | The new CoverEcology v1 **base analytical WorldCover prior** in Section 2 is not the P2A NAIP/canopy/ecology product. |

Only the owner accepts sections. Keep section-specific receipts and the fresh non-author Sol High adversarial review required by the approved plan. Do not use Astra without owner authorization. The current repository guidance says only the user stages/commits; reconcile any checkpoint action with the owner's explicit current authorization rather than assuming this handoff grants Git authority.

## Usable build now

The previously assembled self-contained Windows tester bundle is:

`release/MountainPlanner-P1-Windows-c3275930-20260922T070749/`

Keep the entire folder together. `START SAMPLE TERRAIN.bat` launches the reliable fixture path; `START MOUNTAIN PLANNER.bat` launches the live-provider selector. `README FIRST.txt` describes requirements and limitations; `OPEN DIAGNOSTICS.bat` opens logs. This bundle predates the uncommitted Medium/TerrainCore changes, so it is a **working P1 test build**, not the finished Medium/P1A/P2A release. Its `BUILD-INFO.txt` records passing editor TIFF, acquisition, UI, full automation and Shipping TIFF/acquisition/UI gates. Live-provider qualification remained open when it was assembled.

There is also a newer raw Shipping package at `test-results/p1/packages/Shipping/20260923T003717.640480Z-8bca219c/Windows/SkiAreaDesignChallenge.exe`. Keep its entire `Windows` directory together. It includes newer uncommitted work and is for developer evaluation only; it has not passed the complete Section 2 release/acceptance sequence. Do not replace the assembled tester bundle with it without finishing the gates below.

## Current Section 2 worktree

The worktree includes a Medium profile (2,000-sample longest-axis cap), no mislabeled verified High option, analytical WorldCover class COG reading, TerrainCore v2 + CoverEcology v1/composite installation, native viewer presentation/diagnostics/overlays, a 3,000-marker batch, a responsive selector/panel, and added focused and packaged harness scenarios. Relevant files are under `Source/SkiDomain`, `Source/SkiPreparation`, `Source/SkiPresentation`, `Source/SkiTerrainRuntime`, `Content/P1Selector`, `Tools/Build/p1.py`, and `Build-P1-Release.bat`. Preserve all these user/worktree changes; do not reset or wholesale rewrite them.

The latest aggregate automation and Shipping packaging receipts were reported passing for source digest `4e4020f44d5511305c09fa466730409483397624dc2204449944a7ccfb94db92` (invocations `20260923T003656.633673Z-e83e6bad` and `20260923T003717.640480Z-8bca219c`). The Medium packaged regression passed earlier as `20260922T193254.564219Z-45859623`, including repeated install and offline reopen, but it was for an earlier source digest. **Re-run all required gates against one frozen digest.** The latest performance smoke reported a harness error after the packaged receipt itself passed; a misplaced renderer assertion was moved in `p1.py` afterward, but the performance scenario must be rerun. Do not infer performance acceptance from that partial result.

## Finish Section 2 — original P1 Medium

1. Freeze a source snapshot and inspect the full uncommitted diff. Check for unfinished code, unintended generated assets, source-digest drift, and schema/legacy-reader regressions. Keep schema-1 read behavior intact and package writes atomic. Update the requirement matrix so every original P1 item has implementation, test, packaged receipt, and owner-acceptance status; an implemented but untested item stays open.
2. Run the focused gates using `python Tools/Build/p1.py tiff`, `acquisition`, `ui`, `terraincore`, and `p1`. Require their exact named test sets. Then run `python Tools/Build/p1.py automation` and `python Tools/Build/p1.py package --configuration Shipping`. Retain full UBT/UAT and automation logs. Fix causes, not test manifests or thresholds.
3. Run packaged Shipping scenarios via `python Tools/Build/p1.py smoke --configuration Shipping --scenario <name>` for `geotiff-regression`, `acquisition-regression`, `ui-layout`, `selector`, `terraincore-regression`, `medium-regression`, and `performance-regression`. Run `python Tools/Build/p1.py visual --configuration Shipping` for the required captures. Read receipts and screenshots, not only exit codes. Verify each receipt's source digest and package hash match the frozen package. Recheck the performance harness fix; qualify cold/warm Low/reference runs at required resolution and memory/RHI reporting, not merely the fast fixture values.
4. Inspect the packaged UI at 1280×720, 1920×1080, 2560×1080, 2560×1440, and 576×1024. Recovery actions must stay visible, controls scroll-reachable, panel input isolated, and full terrain inside the actual unobstructed viewport with an 8% margin. Validate CEF selector navigation/popup/bridge restrictions and browser cleanup in Shipping. If packaged CEF fails, stop for owner decision—do not add a second picker.
5. Verify the analytical WorldCover COG reader against a real class-value COG (not mapping RGB), including tiles/ranges, transforms, nodata and categorical values. Prove required cover failure cannot activate terrain. Ensure optional NAIP/vector absence is explicit and contains no fabricated assets. Confirm the source/delivered spacing, bounds, datum and resampling claims against real provider metadata.
6. Run live packaged Crystal Mountain 2 km Medium and Mount Washington NH 10 km Medium, including retry/cancellation and complete network-denied offline reopen. Inspect geometry, seams, canonical probe/LOD alignment, cover, lighting, overlays, marker count, and mutation persistence on the **same installed terrain**. Capture full/close and diagnostic evidence at 1×. Keep synthetic and live evidence separate. If these cannot be run locally, leave them visibly open rather than claiming final P1.
7. Run `Build-P1-Release.bat` only after fixing and passing its component gates. Verify release assembly rejects stale receipts. Update `docs/UnrealRebuild/P1-runbook.md` and the requirement matrix; update architecture documents only with landed architecture. Freeze the diff/receipts for a fresh non-author Sol High adversarial review and resolve or obtain owner acceptance of critical/high findings. Present Section 2 to the owner for acceptance before a checkpoint commit.

## Build Section 3 — P1A verified lidar High

1. Add a selection-time catalog/metadata preflight and availability report. Offer **High — verified 1 m lidar bare-earth** only with complete supported coverage and trustworthy horizontal/vertical metadata. Keep Medium available; never silently downgrade. Show source, epoch, native/delivered spacing, accuracy when documented, datum, grid size, duration and source-specific storage estimates.
2. Prefer complete USGS Seamless 1 m DEM, then a coherent compatible USGS project-based 1 m DEM. Treat Washington DNR as a separately validated optional source. Reject mixed/unknown vertical datums, incomplete footprints, unexplainable overlaps or seams, unsupported codecs/CRS, or a projection approximation.
3. Implement bounded classic TIFF/BigTIFF COG window reads over HTTP ranges with offset/count validation and in-memory cache. No raw provider TIFF on disk. Respect four global/two-provider network slots, two decode jobs, retries/timeouts/cancellation leases, and the 20-minute High deadline. Reproject supported source CRS to the 1 m ENU target using packaged-supported projection code; verify transforms with known control points.
4. Make storage preflight a hard gate: estimate downloads, installed data, peak workspace/cache, free space and `max(1 GiB, 10% of estimated peak)` reserve. Default workspace cap 8 GiB; user adjustment may not exceed 16 GiB. Replace full-mountain residency/builds with bounded tile streaming and preserve finest-data canonical picking and sparse edit sidecars.
5. Add exact named `p1a` tests, Shipping COG streaming/import/edit/reopen scenarios, provenance/seam/accuracy receipts, performance and no-raw-TIFF checks. Qualify Crystal 2 km verified High where coverage passes and two consecutive 10 km verified High preparations on a fully covered site. If Mount Washington lacks coverage, document its unavailable state and request owner approval for a replacement site. Do not claim P1A without post-cook native-resolution and offline-reopen proof plus owner acceptance.

## Build Section 4 — P2A accurate terrain cover

1. First freeze independent training/reference and held-out annotations for Crystal and Mount Washington patches, including groves, holes, clearings, meadow, rock, buildings, water, shadow/snow, sparse canopy, nodata and epoch conflicts. Lock baseline-relative metrics and obtain owner approval **before classifier tuning**. If the owner is unavailable, work on infrastructure/tests but leave this gate open.
2. Extend the independently addressed CoverEcology contract with semantic cover/confidence, canopy fraction, optional canopy height/community, substrate weights, mapped/authored exclusions, transforms, epochs, validity, provenance, licensing and per-channel revisions. Preserve nodata vs zero canopy and confidence vs canopy fraction. Composite installation references separate TerrainCore and CoverEcology IDs.
3. Use WorldCover class COG as broad prior, latest complete NAIP RGB+NIR over the whole selected tile for 1–2 m refinement, one validated optional Meta/WRI canopy-height adapter, and optional LANDFIRE community prior. Apply the explicit epoch rule: newer valid NAIP clearing beats older canopy height. Record conflicts and missing optional data; do not turn visual smoothing into a mapped-accuracy claim.
4. Render geographically aligned weighted textures with gutters/mips and nearest categorical diagnostics. Preserve hard water/exclusion edges, narrow runs and real holes. Prove a synthetic committed exclusion/edit receipt updates only affected tiles and survives offline reopen; do not implement construction grading or tree simulation.
5. Add exact named `p2a` tests, held-out quality reports, packaged cover/reopen captures and bounded performance evidence. P2A can close independently, but combined release promotion waits for its acceptance. Missing optional data must be labeled fallback, not invented data.

## Final combined release

Run all focused gates, full automation, Shipping packaging and tokened scenarios against one source digest. Confirm editor-only Unreal MCP modules/listener are absent from Shipping. Capture required visual and performance receipts (including all frame gaps, resolution, RHI, hardware, memory/cache state, preparation time, cancellation ≤250 ms and reopen ≤30 s). The tester bundle must launch without Editor, Node, Python, a development server or uncooked assets and include a direct launcher, diagnostics opener and concise instructions. Keep section-specific acceptance records; obtain owner review for the actual editable packaged terrain and cover. Stop instead of substituting a renderer, picker, source, datum or product tier when an approved stop rule is hit.

## Useful evidence locations

- `test-results/p1/runs/` — per-invocation logs, receipts, isolated data and crashes.
- `test-results/p1/packages/Shipping/` — packaged builds. Run the EXE with its whole package tree intact.
- `release/MountainPlanner-P1-Windows-c3275930-20260922T070749/` — last assembled tester bundle described above.
- `docs/UnrealRebuild/P1-runbook.md` — P1 authority and section gates (must be updated only as implementation lands).
- `Tools/Build/p1.py` — focused, packaged-smoke and visual command entrypoint.
- `Build-P1-Release.bat` — release sequence; currently modified and unaccepted.
