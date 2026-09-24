# Section 2R: native site picker and complete offline resort

Status: **M0 feasibility complete; M1–M6 and Section 2R acceptance remain open.** This
decision record implements the owner's 2026-09-23 direction and the reviewed
`purring-baking-hejlsberg.md` proposal. It supersedes the CEF selector recovery
work in Section 2 §A and pulls verified USGS 1 m acquisition forward from P1A.
Section 2 B/C remains open until Section 2R's complete packaged resort passes.

## Product contract

- Replace the packaged CEF/MapLibre selector with a native Unreal title screen,
  map, free-aspect rectangle picker, resort name step, and download status.
- Search may be global; new resorts require supported U.S. USGS coverage.
  Picker sides stay at 2–4 km until the full 10 km bundle qualifies.
- Install TerrainCore, CoverEcology, imagery, vectors, provenance/quality, and
  receipt as one verified composite. Staging and interrupted downloads are never
  playable or listed as installed. Mountain opens through the installed-package
  path with the acquisition port denied and zero process endpoints.
- Use S1M first, then Project 1 m only with authoritative vertical-datum proof,
  then 1/3-arcsecond fallback. Show the measured source mix and grade; never call
  sample spacing positional accuracy. An unproven Crystal Project 1 m datum
  forces a lower-tier fallback rather than an assumed A/B grade.
- Preserve schema-1 package readers, legacy install reads, TypeScript app and
  GameSave contracts. Only the owner stages and commits.

## M0 stop gates

The owner accepted the packaged five-minute dense-contour pan/zoom result at
the native 2560×1080 display resolution as meeting the M0 map performance goal on
2026-09-23. That accepted run predates a map-spike upload-scheduling change; the
production picker needs its own performance measurement. The originally specified
1920×1080 and 2560×1440 runs remain untested. A packaged build must prove S1M COG decoding,
GeoPackage lineage reading, Shipping projection transforms, and a project-owned curl
transport that refuses redirects before a forbidden listener accepts anything.
A synthetic 10,001² TerrainCore must write from canonical scratch, verify,
reopen, and visibly render. Its peak preparation RAM must be ≤2 GiB, non-network
build ≤10 minutes on the reference machine, reopen ≤30 seconds, and cancellation
acknowledgment ≤250 ms. A failed or incomplete gate stops dependent milestones;
it is not converted into a release waiver.

After M0, the sequence is: M1 title/flow and proven CEF removal; M2 native map,
search and rectangle; M3 catalog and quality preview; M4 real staged elevation
and cover without a library entry; M5 SiteContext and first atomic playable
install; M6 real 10 km acquisition, offline reopen and full qualification.
Focused tests, one packaged smoke, and a fresh non-author Sol High review run at
each milestone. Owner hands-on acceptance is requested for the complete result.

## M0 closure evidence (2026-09-23 to 2026-09-24)

M0 feasibility **passed** after a fresh non-author Sol High review. The final
Shipping package is invocation `20260924T003132.391316Z-b3307d71`, source
digest `ab66137bde0656518411a6d8697daf3ccbefbd9d1dcf28a20aaf8789e1af1230`.
Its real GeoPackage, network, and 10,001² scale/render gates passed. The map
performance disposition uses the owner's acceptance of the earlier native
2560×1080 run; it is not a current-source benchmark. The independent reviewer
found no remaining M0 blocker and retained the precise PROJ and production
gateway-routing limitations below.

Development and Shipping packages from source digest
`138cacae7fb766e25be406ef4e154b13f82f53b631834433f4c5cacf81a24a5a`
passed the initial probes. A later Shipping package from source digest
`65fd34454fad618bfc218aed2b398dd3a898fb1f16e78ee58dc35a3d2a697f44`
passed the bounded real S1M probe after the project curl transport adopted
Unreal's Windows certificate trust store without disabling TLS verification.
Shipping package `20260923T232013.602676Z-dfffe0c2` added a NOAA NGS
datum-operation control; that control failed at 0.2775 m against a 0.03 m
threshold. The owner accepted the native 2560×1080 map performance result.
The plan's custom-projection contingency was then implemented and passed a
Shipping COG/GeoPackage/projection receipt in package
`20260923T234254.416096Z-4ad15ee2`, retaining the failed PROJ diagnostic and
recording a 1.5 m datum-approximation budget.
The deterministic P1 check and focused M0 bit-exact LOD, COG/GeoPackage,
negative input, ETag pinning, and custom projection Editor tests passed. A
Development packaged HTTPS suite passed nine redirect cases, stable pinned
ranges, and a changed-object 412 rejection with zero forbidden accepts.
M1–M6 work may proceed under the M0 findings and limits recorded here.

| Gate | Evidence | Disposition |
| --- | --- | --- |
| A native map | The accepted Shipping run reported actual 2560×1080 geometry for 300 seconds, p95 6.079 ms and p99 6.967 ms. Texture and UObject counts returned to baseline; physical RAM remained about 178 MB above its start. The map-spike source later changed upload scheduling and memory sampling, so the accepted timing is not a current-source run. | **Owner-accepted M0 performance goal at 2560×1080.** Re-measure the production picker; 1920×1080 and 2560×1440 remain untested. |
| B/C COG and GeoPackage | Development TLS fixture completed 16 validated ranges into LZW/predictor-3 float COG decoding with 15 later `If-Match` requests. Current Shipping decoded a base tile and overview tile from the real 10,000² Mount Washington S1M COG in 21 validated HTTPS ranges, 1,285,609 bytes total. It read the matching official S1M GeoPackage, verified EPSG:6350 feature metadata, and decoded polygon WKB from 2 source rows and 1 blending row. Four focused raster tests and malformed-input/changed-ETag cases passed. | **Pass for M0 COG and real GeoPackage feasibility.** |
| D gateway | Development package denied nine redirect chains, rejected malformed Content-Range and changed ETag, and recorded zero forbidden-listener accepts. The final Shipping process audit observed only an approved S1M-host port-443 endpoint, with package hashes and source digest attached. Its fresh packaged offline Mountain reopen had zero process endpoints. Reciprocal 127.0.0.1 sockets were game self-IPC. | **Pass for M0 external egress and offline guard.** Production `NativeTerrainProvider` must use the gateway before real acquisition is exposed in M4. |
| E PROJ/custom fallback | Shipping GeoReferencing actor passed EPSG:3857 and EPSG:6350 controls plus published GN7-2 Albers (0.00025 m error) and Transverse Mercator (0.00991 m error) controls. A NOAA NGS NAD83(1986)→NAD83(2011) control measured 0.2775 m error against 0.03 m. The plan's custom fallback passed independent GN7-2 Albers/TM controls and S1M/Project round trips in Shipping, with an explicit 1.5 m datum-approximation term. | **Fallback probe pass:** source-specific runtime routing and quality-budget propagation remain M3–M5 work; PROJ precision failure remains visible. |
| F TerrainCore scale | Final Shipping 10,001² build/activation took 247.1 s and external Verify 107.1 s; reopen 0.172 s, observed preparation peak 237,838,336 bytes, cancellation 42.744 ms. The same package rendered 9 terrain tiles and captured a nonblank screenshot after 1.101 s. | **Pass for the synthetic offscreen scale limits.** |

Final-package receipts are
`test-results/m0/shipping-final-real-gpkg.receipt.json`,
`test-results/m0/network-audit-605fdc1f-64e6-496a-b4d8-0555b476241a/summary.json`,
and `test-results/m0/20260924T003442Z-a1217a98/summary.json`.
The source digest above identifies the package before this documentation-only
closure edit; no C++ or packaging input changed afterward.

The earlier map Shipping package invocation was `20260923T222254.540881Z-737e254b`,
and the M0 scale/visible-map attempt is under
`test-results/m0/20260923T222647Z-4b693d94/`. The visible map attempt was
stopped after the owner reported its full-screen synthetic contour window. A
subsequent offscreen attempt is under `test-results/m0/20260923T224112Z-42200c37/`
and was terminated without a receipt. These are diagnostic artifacts, not an
accepted source freeze or release package.

The successful synthetic scale/render summary is
`test-results/m0/20260923T230436Z-170363ae/summary.json` and the bounded real
S1M receipt is `test-results/m0/shipping-real-s1m-tls.receipt.json`. All later
packaged probes used hidden offscreen processes after the owner's report of the
visible contour benchmark. The owner accepted the five-minute real-window
2560×1080 result as the map performance goal. The failed NOAA datum receipt is
`test-results/m0/shipping-noaa-datum.receipt.json`; the passing contingency
receipt is `test-results/m0/shipping-projection-fallback.receipt.json`.
The packaged GeoReferencing resources contain `proj.db` but no NADCON5 grids.
The 0.2775 m observation is consistent with a ballpark NAD83 datum operation;
the exact runtime-selected operation has not yet been captured. The engine's
PROJ 9.1 predates NADCON5 GeoTIFF gridshift support in PROJ 9.2, so adding grids
alone is not a validated fix. A project-owned matching PROJ runtime/database
and offline grids, followed by an explicit operation and NOAA control receipt,
is the candidate repair path if sub-decimetre legacy-datum conversion is later
required. The accepted plan contingency instead uses tested custom projection
formulas and carries a conservative 1.5 m NAD83≈WGS84 approximation term for
WGS84 overlays. Other source datum realizations still require explicit
resolution and uncertainty before use. Native EPSG:6350/6318 S1M does not
require that datum copy and retains its own source positional and vertical
uncertainty terms.
