## Learned User Preferences

- In OpenCode, configure MCP per server in `opencode.json`: use `type: "local"` with `command` and `environment` for stdio servers (e.g. Tracy `mcp/integrator`), and `type: "remote"` with `http://localhost:<port>/mcp` when the tool is served over HTTP.
- Prefer landing an integration scaffold (stdio MCP, wiring docs, minimal tools) before growing full capture-analysis coverage; expand from an explicit tool list when ready.
- For Tracy MCP on large captures, favor bounded tool outputs and stepwise queries instead of single-shot wide aggregations, so clients and connections are not overwhelmed.

## Learned Workspace Facts

- OpenCode MCP on this machine is configured in the global file `C:\Users\Tera\.config\opencode\opencode.json` (project-level `opencode.json` in the repo is not the source of truth).
- This repository is the Tracy Profiler: C++ profiler with GUI, server code, and `public/` client headers.
- Tracy Assist (in-app chat) uses `profiler/src/llm/tools.json` and C++ `TracyLlmTools` (manual RAG, optional web/wiki, source for the profiled app); MCP (`mcp/integrator` + bridge) is the path for external agents analyzing `.tracy` captures via the Worker.
- Saved `.tracy` files are binary captures parsed by the C++ `Worker` (`server/TracyWorker.cpp`); Node does not implement the trace format on its own.
- `mcp/integrator` is the Node MCP server (`tracy-integrator-mcp`); it spawns the native `tracy-mcp-bridge` built from `mcp/bridge` for Worker-backed queries. Communication between Node and the bridge is stdio JSONL. Set `TRACY_MCP_BRIDGE` when the bridge executable is not in the default CMake output path.
- Set `TRACY_MCP_HTTP_PORT` (and optionally `TRACY_MCP_HTTP_HOST`) so the integrator exposes MCP over Streamable HTTP at `http://<host>:<port>/mcp` instead of stdio—useful when tool responses are large. The Tracy Assist settings UI can enable this server, set the port, and show the URL (persisted in `tracy.ini` under `llm` / `integratorMcp*`).
- Optional `TRACY_ROOT` points the integrator at the repo root (directory containing `manual/tracy.md`) when the host working directory is not the clone.
- On very large captures, `tracy-mcp-bridge` may crash (e.g. Windows access violation 0xC0000005) on MCP tools that walk or aggregate the full zone timeline (frame compare/summarize, thread gaps); listing, search, and source-location-scoped stats are comparatively safer.
- Windows x64 Release builds align with `.github/workflows/windows.yml`: CMake per-tool build trees (e.g. `profiler/build`), `-DGIT_REV=...`, optional `CPM_SOURCE_CACHE` under the repo, and `pip install meson ninja` for dependency steps used by the workflow.
