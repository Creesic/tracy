import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { createTracyMcpContext } from "./tracy-mcp-setup.js";
import { runStreamableHttpServer } from "./http-server.js";

function main(): void {
  const httpPortRaw = process.env.TRACY_MCP_HTTP_PORT?.trim();
  if (httpPortRaw && /^\d+$/.test(httpPortRaw)) {
    const port = parseInt(httpPortRaw, 10);
    const host = process.env.TRACY_MCP_HTTP_HOST?.trim() || "127.0.0.1";
    void runStreamableHttpServer(port, host).catch((err) => {
      console.error(err);
      process.exitCode = 1;
    });
    return;
  }

  const { mcp } = createTracyMcpContext();
  const transport = new StdioServerTransport();
  void mcp.connect(transport);
}

main();
