# Tracy integrator MCP

[Model Context Protocol](https://modelcontextprotocol.io/) server for Cursor, OpenCode, Factory, and other MCP hosts. It discovers `.tracy` captures, spawns the native **`tracy-mcp-bridge`** (C++ `Worker` JSONL API), and exposes analysis tools for frames, CPU zones, threads, GPU, memory, locks, screenshots, and source locations.

**Transports:** **stdio** (default) or **Streamable HTTP** when `TRACY_MCP_HTTP_PORT` is set. Prefer HTTP for large trace payloads (avoids stdio pipe issues). The Tracy Profiler **Tracy Assist → Settings → Integrator MCP (HTTP)** panel can show the endpoint URL and launch the Node process.

## Prerequisites

- Node.js 18+
- **Built `tracy-mcp-bridge`** (see below). Without it, `list_tracy_captures` still works; tools that need an open capture return a clear error.

## Build the TypeScript server

```bash
cd mcp/integrator
npm install
npm run build
```

Entry point: `dist/index.js`.

## Build the native bridge

From the Tracy repo root (or any build dir you prefer):

```bash
cmake -S mcp/bridge -B mcp/bridge/build
cmake --build mcp/bridge/build --config Release
```

Typical output paths:

| Platform | Executable |
|----------|------------|
| Windows (MSVC) | `mcp/bridge/build/Release/tracy-mcp-bridge.exe` |
| Windows (single-config) | `mcp/bridge/build/tracy-mcp-bridge.exe` |
| Linux / macOS | `mcp/bridge/build/tracy-mcp-bridge` |

The integrator auto-detects `mcp/bridge/build/Release/` and `mcp/bridge/build/Debug/` on Windows, and `mcp/bridge/build/` otherwise. Override with **`TRACY_MCP_BRIDGE`**.

## Environment

| Variable | Purpose |
|----------|---------|
| `TRACY_ROOT` | Absolute path to the Tracy repository root (directory that contains `manual/tracy.md`). If unset, the server walks `cwd` and parents, then relative to the package. |
| `TRACY_MCP_BRIDGE` | Full path to the `tracy-mcp-bridge` executable. Use this if CMake output lives elsewhere. |
| `TRACY_MCP_HTTP_PORT` | If set to a numeric port, the server listens on **`http://127.0.0.1:<port>/mcp`** (Streamable HTTP) instead of stdio. |
| `TRACY_MCP_HTTP_HOST` | Optional bind address (default `127.0.0.1`). |

### HTTP mode (example)

```bash
# PowerShell
$env:TRACY_MCP_HTTP_PORT="37512"
$env:TRACY_ROOT="C:/path/to/tracy"
$env:TRACY_MCP_BRIDGE="C:/path/to/tracy-mcp-bridge.exe"
node mcp/integrator/dist/index.js
```

Remote MCP clients should use `type: "remote"` / `url: "http://127.0.0.1:37512/mcp"` (or your host/port).

## Cursor

In **Cursor Settings → MCP**, add a server (adjust paths):

```json
{
  "mcpServers": {
    "tracy-integrator": {
      "command": "node",
      "args": [
        "C:/Users/you/Documents/GitHub/tracy/mcp/integrator/dist/index.js"
      ],
      "env": {
        "TRACY_ROOT": "C:/Users/you/Documents/GitHub/tracy",
        "TRACY_MCP_BRIDGE": "C:/Users/you/Documents/GitHub/tracy/mcp/bridge/build/Release/tracy-mcp-bridge.exe"
      }
    }
  }
}
```

If you omit `TRACY_MCP_BRIDGE`, ensure the default build layout exists. If you omit `TRACY_ROOT`, `cwd` should be the Tracy repo (or a parent) so `manual/tracy.md` is found.

## Tools (overview)

- **`tracy_mcp_status`** — `TRACY_ROOT`, manual file, bridge path, and bridge `ping`.
- **Captures** — `list_tracy_captures` (directory scan), `open_tracy_capture`, `get_capture_metadata`.
- **Frames** — `list_frames`, `find_slowest_frames`, `compare_frames`, `summarize_frame`.
- **CPU zones** — `search_zones`, `get_zone_stats`, `get_zone_instances`, `get_zone_callstack`, `compare_zone_between_frames`.
- **Threads** — `list_threads`, `get_thread_timeline_summary`, `find_thread_gaps_or_stalls`, `find_cross_thread_handoff`.
- **GPU** — `list_gpu_contexts`, `get_gpu_zone_stats`, `compare_gpu_passes`, `find_new_or_missing_gpu_zones`.
- **Memory** — `summarize_allocations`, `find_allocation_spikes`, `diff_allocations_between_frames`, `find_hot_allocation_sites`.
- **Locks** — `summarize_locks`, `find_lock_contention_spikes`, `find_threads_blocked_by_lock`, `compare_lock_contention_between_frames`.
- **Screenshots / markers** — `get_frame_screenshot` (base64 RGBA), `find_frame_by_visual_marker` (**message text correlation only** in v1; not pixel-level visual search).
- **Source** — `get_source_locations_for_zone`, `group_stats_by_source_file`, `open_source_context_for_zone` (paths and lines; opening local files is up to the host).

Responses may paginate or cap row counts to limit MCP context size; check tool results for `nextCursor` or `note` fields.

## Smoke test

From `mcp/integrator` after `npm run build`:

```bash
npm run smoke
```

Or:

```bash
npx @modelcontextprotocol/inspector node dist/index.js
```

## License

Same as the Tracy repository (BSD-3-Clause).
