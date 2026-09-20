# Unreal P0 bootstrap

P0 is authorized by the owner's request, "Please proceed with implementing P0". P1 and later gameplay are not part of this change. The unchanged planning archive is retained as `planning-pack-v0.9.zip`; its templates are reference material, not installed repository instructions.

## Status

**P0 is complete and accepted on 2026-09-19.** Standalone C++ checks, the editor build, repeatable asset generation, and cooked Development/Shipping startup checks passed. The owner approved the Shipping visuals and the historical P0 coordinator exception. Sol completed SEC01/SEC02 closeout within the plan's procedural/source-identity and reviewer-tool boundaries. See [acceptance evidence](P0-acceptance.md) and the dated reports under `test-results/p0/`. P1 remains separately authorized work.

## Model usage after P0

The owner's 2026-09-19 direction is Sol for routine coordination and implementation, with Astra reserved for a specific owner-approved escalation. Project defaults request Sol High; verify the actual session model before substantial work, because defaults did not change the earlier active Astra thread. Keep task context, logs and agent count bounded. Independent reviewers remain separately scoped; do not launch extra model sessions just to confirm configuration. The P0 exception is historical, not permission to continue routine Astra work. Global Codex settings remain unchanged.

| Section | Implemented here | Acceptance / later scope |
| --- | --- | --- |
| P0.1 | Root guidance reconciled; baseline and archive retained; bounded scope recorded | Accepted; no automatic next phase |
| P0.2 | Source manifests, failure-detecting runner, bounded independent review, ownership guards and project model profiles | SEC01/SEC02 passed within documented boundaries; historical coordinator exception approved |
| P0.3 | Four native modules, locked toolchain; pure C++ tests; editor build; cooked Development and Shipping players both pass startup | Owner visual approval passed; clean-machine/offline qualification remains later release work |
| P0.4 | Preparation dependency and retirement inventory; original source preserved | P0 inventory complete; extraction/retirement waits for P1 native preparation proof |
| P0.5 | Real map/material/widget assets created; second generation leaves all source and asset bytes unchanged; file-server/messaging plugins disabled | Bootstrap accepted; bounded Shipping network audit and asset-guard evidence retained |

The supplied pack labels future tests NOT_EXECUTED; importing it does not change those statuses. Python tooling results prove only those tools. Before/after hashes detect persistent source changes but are not an enforced read-only sandbox. A bounded source review has run; its client metadata confirms Terra High/read-only, while the main thread remained Astra/xhigh. The planned Sol coordinator routing was therefore not established in this conversation.

The initial project-local configuration attempt caused the Windows sandbox helper to fail with `setup refresh had errors` and was reverted. On retry, `.codex/` was created under the workspace owner's account; ordinary sandbox commands continued to work. The three project files are now installed: `.codex/config.toml`, `.codex/agents/terra_worker.toml` and `.codex/agents/terra_reviewer.toml`. Global configuration and security settings were untouched.

The owner's CLI (`0.154.0-alpha.6.2`) reports `config loaded`, model `gpt-5.6-sol`, and no startup warnings with `--strict-config`. A separate `codex sandbox -P :read-only` shell probe could read source and was denied file creation. One explicitly authorized Terra High reviewer subsequently reviewed an immutable source bundle in one session. Its initial shell reads were denied; the continuation disabled shell, image, app/plugin/browser tools and further agents. This is source review, not certification of actual model routing or complete OS isolation. See the findings and usage receipt under `test-results/p0/review/`. No credentials were copied.

## Boundaries and ownership

- Pure domain implementation: `Source/SkiDomain/Private/Core` and `Source/SkiDomain/Public`. A single manifest, `Tools/Build/domain-sources.json`, drives standalone compilation and checks UBT discovery. The module-registration shim is outside that pure set. The small stale/overflow revision probe demonstrates linkage; it is not a simulation engine or save schema.
- Application code depends on domain; presentation composes application and Unreal; `SkiEditor` and Python tooling are editor-only. No gameplay depends on Python, Electron, UMG state or an actor's lifetime.
- One integration owner controls the project, module rules, target rules, `Config/`, editor recipe and generated assets. The sole owned content is `/Game/P0Generated/Bootstrap`, `M_Bootstrap`, and `WBP_Bootstrap`. No binary assets are hand-written.
- A generated ownership receipt records hashes, class, properties and recipe identity. Unknown/manual changes or dirty editor packages are refused. A partial failed save with no receipt requires investigation; it must not be silently overwritten.
- Existing TypeScript/Electron app, schemas 1-17, weather backends, terrain files, tests, providers, user saves and installed packages remain intact. Native saves are not implemented. This preserves source for extraction, without commissioning a second gameplay product.
- Only the user stages/commits. Global Codex settings, account permissions, branches and worktrees were left unchanged. All native prerequisites are installed. Project-local profiles request Sol High coordination and Terra High execution/review; configuration labels and the CLI load diagnostic are not proof of actual model execution. Files under `codex/` remain reference examples.

## Commands

From the repository root with Python 3.11+ and Node already installed:

```powershell
python Tools/Build/p0.py doctor
python Tools/Build/p0.py check
python Tools/Build/p0.py snapshot
```

`check` runs static project/document checks and nonempty Python tooling tests. It intentionally does not claim native qualification. `doctor` returns exit 2/BLOCKED when prerequisites are unavailable. Output is under ignored `test-results/p0/`; each invocation keeps its report, environment, full source manifest and logs in a unique `runs/<invocation>/` directory. Root reports point to the latest invocation; package/smoke reports are separate for Development and Shipping. Exit 1 is failure. No zero-test result, missing readiness marker, stale smoke token or timeout is a pass.

The original planning checker was also executed from a scratch extraction with UTF-8 enabled: 82/83 checks passed. Its delivery coverage comparison uses Windows backslashes against ZIP forward slashes; all 71 file hashes match. `check` independently validates the archive's complete coverage and hashes with ZIP paths, without modifying the original archive or relabeling the original checker failure.

All prerequisites are installed at the paths below. Run the following serially. Another machine may supply its installation root with `--engine-root` or `UE_ROOT`; `toolchain-lock.json` rejects a different engine patch or compiler binary:

```powershell
$env:UE_ROOT = 'C:\Program Files\Epic Games\UE_5.8'
python Tools/Build/p0.py doctor
python Tools/Build/p0.py domain
python Tools/Build/p0.py build
python Tools/Build/p0.py assets
python Tools/Build/p0.py package --configuration Development
python Tools/Build/p0.py smoke --configuration Development
python Tools/Build/p0.py package --configuration Shipping
python Tools/Build/p0.py smoke --configuration Shipping
```

`domain` uses the `p0-msvc` preset, compiles the actual pure sources, tests stale/overflow handling, requires real direct/transitive forbidden-header compiler failures, and rejects deliberate failed/empty/crashed/timed-out native tests. A timeout cannot stand in for the crash or failed-assertion control. The receipt records the selected compiler/SDK/CMake versions, compiler/CMake binary hashes, test executable hash and individual control outcomes; compiler diagnostics and native output are retained with the invocation. `build` compiles the editor target normally, without Live Coding. `assets` invokes the editor recipe twice, verifies all three owned assets and requires the second invocation to leave source and asset bytes unchanged. `package` builds/cooks/stages the game target into a fresh archive and fingerprints its files. A successful process exit without the expected artifacts is insufficient.

Optional `build --target Game --configuration Development` and `build --target Game --configuration Shipping` check compilation/linking independently of the editor. Both passed on this machine; they are not cooks or launchable packaged builds. Unreal operations needed owner-account access to normal per-user build/cache resources; the first restricted UBT invocation stalled and was terminated. Long command logs now stream to their invocation directory and survive timeouts. Build arguments disable remote UBA execution for this local bootstrap.

`smoke` selects the launcher from the matching successful package receipt. It rejects changed source, the wrong configuration, altered cooked content, changed or added binaries, and a missing launcher. It checks package hashes before and after startup; only runtime `Saved` directories are excluded. Optional `--executable` must match the recorded launcher. Editing inputs after a cook requires a fresh cook; an arbitrary or stale executable cannot certify current source.

The packaged smoke checks native domain/application linkage, map/game-mode initialization, cooked widget loading and widget creation using a unique file receipt. It does not prove visual appearance, clean-machine operation, network-denied operation or absence of listeners; those checks remain explicit P0/Shipping acceptance work. No MCP plugin or automation server is enabled in the project. The local `-SkiP0Smoke` command-line path exits after startup; it is not a network bridge.

`Tools/Build/toolchain-lock.json` records the 5.8.2 build metadata/hash and tested compiler/SDK/CMake identities. Both Unreal targets explicitly use 5.8 include order, V7 build defaults, compiler 14.44.35229 and SDK 10.0.26100.0; UBT confirmed them. The editor exposed a DLL export issue fixed with a compiler-native visibility macro, preserving the pure header's independence from Unreal. Review the asset receipt and open the blank player for owner visual confirmation, followed by fresh non-author review with effective tool restrictions verified.

## Toolchain installation and tested versions

The owner authorized completion including prerequisites. Microsoft C++ tools installed on retry, and the owner started Unreal's launcher installation. There was approximately 655 GiB free on C: and 225 GiB on D: before installation. No engine fork or paid plugin is needed for this bootstrap.

| Tool | Installed version / verification |
| --- | --- |
| Unreal Engine | 5.8.2, changelist 56702186; exercised by UHT/Game builds |
| Build Tools | Visual Studio 2022 17.14.41 |
| MSVC toolset / compiler | v143, 14.44.35207 / 19.44.35229.0 |
| Windows SDK | 10.0.26100.0 |
| CMake | 3.31.6-msvc6 |
| Generator / architecture | Visual Studio 17 2022 / x64 |
| .NET Framework SDK 4.8 | Installed; editor build passed |

`CMakePresets.json` pins the standalone generator, architecture, MSVC toolset and SDK. The engine's installed `Windows_SDK.json` supports the selected compiler family, and UBT accepted the serviced compiler version 14.44.35229 in editor and packaged builds.

The supported package query resolved `Microsoft.VisualStudio.2022.BuildTools` to `17.14.41`, downloaded the installer from Microsoft and passed winget's SHA-256 check. Installation used the C++ workload with recommended components (MSVC, SDK and CMake), with no forced restart. The first attempt ended with exit 1602 while administrator consent was pending; the explicitly requested retry exited 0 and installed successfully. For another machine, the installation command is:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --accept-source-agreements --accept-package-agreements --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.Net.Component.4.8.SDK --add Microsoft.Net.Component.4.8.TargetingPack'
```

No reinstallation is needed. The owner completed the additional .NET SDK installation; `doctor` detects version 4.8 with no missing prerequisites. On another machine the editor command returns BLOCKED before running UBT if that SDK is absent. See [Microsoft's installer parameters](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=vs-2022) and [Epic's installation instructions](https://dev.epicgames.com/documentation/unreal-engine/install-unreal-engine).

Epic's current guidance lists VS 2026 for general UE 5.8 development, also supports VS 2022 17.14+, and lists Windows SDK 10.0.22621.0 minimum. Recheck the actual installed patch's requirements when locking versions. See [Epic toolchain guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-development-environment-for-cplusplus-projects-in-unreal-engine), [module boundaries](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-modules), and [editor Python](https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python).

## Later-phase review follow-ups

The previous planning review remains actionable before its owning phases:

1. P2: define committed undo when the original working blueprint was edited or replaced; test Build A -> Load B -> Undo A.
2. P2: preserve a recoverable resort identity for Save Blueprint before any Save Game, or explicitly resolve that workflow before implementation.
3. P1: use a small packaged editable/visual fixture decision before investing in the full preparation pipeline; retain P1.7/P1.8 full integration acceptance.
4. P4: the archived dependency note calling P4.3 deferrable weighted work is stale. P4.3 is REQUIRED individual-capacity/pending-demand handling; weighted simulation stays deferred.
5. P4: characterize effective clock behavior, including `DUAL_MICRO_RUNTIME_MULTIPLIER = 3`, rather than using the historical 40:1 illustration as current runtime behavior.

These notes do not implement P1-P6 or alter the original archive. See [preparation inventory](preparation-inventory.md) for the protected extraction surface. The building-placement question remains unrelated to P0.
