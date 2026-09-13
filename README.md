# temp-watch

**Matt's Temp Monitor** — a native ImGui desktop dashboard for Linux system health: CPU/GPU/NVMe thermals, fan speed, top CPU consumers, an AI diagnosis panel, and a couple of ad-hoc admin tools, all in one small always-on window.

Originally a terminal tool (`fan-watch`, in `main.cpp`), rewritten as a full GUI app (`temp-watch`, in `gui.cpp`) using [Dear ImGui](https://github.com/ocornut/imgui) + GLFW + OpenGL.

## Features

- **Thermals & Fan** — CPU Tctl, iGPU edge, NVMe, and fan RPM with colour-coded gauges (green/amber/red) and a 2-minute sparkline history
- **Top CPU Consumers** — live `ps`-based process list, sorted by CPU%, in a scrollable panel; right-click a PID to copy it
- **PID Inspector** — look up any PID on demand for its full command line, user, CPU%, and memory%
- **AI Diagnosis** — one-click system health analysis powered by Gemini (2.5 Flash, with automatic fallback)
- **Run Command** — fire off an ad-hoc shell command directly from the dashboard
- **Diagnostic Log** — in-app log window with copy-all/clear
- **Hardware / About tabs** — static hardware info and build/version details

## Requirements

- Linux (developed and tested on CachyOS/Arch, x86_64)
- [`lm_sensors`](https://github.com/lm-sensors/lm-sensors) (the `sensors` command) for thermal readings — run `sensors-detect` once if you haven't already
- `dmidecode` for memory info in the Hardware tab (some fields require root to populate fully)
- GLFW3 and OpenGL development libraries to build from source
- A [Gemini API key](https://aistudio.google.com/apikey) if you want to use the AI Diagnosis panel (optional — everything else works without one)

## Building

```bash
git clone --recurse-submodules https://github.com/mjdeiter/temp-watch.git
cd temp-watch
./build.sh
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init --recursive
```

`build.sh` compiles `gui.cpp` against the vendored Dear ImGui submodule and bakes the current git short hash into the About window. The result is a single binary, `temp-watch`, in the project root.

### Legacy CLI version

The original terminal-only tool still builds separately from `main.cpp`:

```bash
g++ -std=c++17 -O2 -o fan-watch main.cpp
```

It prints the same thermal/process snapshot to stdout with ANSI colour, no GUI dependencies required.

## Running

```bash
./temp-watch
```

For the AI Diagnosis panel, put your Gemini API key in `~/.config/gemini_api_key` (plain text, no quotes) before launching.

## Versioning

Version and changelog are tracked in-app (see the **About** tab) and mirrored in [CHANGELOG.md](CHANGELOG.md).

## License

No license has been chosen yet for this project — all rights reserved by default until one is added.
