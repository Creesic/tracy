#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined( _WIN32 ) && defined( _MSC_VER )
#    include <excpt.h>
#endif

#include <nlohmann/json.hpp>

#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"

#include "bridge_api.hpp"
#include "McpStreamableHttp.hpp"

namespace tracy_mcp_bridge
{

std::unique_ptr<tracy::Worker> g_worker;
std::string g_openPath;

nlohmann::json Err( const std::string& msg )
{
    return { { "ok", false }, { "error", msg } };
}

nlohmann::json Ok( nlohmann::json data = {} )
{
    data["ok"] = true;
    return data;
}

void WriteLine( const nlohmann::json& j )
{
    std::cout << j.dump() << '\n';
    std::cout.flush();
}

tracy::Worker* RequireWorker()
{
    if( !g_worker ) throw std::runtime_error( "no capture open; call open first" );
    return g_worker.get();
}

void WaitSourceZones( tracy::Worker& w )
{
    for( int i = 0; i < 120000 && !w.AreSourceLocationZonesReady(); ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    if( !w.AreSourceLocationZonesReady() ) throw std::runtime_error( "timeout waiting for source location zones" );
}

const tracy::FrameData* GetFrameSet( tracy::Worker& w, size_t idx )
{
    const auto& fv = w.GetFrames();
    if( idx >= fv.size() ) throw std::runtime_error( "frame_set index out of range" );
    return fv[idx];
}

std::string FrameSetLabel( const tracy::Worker& w, const tracy::FrameData& fd )
{
    auto ex = w.GetExternalName( fd.name );
    if( ex.first && ex.first[0] ) return ex.first;
    if( ex.second && ex.second[0] ) return ex.second;
    return std::string( w.GetString( fd.name ) );
}

// Same numbering as Tracy View::GetFrameNumber (TracyView_Utility.cpp) so MCP matches the top bar / frame strip.
uint64_t GuiFrameNumber( const tracy::Worker& w, const tracy::FrameData& fd, size_t i )
{
    if( fd.name == 0 )
    {
        const uint64_t offset = w.GetFrameOffset();
        if( offset == 0 ) return uint64_t( i );
        return uint64_t( i ) + offset - 1;
    }
    return uint64_t( i ) + 1;
}

const char* FrameUiKind( const tracy::Worker& w, const tracy::FrameData& fd, size_t i )
{
    if( fd.name == 0 )
    {
        if( i == 0 ) return "tracy_init";
        if( i == 1 && w.IsOnDemand() ) return "missed_frames";
        return "frame";
    }
    return "named_set";
}

// Inverse of GuiFrameNumber: map a number shown in the GUI to capture-local frame index (frame_index / frames.list "index").
std::optional<size_t> TryFrameIndexFromGuiNumber( const tracy::Worker& w, const tracy::FrameData& fd, size_t n, uint64_t gui )
{
    if( fd.name == 0 )
    {
        const uint64_t o = w.GetFrameOffset();
        if( o == 0 )
        {
            if( gui >= n ) return std::nullopt;
            return size_t( gui );
        }
        const int64_t i = int64_t( gui ) - int64_t( o ) + 1;
        if( i < 0 || size_t( i ) >= n ) return std::nullopt;
        return size_t( i );
    }
    if( gui < 1 || gui > n ) return std::nullopt;
    return size_t( gui - 1 );
}

// 0-based capture index: use frame_index, or gui_frame_number / display_frame_number (Tracy title bar) when present.
// If both index and gui are provided, they must match (guards against passing the bar number as frame_index by mistake).
static size_t ResolveFrameIndexParam( tracy::Worker& w, const tracy::FrameData& fd, const nlohmann::json& p, const char* indexKey, const char* guiKey, const char* dispKey )
{
    const size_t n = w.GetFullFrameCount( fd );
    const bool hasGui = p.contains( guiKey ) && !p[guiKey].is_null();
    const bool hasDisp = dispKey && p.contains( dispKey ) && !p[dispKey].is_null();
    if( hasGui || hasDisp )
    {
        const uint64_t gui = hasGui ? p.at( guiKey ).get<uint64_t>() : p.at( dispKey ).get<uint64_t>();
        const auto opt = TryFrameIndexFromGuiNumber( w, fd, n, gui );
        if( !opt )
        {
            throw std::runtime_error( std::string( "invalid " ) + guiKey
                + ": on-demand captures map the Tracy title bar to a small capture index (list_frames 'index', not the bar value used as frame_index)" );
        }
        if( p.contains( indexKey ) && !p[indexKey].is_null() )
        {
            const size_t j = p.at( indexKey ).get<uint64_t>();
            if( j != *opt ) throw std::runtime_error( std::string( indexKey ) + " does not match " + guiKey + " (remove frame_index to use the GUI number only)" );
        }
        return *opt;
    }
    const size_t idx = p.value( indexKey, 0ull );
    if( idx >= n ) throw std::runtime_error( std::string( indexKey ) + " out of range" );
    return idx;
}

int64_t GetZoneChildTimeFast( const tracy::Worker& worker, const tracy::ZoneEvent& zone )
{
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(tracy::Vector<tracy::ZoneEvent>*)&children;
            for( auto& v : vec )
            {
                time += v.End() - v.Start();
            }
        }
        else
        {
            for( auto& v : children )
            {
                time += v->End() - v->Start();
            }
        }
    }
    return time;
}

std::string Base64Encode( const uint8_t* data, size_t len )
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve( ( len + 2 ) / 3 * 4 );
    for( size_t i = 0; i < len; i += 3 )
    {
        uint32_t n = uint32_t( data[i] ) << 16;
        if( i + 1 < len ) n |= uint32_t( data[i + 1] ) << 8;
        if( i + 2 < len ) n |= uint32_t( data[i + 2] );
        out += tbl[( n >> 18 ) & 63];
        out += tbl[( n >> 12 ) & 63];
        out += ( i + 1 < len ) ? tbl[( n >> 6 ) & 63] : '=';
        out += ( i + 2 < len ) ? tbl[n & 63] : '=';
    }
    return out;
}

using AggMap = std::unordered_map<int16_t, std::pair<int64_t, int64_t>>;

// Worker::GetZoneEnd() infers span from child zones when end is missing; that path can fault or
// be extremely slow on pathological traces. MCP aggregation uses direct timing only (same as
// GetZoneEndDirect in TracyWorker.hpp) — overlap may be smaller than the GUI for open-ended zones.
static int64_t ZoneCpuEndForMcp( const tracy::ZoneEvent& ze )
{
    return tracy::Worker::GetZoneEndDirect( ze );
}

static int64_t ZoneGpuEndForMcp( const tracy::GpuEvent& ge )
{
    return tracy::Worker::GetZoneEndDirect( ge );
}

// Default cap prevents O(hundreds of millions) zone visits from exhausting memory or faulting on huge
// captures. Pass max_timeline_visits: 0 for unlimited (may still fail on very large traces).
static uint64_t ParseMaxTimelineVisits( const nlohmann::json& p )
{
    return p.value( "max_timeline_visits", uint64_t( 50'000'000ull ) );
}

static void CheckTimelineVisit( uint64_t maxVisits, uint64_t& used )
{
    if( maxVisits == 0 ) return;
    ++used;
    if( used > maxVisits )
    {
        throw std::runtime_error(
            "max_timeline_visits exceeded (default 50000000). Use the Tracy GUI for full timeline analysis, "
            "or pass max_timeline_visits: 0 to attempt an unlimited scan (unsafe on huge traces)." );
    }
}

template<typename Accessor>
static size_t LowerBoundByKey( size_t n, int64_t key, Accessor acc )
{
    size_t lo = 0, hi = n;
    while( lo < hi )
    {
        const size_t mid = lo + ( hi - lo ) / 2;
        if( acc( mid ) < key ) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void WaitGpuSourceZones( tracy::Worker& w )
{
    for( int i = 0; i < 120000 && !w.AreGpuSourceLocationZonesReady(); ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    if( !w.AreGpuSourceLocationZonesReady() ) throw std::runtime_error( "timeout waiting for GPU source location zones" );
}

// Aggregate overlap with [t0,t1] using per-source-location zones sorted by start (not per-thread timelines).
// Optional threadFilter: when non-null, only zones on that compressed thread id are counted.
template<typename ZVec>
static void AddCpuOverlapFromSortedZones( tracy::Worker& w, int16_t sl, const ZVec& zsv, int64_t t0, int64_t t1, AggMap& out,
    const uint16_t* threadFilter, uint64_t maxVisits, uint64_t& usedVisits )
{
    const size_t n = zsv.size();
    if( n == 0 ) return;

    const size_t lo = LowerBoundByKey( n, t0, [&]( size_t idx ) {
        const auto* z = zsv[idx].Zone();
        return z ? z->Start() : (std::numeric_limits<int64_t>::min)();
    } );
    const size_t ub = LowerBoundByKey( n, t1, [&]( size_t idx ) {
        const auto* z = zsv[idx].Zone();
        return z ? z->Start() : (std::numeric_limits<int64_t>::min)();
    } );

    for( size_t i = lo; i < ub; ++i )
    {
        CheckTimelineVisit( maxVisits, usedVisits );
        const auto& ztd = zsv[i];
        if( threadFilter != nullptr && ztd.Thread() != *threadFilter ) continue;
        const auto* ze = ztd.Zone();
        if( !ze ) continue;
        const int64_t zs = ze->Start();
        const int64_t ze_ = ZoneCpuEndForMcp( *ze );
        const int64_t a = std::max( zs, t0 );
        const int64_t b = std::min( ze_, t1 );
        if( b > a )
        {
            auto& e = out[sl];
            e.first += b - a;
            e.second += 1;
        }
    }

    for( size_t i = lo; i > 0; )
    {
        --i;
        CheckTimelineVisit( maxVisits, usedVisits );
        const auto& ztd = zsv[i];
        if( threadFilter != nullptr && ztd.Thread() != *threadFilter ) continue;
        const auto* ze = ztd.Zone();
        if( !ze ) continue;
        const int64_t zs = ze->Start();
        const int64_t ze_ = ZoneCpuEndForMcp( *ze );
        if( zs >= t0 ) continue;
        const int64_t a = std::max( zs, t0 );
        const int64_t b = std::min( ze_, t1 );
        if( b > a )
        {
            auto& e = out[sl];
            e.first += b - a;
            e.second += 1;
        }
    }
}

template<typename GVec>
static void AddGpuOverlapFromSortedZones( tracy::Worker& w, int16_t sl, const GVec& zsv, int64_t t0, int64_t t1, AggMap& out,
    const uint16_t* threadFilter, uint64_t maxVisits, uint64_t& usedVisits )
{
    const size_t n = zsv.size();
    if( n == 0 ) return;

    const size_t lo = LowerBoundByKey( n, t0, [&]( size_t idx ) {
        const auto* ge = zsv[idx].Zone();
        if( !ge ) return (std::numeric_limits<int64_t>::min)();
        const int64_t gs = ge->GpuStart();
        return gs < 0 ? (std::numeric_limits<int64_t>::min)() : gs;
    } );
    const size_t ub = LowerBoundByKey( n, t1, [&]( size_t idx ) {
        const auto* ge = zsv[idx].Zone();
        if( !ge ) return (std::numeric_limits<int64_t>::min)();
        const int64_t gs = ge->GpuStart();
        return gs < 0 ? (std::numeric_limits<int64_t>::min)() : gs;
    } );

    for( size_t i = lo; i < ub; ++i )
    {
        CheckTimelineVisit( maxVisits, usedVisits );
        if( threadFilter != nullptr && zsv[i].Thread() != *threadFilter ) continue;
        const auto* ge = zsv[i].Zone();
        if( !ge ) continue;
        const int64_t gs = ge->GpuStart();
        if( gs < 0 ) continue;
        const int64_t ge_ = ZoneGpuEndForMcp( *ge );
        const int64_t a = std::max( gs, t0 );
        const int64_t b = std::min( ge_, t1 );
        if( b > a )
        {
            auto& e = out[sl];
            e.first += b - a;
            e.second += 1;
        }
    }

    for( size_t i = lo; i > 0; )
    {
        --i;
        CheckTimelineVisit( maxVisits, usedVisits );
        if( threadFilter != nullptr && zsv[i].Thread() != *threadFilter ) continue;
        const auto* ge = zsv[i].Zone();
        if( !ge ) continue;
        const int64_t gs = ge->GpuStart();
        if( gs < 0 ) continue;
        if( gs >= t0 ) continue;
        const int64_t ge_ = ZoneGpuEndForMcp( *ge );
        const int64_t a = std::max( gs, t0 );
        const int64_t b = std::min( ge_, t1 );
        if( b > a )
        {
            auto& e = out[sl];
            e.first += b - a;
            e.second += 1;
        }
    }
}

void AggregateCpuZones( tracy::Worker& w, int64_t t0, int64_t t1, AggMap& out, uint64_t maxVisits, uint64_t& usedVisits )
{
    WaitSourceZones( w );
    auto lock = w.ObtainLockForMainThread();
    for( const auto& kv : w.GetSourceLocationZones() )
    {
        const int16_t sl = kv.first;
        if( !w.IsSourceLocationIdValid( sl ) ) continue;
        AddCpuOverlapFromSortedZones( w, sl, kv.second.zones, t0, t1, out, nullptr, maxVisits, usedVisits );
    }
}

void AggregateGpuZones( tracy::Worker& w, int64_t t0, int64_t t1, AggMap& out, uint64_t maxVisits, uint64_t& usedVisits )
{
    WaitGpuSourceZones( w );
    auto lock = w.ObtainLockForMainThread();
    for( const auto& kv : w.GetGpuSourceLocationZones() )
    {
        const int16_t sl = kv.first;
        if( !w.IsSourceLocationIdValid( sl ) ) continue;
        AddGpuOverlapFromSortedZones( w, sl, kv.second.zones, t0, t1, out, nullptr, maxVisits, usedVisits );
    }
}

void AggregateGpuZonesForThread( tracy::Worker& w, uint64_t tid, int64_t t0, int64_t t1, AggMap& out, uint64_t maxVisits, uint64_t& usedVisits )
{
    if( !w.GetThreadData( tid ) ) return;
    const uint16_t thc = w.CompressThread( tid );
    WaitGpuSourceZones( w );
    auto lock = w.ObtainLockForMainThread();
    for( const auto& kv : w.GetGpuSourceLocationZones() )
    {
        const int16_t sl = kv.first;
        if( !w.IsSourceLocationIdValid( sl ) ) continue;
        AddGpuOverlapFromSortedZones( w, sl, kv.second.zones, t0, t1, out, &thc, maxVisits, usedVisits );
    }
}

void AggregateCpuZonesForThread( tracy::Worker& w, uint64_t tid, int64_t t0, int64_t t1, AggMap& out, uint64_t maxVisits, uint64_t& usedVisits )
{
    if( !w.GetThreadData( tid ) ) return;
    const uint16_t thc = w.CompressThread( tid );
    WaitSourceZones( w );
    auto lock = w.ObtainLockForMainThread();
    for( const auto& kv : w.GetSourceLocationZones() )
    {
        const int16_t sl = kv.first;
        if( !w.IsSourceLocationIdValid( sl ) ) continue;
        AddCpuOverlapFromSortedZones( w, sl, kv.second.zones, t0, t1, out, &thc, maxVisits, usedVisits );
    }
}

bool JsonDetailMinimal( const nlohmann::json& p )
{
    const std::string d = p.value( "detail", "full" );
    return d == "minimal";
}

nlohmann::json AggToJson( tracy::Worker& w, const AggMap& m, int topN, bool minimal )
{
    std::vector<std::pair<int16_t, std::pair<int64_t, int64_t>>> vec( m.begin(), m.end() );
    std::sort( vec.begin(), vec.end(), []( const auto& a, const auto& b ) { return a.second.first > b.second.first; } );
    nlohmann::json arr = nlohmann::json::array();
    int c = 0;
    for( const auto& kv : vec )
    {
        if( topN > 0 && c++ >= topN ) break;
        if( !w.IsSourceLocationIdValid( kv.first ) ) continue;
        const auto& sl = w.GetSourceLocation( kv.first );
        if( minimal )
        {
            arr.push_back( {
                { "srcLoc", kv.first },
                { "name", w.GetZoneName( sl ) },
                { "totalNs", kv.second.first },
                { "count", kv.second.second },
            } );
        }
        else
        {
            arr.push_back( {
                { "srcLoc", kv.first },
                { "name", w.GetZoneName( sl ) },
                { "function", w.GetString( sl.function ) },
                { "file", w.GetString( sl.file ) },
                { "line", sl.line },
                { "totalNs", kv.second.first },
                { "count", kv.second.second },
            } );
        }
    }
    return arr;
}

nlohmann::json DiffAgg( tracy::Worker& w, const AggMap& a, const AggMap& b, int topN )
{
    std::unordered_map<int16_t, int64_t> delta;
    for( const auto& kv : b ) delta[kv.first] += kv.second.first;
    for( const auto& kv : a ) delta[kv.first] -= kv.second.first;
    std::vector<std::pair<int16_t, int64_t>> vec;
    for( const auto& kv : delta )
    {
        if( kv.second != 0 ) vec.emplace_back( kv.first, kv.second );
    }
    std::sort( vec.begin(), vec.end(), []( const auto& x, const auto& y ) { return std::llabs( x.second ) > std::llabs( y.second ); } );
    nlohmann::json arr = nlohmann::json::array();
    int c = 0;
    for( const auto& kv : vec )
    {
        if( topN > 0 && c++ >= topN ) break;
        if( !w.IsSourceLocationIdValid( kv.first ) ) continue;
        const auto& sl = w.GetSourceLocation( kv.first );
        arr.push_back( {
            { "srcLoc", kv.first },
            { "name", w.GetZoneName( sl ) },
            { "deltaNs", kv.second },
        } );
    }
    return arr;
}

static int64_t AggCalls( const AggMap& m, int16_t k )
{
    const auto it = m.find( k );
    if( it == m.end() ) return 0;
    return it->second.second;
}

static int64_t AggNs( const AggMap& m, int16_t k )
{
    const auto it = m.find( k );
    if( it == m.end() ) return 0;
    return it->second.first;
}

static nlohmann::json FuncEntryJson( tracy::Worker& w, int16_t id, int64_t overlapNs, int64_t callCount, bool minimal )
{
    if( !w.IsSourceLocationIdValid( id ) ) return nlohmann::json::object();
    const auto& sl = w.GetSourceLocation( id );
    if( minimal )
    {
        return nlohmann::json{ { "srcLoc", id }, { "name", w.GetZoneName( sl ) }, { "callCount", callCount }, { "overlapNs", overlapNs } };
    }
    return nlohmann::json{ { "srcLoc", id },
        { "name", w.GetZoneName( sl ) },
        { "function", w.GetString( sl.function ) },
        { "file", w.GetString( sl.file ) },
        { "line", sl.line },
        { "callCount", callCount },
        { "overlapNs", overlapNs } };
}

static nlohmann::json AggMapToJsonPaged( tracy::Worker& w, const AggMap& m, bool minimal, const std::string& sortBy, size_t offset, size_t limit, size_t& outTotal )
{
    std::vector<std::pair<int16_t, std::pair<int64_t, int64_t>>> vec( m.begin(), m.end() );
    outTotal = vec.size();
    if( sortBy == "name" )
    {
        std::sort( vec.begin(), vec.end(), [&]( const auto& a, const auto& b ) {
            if( !w.IsSourceLocationIdValid( a.first ) || !w.IsSourceLocationIdValid( b.first ) ) return a.first < b.first;
            return w.GetZoneName( w.GetSourceLocation( a.first ) ) < w.GetZoneName( w.GetSourceLocation( b.first ) );
        } );
    }
    else if( sortBy == "src_loc" )
    {
        std::sort( vec.begin(), vec.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
    }
    else
    {
        std::sort( vec.begin(), vec.end(), []( const auto& a, const auto& b ) { return a.second.second > b.second.second; } );
    }
    nlohmann::json arr = nlohmann::json::array();
    for( const auto& e : vec )
    {
        if( offset > 0 )
        {
            --offset;
            continue;
        }
        if( arr.size() >= limit ) break;
        if( !w.IsSourceLocationIdValid( e.first ) ) continue;
        arr.push_back( FuncEntryJson( w, e.first, e.second.first, e.second.second, minimal ) );
    }
    return arr;
}

nlohmann::json HandleOpen( const nlohmann::json& p )
{
    const std::string path = p.value( "path", "" );
    if( path.empty() ) return Err( "missing path" );
    g_worker.reset();
    g_openPath.clear();
    auto f = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( path.c_str() ) );
    if( !f ) return Err( "could not open file" );
    try
    {
        g_worker = std::make_unique<tracy::Worker>( *f );
    }
    catch( const tracy::UnsupportedVersion& e )
    {
        return Err( std::string( "unsupported trace version: " ) + std::to_string( e.version ) );
    }
    catch( const tracy::LegacyVersion& e )
    {
        return Err( std::string( "legacy trace version: " ) + std::to_string( e.version ) );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    g_openPath = path;
    return Ok( { { "path", path } } );
}

nlohmann::json HandleClose( const nlohmann::json& )
{
    g_worker.reset();
    g_openPath.clear();
    return Ok();
}

nlohmann::json HandlePing( const nlohmann::json& )
{
    nlohmann::json extra = { { "pong", true } };
    extra["captureOpen"] = g_worker != nullptr;
    if( g_worker && !g_openPath.empty() )
    {
        extra["path"] = g_openPath;
    }
    else
    {
        extra["path"] = nlohmann::json( nullptr );
    }
    return Ok( std::move( extra ) );
}

nlohmann::json HandleMetadata( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    auto lock = w->ObtainLockForMainThread();
    const bool summaryOnly = p.value( "summary_only", false );
    const size_t frameSetsMax = std::min<size_t>( p.value( "frame_sets_max", 256 ), 10000 );
    const size_t memPoolsMax = std::min<size_t>( p.value( "mem_pools_max", 256 ), 10000 );

    const auto base = nlohmann::json{
        { "traceVersion", w->GetTraceVersion() },
        { "captureName", w->GetCaptureName() },
        { "captureProgram", w->GetCaptureProgram() },
        { "captureTime", w->GetCaptureTime() },
        { "pid", w->GetPid() },
        { "pidInfo", "OS process id of the profiled application in this capture (one stable value for the file). It is not a frame id, not a per-frame value, and not a substitute for list_frames index or guiFrameNumber." },
        { "resolution", w->GetResolution() },
        { "samplingPeriod", w->GetSamplingPeriod() },
        { "zoneCount", w->GetZoneCount() },
        { "gpuZoneCount", w->GetGpuZoneCount() },
        { "lockCount", w->GetLockCount() },
        { "frameImageCount", w->GetFrameImageCount() },
        { "threadCount", w->GetThreadData().size() },
        { "hasContextSwitches", w->HasContextSwitches() },
        { "hasGpuContexts", !w->GetGpuData().empty() },
        { "areFramesUsed", w->AreFramesUsed() },
        { "firstTime", w->GetFirstTime() },
        { "lastTime", w->GetLastTime() },
        { "loadTimeNs", w->GetLoadTime() },
        { "memPoolCount", w->GetMemNameMap().size() },
        { "frameOffset", w->GetFrameOffset() },
        { "onDemand", w->IsOnDemand() },
        { "frameNumbering",
          "Tracy GUI frame labels use guiFrameNumber (see list_frames / frames.resolve), not a different scheme. "
          "Main frame set (unnamed): guiFrameNumber matches View::GetFrameNumber — with frameOffset==0 it equals "
          "capture index i; with on-demand captures, guiFrameNumber = i + frameOffset - 1. Named frame sets: "
          "guiFrameNumber is 1-based (i+1). MCP frame_index / frames.list index is always 0-based capture order." },
    };

    if( summaryOnly )
    {
        nlohmann::json out = base;
        out["frameSetCount"] = w->GetFrames().size();
        out["summaryOnly"] = true;
        return Ok( std::move( out ) );
    }

    nlohmann::json frameSets = nlohmann::json::array();
    size_t idx = 0;
    bool truncatedFrameSets = false;
    for( const auto* fd : w->GetFrames() )
    {
        if( frameSets.size() >= frameSetsMax )
        {
            truncatedFrameSets = true;
            break;
        }
        frameSets.push_back( {
            { "index", idx++ },
            { "name", FrameSetLabel( *w, *fd ) },
            { "isMain", fd->name == 0 },
            { "frameCount", w->GetFullFrameCount( *fd ) },
            { "continuous", fd->continuous != 0 },
        } );
    }
    nlohmann::json memPools = nlohmann::json::array();
    bool truncatedMemPools = false;
    size_t mpAdded = 0;
    for( const auto& kv : w->GetMemNameMap() )
    {
        if( kv.first == 0 ) continue;
        if( mpAdded >= memPoolsMax )
        {
            truncatedMemPools = true;
            break;
        }
        mpAdded++;
        memPools.push_back( {
            { "id", kv.first },
            { "name", w->GetString( kv.first ) },
        } );
    }
    nlohmann::json out = base;
    out["frameSets"] = std::move( frameSets );
    out["memPools"] = std::move( memPools );
    out["truncatedFrameSets"] = truncatedFrameSets;
    out["truncatedMemPools"] = truncatedMemPools;
    return Ok( std::move( out ) );
}

nlohmann::json HandleFramesList( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const size_t offset = p.value( "offset", 0 );
    const size_t limit = std::min<size_t>( p.value( "limit", 50 ), 5000 );
    const auto* fd = GetFrameSet( *w, frame_set );
    const size_t n = w->GetFullFrameCount( *fd );
    nlohmann::json arr = nlohmann::json::array();
    for( size_t i = offset; i < n && i < offset + limit; ++i )
    {
        const int64_t t0 = w->GetFrameBegin( *fd, i );
        const int64_t t1 = w->GetFrameEnd( *fd, i );
        const int64_t dur = w->GetFrameTime( *fd, i );
        const auto* fi = w->GetFrameImage( *fd, i );
        arr.push_back( {
            { "index", i },
            { "guiFrameNumber", GuiFrameNumber( *w, *fd, i ) },
            { "uiKind", FrameUiKind( *w, *fd, i ) },
            { "startNs", t0 },
            { "endNs", t1 },
            { "durationNs", dur },
            { "hasScreenshot", fi != nullptr },
        } );
    }
    return Ok( {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_set_is_main", fd->name == 0 },
        { "frameOffset", w->GetFrameOffset() },
        { "onDemand", w->IsOnDemand() },
        { "frames", std::move( arr ) },
        { "total", n },
    } );
}

nlohmann::json HandleFramesSlowest( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const int top = std::min( p.value( "top_k", 20 ), 500 );
    const size_t skip = p.value( "skip_warmup", 0 );
    const auto* fd = GetFrameSet( *w, frame_set );
    const size_t n = w->GetFullFrameCount( *fd );
    std::vector<std::pair<size_t, int64_t>> scores;
    for( size_t i = skip; i < n; ++i )
    {
        scores.emplace_back( i, w->GetFrameTime( *fd, i ) );
    }
    std::sort( scores.begin(), scores.end(), []( const auto& a, const auto& b ) { return a.second > b.second; } );
    nlohmann::json arr = nlohmann::json::array();
    for( int k = 0; k < top && k < (int)scores.size(); ++k )
    {
        const size_t fi = scores[k].first;
        arr.push_back( {
            { "index", fi },
            { "guiFrameNumber", GuiFrameNumber( *w, *fd, fi ) },
            { "uiKind", FrameUiKind( *w, *fd, fi ) },
            { "durationNs", scores[k].second },
        } );
    }
    return Ok( { { "frame_set", frame_set }, { "slowest", std::move( arr ) } } );
}

nlohmann::json HandleFramesSummarize( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const int topN = std::min( p.value( "top_zones", 15 ), 500 );
    const bool minimal = JsonDetailMinimal( p );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t frame_index;
    try
    {
        frame_index = ResolveFrameIndexParam( *w, *fd, p, "frame_index", "gui_frame_number", "display_frame_number" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    const int64_t t0 = w->GetFrameBegin( *fd, frame_index );
    const int64_t t1 = w->GetFrameEnd( *fd, frame_index );
    AggMap cpu, gpu;
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    uint64_t used = 0;
    AggregateCpuZones( *w, t0, t1, cpu, maxV, used );
    AggregateGpuZones( *w, t0, t1, gpu, maxV, used );
    return Ok( {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_index", frame_index },
        { "guiFrameNumber", GuiFrameNumber( *w, *fd, frame_index ) },
        { "uiKind", FrameUiKind( *w, *fd, frame_index ) },
        { "startNs", t0 },
        { "endNs", t1 },
        { "durationNs", w->GetFrameTime( *fd, frame_index ) },
        { "detail", minimal ? "minimal" : "full" },
        { "topCpuZones", AggToJson( *w, cpu, topN, minimal ) },
        { "topGpuZones", AggToJson( *w, gpu, topN, minimal ) },
        { "note", "Overlap timing uses GetZoneEndDirect when zone end is missing; may differ from GUI. guiFrameNumber matches Tracy's top bar (see get_capture_metadata frameNumbering)." },
    } );
}

nlohmann::json HandleFramesSummarizeThread( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const uint64_t tid = p.value( "tid", 0ull );
    const int topN = std::min( p.value( "top_zones", 15 ), 500 );
    const bool minimal = JsonDetailMinimal( p );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t frame_index;
    try
    {
        frame_index = ResolveFrameIndexParam( *w, *fd, p, "frame_index", "gui_frame_number", "display_frame_number" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    if( !w->GetThreadData( tid ) ) return Err( "thread tid not found" );
    const int64_t t0 = w->GetFrameBegin( *fd, frame_index );
    const int64_t t1 = w->GetFrameEnd( *fd, frame_index );
    AggMap cpu;
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    uint64_t used = 0;
    AggregateCpuZonesForThread( *w, tid, t0, t1, cpu, maxV, used );
    return Ok( {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_index", frame_index },
        { "guiFrameNumber", GuiFrameNumber( *w, *fd, frame_index ) },
        { "uiKind", FrameUiKind( *w, *fd, frame_index ) },
        { "tid", tid },
        { "startNs", t0 },
        { "endNs", t1 },
        { "detail", minimal ? "minimal" : "full" },
        { "note", "CPU zones for this thread only (no GPU). Overlap uses GetZoneEndDirect when end is missing; may differ from GUI. Use frames.summarize for all threads + GPU. guiFrameNumber matches Tracy's top bar." },
        { "topCpuZones", AggToJson( *w, cpu, topN, minimal ) },
    } );
}

nlohmann::json HandleFramesCompare( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const int topN = std::min( p.value( "top_changes", 20 ), 500 );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t a, b;
    try
    {
        a = ResolveFrameIndexParam( *w, *fd, p, "frame_index_a", "gui_frame_number_a", "display_frame_number_a" );
        b = ResolveFrameIndexParam( *w, *fd, p, "frame_index_b", "gui_frame_number_b", "display_frame_number_b" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    const int64_t ta0 = w->GetFrameBegin( *fd, a );
    const int64_t ta1 = w->GetFrameEnd( *fd, a );
    const int64_t tb0 = w->GetFrameBegin( *fd, b );
    const int64_t tb1 = w->GetFrameEnd( *fd, b );
    AggMap ca, cb;
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    uint64_t used = 0;
    AggregateCpuZones( *w, ta0, ta1, ca, maxV, used );
    AggregateCpuZones( *w, tb0, tb1, cb, maxV, used );
    return Ok( {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_index_a", a },
        { "frame_index_b", b },
        { "guiFrameNumberA", GuiFrameNumber( *w, *fd, a ) },
        { "guiFrameNumberB", GuiFrameNumber( *w, *fd, b ) },
        { "uiKindA", FrameUiKind( *w, *fd, a ) },
        { "uiKindB", FrameUiKind( *w, *fd, b ) },
        { "cpuDeltaBySrcLoc", DiffAgg( *w, ca, cb, topN ) },
        { "note", "CPU overlap uses GetZoneEndDirect (no child-tree inference when end is missing); may differ from GUI. guiFrameNumber* matches Tracy's top bar." },
    } );
}

// CPU/GPU zone instances per source location overlapping two frame windows: only-in-A, only-in-B, and both (with call counts).
static nlohmann::json BuildFunctionDiffLayer( tracy::Worker& w, const AggMap& ca, const AggMap& cb, bool minimal, size_t maxPerSection, bool onlyCallCountDeltasInBoth )
{
    std::unordered_set<int16_t> keys;
    for( const auto& kv : ca ) keys.insert( kv.first );
    for( const auto& kv : cb ) keys.insert( kv.first );
    struct Row
    {
        int16_t id;
        int64_t ca, cb;
        int64_t na, nb;
    };
    std::vector<Row> onlyA, onlyB, both;
    for( int16_t k : keys )
    {
        if( !w.IsSourceLocationIdValid( k ) ) continue;
        const int64_t cA = AggCalls( ca, k );
        const int64_t cB = AggCalls( cb, k );
        if( cA == 0 && cB == 0 ) continue;
        const int64_t nsA = AggNs( ca, k );
        const int64_t nsB = AggNs( cb, k );
        if( cA > 0 && cB == 0 ) onlyA.push_back( { k, cA, cB, nsA, nsB } );
        else if( cA == 0 && cB > 0 ) onlyB.push_back( { k, cA, cB, nsA, nsB } );
        else if( cA > 0 && cB > 0 )
        {
            if( !onlyCallCountDeltasInBoth || cA != cB ) both.push_back( { k, cA, cB, nsA, nsB } );
        }
    }
    std::sort( onlyA.begin(), onlyA.end(), [&]( const Row& x, const Row& y ) {
        return w.GetZoneName( w.GetSourceLocation( x.id ) ) < w.GetZoneName( w.GetSourceLocation( y.id ) );
    } );
    std::sort( onlyB.begin(), onlyB.end(), [&]( const Row& x, const Row& y ) {
        return w.GetZoneName( w.GetSourceLocation( x.id ) ) < w.GetZoneName( w.GetSourceLocation( y.id ) );
    } );
    std::sort( both.begin(), both.end(), [&]( const Row& x, const Row& y ) {
        return std::llabs( x.ca - x.cb ) > std::llabs( y.ca - y.cb );
    } );

    nlohmann::json ja = nlohmann::json::array();
    nlohmann::json jb = nlohmann::json::array();
    nlohmann::json jboth = nlohmann::json::array();
    for( size_t i = 0; i < onlyA.size() && i < maxPerSection; ++i )
    {
        ja.push_back( FuncEntryJson( w, onlyA[i].id, onlyA[i].na, onlyA[i].ca, minimal ) );
    }
    for( size_t i = 0; i < onlyB.size() && i < maxPerSection; ++i )
    {
        jb.push_back( FuncEntryJson( w, onlyB[i].id, onlyB[i].nb, onlyB[i].cb, minimal ) );
    }
    for( size_t i = 0; i < both.size() && i < maxPerSection; ++i )
    {
        const Row& r = both[i];
        if( !w.IsSourceLocationIdValid( r.id ) ) continue;
        const auto& sl = w.GetSourceLocation( r.id );
        nlohmann::json o = minimal ? nlohmann::json{ { "srcLoc", r.id }, { "name", w.GetZoneName( sl ) } }
                                   : nlohmann::json{ { "srcLoc", r.id },
                                         { "name", w.GetZoneName( sl ) },
                                         { "function", w.GetString( sl.function ) },
                                         { "file", w.GetString( sl.file ) },
                                         { "line", sl.line } };
        o["callCountA"] = r.ca;
        o["callCountB"] = r.cb;
        o["callCountDelta"] = r.ca - r.cb;
        o["overlapNsA"] = r.na;
        o["overlapNsB"] = r.nb;
        jboth.push_back( std::move( o ) );
    }

    return {
        { "onlyInFrameA", std::move( ja ) },
        { "onlyInFrameB", std::move( jb ) },
        { "inBothFrames", std::move( jboth ) },
        { "counts", { { "onlyInA", onlyA.size() }, { "onlyInB", onlyB.size() }, { "inBoth", both.size() } } },
        { "truncated",
          { { "onlyInA", onlyA.size() > maxPerSection },
            { "onlyInB", onlyB.size() > maxPerSection },
            { "inBoth", both.size() > maxPerSection } } },
    };
}

nlohmann::json HandleFramesListFunctions( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const bool includeGpu = p.value( "include_gpu", true );
    const bool minimal = JsonDetailMinimal( p );
    const std::string sortBy = p.value( "sort", "calls" );
    const size_t offset = p.value( "offset", 0 );
    const size_t limit = std::min<size_t>( p.value( "limit", 500 ), 10000 );
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    const uint64_t tid = p.value( "tid", 0ull );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t frame_index;
    try
    {
        frame_index = ResolveFrameIndexParam( *w, *fd, p, "frame_index", "gui_frame_number", "display_frame_number" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    if( tid != 0 && !w->GetThreadData( tid ) ) return Err( "thread tid not found" );
    const int64_t t0 = w->GetFrameBegin( *fd, frame_index );
    const int64_t t1 = w->GetFrameEnd( *fd, frame_index );
    AggMap cpu, gpu;
    uint64_t used = 0;
    if( tid == 0 )
    {
        AggregateCpuZones( *w, t0, t1, cpu, maxV, used );
        if( includeGpu ) AggregateGpuZones( *w, t0, t1, gpu, maxV, used );
    }
    else
    {
        AggregateCpuZonesForThread( *w, tid, t0, t1, cpu, maxV, used );
        if( includeGpu ) AggregateGpuZonesForThread( *w, tid, t0, t1, gpu, maxV, used );
    }
    size_t cpuTotal = 0, gpuTotal = 0;
    nlohmann::json out = {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_index", frame_index },
        { "guiFrameNumber", GuiFrameNumber( *w, *fd, frame_index ) },
        { "tid", tid },
        { "startNs", t0 },
        { "endNs", t1 },
        { "detail", minimal ? "minimal" : "full" },
        { "cpuFunctions", AggMapToJsonPaged( *w, cpu, minimal, sortBy, offset, limit, cpuTotal ) },
        { "cpuFunctionCount", cpuTotal },
        { "cpuTruncated", cpuTotal > offset + limit },
    };
    if( includeGpu )
    {
        out["gpuFunctions"] = AggMapToJsonPaged( *w, gpu, minimal, sortBy, offset, limit, gpuTotal );
        out["gpuFunctionCount"] = gpuTotal;
        out["gpuTruncated"] = gpuTotal > offset + limit;
    }
    else
    {
        out["gpuFunctions"] = nlohmann::json::array();
        out["gpuFunctionCount"] = 0;
    }
    out["note"] = "Per source location: callCount = zone instances overlapping the frame; overlapNs = CPU/GPU time in-window (GetZoneEndDirect).";
    return Ok( std::move( out ) );
}

nlohmann::json HandleFramesCompareFunctions( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const bool includeGpu = p.value( "include_gpu", true );
    const bool minimal = JsonDetailMinimal( p );
    const size_t maxPer = std::min<size_t>( p.value( "max_per_section", 2000 ), 20000 );
    const bool onlyDeltas = p.value( "only_in_both_with_call_deltas", false );
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    const uint64_t tid = p.value( "tid", 0ull );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t a, b;
    try
    {
        a = ResolveFrameIndexParam( *w, *fd, p, "frame_index_a", "gui_frame_number_a", "display_frame_number_a" );
        b = ResolveFrameIndexParam( *w, *fd, p, "frame_index_b", "gui_frame_number_b", "display_frame_number_b" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    if( tid != 0 && !w->GetThreadData( tid ) ) return Err( "thread tid not found" );
    const int64_t ta0 = w->GetFrameBegin( *fd, a );
    const int64_t ta1 = w->GetFrameEnd( *fd, a );
    const int64_t tb0 = w->GetFrameBegin( *fd, b );
    const int64_t tb1 = w->GetFrameEnd( *fd, b );
    AggMap cpa, cpb, gpa, gpb;
    uint64_t used = 0;
    if( tid == 0 )
    {
        AggregateCpuZones( *w, ta0, ta1, cpa, maxV, used );
        AggregateCpuZones( *w, tb0, tb1, cpb, maxV, used );
        if( includeGpu )
        {
            AggregateGpuZones( *w, ta0, ta1, gpa, maxV, used );
            AggregateGpuZones( *w, tb0, tb1, gpb, maxV, used );
        }
    }
    else
    {
        AggregateCpuZonesForThread( *w, tid, ta0, ta1, cpa, maxV, used );
        AggregateCpuZonesForThread( *w, tid, tb0, tb1, cpb, maxV, used );
        if( includeGpu )
        {
            AggregateGpuZonesForThread( *w, tid, ta0, ta1, gpa, maxV, used );
            AggregateGpuZonesForThread( *w, tid, tb0, tb1, gpb, maxV, used );
        }
    }
    nlohmann::json out = {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_index_a", a },
        { "frame_index_b", b },
        { "guiFrameNumberA", GuiFrameNumber( *w, *fd, a ) },
        { "guiFrameNumberB", GuiFrameNumber( *w, *fd, b ) },
        { "tid", tid },
        { "cpu", BuildFunctionDiffLayer( *w, cpa, cpb, minimal, maxPer, onlyDeltas ) },
    };
    if( includeGpu )
    {
        out["gpu"] = BuildFunctionDiffLayer( *w, gpa, gpb, minimal, maxPer, onlyDeltas );
    }
    out["note"] = "onlyInFrame* = at least one overlapping zone in that frame and none in the other. inBothFrames lists overlap in both; set only_in_both_with_call_deltas=true to restrict inBoth to differing call counts.";
    return Ok( std::move( out ) );
}

nlohmann::json HandleFramesResolve( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const auto* fd = GetFrameSet( *w, frame_set );
    const size_t n = w->GetFullFrameCount( *fd );
    if( !p.contains( "gui_frame_number" ) && !p.contains( "display_frame_number" ) )
    {
        return Err( "pass gui_frame_number (same integer Tracy shows in the top bar for this frame set)" );
    }
    const uint64_t gui = p.contains( "gui_frame_number" ) ? p.value( "gui_frame_number", 0ull ) : p.value( "display_frame_number", 0ull );
    const auto opt = TryFrameIndexFromGuiNumber( *w, *fd, n, gui );
    if( !opt ) return Err( "gui_frame_number out of range for this frame set (check frame_set index, isMain vs named set, and metadata frameOffset / onDemand)" );
    const size_t idx = *opt;
    const int64_t t0 = w->GetFrameBegin( *fd, idx );
    const int64_t t1 = w->GetFrameEnd( *fd, idx );
    nlohmann::json prev = nullptr;
    nlohmann::json next = nullptr;
    if( idx > 0 )
    {
        const size_t j = idx - 1;
        prev = { { "frame_index", j },
                 { "guiFrameNumber", GuiFrameNumber( *w, *fd, j ) },
                 { "uiKind", FrameUiKind( *w, *fd, j ) },
                 { "startNs", w->GetFrameBegin( *fd, j ) },
                 { "endNs", w->GetFrameEnd( *fd, j ) } };
    }
    if( idx + 1 < n )
    {
        const size_t j = idx + 1;
        next = { { "frame_index", j },
                 { "guiFrameNumber", GuiFrameNumber( *w, *fd, j ) },
                 { "uiKind", FrameUiKind( *w, *fd, j ) },
                 { "startNs", w->GetFrameBegin( *fd, j ) },
                 { "endNs", w->GetFrameEnd( *fd, j ) } };
    }
    return Ok( {
        { "frame_set", frame_set },
        { "frame_set_name", FrameSetLabel( *w, *fd ) },
        { "frame_set_is_main", fd->name == 0 },
        { "frame_index", idx },
        { "guiFrameNumber", GuiFrameNumber( *w, *fd, idx ) },
        { "uiKind", FrameUiKind( *w, *fd, idx ) },
        { "startNs", t0 },
        { "endNs", t1 },
        { "durationNs", w->GetFrameTime( *fd, idx ) },
        { "prev", std::move( prev ) },
        { "next", std::move( next ) },
    } );
}

nlohmann::json HandleViewContext( const nlohmann::json& )
{
    auto* w = RequireWorker();
    return Ok( {
        { "mcpMode", "file_bridge" },
        { "guiNotControlled",
          nlohmann::json::array( { "view_span_zoom_eye_icon", "timeline_scroll", "next_prev_frame_buttons", "ruler_position_in_interactive_view" } ) },
        { "useInstead",
          "Use metadata firstTime/lastTime and per-frame startNs/endNs. guiFrameNumber (list_frames / frames.resolve) matches Tracy's bar; quote it when asking the user which frame you mean. "
          "To reason about a time window, use the same ns values as the GUI status line when the file is open (absolute times are consistent)." },
        { "firstTime", w->GetFirstTime() },
        { "lastTime", w->GetLastTime() },
        { "frameOffset", w->GetFrameOffset() },
        { "onDemand", w->IsOnDemand() },
    } );
}

nlohmann::json HandleZonesSearch( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    const std::string q = p.value( "query", "" );
    const bool ic = p.value( "case_insensitive", true );
    const size_t offset = p.value( "offset", 0 );
    const size_t limit = std::min<size_t>( p.value( "limit", 50 ), 500 );
    if( q.empty() ) return Err( "missing query" );
    auto ids = w->GetMatchingSourceLocation( q.c_str(), ic );
    const size_t total = ids.size();
    nlohmann::json arr = nlohmann::json::array();
    size_t skipped = 0;
    for( int16_t id : ids )
    {
        if( skipped++ < offset ) continue;
        if( arr.size() >= limit ) break;
        if( !w->IsSourceLocationIdValid( id ) ) continue;
        const auto& sl = w->GetSourceLocation( id );
        arr.push_back( {
            { "srcLoc", id },
            { "name", w->GetZoneName( sl ) },
            { "function", w->GetString( sl.function ) },
            { "file", w->GetString( sl.file ) },
            { "line", sl.line },
        } );
    }
    const bool truncated = offset + arr.size() < total;
    return Ok( {
        { "matches", std::move( arr ) },
        { "total", total },
        { "offset", offset },
        { "limit", limit },
        { "truncated", truncated },
    } );
}

nlohmann::json HandleZonesStats( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    const int srcloc = p.value( "src_loc", -1 );
    if( srcloc < -1 ) return Err( "invalid src_loc" );
    const auto& z = w->GetZonesForSourceLocation( int16_t( srcloc ) );
    nlohmann::json thr = nlohmann::json::object();
    for( const auto& t : z.threadCnt )
    {
        thr[std::to_string( t.first )] = t.second;
    }
    return Ok( {
        { "srcLoc", srcloc },
        { "zoneEvents", z.zones.size() },
        { "totalNs", z.total },
        { "minNs", z.min },
        { "maxNs", z.max },
        { "meanNs", z.zones.size() ? z.total / (int64_t)z.zones.size() : 0 },
        { "threads", std::move( thr ) },
    } );
}

nlohmann::json HandleZonesInstances( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    const int srcloc = p.value( "src_loc", -1 );
    const size_t offset = p.value( "offset", 0 );
    const size_t limit = std::min<size_t>( p.value( "limit", 50 ), 2000 );
    const auto& z = w->GetZonesForSourceLocation( int16_t( srcloc ) );
    nlohmann::json arr = nlohmann::json::array();
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    uint64_t usedVisits = 0;
    size_t c = 0;
    for( const auto& ztd : z.zones )
    {
        CheckTimelineVisit( maxV, usedVisits );
        if( c++ < offset ) continue;
        if( arr.size() >= limit ) break;
        const auto* ze = ztd.Zone();
        if( !ze ) continue;
        arr.push_back( {
            { "startNs", ze->Start() },
            { "endNs", ZoneCpuEndForMcp( *ze ) },
            { "threadCompressed", ztd.Thread() },
            { "threadTid", w->DecompressThread( ztd.Thread() ) },
        } );
    }
    return Ok( { { "instances", std::move( arr ) }, { "total", z.zones.size() } } );
}

nlohmann::json CallstackToJson( tracy::Worker& w, uint32_t csIdx )
{
    nlohmann::json arr = nlohmann::json::array();
    if( csIdx == 0 ) return arr;
    const auto& cs = w.GetCallstack( csIdx );
    for( size_t i = 0; i < cs.size(); ++i )
    {
        const auto* fr = w.GetCallstackFrame( cs[i] );
        if( !fr || !fr->data ) continue;
        const auto* d = fr->data.get();
        arr.push_back( {
            { "name", w.GetString( d->name ) },
            { "file", w.GetString( d->file ) },
            { "line", d->line },
        } );
    }
    return arr;
}

nlohmann::json HandleZonesCallstack( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    const int srcloc = p.value( "src_loc", -1 );
    const size_t instance_index = p.value( "instance_index", 0 );
    const auto& z = w->GetZonesForSourceLocation( int16_t( srcloc ) );
    if( instance_index >= z.zones.size() ) return Err( "instance_index out of range" );
    const auto* ze = z.zones[instance_index].Zone();
    uint32_t csIdx = 0;
    if( w->HasZoneExtra( *ze ) )
    {
        csIdx = w->GetZoneExtra( *ze ).callstack.Val();
    }
    if( csIdx == 0 )
    {
        return Ok( { { "frames", nlohmann::json::array() }, { "note", "no callstack on this zone instance" } } );
    }
    return Ok( {
        { "frames", CallstackToJson( *w, csIdx ) },
    } );
}

nlohmann::json HandleZonesCompareFrames( const nlohmann::json& p )
{
    return HandleFramesCompare( p );
}

nlohmann::json HandleThreadsList( const nlohmann::json& )
{
    auto* w = RequireWorker();
    nlohmann::json arr = nlohmann::json::array();
    for( const auto* td : w->GetThreadData() )
    {
        arr.push_back( {
            { "tid", td->id },
            { "name", w->GetThreadName( td->id ) },
            { "timelineZones", td->timeline.size() },
            { "messages", td->messages.size() },
            { "isFiber", td->isFiber != 0 },
        } );
    }
    return Ok( { { "threads", std::move( arr ) } } );
}

nlohmann::json HandleThreadsTimelineSummary( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t tid = p.value( "tid", 0 );
    const tracy::ThreadData* td = w->GetThreadData( tid );
    if( !td ) return Err( "thread not found" );
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    const size_t timelineN = td->timeline.size();
    if( maxV != 0 && timelineN > maxV )
    {
        return Err( "thread timeline has " + std::to_string( timelineN ) + " zone events; exceeds max_timeline_visits (" + std::to_string( maxV )
            + "). Use Tracy GUI for full-thread stats, or pass max_timeline_visits: 0 to force a scan (may fault on huge or corrupt traces)." );
    }
    uint64_t usedVisits = 0;
    int64_t tmin = std::numeric_limits<int64_t>::max();
    int64_t tmax = std::numeric_limits<int64_t>::min();
    auto lock = w->ObtainLockForMainThread();
    for( const auto& zp : td->timeline )
    {
        CheckTimelineVisit( maxV, usedVisits );
        const auto* z = zp.get();
        if( !z ) continue;
        tmin = std::min( tmin, z->Start() );
        tmax = std::max( tmax, ZoneCpuEndForMcp( *z ) );
    }
    return Ok( {
        { "tid", tid },
        { "zoneCount", td->timeline.size() },
        { "firstZoneStartNs", tmin == std::numeric_limits<int64_t>::max() ? nlohmann::json( nullptr ) : nlohmann::json( tmin ) },
        { "lastZoneEndNs", tmax == std::numeric_limits<int64_t>::min() ? nlohmann::json( nullptr ) : nlohmann::json( tmax ) },
    } );
}

nlohmann::json HandleThreadsGaps( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t tid = p.value( "tid", 0 );
    const int64_t minGapNs = p.value( "min_gap_ns", 1'000'000 );
    const int top = std::min( p.value( "top_k", 30 ), 500 );
    const tracy::ThreadData* td = w->GetThreadData( tid );
    if( !td ) return Err( "thread not found" );
    if( top <= 0 ) return Ok( { { "gaps", nlohmann::json::array() } } );

    const uint64_t maxV = ParseMaxTimelineVisits( p );
    const size_t timelineN = td->timeline.size();
    if( maxV != 0 && timelineN > maxV )
    {
        return Err( "thread timeline has " + std::to_string( timelineN ) + " zone events; exceeds max_timeline_visits (" + std::to_string( maxV )
            + "). Gap detection requires a full ordered walk of that timeline; use Tracy GUI, or pass max_timeline_visits: 0 to force a scan (may fault on huge or corrupt traces)." );
    }

    auto lock = w->ObtainLockForMainThread();

    struct GapRec
    {
        int64_t gapNs;
        int64_t afterNs;
        int64_t beforeNs;
    };
    struct MinGapBySize
    {
        bool operator()( const GapRec& a, const GapRec& b ) const { return a.gapNs > b.gapNs; }
    };
    std::priority_queue<GapRec, std::vector<GapRec>, MinGapBySize> pq;

    uint64_t usedVisits = 0;
    int64_t prevEnd = -1;
    for( const auto& zp : td->timeline )
    {
        CheckTimelineVisit( maxV, usedVisits );
        const auto* z = zp.get();
        if( !z ) continue;
        const int64_t s = z->Start();
        const int64_t e = ZoneCpuEndForMcp( *z );
        if( prevEnd >= 0 )
        {
            const int64_t g = s - prevEnd;
            if( g >= minGapNs )
            {
                if( (int)pq.size() < top )
                {
                    pq.push( { g, prevEnd, s } );
                }
                else if( g > pq.top().gapNs )
                {
                    pq.pop();
                    pq.push( { g, prevEnd, s } );
                }
            }
        }
        prevEnd = std::max( prevEnd, e );
    }

    std::vector<GapRec> out;
    out.reserve( pq.size() );
    while( !pq.empty() )
    {
        out.push_back( pq.top() );
        pq.pop();
    }
    std::sort( out.begin(), out.end(), []( const GapRec& a, const GapRec& b ) { return a.gapNs > b.gapNs; } );

    nlohmann::json arr = nlohmann::json::array();
    for( const auto& g : out )
    {
        arr.push_back( {
            { "afterNs", g.afterNs },
            { "beforeNs", g.beforeNs },
            { "gapNs", g.gapNs },
        } );
    }
    return Ok( { { "gaps", std::move( arr ) } } );
}

nlohmann::json HandleThreadsHandoff( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const int64_t t0 = p.value( "start_ns", w->GetFirstTime() );
    const int64_t t1 = p.value( "end_ns", w->GetLastTime() );
    const int limit = std::min( p.value( "limit", 100 ), 2000 );
    nlohmann::json arr = nlohmann::json::array();
    int c = 0;
    for( const auto& msg : w->GetMessages() )
    {
        if( !msg ) continue;
        if( msg->time < t0 || msg->time > t1 ) continue;
        if( c++ >= limit ) break;
        arr.push_back( {
            { "timeNs", msg->time },
            { "threadCompressed", msg->thread },
            { "threadTid", w->DecompressThread( msg->thread ) },
            { "text", w->GetString( msg->ref ) },
        } );
    }
    return Ok( { { "messages", std::move( arr ) }, { "note", "message stream as weak handoff signal; refine with locks if needed" } } );
}

nlohmann::json HandleGpuContexts( const nlohmann::json& )
{
    auto* w = RequireWorker();
    nlohmann::json arr = nlohmann::json::array();
    int idx = 0;
    for( const auto* ctx : w->GetGpuData() )
    {
        arr.push_back( {
            { "index", idx++ },
            { "name", w->GetString( ctx->name ) },
            { "thread", ctx->thread },
            { "type", int( ctx->type ) },
            { "gpuThreadCount", ctx->threadData.size() },
        } );
    }
    return Ok( { { "contexts", std::move( arr ) } } );
}

nlohmann::json HandleGpuZoneStats( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    for( int i = 0; i < 120000 && !w->AreGpuSourceLocationZonesReady(); ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    if( !w->AreGpuSourceLocationZonesReady() ) return Err( "timeout waiting for GPU source location zones" );
    const int srcloc = p.value( "src_loc", 0 );
    const auto& gz = w->GetGpuSourceLocationZones();
    auto it = gz.find( int16_t( srcloc ) );
    if( it == gz.end() ) return Ok( { { "found", false } } );
    const auto& z = it->second;
    return Ok( {
        { "found", true },
        { "srcLoc", srcloc },
        { "events", z.zones.size() },
        { "totalNs", z.total },
        { "minNs", z.min },
        { "maxNs", z.max },
    } );
}

nlohmann::json HandleGpuComparePasses( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const int topN = std::min( p.value( "top_changes", 20 ), 500 );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t a, b;
    try
    {
        a = ResolveFrameIndexParam( *w, *fd, p, "frame_index_a", "gui_frame_number_a", "display_frame_number_a" );
        b = ResolveFrameIndexParam( *w, *fd, p, "frame_index_b", "gui_frame_number_b", "display_frame_number_b" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    AggMap ca, cb;
    const uint64_t maxV = ParseMaxTimelineVisits( p );
    uint64_t used = 0;
    AggregateGpuZones( *w, w->GetFrameBegin( *fd, a ), w->GetFrameEnd( *fd, a ), ca, maxV, used );
    AggregateGpuZones( *w, w->GetFrameBegin( *fd, b ), w->GetFrameEnd( *fd, b ), cb, maxV, used );
    return Ok( { { "gpuDeltaBySrcLoc", DiffAgg( *w, ca, cb, topN ) } } );
}

nlohmann::json HandleGpuDiffZones( const nlohmann::json& p )
{
    return HandleGpuComparePasses( p );
}

nlohmann::json HandleMemorySummarize( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t pool = p.value( "pool_name_id", 0ull );
    const tracy::MemData& md = w->GetMemoryNamed( pool );
    auto lock = w->ObtainLockForMainThread();
    int64_t t0 = p.value( "start_ns", w->GetFirstTime() );
    int64_t t1 = p.value( "end_ns", w->GetLastTime() );
    int64_t allocVol = 0, freeVol = 0;
    uint32_t allocCnt = 0, freeCnt = 0;
    for( const auto& ev : md.data )
    {
        const int64_t ta = ev.TimeAlloc();
        if( ta < t0 || ta > t1 ) continue;
        const uint64_t sz = ev.Size();
        allocVol += (int64_t)sz;
        allocCnt++;
    }
    for( auto idx : md.frees )
    {
        const auto& ev = md.data[idx];
        const int64_t tf = ev.TimeFree();
        if( tf < t0 || tf > t1 ) continue;
        freeVol += (int64_t)ev.Size();
        freeCnt++;
    }
    return Ok( {
        { "poolNameId", pool },
        { "poolName", pool ? w->GetString( pool ) : std::string( "default" ) },
        { "windowStartNs", t0 },
        { "windowEndNs", t1 },
        { "allocCount", allocCnt },
        { "freeCount", freeCnt },
        { "allocBytesApprox", allocVol },
        { "freeBytesApprox", freeVol },
        { "trackedHigh", md.high },
        { "trackedLow", md.low },
    } );
}

nlohmann::json HandleMemorySpikes( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t pool = p.value( "pool_name_id", 0ull );
    const tracy::MemData& md = w->GetMemoryNamed( pool );
    const int top = std::min( p.value( "top_k", 20 ), 500 );
    struct S
    {
        int64_t t;
        uint64_t sz;
    };
    std::vector<S> allocs;
    auto lock = w->ObtainLockForMainThread();
    for( const auto& ev : md.data )
    {
        allocs.push_back( { ev.TimeAlloc(), ev.Size() } );
    }
    std::sort( allocs.begin(), allocs.end(), []( const S& a, const S& b ) { return a.sz > b.sz; } );
    nlohmann::json arr = nlohmann::json::array();
    for( int i = 0; i < top && i < (int)allocs.size(); ++i )
    {
        arr.push_back( { { "timeNs", allocs[i].t }, { "size", allocs[i].sz } } );
    }
    return Ok( { { "largestAllocs", std::move( arr ) } } );
}

nlohmann::json HandleMemoryDiffFrames( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t pool = p.value( "pool_name_id", 0ull );
    const size_t frame_set = p.value( "frame_set", 0 );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t a, b;
    try
    {
        a = ResolveFrameIndexParam( *w, *fd, p, "frame_index_a", "gui_frame_number_a", "display_frame_number_a" );
        b = ResolveFrameIndexParam( *w, *fd, p, "frame_index_b", "gui_frame_number_b", "display_frame_number_b" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    const tracy::MemData& md = w->GetMemoryNamed( pool );
    auto lock = w->ObtainLockForMainThread();
    auto countWindow = [&]( int64_t t0, int64_t t1 ) {
        int64_t allocB = 0, freeB = 0;
        uint32_t ac = 0, fc = 0;
        for( const auto& ev : md.data )
        {
            const int64_t ta = ev.TimeAlloc();
            if( ta >= t0 && ta <= t1 )
            {
                allocB += (int64_t)ev.Size();
                ac++;
            }
        }
        for( auto idx : md.frees )
        {
            const auto& ev = md.data[idx];
            const int64_t tf = ev.TimeFree();
            if( tf >= t0 && tf <= t1 )
            {
                freeB += (int64_t)ev.Size();
                fc++;
            }
        }
        return std::tuple{ ac, fc, allocB, freeB };
    };
    const auto [acA, fcA, abA, fbA] = countWindow( w->GetFrameBegin( *fd, a ), w->GetFrameEnd( *fd, a ) );
    const auto [acB, fcB, abB, fbB] = countWindow( w->GetFrameBegin( *fd, b ), w->GetFrameEnd( *fd, b ) );
    return Ok( {
        { "frameA", { { "allocCount", acA }, { "freeCount", fcA }, { "allocBytes", abA }, { "freeBytes", fbA } } },
        { "frameB", { { "allocCount", acB }, { "freeCount", fcB }, { "allocBytes", abB }, { "freeBytes", fbB } } },
    } );
}

nlohmann::json HandleMemoryHotSites( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint64_t pool = p.value( "pool_name_id", 0ull );
    const int top = std::min( p.value( "top_k", 30 ), 500 );
    const tracy::MemData& md = w->GetMemoryNamed( pool );
    std::unordered_map<uint64_t, std::pair<int64_t, uint32_t>> byPtr;
    auto lock = w->ObtainLockForMainThread();
    for( const auto& ev : md.data )
    {
        auto& e = byPtr[ev.Ptr()];
        e.first += (int64_t)ev.Size();
        e.second++;
    }
    std::vector<std::pair<uint64_t, std::pair<int64_t, uint32_t>>> vec( byPtr.begin(), byPtr.end() );
    std::sort( vec.begin(), vec.end(), []( const auto& a, const auto& b ) { return a.second.first > b.second.first; } );
    nlohmann::json arr = nlohmann::json::array();
    for( int i = 0; i < top && i < (int)vec.size(); ++i )
    {
        arr.push_back( {
            { "ptr", vec[i].first },
            { "approxTotalBytes", vec[i].second.first },
            { "allocEvents", vec[i].second.second },
        } );
    }
    return Ok( { { "hotPointers", std::move( arr ) }, { "note", "pointer-keyed aggregates; use with allocation callstacks in GUI for symbol names" } } );
}

nlohmann::json HandleLocksSummarize( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t offset = p.value( "offset", 0 );
    const size_t limit = std::min<size_t>( p.value( "limit", 50 ), 500 );
    struct Row
    {
        uint32_t lockId;
        int16_t srcloc;
        size_t timelineEvents;
        bool isContended;
        bool valid;
    };
    std::vector<Row> rows;
    auto lock = w->ObtainLockForMainThread();
    for( const auto& kv : w->GetLockMap() )
    {
        const auto* lm = kv.second;
        if( !lm ) continue;
        rows.push_back( { kv.first, lm->srcloc, lm->timeline.size(), lm->isContended != 0, lm->valid != 0 } );
    }
    const size_t total = rows.size();
    nlohmann::json arr = nlohmann::json::array();
    for( size_t i = offset; i < rows.size() && arr.size() < limit; ++i )
    {
        arr.push_back( {
            { "lockId", rows[i].lockId },
            { "srcloc", rows[i].srcloc },
            { "timelineEvents", rows[i].timelineEvents },
            { "isContended", rows[i].isContended },
            { "valid", rows[i].valid },
        } );
    }
    const bool truncated = offset + arr.size() < total;
    return Ok( {
        { "locks", std::move( arr ) },
        { "total", total },
        { "offset", offset },
        { "limit", limit },
        { "truncated", truncated },
    } );
}

nlohmann::json HandleLocksContentionSpikes( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const int top = std::min( p.value( "top_k", 20 ), 500 );
    struct WaitRec
    {
        uint32_t lockId;
        int64_t dur;
        int64_t time;
        uint8_t thread;
    };
    std::vector<WaitRec> waits;
    auto lock = w->ObtainLockForMainThread();
    for( const auto& kv : w->GetLockMap() )
    {
        const auto* lm = kv.second;
        if( !lm ) continue;
        for( const auto& le : lm->timeline )
        {
            const auto* e = le.ptr.get();
            if( !e ) continue;
            if( e->type != tracy::LockEvent::Type::Wait && e->type != tracy::LockEvent::Type::WaitShared ) continue;
            int64_t end = e->Time();
            uint8_t th = e->thread;
            const tracy::LockEvent* rel = nullptr;
            for( size_t i = 0; i < lm->timeline.size(); ++i )
            {
                const auto* e2 = lm->timeline[i].ptr.get();
                if( !e2 || e2->thread != th ) continue;
                if( e2->Time() <= e->Time() ) continue;
                if( e2->type == tracy::LockEvent::Type::Obtain || e2->type == tracy::LockEvent::Type::ObtainShared )
                {
                    rel = e2;
                    break;
                }
            }
            if( rel ) waits.push_back( { kv.first, rel->Time() - e->Time(), e->Time(), th } );
        }
    }
    std::sort( waits.begin(), waits.end(), []( const WaitRec& a, const WaitRec& b ) { return a.dur > b.dur; } );
    nlohmann::json arr = nlohmann::json::array();
    for( int i = 0; i < top && i < (int)waits.size(); ++i )
    {
        arr.push_back( {
            { "lockId", waits[i].lockId },
            { "waitNs", waits[i].dur },
            { "timeNs", waits[i].time },
            { "thread", waits[i].thread },
        } );
    }
    return Ok( { { "longestWaits", std::move( arr ) }, { "note", "heuristic pairing wait->next obtain on same thread" } } );
}

nlohmann::json HandleLocksBlockedThreads( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const uint32_t lockId = p.value( "lock_id", 0 );
    auto lock = w->ObtainLockForMainThread();
    auto it = w->GetLockMap().find( lockId );
    if( it == w->GetLockMap().end() || !it->second ) return Err( "lock_id not found" );
    const auto* lm = it->second;
    nlohmann::json arr = nlohmann::json::array();
    for( const auto& le : lm->timeline )
    {
        const auto* e = le.ptr.get();
        if( !e ) continue;
        if( e->type == tracy::LockEvent::Type::Wait || e->type == tracy::LockEvent::Type::WaitShared )
        {
            arr.push_back( { { "timeNs", e->Time() }, { "thread", e->thread }, { "type", int( e->type ) } } );
        }
    }
    return Ok( { { "waitEvents", std::move( arr ) } } );
}

nlohmann::json HandleLocksCompareFrames( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t a, b;
    try
    {
        a = ResolveFrameIndexParam( *w, *fd, p, "frame_index_a", "gui_frame_number_a", "display_frame_number_a" );
        b = ResolveFrameIndexParam( *w, *fd, p, "frame_index_b", "gui_frame_number_b", "display_frame_number_b" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    const int64_t ta0 = w->GetFrameBegin( *fd, a );
    const int64_t ta1 = w->GetFrameEnd( *fd, a );
    const int64_t tb0 = w->GetFrameBegin( *fd, b );
    const int64_t tb1 = w->GetFrameEnd( *fd, b );
    auto countWaits = [&]( int64_t t0, int64_t t1 ) {
        uint32_t cnt = 0;
        auto lock = w->ObtainLockForMainThread();
        for( const auto& kv : w->GetLockMap() )
        {
            const auto* lm = kv.second;
            if( !lm ) continue;
            for( const auto& le : lm->timeline )
            {
                const auto* e = le.ptr.get();
                if( !e ) continue;
                if( e->Time() < t0 || e->Time() > t1 ) continue;
                if( e->type == tracy::LockEvent::Type::Wait || e->type == tracy::LockEvent::Type::WaitShared ) cnt++;
            }
        }
        return cnt;
    };
    return Ok( {
        { "frameA", { { "waitEvents", countWaits( ta0, ta1 ) } } },
        { "frameB", { { "waitEvents", countWaits( tb0, tb1 ) } } },
        { "note", "counts wait events only; extend with durations if needed" },
    } );
}

nlohmann::json HandleFrameImage( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const size_t frame_set = p.value( "frame_set", 0 );
    const bool includeBase64 = p.value( "include_base64", false );
    const size_t maxPixels = std::min<size_t>( p.value( "max_pixels", 2'000'000 ), 50'000'000 );
    const auto* fd = GetFrameSet( *w, frame_set );
    size_t frame_index;
    try
    {
        frame_index = ResolveFrameIndexParam( *w, *fd, p, "frame_index", "gui_frame_number", "display_frame_number" );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
    const auto* fi = w->GetFrameImage( *fd, frame_index );
    if( !fi ) return Err( "no screenshot for this frame" );
    const char* raw = w->UnpackFrameImage( *fi );
    if( !raw ) return Err( "unpack failed" );
    const size_t bytes = size_t( fi->w ) * fi->h * 4;
    const size_t pixels = size_t( fi->w ) * size_t( fi->h );
    nlohmann::json meta = {
        { "frame_set", frame_set },
        { "frame_index", frame_index },
        { "guiFrameNumber", GuiFrameNumber( *w, *fd, frame_index ) },
        { "uiKind", FrameUiKind( *w, *fd, frame_index ) },
        { "width", fi->w },
        { "height", fi->h },
        { "flip", fi->flip != 0 },
        { "format", "rgba8" },
        { "rgbaBytes", bytes },
    };
    if( !includeBase64 )
    {
        meta["note"] = "Set include_base64=true to fetch base64 (large). Use max_pixels to cap decoded size.";
        return Ok( std::move( meta ) );
    }
    if( pixels > maxPixels )
    {
        meta["tooLarge"] = true;
        meta["maxPixels"] = maxPixels;
        meta["note"] = "Screenshot exceeds max_pixels; increase max_pixels or view in Tracy GUI.";
        return Ok( std::move( meta ) );
    }
    meta["base64"] = Base64Encode( reinterpret_cast<const uint8_t*>( raw ), bytes );
    return Ok( std::move( meta ) );
}

nlohmann::json HandleFrameFindMarker( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    std::string sub = p.value( "substring", "" );
    std::transform( sub.begin(), sub.end(), sub.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
    GetFrameSet( *w, p.value( "frame_set", 0 ) );
    nlohmann::json hits = nlohmann::json::array();
    for( const auto& msg : w->GetMessages() )
    {
        if( !msg ) continue;
        std::string t = w->GetString( msg->ref );
        std::string tl = t;
        std::transform( tl.begin(), tl.end(), tl.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
        if( sub.empty() || tl.find( sub ) != std::string::npos )
        {
            hits.push_back( { { "timeNs", msg->time }, { "threadTid", w->DecompressThread( msg->thread ) }, { "text", std::move( t ) } } );
        }
    }
    return Ok( { { "messages", std::move( hits ) }, { "note", "text search on messages; correlate times to frames in the GUI workflow" } } );
}

nlohmann::json HandleSourceForZone( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const int srcloc = p.value( "src_loc", 0 );
    const auto& sl = w->GetSourceLocation( int16_t( srcloc ) );
    return Ok( {
        { "srcLoc", srcloc },
        { "name", w->GetZoneName( sl ) },
        { "function", w->GetString( sl.function ) },
        { "file", w->GetString( sl.file ) },
        { "line", sl.line },
    } );
}

nlohmann::json HandleSourceGroupFile( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    WaitSourceZones( *w );
    const int top = std::min( p.value( "top_files", 30 ), 500 );
    std::unordered_map<std::string, std::pair<int64_t, uint32_t>> byFile;
    auto lock = w->ObtainLockForMainThread();
    for( const auto& kv : w->GetSourceLocationZones() )
    {
        const auto& sl = w->GetSourceLocation( kv.first );
        std::string f = w->GetString( sl.file );
        auto& e = byFile[f];
        e.first += kv.second.total;
        e.second += (uint32_t)kv.second.zones.size();
    }
    std::vector<std::pair<std::string, std::pair<int64_t, uint32_t>>> vec( byFile.begin(), byFile.end() );
    std::sort( vec.begin(), vec.end(), []( const auto& a, const auto& b ) { return a.second.first > b.second.first; } );
    nlohmann::json arr = nlohmann::json::array();
    for( int i = 0; i < top && i < (int)vec.size(); ++i )
    {
        arr.push_back( {
            { "file", vec[i].first },
            { "totalNs", vec[i].second.first },
            { "zoneEvents", vec[i].second.second },
        } );
    }
    return Ok( { { "files", std::move( arr ) } } );
}

nlohmann::json HandleSourceOpenContext( const nlohmann::json& p )
{
    auto* w = RequireWorker();
    const int srcloc = p.value( "src_loc", 0 );
    const int ctxLines = std::min( p.value( "context_lines", 4 ), 40 );
    const auto& sl = w->GetSourceLocation( int16_t( srcloc ) );
    const char* path = w->GetString( sl.file );
    nlohmann::json out = {
        { "srcLoc", srcloc },
        { "file", path },
        { "line", sl.line },
    };
    auto blk = w->GetSourceFileFromCache( path );
    if( blk.data && blk.len > 0 && sl.line > 0 )
    {
        std::string_view sv( blk.data, blk.len );
        std::vector<std::string_view> lines;
        size_t pos = 0;
        while( pos < sv.size() )
        {
            size_t nl = sv.find( '\n', pos );
            if( nl == std::string_view::npos )
            {
                lines.push_back( sv.substr( pos ) );
                break;
            }
            lines.push_back( sv.substr( pos, nl - pos ) );
            pos = nl + 1;
        }
        const int li = (int)sl.line - 1;
        const int from = std::max( 0, li - ctxLines );
        const int to = std::min( (int)lines.size(), li + ctxLines + 1 );
        nlohmann::json snip = nlohmann::json::array();
        for( int i = from; i < to; ++i )
        {
            snip.push_back( { { "line", i + 1 }, { "text", std::string( lines[i] ) } } );
        }
        out["cachedSnippet"] = std::move( snip );
    }
    else
    {
        out["cachedSnippet"] = nullptr;
    }
    return Ok( std::move( out ) );
}

static nlohmann::json DispatchImpl( const std::string& method, const nlohmann::json& params )
{
    try
    {
        if( method == "open" ) return HandleOpen( params );
        if( method == "close" ) return HandleClose( params );
        if( method == "ping" ) return HandlePing( params );
        if( method == "metadata" ) return HandleMetadata( params );
        if( method == "frames.list" ) return HandleFramesList( params );
        if( method == "frames.slowest" ) return HandleFramesSlowest( params );
        if( method == "frames.summarize" ) return HandleFramesSummarize( params );
        if( method == "frames.summarize_thread" ) return HandleFramesSummarizeThread( params );
        if( method == "frames.compare" ) return HandleFramesCompare( params );
        if( method == "frames.list_functions" ) return HandleFramesListFunctions( params );
        if( method == "frames.compare_functions" ) return HandleFramesCompareFunctions( params );
        if( method == "frames.resolve" ) return HandleFramesResolve( params );
        if( method == "view.context" ) return HandleViewContext( params );
        if( method == "zones.search" ) return HandleZonesSearch( params );
        if( method == "zones.stats" ) return HandleZonesStats( params );
        if( method == "zones.instances" ) return HandleZonesInstances( params );
        if( method == "zones.callstack" ) return HandleZonesCallstack( params );
        if( method == "zones.compare_frames" ) return HandleZonesCompareFrames( params );
        if( method == "threads.list" ) return HandleThreadsList( params );
        if( method == "threads.timeline_summary" ) return HandleThreadsTimelineSummary( params );
        if( method == "threads.gaps" ) return HandleThreadsGaps( params );
        if( method == "threads.handoff" ) return HandleThreadsHandoff( params );
        if( method == "gpu.contexts" ) return HandleGpuContexts( params );
        if( method == "gpu.zone_stats" ) return HandleGpuZoneStats( params );
        if( method == "gpu.compare_passes" ) return HandleGpuComparePasses( params );
        if( method == "gpu.diff_zones" ) return HandleGpuDiffZones( params );
        if( method == "memory.summarize" ) return HandleMemorySummarize( params );
        if( method == "memory.spikes" ) return HandleMemorySpikes( params );
        if( method == "memory.diff_frames" ) return HandleMemoryDiffFrames( params );
        if( method == "memory.hot_sites" ) return HandleMemoryHotSites( params );
        if( method == "locks.summarize" ) return HandleLocksSummarize( params );
        if( method == "locks.contention_spikes" ) return HandleLocksContentionSpikes( params );
        if( method == "locks.blocked_threads" ) return HandleLocksBlockedThreads( params );
        if( method == "locks.compare_frames" ) return HandleLocksCompareFrames( params );
        if( method == "frame.image" ) return HandleFrameImage( params );
        if( method == "frame.find_marker" ) return HandleFrameFindMarker( params );
        if( method == "source.for_zone" ) return HandleSourceForZone( params );
        if( method == "source.group_by_file" ) return HandleSourceGroupFile( params );
        if( method == "source.open_context" ) return HandleSourceOpenContext( params );
        return Err( "unknown method: " + method );
    }
    catch( const std::exception& e )
    {
        return Err( e.what() );
    }
}

#if defined( _WIN32 ) && defined( _MSC_VER )
struct DispatchThunkCtx
{
    const std::string* method;
    const nlohmann::json* params;
    nlohmann::json* result;
};

static void DispatchThunk( void* ctx )
{
    auto* c = static_cast<DispatchThunkCtx*>( ctx );
    *c->result = DispatchImpl( *c->method, *c->params );
}

// C2712: __try cannot live in a function that needs C++ unwinding; keep this frame pointer-only.
static bool RunDispatchWithSeh( void ( *thunk )( void* ), void* ctx )
{
    __try
    {
        thunk( ctx );
        return true;
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        return false;
    }
}
#endif

nlohmann::json Dispatch( const std::string& method, const nlohmann::json& params )
{
#if defined( _WIN32 ) && defined( _MSC_VER )
    nlohmann::json result;
    DispatchThunkCtx ctx{ &method, &params, &result };
    if( !RunDispatchWithSeh( DispatchThunk, &ctx ) )
    {
        return Err(
            "native access violation while reading trace data (often corrupt pointers or memory pressure on huge "
            "timelines). Use the Tracy GUI; avoid max_timeline_visits: 0 on very large captures." );
    }
    return result;
#else
    return DispatchImpl( method, params );
#endif
}

}

#if defined( TRACY_MCP_BRIDGE_STANDALONE )
int main( int, char** )
{
    std::ios::sync_with_stdio( false );
    if( const char* p = std::getenv( "TRACY_MCP_HTTP_PORT" ) )
    {
        if( *p && tracy::mcp::RunStreamableHttp( p, std::getenv( "TRACY_MCP_HTTP_HOST" ) ) ) return 0;
        if( *p ) return 1;
    }

    std::string line;
    while( std::getline( std::cin, line ) )
    {
        if( line.empty() ) continue;
        nlohmann::json req;
        try
        {
            req = nlohmann::json::parse( line );
        }
        catch( ... )
        {
            tracy_mcp_bridge::WriteLine( { { "id", nullptr }, { "error", "invalid json" } } );
            continue;
        }
        const auto id = req.contains( "id" ) ? req["id"] : nlohmann::json( nullptr );
        const std::string method = req.value( "method", "" );
        nlohmann::json params = req.contains( "params" ) ? req["params"] : nlohmann::json::object();
        nlohmann::json res = tracy_mcp_bridge::Dispatch( method, params );
        nlohmann::json out = { { "id", id } };
        if( res.contains( "ok" ) && res["ok"].get<bool>() )
        {
            out["result"] = std::move( res );
        }
        else
        {
            out["error"] = res.contains( "error" ) ? res["error"] : "unknown error";
        }
        tracy_mcp_bridge::WriteLine( out );
    }
    return 0;
}
#endif
