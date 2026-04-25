import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import { TracyBridgeClient } from "./bridge-client.js";
import { registerTraceTools, resolveBridgeExecutable } from "./trace-tools.js";

export const SERVER_NAME = "tracy-integrator-mcp";
export const SERVER_VERSION = "0.3.4";

export type TracyMcpContext = {
  mcp: McpServer;
  tracyRoot: string;
  manualFound: boolean;
  bridge: TracyBridgeClient | null;
  bridgePath: string | null;
};

export function resolveTracyRoot(): { root: string; manualFound: boolean } {
  const fromEnv = process.env.TRACY_ROOT?.trim();
  if (fromEnv) {
    const resolved = path.resolve(fromEnv);
    const manual = path.join(resolved, "manual", "tracy.md");
    return { root: resolved, manualFound: fs.existsSync(manual) };
  }

  let dir = process.cwd();
  for (let i = 0; i < 12; i++) {
    const manual = path.join(dir, "manual", "tracy.md");
    if (fs.existsSync(manual)) {
      return { root: dir, manualFound: true };
    }
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }

  const pkgDir = path.dirname(fileURLToPath(import.meta.url));
  dir = path.resolve(pkgDir, "..", "..", "..");
  const manual = path.join(dir, "manual", "tracy.md");
  if (fs.existsSync(manual)) {
    return { root: dir, manualFound: true };
  }

  return { root: process.cwd(), manualFound: false };
}

export function createTracyMcpContext(): TracyMcpContext {
  const { root: tracyRoot, manualFound } = resolveTracyRoot();
  const bridgePath = resolveBridgeExecutable(tracyRoot);
  const bridge = bridgePath ? new TracyBridgeClient(bridgePath) : null;

  const mcp = new McpServer(
    { name: SERVER_NAME, version: SERVER_VERSION },
    {
      instructions: `Tracy Profiler MCP: discover .tracy captures, open them in the native bridge, and run frame/zone/GPU/memory/lock/source analysis. Prefer HTTP transport (TRACY_MCP_HTTP_PORT) for large responses. Set TRACY_ROOT and TRACY_MCP_BRIDGE as needed.

**Open capture (avoid redundant work):** The Cursor chat often already knows the user’s **focused editor file**. If it is a \`.tracy\` path, use that as the path for \`open_tracy_capture\`—do not ask the user to paste a path. Opening a file in the Tracy *GUI* does **not** load the MCP bridge; only \`open_tracy_capture\` does. **Before** opening, call \`tracy_mcp_status\`: if \`captureOpen\` is true and \`openPath\` matches the file you need (same file), **skip** \`open_tracy_capture\` and go straight to analysis tools.

**GUI frame numbers vs capture index:** \`frame_index\` is 0-based in **capture order** (same as \`list_frames\` → \`index\`). The Tracy **title bar** often shows a different number (on-demand, \`frameOffset\`, main vs named frame set). **Do not** put the title bar number in \`frame_index\`—use \`gui_frame_number\` on tools like \`summarize_frame\`, \`compare_frames\`, and \`get_frame_screenshot\`, or call \`resolve_tracy_frame\` / read \`list_frames\` → \`guiFrameNumber\` + \`index\`. (If you pass both, they must match.) get_capture_metadata has \`frameNumbering\`, \`frameOffset\`, and \`onDemand\`.

**GUI vs MCP (live window):** The Tracy app and this MCP use the same trace file, but the MCP does not drive zoom, view span, ruler, or next/prev. Use get_tracy_view_context for that. Responses from summarize_frame / compare_frames echo \`guiFrameNumber\` when applicable.

**Function coverage across frames:** use list_frame_functions to enumerate all zone source locations in a frame (call counts and overlap), and compare_frame_functions to see functions only in one of two frames or call-count deltas—useful when comparing states across a transition.

Workflow for large captures (keeps MCP payloads small): (0) tracy_mcp_status to see if the right file is already open, (1) open_tracy_capture only if needed, (2) get_capture_metadata, (3) list_frames or resolve_tracy_frame, (4) prefer summarize_frame_thread with a known tid on huge captures, or summarize_frame with low top_zones and detail=minimal, (5) paginate search_zones and summarize_locks. get_frame_screenshot defaults to metadata only.`,
    },
  );

  mcp.registerTool(
    "tracy_mcp_status",
    {
      description:
        "Call first: server readiness, whether the native bridge has a .tracy file loaded (captureOpen, openPath), TRACY_ROOT, transport, and bridge path. If openPath already matches the capture you need, skip open_tracy_capture.",
    },
    async () => {
      let bridgePing: boolean | null = null;
      let captureOpen: boolean | null = null;
      let openPath: string | null = null;
      if (bridge) {
        try {
          const r = (await bridge.request("ping", {}, 5_000)) as {
            ok?: boolean;
            pong?: boolean;
            captureOpen?: boolean;
            path?: string | null;
          };
          bridgePing = r?.ok === true && r?.pong === true;
          if (r?.ok) {
            captureOpen = r.captureOpen === true;
            openPath = typeof r.path === "string" && r.path.length > 0 ? r.path : null;
          }
        } catch {
          bridgePing = false;
        }
      }
      const payload = {
        ok: true,
        server: SERVER_NAME,
        version: SERVER_VERSION,
        transport: process.env.TRACY_MCP_HTTP_PORT ? "streamable-http" : "stdio",
        tracyRoot,
        manualFound,
        bridgePath,
        bridgePing,
        captureOpen,
        openPath,
        pid: process.pid,
        node: process.version,
      };
      return {
        content: [{ type: "text", text: JSON.stringify(payload) }],
      };
    },
  );

  registerTraceTools(mcp, bridge);

  return { mcp, tracyRoot, manualFound, bridge, bridgePath };
}
