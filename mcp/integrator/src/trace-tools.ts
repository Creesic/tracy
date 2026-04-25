import fs from "node:fs";
import path from "node:path";

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { TracyBridgeClient } from "./bridge-client.js";

function textJson(data: unknown): { content: Array<{ type: "text"; text: string }> } {
  return { content: [{ type: "text", text: JSON.stringify(data) }] };
}

function needBridge(b: TracyBridgeClient | null): TracyBridgeClient {
  if (!b) {
    throw new Error(
      "Native tracy-mcp-bridge is not available. Build it (mcp/bridge CMake) and set TRACY_MCP_BRIDGE to the executable, or place it at the default path under mcp/bridge/build/.",
    );
  }
  return b;
}

async function br(b: TracyBridgeClient | null, method: string, params: Record<string, unknown> = {}): Promise<unknown> {
  return needBridge(b).request(method, params);
}

function findTracyCaptures(
  dir: string,
  recursive: boolean,
  limit: number,
): Array<{ path: string; size: number; mtimeMs: number }> {
  const out: Array<{ path: string; size: number; mtimeMs: number }> = [];
  const walk = (d: string): void => {
    if (out.length >= limit) return;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(d, { withFileTypes: true });
    } catch {
      return;
    }
    for (const ent of entries) {
      if (out.length >= limit) return;
      const p = path.join(d, ent.name);
      if (ent.isDirectory()) {
        if (recursive) walk(p);
      } else if (ent.isFile() && ent.name.endsWith(".tracy")) {
        const st = fs.statSync(p);
        out.push({ path: p, size: st.size, mtimeMs: st.mtimeMs });
      }
    }
  };
  walk(dir);
  return out;
}

function defaultBridgeCandidates(tracyRoot: string): string[] {
  if (process.platform === "win32") {
    return [
      path.join(tracyRoot, "mcp", "bridge", "build", "Release", "tracy-mcp-bridge.exe"),
      path.join(tracyRoot, "mcp", "bridge", "build", "Debug", "tracy-mcp-bridge.exe"),
    ];
  }
  return [path.join(tracyRoot, "mcp", "bridge", "build", "tracy-mcp-bridge")];
}

export function resolveBridgeExecutable(tracyRoot: string): string | null {
  const env = process.env.TRACY_MCP_BRIDGE?.trim();
  if (env && fs.existsSync(env)) return path.resolve(env);
  for (const c of defaultBridgeCandidates(tracyRoot)) {
    if (fs.existsSync(c)) return c;
  }
  return null;
}

/** Bridge accepts 0-based frame_index and/or gui_frame_number (Tracy title bar); on-demand captures often differ. */
const frameKeySingle = {
  frame_index: z.number().int().nonnegative().optional().describe("0-based in capture, same as list_frames index"),
  gui_frame_number: z
    .number()
    .int()
    .nonnegative()
    .optional()
    .describe("Tracy title bar / strip number — prefer this if you only know the GUI label"),
  display_frame_number: z.number().int().nonnegative().optional().describe("Alias for gui_frame_number"),
};

const frameKeyPairA = {
  frame_index_a: z.number().int().nonnegative().optional(),
  gui_frame_number_a: z.number().int().nonnegative().optional(),
  display_frame_number_a: z.number().int().nonnegative().optional(),
};

const frameKeyPairB = {
  frame_index_b: z.number().int().nonnegative().optional(),
  gui_frame_number_b: z.number().int().nonnegative().optional(),
  display_frame_number_b: z.number().int().nonnegative().optional(),
};

export function registerTraceTools(mcp: McpServer, bridge: TracyBridgeClient | null): void {

  mcp.registerTool(
    "list_tracy_captures",
    {
      description:
        "List .tracy capture files under a directory (optionally recursive). Does not load traces; use open_tracy_capture then get_capture_metadata for session details.",
      inputSchema: {
        directory: z.string().describe("Directory to scan"),
        recursive: z.boolean().optional().default(true),
        limit: z.number().int().positive().max(5000).optional().default(200),
      },
    },
    async ({ directory, recursive, limit }) => {
      const abs = path.resolve(directory);
      const captures = findTracyCaptures(abs, recursive ?? true, limit ?? 200);
      return textJson({ directory: abs, count: captures.length, captures });
    },
  );

  mcp.registerTool(
    "open_tracy_capture",
    {
      description:
        "Load a .tracy file into the native bridge (single process-wide session). Call tracy_mcp_status first: if captureOpen and openPath already match the file you need, skip this. Use the user’s focused editor path when it is a .tracy file—MCP does not auto-sync Tracy GUI. Required before other tools only when no suitable capture is open.",
      inputSchema: { path: z.string().describe("Absolute or relative path to .tracy file") },
    },
    async ({ path: capturePath }) => {
      const abs = path.resolve(capturePath);
      const r = await br(bridge, "open", { path: abs });
      return textJson(r);
    },
  );

  mcp.registerTool(
    "get_capture_metadata",
    {
      description:
        "Metadata for the open capture: counts, frame sets (with isMain), memory pools, frameOffset, onDemand, and frameNumbering note (how guiFrameNumber relates to Tracy’s top bar vs 0-based frame_index). Prefer summary_only or small caps on huge traces.",
      inputSchema: {
        summary_only: z.boolean().optional().default(false),
        frame_sets_max: z.number().int().positive().max(10000).optional().default(256),
        mem_pools_max: z.number().int().positive().max(10000).optional().default(256),
      },
    },
    async (p) => {
      const r = await br(bridge, "metadata", { ...p });
      return textJson(r);
    },
  );

  mcp.registerTool(
    "list_frames",
    {
      description:
        "List frames: duration, time range (startNs/endNs), and guiFrameNumber matching Tracy’s top bar / frame strip (unlike raw 0-based capture index when labels differ, e.g. on-demand). uiKind: tracy_init, missed_frames, frame, or named_set. Paginate with offset/limit.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        offset: z.number().int().nonnegative().optional().default(0),
        limit: z.number().int().positive().max(5000).optional().default(50),
      },
    },
    async (p) => textJson(await br(bridge, "frames.list", { ...p })),
  );

  mcp.registerTool(
    "resolve_tracy_frame",
    {
      description:
        "Map a frame number you see in the Tracy GUI (top bar) to the bridge’s 0-based frame_index and get prev/next for Step-back/Step-fwd—style coordination with the user. Pass gui_frame_number; see get_capture_metadata frameNumbering and get_tracy_view_context.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        gui_frame_number: z.number().int().nonnegative().optional(),
        display_frame_number: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => {
      const { display_frame_number: d, gui_frame_number: g, ...rest } = p;
      const params =
        d !== undefined
          ? { ...rest, gui_frame_number: d }
          : g !== undefined
            ? { ...rest, gui_frame_number: g }
            : { ...rest };
      return textJson(await br(bridge, "frames.resolve", params));
    },
  );

  mcp.registerTool(
    "get_tracy_view_context",
    {
      description:
        "Explains what this MCP can and cannot mirror vs the live Tracy app: it reads a .tracy file; it does not control zoom, view span, or next/prev in the running GUI, but you can still align on guiFrameNumber and startNs/endNs. Also returns firstTime/lastTime and frame offset flags.",
      inputSchema: {},
    },
    async () => textJson(await br(bridge, "view.context", {})),
  );

  mcp.registerTool(
    "find_slowest_frames",
    {
      description: "Find slowest frames by duration; each hit includes guiFrameNumber for the Tracy UI label.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        top_k: z.number().int().positive().max(500).optional().default(20),
        skip_warmup: z.number().int().nonnegative().optional().default(0),
      },
    },
    async (p) => textJson(await br(bridge, "frames.slowest", { ...p })),
  );

  mcp.registerTool(
    "compare_frames",
    {
      description:
        "Compare CPU zone time aggregates between two frame indices (by source location). You may use gui_frame_number_a/b (Tracy title bar) **instead of** frame_index_a/b when on-demand/offset means the bar number is not the capture index. Prefer small top_changes on large captures. Bridge stops after max_timeline_visits (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
        top_changes: z.number().int().positive().max(500).optional().default(20),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "frames.compare", { ...p })),
  );

  mcp.registerTool(
    "list_frame_functions",
    {
      description:
        "All source locations (zone names / functions) with any CPU or GPU work overlapping a frame. Pass gui_frame_number (title bar) or frame_index (list_frames index). Optional tid scopes CPU+GPU to that thread. Paginate with offset/limit; sort by calls, name, or src_loc.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeySingle,
        include_gpu: z.boolean().optional().default(true),
        tid: z.number().int().nonnegative().optional().default(0),
        sort: z.enum(["calls", "name", "src_loc"]).optional().default("calls"),
        offset: z.number().int().nonnegative().optional().default(0),
        limit: z.number().int().positive().max(10000).optional().default(500),
        detail: z.enum(["full", "minimal"]).optional().default("full"),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "frames.list_functions", { ...p })),
  );

  mcp.registerTool(
    "compare_frame_functions",
    {
      description:
        "Diff functions (source locations) between two frame windows. Use gui_frame_number_a/b for Tracy title bar numbers, or frame_index_a/b from list_frames. optional tid, only_in_both_with_call_deltas, max_per_section.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
        include_gpu: z.boolean().optional().default(true),
        tid: z.number().int().nonnegative().optional().default(0),
        max_per_section: z.number().int().positive().max(20000).optional().default(2000),
        only_in_both_with_call_deltas: z.boolean().optional().default(false),
        detail: z.enum(["full", "minimal"]).optional().default("full"),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "frames.compare_functions", { ...p })),
  );

  mcp.registerTool(
    "summarize_frame",
    {
      description:
        "Top CPU and GPU zones for one frame. **Prefer `gui_frame_number` (Tracy title bar) over `frame_index` when the user reads a large frame number** — on on-demand captures the bar is not the 0-based index. Omitted index defaults to frame 0. max_timeline_visits caps work (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeySingle,
        top_zones: z.number().int().positive().max(500).optional().default(15),
        detail: z.enum(["full", "minimal"]).optional().default("full"),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "frames.summarize", { ...p })),
  );

  mcp.registerTool(
    "summarize_frame_thread",
    {
      description:
        "Top CPU zones for one thread (tid) in one frame. Use gui_frame_number (title bar) or frame_index (list_frames). Does not include GPU. max_timeline_visits (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeySingle,
        tid: z.number().int().nonnegative(),
        top_zones: z.number().int().positive().max(500).optional().default(15),
        detail: z.enum(["full", "minimal"]).optional().default("full"),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "frames.summarize_thread", { ...p })),
  );

  mcp.registerTool(
    "search_zones",
    {
      description:
        "Search zone source locations by substring. Paginate with offset/limit; response includes total and truncated.",
      inputSchema: {
        query: z.string(),
        case_insensitive: z.boolean().optional().default(true),
        offset: z.number().int().nonnegative().optional().default(0),
        limit: z.number().int().positive().max(500).optional().default(50),
      },
    },
    async (p) => textJson(await br(bridge, "zones.search", { ...p })),
  );

  mcp.registerTool(
    "get_zone_stats",
    {
      description: "Aggregate statistics for a source location id (src_loc).",
      inputSchema: { src_loc: z.number().int() },
    },
    async (p) => textJson(await br(bridge, "zones.stats", { ...p })),
  );

  mcp.registerTool(
    "get_zone_instances",
    {
      description:
        "Paginated zone instances for a src_loc. Large offset walks from the start of the list; max_timeline_visits caps how many zone rows are walked (default 50M; 0 = unlimited).",
      inputSchema: {
        src_loc: z.number().int(),
        offset: z.number().int().nonnegative().optional().default(0),
        limit: z.number().int().positive().max(2000).optional().default(50),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "zones.instances", { ...p })),
  );

  mcp.registerTool(
    "get_zone_callstack",
    {
      description: "Callstack for a specific zone instance index (if recorded).",
      inputSchema: {
        src_loc: z.number().int(),
        instance_index: z.number().int().nonnegative().optional().default(0),
      },
    },
    async (p) => textJson(await br(bridge, "zones.callstack", { ...p })),
  );

  mcp.registerTool(
    "compare_zone_between_frames",
    {
      description: "Same as compare_frames. Supports gui_frame_number_a/b or frame_index_a/b. max_timeline_visits caps zone visits (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
        top_changes: z.number().int().positive().max(500).optional().default(20),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "zones.compare_frames", { ...p })),
  );

  mcp.registerTool(
    "list_threads",
    {
      description: "Threads with zone/message counts.",
      inputSchema: {},
    },
    async () => textJson(await br(bridge, "threads.list", {})),
  );

  mcp.registerTool(
    "get_thread_timeline_summary",
    {
      description:
        "Time span covered by CPU zones on a thread (tid). Scans the full timeline; max_timeline_visits caps visits (default 50M; 0 = unlimited).",
      inputSchema: {
        tid: z.number().int().nonnegative(),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "threads.timeline_summary", { ...p })),
  );

  mcp.registerTool(
    "find_thread_gaps_or_stalls",
    {
      description:
        "Largest gaps between consecutive zone spans on one thread (top_k by gap size). Scans the full thread timeline; only top_k gaps are kept in memory. max_timeline_visits stops the scan early (default 50M; 0 = unlimited).",
      inputSchema: {
        tid: z.number().int().nonnegative(),
        min_gap_ns: z.number().int().positive().optional().default(1_000_000),
        top_k: z.number().int().positive().max(500).optional().default(30),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "threads.gaps", { ...p })),
  );

  mcp.registerTool(
    "find_cross_thread_handoff",
    {
      description: "Messages in a time window (weak signal for cross-thread coordination).",
      inputSchema: {
        start_ns: z.number().int().optional(),
        end_ns: z.number().int().optional(),
        limit: z.number().int().positive().max(2000).optional().default(100),
      },
    },
    async (p) => textJson(await br(bridge, "threads.handoff", { ...p })),
  );

  mcp.registerTool(
    "list_gpu_contexts",
    {
      description: "GPU contexts in the capture.",
      inputSchema: {},
    },
    async () => textJson(await br(bridge, "gpu.contexts", {})),
  );

  mcp.registerTool(
    "get_gpu_zone_stats",
    {
      description: "Aggregate stats for GPU zones at a source location.",
      inputSchema: { src_loc: z.number().int() },
    },
    async (p) => textJson(await br(bridge, "gpu.zone_stats", { ...p })),
  );

  mcp.registerTool(
    "compare_gpu_passes",
    {
      description:
        "GPU zone time delta between two frames (by src_loc). Use gui_frame_number_a/b (title bar) or frame_index_a/b (list_frames). max_timeline_visits caps GPU zone visits (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
        top_changes: z.number().int().positive().max(500).optional().default(20),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "gpu.compare_passes", { ...p })),
  );

  mcp.registerTool(
    "find_new_or_missing_gpu_zones",
    {
      description:
        "Same delta view as compare_gpu_passes. Supports gui_frame_number_a/b or frame_index_a/b. max_timeline_visits caps GPU zone visits (default 50M; 0 = unlimited).",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
        top_changes: z.number().int().positive().max(500).optional().default(20),
        max_timeline_visits: z.number().int().nonnegative().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "gpu.diff_zones", { ...p })),
  );

  mcp.registerTool(
    "summarize_allocations",
    {
      description: "Summarize allocation activity for a memory pool in a time window (pool_name_id 0 = default).",
      inputSchema: {
        pool_name_id: z.number().int().nonnegative().optional().default(0),
        start_ns: z.number().int().optional(),
        end_ns: z.number().int().optional(),
      },
    },
    async (p) => textJson(await br(bridge, "memory.summarize", { ...p })),
  );

  mcp.registerTool(
    "find_allocation_spikes",
    {
      description: "Largest single allocations in a pool.",
      inputSchema: {
        pool_name_id: z.number().int().nonnegative().optional().default(0),
        top_k: z.number().int().positive().max(500).optional().default(20),
      },
    },
    async (p) => textJson(await br(bridge, "memory.spikes", { ...p })),
  );

  mcp.registerTool(
    "diff_allocations_between_frames",
    {
      description: "Allocation/free counts and bytes in each of two frame windows. Use gui_frame_number_a/b or frame_index_a/b.",
      inputSchema: {
        pool_name_id: z.number().int().nonnegative().optional().default(0),
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
      },
    },
    async (p) => textJson(await br(bridge, "memory.diff_frames", { ...p })),
  );

  mcp.registerTool(
    "find_hot_allocation_sites",
    {
      description: "Hot pointers by aggregated size (heuristic; pair with symbols in GUI).",
      inputSchema: {
        pool_name_id: z.number().int().nonnegative().optional().default(0),
        top_k: z.number().int().positive().max(500).optional().default(30),
      },
    },
    async (p) => textJson(await br(bridge, "memory.hot_sites", { ...p })),
  );

  mcp.registerTool(
    "summarize_locks",
    {
      description: "Lock objects and timeline sizes. Paginate with offset/limit; response includes total and truncated.",
      inputSchema: {
        offset: z.number().int().nonnegative().optional().default(0),
        limit: z.number().int().positive().max(500).optional().default(50),
      },
    },
    async (p) => textJson(await br(bridge, "locks.summarize", { ...p })),
  );

  mcp.registerTool(
    "find_lock_contention_spikes",
    {
      description: "Heuristic longest lock waits (wait paired with next obtain on same thread).",
      inputSchema: { top_k: z.number().int().positive().max(500).optional().default(20) },
    },
    async (p) => textJson(await br(bridge, "locks.contention_spikes", { ...p })),
  );

  mcp.registerTool(
    "find_threads_blocked_by_lock",
    {
      description: "List wait events for a lock id.",
      inputSchema: { lock_id: z.number().int().nonnegative() },
    },
    async ({ lock_id }) => textJson(await br(bridge, "locks.blocked_threads", { lock_id })),
  );

  mcp.registerTool(
    "compare_lock_contention_between_frames",
    {
      description: "Count lock wait events in each frame window. Use gui_frame_number_a/b or frame_index_a/b.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeyPairA,
        ...frameKeyPairB,
      },
    },
    async (p) => textJson(await br(bridge, "locks.compare_frames", { ...p })),
  );

  mcp.registerTool(
    "get_frame_screenshot",
    {
      description:
        "Frame screenshot metadata (width/height/bytes). Prefer gui_frame_number (title bar) over frame_index when they differ. Set include_base64=true to fetch base64; max_pixels caps decoded size.",
      inputSchema: {
        frame_set: z.number().int().nonnegative().optional().default(0),
        ...frameKeySingle,
        include_base64: z.boolean().optional().default(false),
        max_pixels: z.number().int().positive().max(50_000_000).optional().default(2_000_000),
      },
    },
    async (p) => textJson(await br(bridge, "frame.image", { ...p })),
  );

  mcp.registerTool(
    "find_frame_by_visual_marker",
    {
      description:
        "Search user messages by substring (v1: text only, not pixel CV). Validates frame_set exists.",
      inputSchema: {
        substring: z.string().optional().default(""),
        frame_set: z.number().int().nonnegative().optional().default(0),
      },
    },
    async (p) => textJson(await br(bridge, "frame.find_marker", { ...p })),
  );

  mcp.registerTool(
    "get_source_locations_for_zone",
    {
      description: "Resolve src_loc to file/function/line.",
      inputSchema: { src_loc: z.number().int() },
    },
    async (p) => textJson(await br(bridge, "source.for_zone", { ...p })),
  );

  mcp.registerTool(
    "group_stats_by_source_file",
    {
      description: "Aggregate CPU zone time grouped by source file.",
      inputSchema: { top_files: z.number().int().positive().max(500).optional().default(30) },
    },
    async (p) => textJson(await br(bridge, "source.group_by_file", { ...p })),
  );

  mcp.registerTool(
    "open_source_context_for_zone",
    {
      description: "File/line for src_loc plus optional cached source snippet from the trace.",
      inputSchema: {
        src_loc: z.number().int(),
        context_lines: z.number().int().positive().max(40).optional().default(4),
      },
    },
    async (p) => textJson(await br(bridge, "source.open_context", { ...p })),
  );
}
