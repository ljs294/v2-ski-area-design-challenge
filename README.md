# Ski Area Design Challenge

The Unreal replacement is at the P0 bootstrap stage. See [native bootstrap status and commands](docs/UnrealRebuild/README.md). The Electron instructions below remain usable while native preparation and cutover are unqualified.

Build and explore a ski resort on real terrain. Ski Area Design Challenge includes tools for lifts, trails, roads, ponds, and snowmaking, along with map layers, dashboards, weather, and guest simulation.

The desktop app uses Electron, React, and MapLibre. Start with the desktop version for the full experience.

## Requirements

- Node.js 22.12 or newer in the Node 22 release line, with npm. The project's CI uses Node 22.
- Git, or a downloaded and extracted copy of this repository.
- A graphical desktop with hardware graphics acceleration available.
- An internet connection to install dependencies and download terrain, map context, and weather data when preparing a mountain.

Windows is the configured portable packaging target. Development on other operating systems depends on their Electron support; this repository does not provide dedicated macOS or Linux packaging instructions.

## Run from source

Open a terminal in the repository folder—the folder containing `package.json`—and run:

```sh
npm ci
npm run dev
```

`npm ci` installs the versions recorded in the lockfile, including Electron. It may take several minutes on the first run. `npm run dev` starts the development server and opens the desktop app automatically. Keep the terminal open while using it; press Ctrl+C in that terminal to stop the development session.

On Windows, if PowerShell reports that `npm.ps1` cannot run because scripts are disabled, use the command wrappers instead:

```powershell
npm.cmd ci
npm.cmd run dev
```

The desktop app starts its local weather preparation service automatically. You normally do not need to start another service manually.

## Start a mountain

1. Create a new game from the main menu and follow the mountain setup prompts.
2. Allow terrain and weather preparation to finish. Download time depends on the selected area and provider availability.
3. Open **Toolbox** to build resort infrastructure and **Dashboards** to inspect the mountain's systems.
4. Use the top-right controls for layers, map colors, settings, and the game menu.
5. Save progress through the game menu. Use **Continue** or the load option to return to a saved mountain.

Click map features to inspect them. Imported lakes have properties including surface area, estimated depth, volume, and the option to use them for snowmaking.

Desktop saves and terrain packages are stored under Electron's application user-data directory, in `saves` and `terrains` subfolders. They are separate from the source checkout. Back up the whole application user-data directory if you want to preserve saves and their associated terrain and simulation data together.

## Build a desktop executable

To check types and build the desktop app:

```sh
npm run build
```

This produces renderer files in `dist/` and Electron files in `dist-electron/`. To launch that build directly:

```sh
npx electron .
```

To create the configured Windows portable executable, run on Windows:

```sh
npm run package
```

Packaging output goes into `release/`. Users of the resulting executable do not need Node.js or the source checkout. Packaging may download additional build tools.

## Browser version

The browser version uses the same app entrypoint but has different storage capabilities; desktop guest-simulation persistence and full application restart are desktop features.

```sh
npm run build:web
npx vite preview --config vite.config.web.ts --outDir dist-web
```

Open the URL printed by Vite, including the configured `/ski-area-design-challenge/` path. Serve the build over HTTP rather than opening `index.html` directly. Browser data is stored separately from desktop data; clearing browser site data can remove it.

If preparing weather data locally in the browser, start the weather service in a separate terminal:

```sh
npm run weather:service
```

Its default port is `8787`. The desktop app manages this service automatically.

## Developer console

In the desktop game, press **F10** or the **backtick** key to open the console. Type `help` to list commands.

| Command | Action |
| --- | --- |
| `restart-app` | Save progress, quit Electron, launch a new process, and automatically load the current mountain. |
| `restart` | Save progress and open the mountain in a fresh window within the current Electron process. |
| `time` | Show the current game timestamp. |
| `skip 30m` | Advance the clock by 30 minutes without simulating the skipped weather, snow, or guest events. |
| `clear` | Clear console output. |

Restart commands require an open saved mountain. When updating Electron or preload code, restart the desktop app manually once so the running process picks up the new code.

## Checks and developer tools

The opening Crystal Mountain scene includes local elevation and smoothed ESA WorldCover tiles (about 33 MB including regeneration sources), so it works without an internet connection. These are decorative assets, separate from your saved mountains.

To explicitly regenerate the background, install Playwright Chromium and run `npm run prepare:menu-background` with internet access. Run `npm run check:menu-background` to verify every bundled tile against its manifest. Normal builds copy the existing assets; they do not download background data. Source licenses and attribution are included in `public/menu-background/NOTICE.md`.

| Command | Purpose |
| --- | --- |
| `npm test` | Run deterministic offline unit tests. |
| `npm run typecheck` | Check TypeScript types. |
| `npm run lint` | Check code style and lint rules. |
| `npm run check` | Run the repository's aggregate deterministic checks and builds. |
| `npm run test:e2e` | Build and run browser smoke tests and feature workflows. |
| `npm run dev:lab` | Open the graphics development harness. |
| `npm run dev:weather-lab` | Start the weather development harness. |

Before running browser tests for the first time, install Chromium for Playwright:

```sh
npx playwright install chromium
```

See [docs/architecture.md](docs/architecture.md) for the application structure and [AGENTS.md](AGENTS.md) for repository working rules.

## Troubleshooting

- **Dependencies fail to install:** check your Node version with `node --version`, verify internet access, and run `npm ci` again. Electron must be allowed to download its binary for desktop development.
- **The app opens but mountain preparation fails:** check internet access and the terminal output. Terrain and weather preparation rely on external providers and can fail when those services are unavailable.
- **A port is already in use:** stop the previous development or weather-service session before starting another one.
- **A code change is not visible:** renderer changes normally update during development, but Electron process changes can require closing and restarting `npm run dev`.
