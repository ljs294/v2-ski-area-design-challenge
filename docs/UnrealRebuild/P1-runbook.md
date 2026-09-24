# Unreal P1 runbook

P1 starts from committed P0 baseline `31af9880e12bd8230508a42ca6789d405e5b4bbe`. The v0.9 planning archive is retained reference material; its P0 prompts and historical archives are not active instructions. Sol High is the main coordination and implementation agent. The Section 0 implementation checkpoint is frozen at `de7413dee3e011529c6289ce631c782a58873b1e`; that records a baseline, not owner acceptance of the still-open Section 0 rows. For the remaining-plan execution begun 2026-09-22, the owner explicitly authorized the main agent to continue beyond that open acceptance boundary and to stage and commit tested section checkpoints while the owner is away. That exception does not grant unrelated repository or global-system changes.

The live accounting record is [the P1 requirement and acceptance matrix](P1-requirement-matrix.md). Code presence does not close a row without its named deterministic and packaged evidence; owner-review rows also require explicit owner acceptance.

**Section 2R amendment (2026-09-23):** the owner authorized a native Unreal site
picker and complete offline resort in place of the blocked CEF selector route,
with verified USGS 1 m acquisition moved forward from P1A. The approved
[Section 2R decision record](Section2R-site-picker-plan.md) governs this new
candidate. The CEF Shipping failure remains historical evidence; its recovery
steps and the no-replacement-picker rule below do not constrain Section 2R.
Section 2R's M0 gates must pass before dependent milestones proceed. The
current repository rule reserves staging and committing to the owner.

## Scope and architecture

P1 proves that one packaged Windows runtime can select, prepare, install, render, edit, query and reopen real terrain. Preparation is native C++; the embedded MapLibre page submits only a bounded site request. Existing TypeScript terrain and `GameSave` schemas remain unchanged. P2 construction, full grading/undo, simulation, weather and source retirement are excluded.

Dependency direction is `SkiDomain -> SkiApplication -> SkiPreparation/SkiTerrainRuntime -> SkiPresentation`. `SkiDomain` remains ordinary C++ and owns coordinates, heightfields, manifests, tiling, queries and revisions. `SkiPreparation` owns runtime provider/decoder/storage adapters. `SkiTerrainRuntime` owns Unreal geometry and presentation readiness. Only the integrator changes targets, module rules, project/config files, generated assets and packaging.

## Original P1 product gates

The numbered P1.1-P1.8 gates below are the original product-scope sections. They
are distinct from the later remaining-plan delivery checkpoints, whose Section 1
is the shared TerrainCore foundation and whose Section 2 completes the original
P1 product on Medium terrain.

1. **P1.1:** schema-1 fixture package, pure coordinate/heightfield/manifest tests, packaged Dynamic Mesh tile import, query and bounded scratch mutation.
2. **P1.2:** the same terrain gains pre-cooked slope/altitude/cover materials, three art-light presets, overlays and 3,000 batched dots.
3. **P1.3:** runtime UMG/Slate shell, minimal node-view prototype and single-owner focus/camera/tool input.
4. **P1.4:** packaged local MapLibre/CEF selector with origin/token/generation/schema validation and browser cleanup.
5. **P1.5:** native USGS/WorldCover/optional NAIP/Overpass acquisition and bounded derivation with honest progress and cancellation.
6. **P1.6:** portable writer, hostile-input validator, staging verification and atomic content-addressed activation.
7. **P1.7:** combined editability, picking, lighting, LOD and resource/performance proof on the same implementation.
8. **P1.8:** prepare after build, activate, exit and reopen offline from an isolated data root without editor, Node, Python, dev server or uncooked content.

Here, "offline" is an application contract: the production acquisition transport is
denied before request construction and the harness samples the owned process tree for TCP
listeners and remote endpoints. It is not a claim of OS-level network isolation; the harness
does not change Windows Firewall or other machine-wide security settings.

No section automatically authorizes the next. Failure of packaged Geometry Framework editability/appearance, CEF selector support, security validation or the declared resource envelope stops the owning section; no alternate product or picker is substituted without owner direction.

## Remaining-plan checkpoint map

0. **Current-remediation baseline:** GeoTIFF, acquisition, diagnostics, responsive UI and release harness. Commit `de7413dee3e011529c6289ce631c782a58873b1e` freezes the implementation baseline; open owner/evidence rows remain open.
1. **Shared TerrainCore foundation:** TerrainCore v2 contracts, deterministic LOD shards, bounded disk-backed residency, canonical queries, sparse edit sidecars and schema-1 read compatibility. These are the `TC1-*` matrix rows.
2. **Complete original P1 on Medium:** analytical cover, remaining viewer/selector/product behavior, integrated visual/performance/offline qualification and owner acceptance.
3. **P1A:** verified 1 m lidar availability, bounded COG acquisition/reprojection and packaged High qualification.
4. **P2A:** independent CoverEcology channels, source/epoch policy, held-out quality gates and packaged ecology evidence.

Implementation completion never substitutes for an evidence or owner-acceptance
row. The explicit 2026-09-22 continuation authorization permits work on later
checkpoints while earlier owner-review rows remain open; it does not mark those
rows accepted.

## Commands and evidence

Run focused pure and adapter tests first, then expand only for the owning gate:

```powershell
python Tools/Build/p1.py check
python Tools/Build/p1.py domain
python Tools/Build/p1.py terraincore
python Tools/Build/p1.py build
python Tools/Build/p1.py automation
python Tools/Build/p1.py assets
python Tools/Build/p1.py package --configuration Development
python Tools/Build/p1.py smoke --configuration Development --scenario selector
python Tools/Build/p1.py smoke --configuration Development --scenario import
python Tools/Build/p1.py smoke --configuration Development --scenario offline-reopen --content-id <content-id-from-import-receipt>
python Tools/Build/p1.py package --configuration Shipping
python Tools/Build/p1.py smoke --configuration Shipping --scenario selector
python Tools/Build/p1.py smoke --configuration Shipping --scenario import
python Tools/Build/p1.py smoke --configuration Shipping --scenario offline-reopen --content-id <content-id-from-import-receipt>
python Tools/Build/p1.py smoke --configuration Shipping --scenario terraincore-regression
```

Receipts belong under ignored `test-results/p1/` and identify frozen source, engine/toolchain, configuration, package manifest, fixture/content hashes, actual hardware/RHI/resolution, test counts, failures/skips and unavailable metrics. Live-provider evidence is separately labeled and never inferred from saved fixtures. Source/editor writes stop during final qualification and any separately authorized non-author review.

## Tester distribution

`Build-P1-Release.bat` builds the Shipping target and creates both an extracted tester folder and a shareable ZIP under ignored `release/`. The distribution is self-contained and does not require the repository, Unreal Editor, Python, Node.js or a development server. Its two visible launchers distinguish the live-provider path from the deterministic sample-terrain path.

Repository users can double-click `Run-P1.bat` to launch the sample-terrain path. It resolves the exact validated versioned build through `release/MountainPlanner-P1-LATEST.json`, creates a release when no valid handoff exists, and never guesses from directory timestamps; `Run-P1.bat live` selects the live-provider path.

## Acceptance summary

- TER01-05, VIS01-03/05, UI01-02, IO01-06/08, SEC01/03-05 and PERF01-02 are exercised by their owning sections; partial assertions are not blanket passes.
- Legacy Standard was capped at 1000 longest-axis heights and legacy resampled High at 2000; neither is verified lidar High. New original-P1 Medium targets at most 2000 longest-axis samples and four million total. Verified 1 m lidar High is a separate P1A gate and is withheld until coverage/preflight exists. Schema-1 package limits and read compatibility remain unchanged.
- P1.7 uses synthetic seam/steep/nodata fixtures, a 2 km Crystal Mountain Medium iteration package and a 10 km Mount Washington Medium reference package. Synthetic receipts cannot substitute for live-provider or owner visual acceptance.
- The initial performance contract is p95 <=33.3 ms Low / <=20 ms reference, p99 <=50/33.3 ms, ordinary maximum gap <=250 ms, cancellation acknowledgement <=250 ms and prepared-package reopen <=30 seconds. Cold/warm and first-use costs remain distinct.
- Owner visual approval applies to the actual editable packaged terrain from resort-wide and moderately close views under midday, low-angle and overcast presets.
- As of 2026-09-23, Section 2 is blocked on the packaged selector's unexpected CEF egress. A Shipping `about:blank` comparison and a project-owned direct CEF initialization with default-deny handlers for exposed resource requests and a resolver rule in browser/child launch hooks still observed unapproved helper TCP connections; see the requirement matrix for receipt locations. No credible remaining in-scope project-local enforcement path was established. The owner chose to keep the section blocked if an in-scope fix fails. Do not broaden the tile-host allow-list, weaken the network audit, add a fallback picker, or promote a release while this gate is red.
