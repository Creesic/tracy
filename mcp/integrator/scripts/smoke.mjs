/**
 * Minimal MCP stdio handshake + tools/list (no extra dependencies).
 */
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const entry = path.join(root, "dist", "index.js");
const tracyRoot = path.resolve(root, "..", "..");

const child = spawn(process.execPath, [entry], {
  stdio: ["pipe", "pipe", "pipe"],
  cwd: tracyRoot,
  env: { ...process.env, TRACY_ROOT: tracyRoot },
});

let buf = "";
let id = 0;
const pending = new Map();

function send(msg) {
  child.stdin.write(`${JSON.stringify(msg)}\n`);
}

child.stdout.on("data", (chunk) => {
  buf += chunk.toString("utf8");
  let idx;
  while ((idx = buf.indexOf("\n")) !== -1) {
    const line = buf.slice(0, idx).replace(/\r$/, "");
    buf = buf.slice(idx + 1);
    if (!line.trim()) continue;
    const msg = JSON.parse(line);
    if (msg.id !== undefined && pending.has(msg.id)) {
      pending.get(msg.id)(msg);
      pending.delete(msg.id);
    }
  }
});

function request(method, params) {
  const reqId = ++id;
  const p = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`timeout: ${method}`)), 5000);
    pending.set(reqId, (msg) => {
      clearTimeout(t);
      if (msg.error) reject(new Error(JSON.stringify(msg.error)));
      else resolve(msg.result);
    });
  });
  send({ jsonrpc: "2.0", id: reqId, method, params });
  return p;
}

try {
  await request("initialize", {
    protocolVersion: "2024-11-05",
    capabilities: {},
    clientInfo: { name: "tracy-mcp-smoke", version: "0" },
  });
  send({
    jsonrpc: "2.0",
    method: "notifications/initialized",
    params: {},
  });
  const tools = await request("tools/list", {});
  const names = tools.tools.map((t) => t.name);
  if (!names.includes("tracy_mcp_status")) {
    throw new Error(`expected tracy_mcp_status in ${JSON.stringify(names)}`);
  }
  const call = await request("tools/call", {
    name: "tracy_mcp_status",
    arguments: {},
  });
  const text = call.content?.[0]?.text;
  if (!text || !text.includes('"ok": true')) {
    throw new Error(`unexpected tool result: ${text}`);
  }
  console.log("smoke ok:", names.join(", "));
} catch (e) {
  console.error(e);
  process.exitCode = 1;
} finally {
  child.kill();
}
