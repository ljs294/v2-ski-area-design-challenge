# P0.4 preparation and retirement inventory

Reference checkout: `1cca4e0808b96fa0fc43d48c4289a118b29b30ec` (2026-09-15). P0 adds native scaffolding beside the still-retained extraction/reference source. No provider acquisition, dependency installation, extraction or retirement is claimed here.

## Required source chain

| Boundary | Actual source | Extraction consequence |
| --- | --- | --- |
| Window / screen handoff | `src/app/App.tsx`, `src/app/MapView.tsx`, `src/app/resortBoot.ts` | The selector/preparation gate is coupled to the map composition today; it is not a standalone ready-to-embed bundle. Trace the picking-mode boundary and `prepareResortPackage` call when extracting. Preserve only selection/preparation UI in the eventual web bundle. |
| Acquisition orchestration | `src/terrainIngest.ts` | Fetch elevation and surroundings, cover and context, derive products, persist and reload/validate. Retain request generation, abort behavior, actual extents and error status. This source calls browser/desktop storage and is not a standalone Node CLI. |
| Elevation decode | `src/elevation.ts`, `src/bicubicUpscale.ts` | USGS raster requests and GeoTIFF decoding; `geotiff` comes from the pinned package graph. Preserve returned extent, orientation/nodata and native spacing. Upsampling does not improve source accuracy. |
| Cover acquisition | `src/usgsTerrainCover.ts`, `src/fourClassCover.ts` | NAIP imagery service plus four-class derivation; preserve optional-data policy and acquired provenance. Do not replace missing imagery with invented measured data. |
| Vector context | `src/vectorFeatures.ts`, `src/overpassConfig.ts`, `src/roadAnalysis.ts`, `src/streamAnalysis.ts` | Overpass request/retry and parsed water/road/building context. Existing imported context does not commission new construction tools. |
| Derived assets | `src/marchingSquares.ts`, `src/coverAnalysis.ts`, `src/coverDisplay.ts` | Contours, boundaries and compact cover-display geometry. Select retained calculations with source fixtures before porting. |
| Worker boundary | `src/terrainPreparationClient.ts`, `src/terrainPreparation.worker.ts` | Browser Worker URL, transferable arrays and synchronous no-Worker fallback. A helper host needs new bounded task/cancel/storage adapters; copying worker entrypoints alone is insufficient. |
| Package validation | `src/terrainPackage.ts`, `src/types/terrain.ts`, `src/types/cover.ts`, `src/types/geo.ts` | Existing terrain schema 6 and older hydration are reference behavior. The native portable manifest must explicitly describe coordinate frame, actual bounds, hashes, tile encoding and limits; it is not Actor serialization. |
| Browser storage | `src/terrainStorageClient.ts`, `src/weatherStorageClient.ts` | IndexedDB and legacy read migration. Terrain cleanup is coupled to weather storage. A bundled helper cannot assume IndexedDB/localStorage exists. |
| Desktop storage | `src/desktopBridge.ts`, `src/ipcContract.ts`, `electron/preload.ts`, `electron/ipcTerrainStorage.ts` | Typed preload bridge and filesystem IPC are Electron-specific adapters, not APIs for the native player. Preserve terrain-before-save behavior and asset revision checks when designing the native activation handshake. |
| Gameplay weather preparation | `src/app/preparedWeatherClient.ts`, `src/app/preparedWeather.worker.ts`, `src/app/useGameWeather.ts`, `src/weather/`, `electron/ipcWeatherStorage.ts` | Retain versioned prepared data, seed/timezone/checkpoints and fixtures. P0 does not select or translate the numerical model. |
| Separate weather lab | `weather-engine/`, `weather-lab/`, `weather-service/` | Distinct lab model/provider/runtime paths exist. Keep them until the P6 source trace identifies intended numerical behavior; no scientific redesign is authorized. |
| Build / dependency inputs | `package.json`, `package-lock.json`, `vite.config.ts`, `vite.config.web.ts`, `electron/main.ts` | A native helper must intentionally bundle required runtime/dependencies and local web assets. The old Electron weather-service staging is not automatically inherited by Unreal cooking. |

This is a bounded manually traced inventory, not a claim that every transitive import can already be extracted. P1 extraction must resolve each retained import/worker/asset edge, pin provider fixture rights and introduce a native package writer/validator. `Tools/Preparation/check_inventory.py` checks that these source anchors and their current hashes still match the captured inventory; a hash pass is not a provider test.

## Characterization entrypoints to retain

`src/terrainIngest.test.ts`, `src/terrainIngestReturn.test.ts`, `src/terrainPreparationClient.test.ts`, `src/terrainPackage.test.ts`, `src/terrainMapContext.test.ts`, `src/app/preparedWeatherClient.test.ts`, `src/gameSaveSchema.test.ts`, `src/gameSaveClient.test.ts`, `tests/e2e/` and the weather engine/lab fixtures remain reference evidence. Live provider checks are separate from offline deterministic tests. Existing expected outputs must precede translated implementations.

## Retirement dependencies

| Candidate retirement | Earliest necessary evidence |
| --- | --- |
| React gameplay entry (`index.html`, `src/app/main.tsx`, gameplay composition) | P2/P5 selected native workflow accepted; P1 selector/preparation extraction no longer imports gameplay. Keep all source until then. |
| Electron main/preload/storage and desktop packaging | P1.8 packaged helper/native activation and P5.3 clean-machine dependency checks; no required provider/storage path still depends on Electron. |
| Browser gameplay build / old CI entrypoints | Replace each required gate with a native failure-propagating equivalent at cutover; do not remove the only working verification path in P0. |
| Spike / Graphics Lab / Weather Lab | Inventory retained diagnostic and fixture responsibilities separately; P6 numerical trace precedes weather-source retirement. |
| Old user save/terrain directories | Never a source retirement target. No deletion, migration or relocation is authorized by P0. |

No files are copied into a second maintained preparation implementation yet. The approved P0.4 phase row is inventory-only; production extraction, browser-host proof, packaging and offline reopen belong to P1.
