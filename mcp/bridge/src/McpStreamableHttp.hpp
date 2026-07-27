#pragma once

namespace tracy
{
namespace mcp
{
/** Binds streamable HTTP MCP to /mcp; blocks until the server stops. Returns true on clean listen. */
bool RunStreamableHttp( const char* portStr, const char* hostOrNull );
/** Start MCP on a background thread (for embedding in tracy-profiler). Stop with StopStreamableHttpInBackground. */
bool StartStreamableHttpInBackground( int port, const char* hostOrNull );
void StopStreamableHttpInBackground();
/** True if the in-process streamable HTTP server is listening (same process as the profiler). */
bool IsStreamableHttpMcpRunning();
/** Version string; keep in sync with the Node mcp/integrator package when changing behavior. */
inline constexpr const char kIntegratorVersion[] = "0.3.4";
}
}
