# Changelog

All notable changes to this project are documented here. This file mirrors the in-app changelog shown in the About tab.

## [1.6.0] - 2026-09-12
- Process list: now scrollable (fixed-height child window, mouse wheel + scrollbar)
- Process list: fetches all running processes, not just top 14
- Process list: removed 0.3% CPU cutoff so idle processes are visible too

## [1.5.0] - 2026-04-24
- AI: fallback chain corrected: gemini-2.5-flash -> gemini-2.5-flash-lite
- AI: git hash embedded in About window (build.sh)
- Build: build.sh bakes git short hash at compile time
- Build: git repo initialized, releases tagged

## [1.4.0] - 2026-04-24
- AI: removed thinkingConfig (caused 400 on every request)
- AI: non-streaming generateContent; error display in red
- AI: model fallback with retry; animated spinner
- Process list: removed mini bar clipping names

## [1.3.0] - 2025-04-20
- AI: fixed gemini-2.5-flash token starvation (thinkingBudget=0, 2048 tok)
- AI: word-wrap display (BeginChild + PushTextWrapPos)
- AI: Copy response button; prompt now includes load avg + memory
- AI: always sends top 10 procs regardless of CPU%
- PID Inspector: selectable text + Copy button

## [1.2.0] - 2025-04-19
- Thermometer taskbar icon (procedural 32x32, no external file)
- Diagnostic Log window with copy-all and clear
- Background refresh thread with configurable interval
- Sparkline history graphs (2-min ring buffer)

## [1.1.0] - 2025-04-19
- AI Diagnosis panel powered by Gemini 2.5 Flash (SSE streaming)
- Run Command panel for ad-hoc shell commands
- PID Inspector: look up any PID on demand

## [1.0.0] - 2025-04-19
- Initial ImGui release: thermals, fan, top CPU consumers
- Colour-coded gauges (green/amber/red thresholds)
- Replaced terminal fan-watch with full GUI
