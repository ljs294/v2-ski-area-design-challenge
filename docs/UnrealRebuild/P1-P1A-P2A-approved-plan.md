# Remaining P1, P1A Lidar Terrain, and P2A Terrain Cover Plan

Owner-approved implementation plan, transcribed from the owner's conversation request. This plan is authoritative where it conflicts with the original v0.9 planning pack and the attached `P1_LIDAR_TERRAIN_PLAN.md` / `P2_TERRAIN_COVER_PLAN.md`. The attachments are renamed P1A and P2A for this implementation. This file is a reference copy for handoff, not a claim that the work is complete.

## Summary

- Treat the original v0.9 pack and both attachments as reference material. This plan is authoritative where they conflict. The attachments are renamed P1A and P2A.
- Current elevation resolution is selection-dependent: existing Standard reaches up to 1,000 samples on its longest axis (about 2 m over 2 km or 10 m over 10 km); existing resampled High reaches up to 2,000 (about 1 m over 2 km or 5 m over 10 km). The inspected Mount Washington-area package was about 6.0 × 8.4 m per sample. These delivered spacings are not native-source-detail guarantees.
- The finished workflow is select bounds → verify available datasets → show Medium and eligible High options with quality and storage estimates → user confirms → prepare and install → inspect in the packaged verification viewer.
- Medium is the broad-coverage path, capped at 2,000 samples on its longest axis and four million total. High means verified lidar-derived 1 m bare-earth data, unavailable without complete coverage and trustworthy metadata.
- New preparations use tiled TerrainCore v2. Schema-1 packages remain readable and immutable; never silently migrate or label them verified High.
- A 10 km × 10 km 1 m height grid is about 400 MB uncompressed; typical installed High may be roughly 0.7–2 GB with potentially several GB temporary processing data. Calculate a source-specific estimate rather than promising that range.
- The approved plan incorporated independent Sol High adversarial reviews of P1 gaps, P1A architecture, P2A data quality, UI, and release qualification.

## Delivery sequence

### 0. Freeze current remediation baseline

- Verify the current Mount Washington acquisition, GeoTIFF, logging, responsive-panel, retry and cancellation work.
- Pass exact TIFF, acquisition and UI named-test sets and their Shipping regressions. Record source digest, engine version, fixture hashes and packaged output.
- Create a requirement matrix mapping each original P1 item to implementation, test, packaged receipt and owner acceptance. Implemented but untested remains open.
- Owner accepts, stages and commits this section. Later sections start and end with a clean worktree.

### 1. Shared TerrainCore foundation

- Add ground-only TerrainCore v2 for new Medium and High: tiled little-endian float32 heights, validity masks, provenance, native/delivered spacing, documented accuracy, CRS, horizontal/vertical datum, pixel registration, sample-center/outer bounds, hashes and processing versions.
- Geometry uses 256×256 overlapping samples for 255×255 cells, partial edges, shared borders and a one-sample normal halo. Store deterministic 1/2/4/8/16 m LOD derivatives with fixed compression and per-tile hashes; canonical finest data remains separate. Validate incrementally without allocating the whole mountain, preserving path/hash/overflow/asset-count/space protections.
- Keep schema-1 reading unchanged. Old `standard` and resampled `high` display as legacy/resampled, never new High.
- Replace whole-mountain residency with disk-backed cache: 512 MiB default CPU budget, bounded async jobs and game-thread publication, screen-space LOD with hysteresis and neighbor-level difference ≤1, consistent borders/skirts/normals, no whole-mountain full-detail mesh. Fixed diagnostic LOD remains available.
- Finest installed ground data drives canonical picking/slope with the render triangle diagonal. Missing query tiles explicitly load rather than using visual LOD.
- TerrainEditSet sidecar is keyed to immutable TerrainCore ID; persist sparse deltas/change receipts, rebuild affected tiles and normal-neighbor ring, reject stale work and reopen edits offline.
- Reuse overview, steepest-quadrant and probe cameras; no ground-level or first-person camera.

### 2. Complete original P1 on Medium

- Convert broad-coverage USGS preparation to TerrainCore v2. Medium targets aspect-preserving ≤2,000 longest-axis samples and ≤4 million total.
- Show requested and returned bounds, native and delivered spacing, datum, dimensions and resampling status.
- Use analytical ESA WorldCover class-value COGs, not WMTS RGB intended for mapping.
- Add minimal required CoverEcology extension: categorical WorldCover prior, transform, validity, provenance, attribution and revision. TerrainCore can load independently, but a new P1 installation activates only after required ground and base cover validate.
- Optional NAIP and vector-context absence is explicit; no zero-byte placeholder or false acquisition claim.
- Finish existing responsive right shell, fixed progress/recovery and scroll area, isolated panel input, presentation/elevation/slope/cover/tile-LOD/probe-topology modes, midday/low-angle/overcast light, packaged sky/horizon, 1× qualification captures, Auto/fixed LOD, labeled 2×/4× interactive exaggeration, batched thin overlays, 3,000 synthetic markers without per-marker Actors/controllers, and minimal runtime node view for selection/package/source/readiness.
- Qualify packaged local MapLibre selector: allow-listed navigation, denied popups, tokened generation-fenced JSON, finite 2–10 km bounds, no paths/commands/credentials/bulk terrain arrays, browser unbound/closed before terrain. Stop with evidence if packaged CEF fails; no substitute picker.
- Preserve app JSONL log, operation journal, safe diagnostics, retries, cancellation fencing and atomic activation.
- Original P1 closes only after Medium packaged import, mutation, LOD/query alignment, close/network-denied reopen, visual review and performance acceptance.

### 3. P1A — verified 1 m lidar terrain

- After selection, catalog/metadata preflight precedes quality choice. Medium is broad-coverage/resampled; High is `Verified 1 m lidar bare-earth`, offered only for complete supported coverage. Show source/product, epoch, native/delivered spacing, datum, documented quality, dimensions, duration and storage.
- Source order: complete USGS Seamless 1 Meter DEM; then a coherent USGS project-based 1 m bare-earth DEM or exactly compatible projects; Washington DNR only as a separately supported source after footprint/datum/terms/transform tests.
- Reject incomplete coverage, mixed/unknown vertical datums, incompatible grids, unsupported compression/CRS, gaps, overlaps or unexplained seams. Never silently downgrade High.
- Implement bounded COG window reading with HTTP ranges, classic TIFF/BigTIFF directories, required source tiles only and bounded memory cache, never raw provider TIFF persistence. Validate offsets, byte counts, codecs, transforms, nodata and response limits. Limit to four network requests globally/two per elevation provider and two decode/derive jobs; retain classified retries, 90 s activity/180 s request timeout, three attempts, cancellation-aware backoff and 20-minute High deadline.
- Transform supported EPSG:6350 and NAD83 UTM to existing double-precision local ENU using packaged Unreal georeferencing/projection support. Reproject to regular 1 m ENU target while retaining native metadata; no relabeling or undocumented approximation.
- Preflight download bytes/range, installed size, peak temporary/cache, reusable normalized cache, free space and reserve `max(1 GiB, 10% of estimated peak)`. Default per-mountain workspace cap 8 GiB; project-local adjustment to hard ceiling 16 GiB. Block when upper bound plus reserve exceeds cap/free space; offer smaller selection or Medium, not automatic downgrade.
- P1A closes after post-cook High acquisition, native-resolution proof, streaming/LOD/query/edit correctness, offline reopen and owner acceptance of the same editable packaged terrain.

### 4. P2A — accurate and smooth terrain cover

- Extend separately content-addressed CoverEcology; composite receipt references independent TerrainCore and CoverEcology IDs.
- Independently transformed/versioned channels: semantic cover/confidence, canopy fraction, optional canopy-height statistics (method/epoch/support/missingness), optional community prior, substrate/material weights, mapped/authored exclusions, validity, provenance, epochs, licensing and per-channel revisions. Nodata differs from zero canopy; confidence differs from canopy fraction; canopy height differs from class.
- Processing order: analytical WorldCover class COG broad 10 m prior; latest complete NAIP RGB+NIR for whole-tile 1–2 m refinement (not an old boundary corridor); one validated Meta/WRI canopy-height adapter with explicit absence; optional LANDFIRE community crosswalk. Dynamic World is optional, not mandatory.
- Epoch rule: newer valid NAIP controls fine clearing boundaries; WorldCover is broad prior; older canopy height cannot recreate forest in newer imagery's clearing. Record conflicts.
- Freeze separate training/reference and held-out annotations for isolated groves, internal clearings, meadow, rock, buildings, water, snow/shadow, sparse canopy, nodata and epoch conflicts. Lock baseline-relative thresholds before tuning; owner approval of benchmark gates implementation.
- Render normalized weights with geographic transforms, gutters and mips; nearest only for categorical diagnostics. Preserve hard water/committed exclusions. Smooth transitions around 1–3 m where supported without erasing narrow runs, real holes, groves and water edges or claiming procedural smoothness as mapped accuracy.
- Use only synthetic committed exclusion/edit receipt to prove bounded invalidation, persistence and offline reopen. Real grading, clearing, undo and tree generation remain separately authorized original P2/P3.
- P2A closes independently; absent optional canopy/community data yields labeled fallback without invalidating accepted P1/P1A terrain.

## Public interfaces and compatibility

- `TerrainQualityTier { Medium, HighVerifiedLidar }`, `TerrainAvailabilityReport`, `SourceQuality`, `StorageEstimate`.
- TerrainCore v2 manifest, tile/LOD descriptors, source-window/transform descriptors, immutable snapshots, residency and canonical/render/query/edit revisions.
- Range-capable transport and COG request/result with tile identity, timing, limits, retry classification and operation lease.
- TerrainEditSet/ChangeReceipt keyed to immutable TerrainCore ID.
- CoverEcology v1 channels, independent transforms, validity/confidence semantics, revisions, epochs and license/attribution.
- Composite installed receipt references independent TerrainCore and CoverEcology IDs.
- Preserve schema-1 reader, TypeScript terrain schemas, `GameSave` and existing saves; no automatic migration/rewrite.
- Unreal MCP stays project-local, editor-only, loopback-only and restricted to needed editor/automation/Slate/UMG toolsets. Shipping has no MCP modules/listener.

## Tests, review and acceptance

- Retain `p1.py tiff`, `acquisition` and `ui`; add exact named-test `terraincore`, `p1`, `p1a`, `p2a` gates. Missing tests cannot pass silently.
- TerrainCore: schema-1 compatibility, malicious manifests, bound/cell math, asymmetric grids, EPSG transforms/datum rejection, COG/BigTIFF ranges, edges/hashes, LOD seams/normals, canonical queries, bounded residency, stale builds, sparse edits, extension preservation.
- P1: UI states/resolutions, hit-testing, browser security/cleanup, source policy, overlays/markers, lighting/materials, activation fencing and offline reopen.
- P1A: coverage/tier labels, storage/free-space caps, projections/provenance, mixed projects/source seams, cancellation/no raw TIFF.
- P2A: independent transforms, categorical sampling, whole-tile refinement, validity/confidence, epoch conflict, held-out metrics, texture gutters/mips, hard exclusions, localized invalidation/cache/reopen.
- Tokened packaged scenarios: TIFF, acquisition, UI, selector, Medium import/edit/reopen, High COG streaming, CoverEcology, visual, performance, abnormal-exit context and network-denied reopen.
- `Build-P1-Release.bat` runs applicable focused gates, full automation, Shipping package/scenarios and assembly; rejects stale/digest-mismatched receipts and prints exact logs on failure. Tester bundle has direct launcher, diagnostics opener and concise instructions, with no Editor/Node/Python/server/uncooked-asset requirement.
- Live: Crystal 2 km Medium baseline; Mount Washington 10 km Medium including retry/offline reopen; Crystal 2 km verified High where coverage passes; two consecutive 10 km verified High at fully covered site. Use Mount Washington if coverage passes, otherwise obtain owner-approved replacement and show correctly unavailable Mount Washington High. P2A uses independently annotated Crystal/Mount Washington patches and labeled optional-data fallbacks.
- Visual: existing full, steepest and probe views, 2560×1440 plus one 2560×1080 UI proof, all light/diagnostic modes at 1×, synthetic/Medium/verified High separate.
- Performance: Low p95 ≤33.3 ms/p99 ≤50 ms; reference p95 ≤20 ms/p99 ≤33.3 ms; ordinary max frame gap ≤250 ms; cancellation acknowledgment ≤250 ms; prepared reopen ≤30 s. Record cold/warm, all gaps, RHI, resolution, hardware, memory/cache and preparation time.
- At each section boundary, freeze diff/receipts for fresh non-author Sol High adversarial review; fix or owner-accept critical/high source/provenance, package/security and UI/release findings. No Astra without owner approval.

## Assumptions and stop rules

- Current remediation accepted/committed before new sections; only user stages/commits unless separately authorized.
- P1, P1A and P2A have separate acceptance receipts. P2A does not invalidate accepted terrain, but blocks combined release.
- High always means verified lidar-derived 1 m bare earth, never resampled coarse DEM.
- Runtime Geometry Framework stays unless a recorded packaged gate proves it unsuitable; no silent substitution.
- Stop for owner review if packaged projection support is absent, source needs unsupported/paid dependency, coverage cannot be proved, storage exceeds hard cap, metadata/seams conflict, packaged CEF fails or performance would require losing canonical edit/query fidelity.
- No engine fork, hosted processor, raw LAZ pipeline, global Codex/Unreal/Windows change, paid dependency, raw provider TIFF persistence, automatic downgrade, construction gameplay, tree simulation or save migration.
