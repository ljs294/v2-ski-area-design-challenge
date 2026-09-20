# Unreal P1 runbook

P1 starts from committed P0 baseline `31af9880e12bd8230508a42ca6789d405e5b4bbe`. The v0.9 planning archive is retained reference material; its P0 prompts and historical archives are not active instructions. Sol High is the main coordination and implementation agent. The user alone stages and commits accepted section boundaries.

## Scope and architecture

P1 proves that one packaged Windows runtime can select, prepare, install, render, edit, query and reopen real terrain. Preparation is native C++; the embedded MapLibre page submits only a bounded site request. Existing TypeScript terrain and `GameSave` schemas remain unchanged. P2 construction, full grading/undo, simulation, weather and source retirement are excluded.

Dependency direction is `SkiDomain -> SkiApplication -> SkiPreparation/SkiTerrainRuntime -> SkiPresentation`. `SkiDomain` remains ordinary C++ and owns coordinates, heightfields, manifests, tiling, queries and revisions. `SkiPreparation` owns runtime provider/decoder/storage adapters. `SkiTerrainRuntime` owns Unreal geometry and presentation readiness. Only the integrator changes targets, module rules, project/config files, generated assets and packaging.

## Section gates

1. **P1.1:** schema-1 fixture package, pure coordinate/heightfield/manifest tests, packaged Dynamic Mesh tile import, query and bounded scratch mutation.
2. **P1.2:** the same terrain gains pre-cooked slope/altitude/cover materials, three art-light presets, overlays and 3,000 batched dots.
3. **P1.3:** runtime UMG/Slate shell, minimal node-view prototype and single-owner focus/camera/tool input.
4. **P1.4:** packaged local MapLibre/CEF selector with origin/token/generation/schema validation and browser cleanup.
5. **P1.5:** native USGS/WorldCover/optional NAIP/Overpass acquisition and bounded derivation with honest progress and cancellation.
6. **P1.6:** portable writer, hostile-input validator, staging verification and atomic content-addressed activation.
7. **P1.7:** combined editability, picking, lighting, LOD and resource/performance proof on the same implementation.
8. **P1.8:** prepare after build, activate, exit and reopen offline from an isolated data root without editor, Node, Python, dev server or uncooked content.

No section automatically authorizes the next. Failure of packaged Geometry Framework editability/appearance, CEF selector support, security validation or the declared resource envelope stops the owning section; no alternate product or picker is substituted without owner direction.

## Commands and evidence

Run focused pure and adapter tests first, then expand only for the owning gate:

```powershell
python Tools/Build/p1.py check
python Tools/Build/p1.py domain
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
```

Receipts belong under ignored `test-results/p1/` and identify frozen source, engine/toolchain, configuration, package manifest, fixture/content hashes, actual hardware/RHI/resolution, test counts, failures/skips and unavailable metrics. Live-provider evidence is separately labeled and never inferred from saved fixtures. Source/editor writes stop during final qualification and any separately authorized non-author review.

## Tester distribution

`Build-P1-Release.bat` builds the Shipping target and creates both an extracted tester folder and a shareable ZIP under ignored `release/`. The distribution is self-contained and does not require the repository, Unreal Editor, Python, Node.js or a development server. Its two visible launchers distinguish the live-provider path from the deterministic sample-terrain path.

Repository users can double-click `Run-P1.bat` to launch the sample-terrain path. It creates the release first only when one does not already exist; `Run-P1.bat live` selects the live-provider path.

## Acceptance summary

- TER01-05, VIS01-03/05, UI01-02, IO01-06/08, SEC01/03-05 and PERF01-02 are exercised by their owning sections; partial assertions are not blanket passes.
- Standard terrain is capped at 1000x1000 heights and High at 2000x2000. The package validator caps 4 million height samples, 16 million cover cells, 64 assets, 1 MiB manifests, 512 MiB per asset and 1 GiB total staging.
- P1.7 uses synthetic seam/steep/nodata fixtures, a 2 km Crystal Mountain Standard iteration package and a 10 km Crystal Mountain High reference package.
- The initial performance contract is p95 <=33.3 ms Low / <=20 ms reference, p99 <=50/33.3 ms, ordinary maximum gap <=250 ms, cancellation acknowledgement <=250 ms and prepared-package reopen <=30 seconds. Cold/warm and first-use costs remain distinct.
- Owner visual approval applies to the actual editable packaged terrain from resort-wide and moderately close views under midday, low-angle and overcast presets.
