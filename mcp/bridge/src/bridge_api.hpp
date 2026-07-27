#pragma once

#include <nlohmann/json.hpp>
#include <string>

// Public entry for the JSONL bridge methods (open, metadata, …). See bridge.cpp.
namespace tracy_mcp_bridge
{
nlohmann::json Dispatch( const std::string& method, const nlohmann::json& params );
void WriteLine( const nlohmann::json& j );
}
