## Learned User Preferences

- In OpenCode, configure MCP per server in `opencode.json`: use `type: "local"` with `command` and `environment` for stdio servers (e.g. Tracy `mcp/integrator`), and `type: "remote"` with `http://localhost:<port>/mcp` when the tool is served over HTTP.
- Prefer landing an integration scaffold (stdio MCP, wiring docs, minimal tools) before growing full capture-analysis coverage; expand from an explicit tool list when ready.
- For Tracy MCP on large captures, favor bounded tool outputs and stepwise queries instead of single-shot wide aggregations, so clients and connections are not overwhelmed.
- Prefer shipping or using native `tracy-mcp-bridge` with Tracy so end users are not required to install Node or run `npm run build` in `mcp/integrator`.

## Learned Workspace Facts

- OpenCode MCP on this machine is configured in the global file `C:\Users\Tera\.config\opencode\opencode.json` (project-level `opencode.json` in the repo is not the source of truth).
- This repository is the Tracy Profiler: C++ profiler with GUI, server code, and `public/` client headers.
- Tracy Assist (in-app chat) uses `profiler/src/llm/tools.json` and C++ `TracyLlmTools` (manual RAG, optional web/wiki, source for the profiled app); external agents analyze `.tracy` captures through `tracy-mcp-bridge`, which uses the C++ `Worker` like the GUI server.
- Saved `.tracy` files are binary captures parsed by the C++ `Worker` (`server/TracyWorker.cpp`); Node does not implement the trace format on its own.
- `tracy-mcp-bridge` is built from `mcp/bridge`: in the default mode it speaks JSONL on stdio (used when the Node integrator spawns it). If `TRACY_MCP_HTTP_PORT` is set (and optionally `TRACY_MCP_HTTP_HOST`), the same executable serves Streamable HTTP MCP at `http://<host>:<port>/mcp` without Node. Tracy Assist can launch that native HTTP server when Integrator script is empty and it finds `tracy-mcp-bridge` beside the profiler or under `mcp/bridge/build` (Release/Debug on Windows). Set `TRACY_MCP_BRIDGE` when a non-default bridge path is needed or when the Node integrator spawns the bridge.
- `mcp/integrator` is an optional Node MCP server (`tracy-integrator-mcp`) for development or explicit script-based workflows; it talks to `tracy-mcp-bridge` over stdio JSONL.
- Tracy Assist settings can enable the MCP server, set the port, and show the URL (persisted in `tracy.ini` under `llm` / `integratorMcp*`).
- Optional `TRACY_ROOT` points the integrator or bridge at the repo root (directory containing `manual/tracy.md`) when the host working directory is not the clone.
- On very large captures, `tracy-mcp-bridge` may crash (e.g. Windows access violation 0xC0000005 / exit 3221225477) on MCP tools that walk or aggregate the full zone timeline (frame compare/summarize, thread gaps); listing, search, and source-location-scoped stats are comparatively safer.
- Building the main Tracy profiler (e.g. Windows x64 GUI) does not automatically rebuild `tracy-mcp-bridge`; after bridge or MCP changes, configure and build `mcp/bridge` separately.
- Windows x64 Release builds align with `.github/workflows/windows.yml`: CMake per-tool build trees (e.g. `profiler/build`), `-DGIT_REV=...`, optional `CPM_SOURCE_CACHE` under the repo, and `pip install meson ninja` for dependency steps used by the workflow.
