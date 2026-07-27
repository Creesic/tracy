// Native MCP "Streamable HTTP" server (replaces the Node mcp/integrator for HTTP).
// Protocol aligned with @modelcontextprotocol/sdk StreamableHTTPServerTransport (SSE over POST /mcp).

#include "McpStreamableHttp.hpp"

#include "bridge_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#    include <process.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <httplib.h>  // from third_party (see mcp/bridge/CMakeLists.txt)

namespace
{

constexpr const char* kServerName = "tracy-integrator-mcp";
constexpr const char* kDefaultProtocol = "2025-03-26";

const char* kSupportedProtocolVersions[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07" };

struct Session
{
    bool initialized = false;
    std::string protocolVersion;
};

std::mutex gSessionMu;
std::map<std::string, Session> gSessions;

nlohmann::json GenericObjectSchema()
{
    return nlohmann::json::object(
        { { "type", "object" },
          { "properties", nlohmann::json::object() },
          { "additionalProperties", true } } );
}

std::string NewSessionId()
{
    static thread_local std::mt19937 gen{ std::random_device{}() };
    std::uniform_int_distribution<int> d( 0, 15 );
    const char* h = "0123456789abcdef";
    std::string s( 36, '-' );
    for( int i = 0; i < 8; i++ ) s[i] = h[d( gen )];
    s[8] = '-';
    for( int i = 9; i < 13; i++ ) s[i] = h[d( gen )];
    s[13] = '-';
    s[14] = '4';
    for( int i = 15; i < 18; i++ ) s[i] = h[d( gen )];
    s[18] = '-';
    s[19] = h[( d( gen ) & 3 ) | 8];
    for( int i = 20; i < 23; i++ ) s[i] = h[d( gen )];
    s[23] = '-';
    for( int i = 24; i < 36; i++ ) s[i] = h[d( gen )];
    return s;
}

int64_t CurrentPid()
{
#if defined( _WIN32 )
    return (int64_t) GetCurrentProcessId();
#else
    return (int64_t) getpid();
#endif
}

std::string GetOurExe()
{
#if defined( _WIN32 )
    char buf[MAX_PATH];
    if( GetModuleFileNameA( NULL, buf, MAX_PATH ) == 0 ) return {};
    return std::string( buf );
#else
    char path[4096];
    const ssize_t n = readlink( "/proc/self/exe", path, sizeof( path ) - 1 );
    if( n <= 0 ) return {};
    path[n] = '\0';
    return std::string( path );
#endif
}

std::string FindManualRoot()
{
    if( const char* e = std::getenv( "TRACY_ROOT" ) )
    {
        if( *e ) return e;
    }
    std::error_code ec;
    for( const auto& base : { std::filesystem::current_path( ec ),
                              std::filesystem::path( GetOurExe() ).parent_path() } )
    {
        if( ec && base.empty() ) continue;
        std::filesystem::path walk = base;
        for( int i = 0; i < 16 && !walk.empty(); ++i )
        {
            if( std::filesystem::is_regular_file( walk / "manual" / "tracy.md" ) ) return walk.string();
            const auto up = walk.parent_path();
            if( up == walk ) break;
            walk = up;
        }
    }
    return std::filesystem::current_path( ec ).string();
}

bool ManualFound()
{
    const std::string r = FindManualRoot();
    if( r.empty() ) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file( std::filesystem::path( r ) / "manual" / "tracy.md", ec );
}

int64_t FileTimeToMs( const std::filesystem::file_time_type& t )
{
#if defined( _MSC_VER )
    (void) t;
    return 0; // MSVC <chrono> for file_time lacks portable to_sys in all toolsets
#else
    try
    {
        using namespace std::chrono;
        const auto sctp = std::filesystem::file_time_type::clock::to_sys( t );
        return (int64_t) duration_cast<milliseconds>( sctp.time_since_epoch() ).count();
    }
    catch( ... )
    {
        return 0;
    }
#endif
}

nlohmann::json ListTracyCaptures( const nlohmann::json& a )
{
    const std::string directory = a.value( "directory", "." );
    const bool rec = a.value( "recursive", true );
    int limit = a.value( "limit", 200 );
    if( limit < 1 ) limit = 1;
    if( limit > 5000 ) limit = 5000;

    const std::filesystem::path abs = std::filesystem::absolute( directory );
    nlohmann::json captures = nlohmann::json::array();
    size_t n = 0;
    std::error_code ec;

    const auto tryFile = [&]( const std::filesystem::path& p ) {
        if( n >= (size_t) limit ) return;
        if( !p.has_extension() || p.extension() != ".tracy" ) return;
        std::error_code e2;
        if( !std::filesystem::is_regular_file( p, e2 ) ) return;
        int64_t ms = 0;
        if( const auto ftime = std::filesystem::last_write_time( p, e2 ); !e2 ) ms = FileTimeToMs( ftime );
        captures.push_back( nlohmann::json( { { "path", p.string() },
            { "size", std::filesystem::file_size( p, e2 ) },
            { "mtimeMs", ms } } ) );
        n++;
    };

    if( !rec )
    {
        for( const auto& e : std::filesystem::directory_iterator( abs, ec ) )
        {
            if( n >= (size_t) limit ) break;
            tryFile( e.path() );
        }
    }
    else
    {
        for( const auto& e : std::filesystem::recursive_directory_iterator( abs, std::filesystem::directory_options::skip_permission_denied, ec ) )
        {
            if( n >= (size_t) limit ) break;
            if( e.is_regular_file() ) tryFile( e.path() );
        }
    }
    return nlohmann::json( { { "ok", true }, { "directory", abs.string() }, { "count", n }, { "captures", std::move( captures ) } } );
}

nlohmann::json BuildMcpStatus()
{
    const nlohmann::json pr = tracy_mcp_bridge::Dispatch( "ping", nlohmann::json::object() );
    const bool brOk = pr.value( "ok", false ) && pr.value( "pong", false );
    const bool cap = pr.value( "captureOpen", false );
    nlohmann::json openPath;
    if( pr.contains( "path" ) && pr["path"].is_string() && !pr["path"].get<std::string>().empty() )
    {
        openPath = pr["path"];
    }
    else
    {
        openPath = nullptr;
    }
    nlohmann::json j;
    j["ok"] = true;
    j["server"] = kServerName;
    j["version"] = tracy::mcp::kIntegratorVersion;
    j["transport"] = "streamable-http";
    j["tracyRoot"] = FindManualRoot();
    j["manualFound"] = ManualFound();
    j["bridgePath"] = GetOurExe();
    j["bridgePing"] = brOk;
    j["captureOpen"] = cap;
    j["openPath"] = openPath;
    j["integrator"] = "native";
    j["pid"] = CurrentPid();
    return j;
}

static const char* gInstructions = R"mcp(Tracy Profiler MCP: discover .tracy captures, open them in the native bridge, and run frame/zone/GPU/memory/lock/source analysis. Use streamable HTTP (this server). Set TRACY_ROOT and TRACY_MCP_BRIDGE for older Node integrators; this native build does not require Node.

**Open capture (avoid redundant work):** The Cursor chat often already knows the user's **focused editor file**. If it is a `.tracy` path, use that for `open_tracy_capture`—do not ask the user to paste a path. The Tracy GUI does not load the MCP session; only `open_tracy_capture` does. **Before** opening, call `tracy_mcp_status`: if `captureOpen` is true and `openPath` matches, skip `open_tracy_capture`.

**GUI frame numbers vs capture index:** `frame_index` is 0-based. The Tracy **title bar** may differ. Use `gui_frame_number` on `summarize_frame`, `compare_frames`, `get_frame_screenshot`, or `resolve_tracy_frame` / `list_frames` with guiFrameNumber and index. If both index and GUI number are passed, they must match.

**GUI vs MCP:** The MCP reads a `.tracy` file; it does not control live GUI zoom. Use `get_tracy_view_context` for that.

**Large captures:** (0) tracy_mcp_status (1) open_tracy_capture if needed (2) get_capture_metadata (3) list_frames / resolve_tracy_frame (4) summarize_frame_thread with known tid, or summarize_frame with low top_zones and detail=minimal (5) paginate search_zones and summarize_locks.)mcp";

struct ToolDef
{
    const char* name;
    const char* desc;
};

const ToolDef gTools[] = {
    { "tracy_mcp_status", "Call first: server readiness, open capture state, TRACY_ROOT, and bridge. Skip open if openPath already matches your file." },
    { "list_tracy_captures", "List .tracy files under a directory. Does not load traces; use open_tracy_capture first." },
    { "open_tracy_capture", "Load a .tracy file in the native bridge. Call tracy_mcp_status first; skip if already open at the right path." },
    { "get_capture_metadata", "Metadata for the open capture: frame sets, memory pools, frameNumbering, onDemand, frameOffset." },
    { "list_frames", "Paginated frames with duration, time range, guiFrameNumber, uiKind." },
    { "resolve_tracy_frame", "Map GUI frame number to 0-based frame_index and prev/next." },
    { "get_tracy_view_context", "What MCP can mirror vs the live Tracy app." },
    { "find_slowest_frames", "Top slowest frames; includes guiFrameNumber." },
    { "compare_frames", "Compare two frames by source location aggregates." },
    { "list_frame_functions", "All source locations overlapping a frame; paginate, sort, optional thread." },
    { "compare_frame_functions", "Diff function/source locations between two frame windows." },
    { "summarize_frame", "Top CPU and GPU zones for one frame. Prefer gui_frame_number when the bar label is not the 0-based index." },
    { "summarize_frame_thread", "Top CPU zones for one thread in one frame." },
    { "search_zones", "Search zone source locations by substring; paginate." },
    { "get_zone_stats", "Aggregate stats for a source location id (src_loc)." },
    { "get_zone_instances", "Paginate zone instances for a src_loc." },
    { "get_zone_callstack", "Callstack for a zone instance (if recorded)." },
    { "compare_zone_between_frames", "Same aggregate compare as compare_frames, zone-style." },
    { "list_threads", "Threads with zone/message counts." },
    { "get_thread_timeline_summary", "Time span of CPU zones on a thread; cap with max_timeline_visits on huge captures." },
    { "find_thread_gaps_or_stalls", "Largest gaps between consecutive zone spans on a thread." },
    { "find_cross_thread_handoff", "User messages in a time window (weak coordination signal)." },
    { "list_gpu_contexts", "GPU contexts in the capture." },
    { "get_gpu_zone_stats", "Aggregate stats for a GPU source location." },
    { "compare_gpu_passes", "GPU zone time delta between two frames by src_loc." },
    { "find_new_or_missing_gpu_zones", "GPU zone delta / diff between two frames." },
    { "summarize_allocations", "Allocation summary for a memory pool in a time window." },
    { "find_allocation_spikes", "Largest single allocations in a pool." },
    { "diff_allocations_between_frames", "Allocation diff across two frame windows." },
    { "find_hot_allocation_sites", "Heuristic hot allocation pointers (pair with symbols in the GUI)." },
    { "summarize_locks", "Lock objects; paginate." },
    { "find_lock_contention_spikes", "Heuristic longest lock waits." },
    { "find_threads_blocked_by_lock", "List wait events for a lock id." },
    { "compare_lock_contention_between_frames", "Count lock wait events in two frame windows." },
    { "get_frame_screenshot", "Frame screenshot metadata; optional base64; max_pixels cap." },
    { "find_frame_by_visual_marker", "Search user messages by substring (text; not computer vision)." },
    { "get_source_locations_for_zone", "Resolve src_loc to file/function/line." },
    { "group_stats_by_source_file", "CPU zone time grouped by source file." },
    { "open_source_context_for_zone", "File/line plus optional source snippet from the trace." },
};

nlohmann::json ToolsList()
{
    nlohmann::json arr = nlohmann::json::array();
    for( const auto& t : gTools )
    {
        nlohmann::json j;
        j["name"] = t.name;
        j["description"] = t.desc;
        j["inputSchema"] = GenericObjectSchema();
        arr.push_back( std::move( j ) );
    }
    return arr;
}

std::string NegotiateProtocol( const nlohmann::json& initParams )
{
    if( !initParams.contains( "protocolVersion" ) || !initParams["protocolVersion"].is_string() ) return kDefaultProtocol;
    const std::string cl = initParams["protocolVersion"].get<std::string>();
    for( const char* s : kSupportedProtocolVersions )
    {
        if( cl == s ) return cl;
    }
    return kDefaultProtocol;
}

std::optional<std::string> FindBridgeMethod( const std::string& toolName )
{
    static const std::map<std::string, std::string> m = {
        { "open_tracy_capture", "open" },
        { "get_capture_metadata", "metadata" },
        { "list_frames", "frames.list" },
        { "resolve_tracy_frame", "frames.resolve" },
        { "get_tracy_view_context", "view.context" },
        { "find_slowest_frames", "frames.slowest" },
        { "compare_frames", "frames.compare" },
        { "list_frame_functions", "frames.list_functions" },
        { "compare_frame_functions", "frames.compare_functions" },
        { "summarize_frame", "frames.summarize" },
        { "summarize_frame_thread", "frames.summarize_thread" },
        { "search_zones", "zones.search" },
        { "get_zone_stats", "zones.stats" },
        { "get_zone_instances", "zones.instances" },
        { "get_zone_callstack", "zones.callstack" },
        { "compare_zone_between_frames", "zones.compare_frames" },
        { "list_threads", "threads.list" },
        { "get_thread_timeline_summary", "threads.timeline_summary" },
        { "find_thread_gaps_or_stalls", "threads.gaps" },
        { "find_cross_thread_handoff", "threads.handoff" },
        { "list_gpu_contexts", "gpu.contexts" },
        { "get_gpu_zone_stats", "gpu.zone_stats" },
        { "compare_gpu_passes", "gpu.compare_passes" },
        { "find_new_or_missing_gpu_zones", "gpu.diff_zones" },
        { "summarize_allocations", "memory.summarize" },
        { "find_allocation_spikes", "memory.spikes" },
        { "diff_allocations_between_frames", "memory.diff_frames" },
        { "find_hot_allocation_sites", "memory.hot_sites" },
        { "summarize_locks", "locks.summarize" },
        { "find_lock_contention_spikes", "locks.contention_spikes" },
        { "find_threads_blocked_by_lock", "locks.blocked_threads" },
        { "compare_lock_contention_between_frames", "locks.compare_frames" },
        { "get_frame_screenshot", "frame.image" },
        { "find_frame_by_visual_marker", "frame.find_marker" },
        { "get_source_locations_for_zone", "source.for_zone" },
        { "group_stats_by_source_file", "source.group_by_file" },
        { "open_source_context_for_zone", "source.open_context" },
    };
    const auto it = m.find( toolName );
    if( it == m.end() ) return std::nullopt;
    return it->second;
}

nlohmann::json ToolCall( const nlohmann::json& toolNameJ, nlohmann::json argsJ )
{
    if( !toolNameJ.is_string() ) return nlohmann::json( { { "isError", true },
        { "content", nlohmann::json::array( { nlohmann::json::object( { { "type", "text" },
            { "text", "invalid tool name" } } ) } ) } } );
    const std::string tname = toolNameJ.get<std::string>();
    nlohmann::json rawOut;

    if( tname == "tracy_mcp_status" )
    {
        rawOut = BuildMcpStatus();
    }
    else if( tname == "list_tracy_captures" )
    {
        try
        {
            rawOut = ListTracyCaptures( argsJ );
        }
        catch( const std::exception& e )
        {
            rawOut = nlohmann::json( { { "ok", false }, { "error", e.what() } } );
        }
    }
    else
    {
        const auto m = FindBridgeMethod( tname );
        if( !m )
        {
            return nlohmann::json( { { "isError", true },
                { "content", nlohmann::json::array( { nlohmann::json::object( { { "type", "text" },
                    { "text", "unknown tool: " + tname } } ) } ) } } );
        }
        if( tname == "open_tracy_capture" && argsJ.contains( "path" ) && argsJ["path"].is_string() )
        {
            const std::string p = argsJ["path"].get<std::string>();
            argsJ["path"] = std::filesystem::absolute( p ).string();
        }
        if( tname == "resolve_tracy_frame" )
        {
            if( argsJ.contains( "display_frame_number" ) && !argsJ.contains( "gui_frame_number" ) )
            {
                argsJ["gui_frame_number"] = argsJ["display_frame_number"];
            }
        }
        try
        {
            rawOut = tracy_mcp_bridge::Dispatch( *m, argsJ );
        }
        catch( const std::exception& e )
        {
            rawOut = nlohmann::json( { { "ok", false }, { "error", e.what() } } );
        }
    }

    nlohmann::json out;
    out["content"] = nlohmann::json::array( { nlohmann::json::object( { { "type", "text" },
        { "text", rawOut.dump() } } ) } );
    if( rawOut.is_object() && rawOut.value( "ok", false ) == false )
    {
        out["isError"] = true;
    }
    return out;
}

bool AcceptOk( const httplib::Request& req )
{
    const std::string a = req.get_header_value( "Accept" );
    return a.find( "application/json" ) != std::string::npos && a.find( "text/event-stream" ) != std::string::npos;
}

nlohmann::json JsonRpcError( int code, nlohmann::json id, int errCode, const char* message )
{
    nlohmann::json err;
    err["code"] = errCode;
    err["message"] = message;
    return nlohmann::json( { { "jsonrpc", "2.0" },
        { "id", id },
        { "error", std::move( err ) } } );
}

void SseEvent( std::string& out, const nlohmann::json& msg )
{
    out += "event: message\ndata: ";
    out += msg.dump();
    out += "\n\n";
}

bool IsRequest( const nlohmann::json& j )
{
    return j.is_object() && j.contains( "method" ) && j["method"].is_string() && j.contains( "id" );
}

nlohmann::json HandleJsonRpc( const nlohmann::json& j, const std::string& sessionId, Session& sess, bool isInitPath )
{
    if( j.contains( "method" ) && j["method"] == "initialize" && j.contains( "id" ) )
    {
        if( !isInitPath )
        {
            return JsonRpcError( 400, j["id"], -32600, "Invalid Request" );
        }
        const auto params = j.contains( "params" ) && j["params"].is_object() ? j["params"] : nlohmann::json::object();
        const std::string pv = NegotiateProtocol( params );
        sess.initialized = true;
        sess.protocolVersion = pv;
        nlohmann::json cap;
        cap["tools"] = nlohmann::json( { { "listChanged", true } } );
        nlohmann::json result;
        result["protocolVersion"] = pv;
        result["capabilities"] = std::move( cap );
        result["serverInfo"] = nlohmann::json( { { "name", kServerName },
            { "version", tracy::mcp::kIntegratorVersion } } );
        result["instructions"] = gInstructions;
        return nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", j["id"] },
            { "result", std::move( result ) } } );
    }
    if( j.contains( "method" ) && j["method"] == "ping" && j.contains( "id" ) )
    {
        nlohmann::json res;
        res["pong"] = true;
        return nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", j["id"] },
            { "result", std::move( res ) } } );
    }
    if( j.contains( "method" ) && j["method"] == "tools/list" && j.contains( "id" ) )
    {
        return nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", j["id"] },
            { "result", nlohmann::json( { { "tools", ToolsList() } } ) } } );
    }
    if( j.contains( "method" ) && j["method"] == "tools/call" && j.contains( "id" ) )
    {
        nlohmann::json args = nlohmann::json::object();
        if( j.contains( "params" ) && j["params"].is_object() && j["params"].contains( "arguments" ) )
        {
            args = j["params"]["arguments"];
        }
        nlohmann::json nameJ;
        if( j.contains( "params" ) && j["params"].is_object() && j["params"].contains( "name" ) )
        {
            nameJ = j["params"]["name"];
        }
        const nlohmann::json toolResult = ToolCall( nameJ, std::move( args ) );
        return nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", j["id"] },
            { "result", toolResult } } );
    }
    if( j.contains( "method" ) && ( std::string_view( j["method"].get<std::string>() ) ).substr( 0, 14 ) == "notifications/" )
    {
        return nlohmann::json();  // no JSON response (notification)
    }
    if( j.contains( "id" ) )
    {
        return JsonRpcError( 400, j["id"], -32601, "Method not found" );
    }
    return nlohmann::json();
}

void PostMcp( const httplib::Request& req, httplib::Response& res )
{
    if( !AcceptOk( req ) )
    {
        res.status = 406;
        res.set_header( "Content-Type", "application/json" );
        res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", nlohmann::json( { { "code", -32000 },
                { "message", "Not Acceptable: need application/json and text/event-stream" } } ) } } )
                           .dump(),
            "application/json" );
        return;
    }
    const std::string body = req.body;
    nlohmann::json raw;
    try
    {
        raw = nlohmann::json::parse( body );
    }
    catch( ... )
    {
        res.status = 400;
        res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", nlohmann::json( { { "code", -32700 },
                { "message", "parse error" } } ) } } )
                            .dump(),
            "application/json" );
        return;
    }
    nlohmann::json messages = raw.is_array() ? raw : nlohmann::json::array( { raw } );

    const std::string sidHdr = req.get_header_value( "mcp-session-id" );
    const bool isInit = messages.is_array() && !messages.empty() && messages[0].is_object() && messages[0].value( "method", "" ) == "initialize";
    if( isInit && sidHdr.size() > 0 )
    {
        res.status = 400;
        res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", nlohmann::json( { { "code", -32600 },
                { "message", "Invalid Request" } } ) } } )
                            .dump(),
            "application/json" );
        return;
    }
    if( !isInit )
    {
        if( sidHdr.empty() )
        {
            res.status = 400;
            res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
                { "id", nullptr },
                { "error", nlohmann::json( { { "code", -32000 },
                    { "message", "Mcp-Session-Id required" } } ) } } )
                                .dump(),
                "application/json" );
            return;
        }
        std::lock_guard<std::mutex> lk( gSessionMu );
        if( gSessions.find( sidHdr ) == gSessions.end() )
        {
            res.status = 404;
            res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
                { "id", nullptr },
                { "error", nlohmann::json( { { "code", -32001 },
                    { "message", "Session not found" } } ) } } )
                                .dump(),
                "application/json" );
            return;
        }
    }

    std::string newSid;
    std::lock_guard<std::mutex> lk2( gSessionMu );
    Session* sess = nullptr;
    if( isInit )
    {
        newSid = NewSessionId();
        gSessions[newSid] = Session{};
        sess = &gSessions[newSid];
    }
    else
    {
        sess = &gSessions[sidHdr];
    }
    if( !sess ) return;
    if( isInit && messages.size() != 1 )
    {
        res.status = 400;
        gSessions.erase( newSid );
        res.set_content( nlohmann::json( { { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", nlohmann::json( { { "code", -32600 },
                { "message", "only one init message" } } ) } } )
                            .dump(),
            "application/json" );
        return;
    }

    std::string sse;
    int onlyNotifications = 0, requests = 0;
    for( const auto& m : messages )
    {
        if( !m.is_object() ) continue;
        if( m.contains( "method" ) && m["method"] == "initialize" && m.contains( "id" ) )
        {
            const nlohmann::json out = HandleJsonRpc( m, isInit ? newSid : sidHdr, *sess, true );
            SseEvent( sse, out );
            requests++;
            continue;
        }
        if( IsRequest( m ) )
        {
            const nlohmann::json out = HandleJsonRpc( m, isInit ? newSid : sidHdr, *sess, false );
            if( !out.is_null() && !out.empty() )
            {
                SseEvent( sse, out );
                requests++;
            }
        }
        else
        {
            if( m.contains( "method" ) && ( std::string_view( m["method"].get<std::string>() ) ).substr( 0, 14 ) == "notifications/" )
            {
                onlyNotifications++;
            }
        }
    }
    if( requests == 0 && onlyNotifications == (int) messages.size() && messages.size() > 0 )
    {
        res.status = 202;
        return;
    }
    if( sse.empty() && requests == 0 )
    {
        res.status = 400;
        return;
    }
    res.status = 200;
    res.set_header( "Content-Type", "text/event-stream" );
    res.set_header( "Cache-Control", "no-cache" );
    if( isInit && !newSid.empty() ) res.set_header( "mcp-session-id", newSid );
    else if( !sidHdr.empty() ) res.set_header( "mcp-session-id", sidHdr );
    if( isInit && sess && !newSid.empty() ) res.set_header( "mcp-protocol-version", sess->protocolVersion );
    res.set_content( std::move( sse ), "text/event-stream" );
}

void SetupMcpServerRoutes( httplib::Server& svr )
{
    svr.Post( "/mcp", PostMcp );
    svr.Delete( "/mcp", []( const httplib::Request& req, httplib::Response& res ) {
        const std::string sid = req.get_header_value( "mcp-session-id" );
        if( sid.empty() )
        {
            res.status = 400;
            return;
        }
        std::lock_guard<std::mutex> lk( gSessionMu );
        gSessions.erase( sid );
        res.status = 200;
    } );
    svr.Get( "/mcp", []( const httplib::Request& req, httplib::Response& res ) {
        if( !AcceptOk( req ) && req.get_header_value( "Accept" ).find( "text/event-stream" ) == std::string::npos )
        {
            res.status = 406;
            return;
        }
        if( req.get_header_value( "Accept" ).find( "text/event-stream" ) == std::string::npos )
        {
            res.status = 406;
            return;
        }
        res.status = 200;
        res.set_header( "Content-Type", "text/event-stream" );
        res.set_header( "Cache-Control", "no-cache" );
        res.set_content( "event: message\ndata: {}\n\n", "text/event-stream" );
    } );
    const std::string serverHdr = std::string( "tracy-mcp-bridge " ) + tracy::mcp::kIntegratorVersion;
    svr.set_default_headers( { { "Server", serverHdr } } );
}

}  // namespace

namespace tracy
{
namespace mcp
{

namespace
{
std::mutex gInprocMtx;
std::shared_ptr<httplib::Server> gInprocSvr;
std::thread gInprocTh;
std::atomic<bool> gInprocListenFailed{ false };
}  // namespace

bool RunStreamableHttp( const char* portStr, const char* hostOrNull )
{
    int port = 0;
    try
    {
        port = std::stoi( portStr );
    }
    catch( ... )
    {
        return false;
    }
    if( port < 1 || port > 65535 ) return false;
    const std::string host = ( hostOrNull && *hostOrNull ) ? std::string( hostOrNull ) : "127.0.0.1";

    httplib::Server svr;
    SetupMcpServerRoutes( svr );
    return svr.listen( host, port );
}

bool IsStreamableHttpMcpRunning()
{
    std::lock_guard<std::mutex> lock( gInprocMtx );
    return gInprocSvr && gInprocSvr->is_running();
}

bool StartStreamableHttpInBackground( int port, const char* hostOrNull )
{
    if( port < 1 || port > 65535 ) return false;
    const std::string host = ( hostOrNull && *hostOrNull ) ? std::string( hostOrNull ) : "127.0.0.1";
    std::lock_guard<std::mutex> lock( gInprocMtx );
    if( gInprocSvr )
    {
        if( gInprocSvr->is_running() ) return true;
        if( gInprocTh.joinable() ) gInprocTh.join();
        gInprocSvr.reset();
    }
    gInprocListenFailed = false;
    gInprocSvr = std::make_shared<httplib::Server>();
    SetupMcpServerRoutes( *gInprocSvr );
    const std::shared_ptr<httplib::Server> s = gInprocSvr;
    gInprocTh = std::thread( [s, port, host]() {
        if( !s->listen( host, port ) ) gInprocListenFailed = true;
    } );
    for( int i = 0; i < 200; ++i )
    {
        if( gInprocListenFailed.load( std::memory_order_relaxed ) )
        {
            gInprocTh.join();
            gInprocSvr.reset();
            return false;
        }
        if( s->is_running() ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    if( gInprocListenFailed.load( std::memory_order_relaxed ) )
    {
        gInprocTh.join();
        gInprocSvr.reset();
        return false;
    }
    if( !s->is_running() )
    {
        gInprocSvr->stop();
        gInprocTh.join();
        gInprocSvr.reset();
        return false;
    }
    return true;
}

void StopStreamableHttpInBackground()
{
    std::lock_guard<std::mutex> lock( gInprocMtx );
    if( !gInprocSvr ) return;
    gInprocSvr->stop();
    if( gInprocTh.joinable() ) gInprocTh.join();
    gInprocSvr.reset();
}

}  // namespace mcp
}  // namespace tracy

