import { randomUUID } from "node:crypto";
import type { IncomingMessage, ServerResponse } from "node:http";

import { createMcpExpressApp } from "@modelcontextprotocol/sdk/server/express.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";

import { createTracyMcpContext, SERVER_NAME, SERVER_VERSION } from "./tracy-mcp-setup.js";

/**
 * Streamable HTTP MCP (stateful session). One Node process; clients send Mcp-Session-Id after init.
 * Binds to host/port — large tool payloads use HTTP bodies instead of stdio pipes.
 */
export async function runStreamableHttpServer(port: number, host: string): Promise<void> {
  const { mcp } = createTracyMcpContext();
  const transport = new StreamableHTTPServerTransport({
    sessionIdGenerator: () => randomUUID(),
  });
  await mcp.connect(transport);

  const app = createMcpExpressApp({ host });
  app.all("/mcp", async (req: IncomingMessage, res: ServerResponse) => {
    await transport.handleRequest(req, res, (req as IncomingMessage & { body?: unknown }).body);
  });

  await new Promise<void>((resolve, reject) => {
    try {
      const srv = app.listen(port, host, () => {
        const url = `http://${host}:${port}/mcp`;
        console.error(`${SERVER_NAME} ${SERVER_VERSION} — Streamable HTTP at ${url}`);
        resolve();
      });
      srv.on("error", reject);
    } catch (e) {
      reject(e);
    }
  });
}
