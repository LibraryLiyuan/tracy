#include "TracyQueryService.hpp"

#include "TracyAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace tracy::query
{
namespace
{

using nlohmann::json;

std::string Decimal( uint64_t value ) { return std::to_string( value ); }
std::string Decimal( int64_t value ) { return std::to_string( value ); }

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
    return value;
}

uint64_t Fnv1a( const std::string& value )
{
    uint64_t hash = 14695981039346656037ull;
    for( const unsigned char c : value )
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex16( uint64_t value )
{
    std::ostringstream out;
    out << std::hex << std::setw( 16 ) << std::setfill( '0' ) << value;
    return out.str();
}

std::string Base64UrlEncode( const std::string& input )
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve( ( input.size() * 4 + 2 ) / 3 );
    uint32_t accumulator = 0;
    int bits = 0;
    for( const unsigned char value : input )
    {
        accumulator = ( accumulator << 8 ) | value;
        bits += 8;
        while( bits >= 6 )
        {
            bits -= 6;
            output.push_back( alphabet[( accumulator >> bits ) & 63] );
        }
    }
    if( bits != 0 ) output.push_back( alphabet[( accumulator << ( 6 - bits ) ) & 63] );
    return output;
}

std::string Base64UrlDecode( const std::string& input )
{
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> result {};
        result.fill( -1 );
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for( size_t i = 0; i < alphabet.size(); i++ ) result[uint8_t( alphabet[i] )] = int8_t( i );
        return result;
    }();
    std::string output;
    uint32_t accumulator = 0;
    int bits = 0;
    for( const unsigned char value : input )
    {
        if( table[value] < 0 ) throw QueryError( "STALE_CURSOR", "cursor is malformed" );
        accumulator = ( accumulator << 6 ) | uint32_t( table[value] );
        bits += 6;
        if( bits >= 8 )
        {
            bits -= 8;
            output.push_back( char( ( accumulator >> bits ) & 255 ) );
        }
    }
    return output;
}

std::vector<std::string> Split( const std::string& value, char separator )
{
    std::vector<std::string> result;
    std::string item;
    std::istringstream stream( value );
    while( std::getline( stream, item, separator ) ) result.emplace_back( item );
    return result;
}

struct PageRequest
{
    size_t offset = 0;
    size_t limit = DefaultPageSize;
    std::string binding;
};

PageRequest ParsePage( const json& params, const std::string& method, const TraceSessionSnapshot& trace )
{
    PageRequest page;
    if( params.contains( "limit" ) )
    {
        if( !params["limit"].is_number_unsigned() && !params["limit"].is_number_integer() ) throw QueryError( "INVALID_PARAMS", "limit must be an integer" );
        const auto limit = params["limit"].get<int64_t>();
        if( limit < 1 || limit > int64_t( MaximumPageSize ) ) throw QueryError( "INVALID_PARAMS", "limit must be between 1 and 1000" );
        page.limit = size_t( limit );
    }

    json bound = params;
    bound.erase( "cursor" );
    bound.erase( "limit" );
    page.binding = Hex16( Fnv1a( bound.dump() ) );
    if( params.contains( "cursor" ) )
    {
        if( !params["cursor"].is_string() ) throw QueryError( "INVALID_PARAMS", "cursor must be a string" );
        const auto parts = Split( Base64UrlDecode( params["cursor"].get<std::string>() ), '|' );
        if( parts.size() != 7 || parts[0] != "v1" || parts[1] != trace.id || parts[2] != Decimal( trace.revision ) || parts[3] != method || parts[4] != page.binding )
        {
            throw QueryError( "STALE_CURSOR", "cursor does not match the trace, revision, method, or filters" );
        }
        try
        {
            page.offset = size_t( std::stoull( parts[5] ) );
            if( parts[6] != "stable" ) throw std::invalid_argument( "tie breaker" );
        }
        catch( const std::exception& )
        {
            throw QueryError( "STALE_CURSOR", "cursor offset is invalid" );
        }
    }
    return page;
}

std::string NextCursor( const PageRequest& page, const std::string& method, const TraceSessionSnapshot& trace, size_t returned, bool hasMore )
{
    if( !hasMore ) return {};
    const std::string value = "v1|" + trace.id + '|' + Decimal( trace.revision ) + '|' + method + '|' + page.binding + '|' + std::to_string( page.offset + returned ) + "|stable";
    return Base64UrlEncode( value );
}

json PageJson( const PageRequest& page, size_t returned, const std::string& nextCursor, bool truncated = false )
{
    return {
        { "limit", page.limit }, { "returned", returned },
        { "next_cursor", nextCursor.empty() ? json( nullptr ) : json( nextCursor ) }, { "truncated", truncated }
    };
}

json TraceJson( const TraceSessionSnapshot& trace )
{
    return {
        { "id", trace.id }, { "fingerprint", trace.fingerprint },
        { "source_kind", analysis::ToString( trace.sourceKind ) }, { "state", analysis::ToString( trace.state ) },
        { "revision", Decimal( trace.revision ) }, { "watermark_ns", Decimal( trace.watermarkNs ) }, { "complete", trace.complete }
    };
}

json Success( const json& id, json data, const std::optional<TraceSessionSnapshot>& trace = std::nullopt, const std::optional<json>& page = std::nullopt, std::vector<std::string> warnings = {} )
{
    json result = {
        { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion }, { "id", id }, { "ok", true },
        { "data", std::move( data ) }, { "warnings", std::move( warnings ) }
    };
    if( trace ) result["trace"] = TraceJson( *trace );
    if( page ) result["page"] = *page;
    return result;
}

json CountsJson( const analysis::TraceCountsDto& value )
{
    return {
        { "frames", Decimal( value.frames ) }, { "frame_sets", Decimal( value.frameSets ) },
        { "cpu_zones", Decimal( value.cpuZones ) }, { "gpu_zones", Decimal( value.gpuZones ) },
        { "threads", Decimal( value.threads ) }, { "locks", Decimal( value.locks ) },
        { "plots", Decimal( value.plots ) }, { "messages", Decimal( value.messages ) },
        { "memory_events", Decimal( value.memoryEvents ) }, { "memory_pools", Decimal( value.memoryPools ) },
        { "context_switches", Decimal( value.contextSwitches ) },
        { "callstack_payloads", Decimal( value.callstackPayloads ) }, { "callstack_frames", Decimal( value.callstackFrames ) },
        { "samples", Decimal( value.samples ) }, { "hardware_samples", Decimal( value.hardwareSamples ) },
        { "symbols", Decimal( value.symbols ) }, { "symbol_code_bytes", Decimal( value.symbolCodeBytes ) },
        { "source_locations", Decimal( value.sourceLocations ) }, { "source_cache_files", Decimal( value.sourceCacheFiles ) },
        { "source_cache_bytes", Decimal( value.sourceCacheBytes ) }, { "frame_images", Decimal( value.frameImages ) }
    };
}

json TraceInfoJson( const analysis::TraceInfoDto& value )
{
    return {
        { "fingerprint", value.fingerprint }, { "capture_name", value.captureName }, { "capture_program", value.captureProgram },
        { "host_info", value.hostInfo }, { "capture_time", Decimal( value.captureTime ) }, { "executable_time", Decimal( value.executableTime ) },
        { "process_id", Decimal( value.processId ) }, { "trace_version", value.traceVersion }, { "resolution", Decimal( value.resolution ) },
        { "first_time_ns", Decimal( value.firstTimeNs ) }, { "last_time_ns", Decimal( value.lastTimeNs ) },
        { "load_time_ns", Decimal( value.loadTimeNs ) }, { "cpu_id", value.cpuId }, { "cpu_manufacturer", value.cpuManufacturer },
        { "has_crash", value.hasCrash }, { "samples_inconsistent", value.samplesInconsistent },
        { "counts", CountsJson( value.counts ) }, { "app_info", value.appInfo }
    };
}

json CapabilityJson( const analysis::Capability& value )
{
    return {
        { "domain", value.domain }, { "present", value.present }, { "queryable", value.queryable }, { "indexed", value.indexed },
        { "reason", value.reason }, { "methods", value.methods },
        { "limits", { { "default_page_size", DefaultPageSize }, { "maximum_page_size", MaximumPageSize }, { "response_bytes", MaximumResponseBytes } } }
    };
}

json StatisticsJson( const analysis::Statistics& value )
{
    return {
        { "count", Decimal( value.count ) }, { "total_ns", Decimal( value.total ) }, { "min_ns", Decimal( value.min ) }, { "max_ns", Decimal( value.max ) },
        { "mean_ns", value.mean }, { "median_ns", value.median }, { "stddev_ns", value.stddev },
        { "p50_ns", value.p50 }, { "p90_ns", value.p90 }, { "p95_ns", value.p95 }, { "p99_ns", value.p99 },
        { "truncated_mean_ns", value.truncatedMean }
    };
}

json ThreadJson( const analysis::ThreadDto& value )
{
    return {
        { "ref", value.ref }, { "native_id", Decimal( value.nativeId ) }, { "process_id", Decimal( value.processId ) },
        { "name", value.name }, { "fiber", value.fiber }, { "zone_count", Decimal( value.zoneCount ) },
        { "message_count", Decimal( value.messageCount ) }, { "sample_count", Decimal( value.sampleCount ) },
        { "context_switch_count", Decimal( value.contextSwitchCount ) }, { "running_time_ns", Decimal( value.runningTimeNs ) },
        { "migrations", value.migrations }
    };
}

json FrameSetJson( const analysis::FrameSetDto& value )
{
    return {
        { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "continuous", value.continuous },
        { "frame_count", value.frameCount }, { "complete_frame_count", value.completeFrameCount }
    };
}

json FrameJson( const analysis::FrameDto& value )
{
    return {
        { "ref", value.ref }, { "frame_set_ref", value.frameSetRef }, { "index", value.index },
        { "begin_ns", Decimal( value.beginNs ) }, { "end_ns", value.endNs ? json( Decimal( *value.endNs ) ) : json( nullptr ) },
        { "duration_ns", value.endNs ? json( Decimal( *value.endNs - value.beginNs ) ) : json( nullptr ) },
        { "frame_image_ref", value.imageRef ? json( *value.imageRef ) : json( nullptr ) }, { "complete", value.complete }
    };
}

json CpuZoneJson( const analysis::CpuZoneDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef }, { "source_location_ref", value.sourceLocationRef },
        { "parent_ref", value.parentRef ? json( *value.parentRef ) : json( nullptr ) }, { "name", value.name },
        { "function", value.function }, { "file", value.file }, { "line", value.line },
        { "start_ns", Decimal( value.startNs ) }, { "end_ns", value.endNs ? json( Decimal( *value.endNs ) ) : json( nullptr ) },
        { "duration_ns", value.endNs ? json( Decimal( *value.endNs - value.startNs ) ) : json( nullptr ) },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) }, { "complete", value.complete }
    };
}

json GpuZoneJson( const analysis::GpuZoneDto& value )
{
    return {
        { "ref", value.ref }, { "context_ref", value.contextRef }, { "thread_ref", value.threadRef },
        { "source_location_ref", value.sourceLocationRef }, { "parent_ref", value.parentRef ? json( *value.parentRef ) : json( nullptr ) },
        { "name", value.name }, { "function", value.function }, { "file", value.file }, { "line", value.line },
        { "gpu_start_ns", Decimal( value.gpuStartNs ) }, { "gpu_end_ns", value.gpuEndNs ? json( Decimal( *value.gpuEndNs ) ) : json( nullptr ) },
        { "gpu_duration_ns", value.gpuEndNs ? json( Decimal( *value.gpuEndNs - value.gpuStartNs ) ) : json( nullptr ) },
        { "cpu_start_ns", Decimal( value.cpuStartNs ) }, { "cpu_end_ns", value.cpuEndNs ? json( Decimal( *value.cpuEndNs ) ) : json( nullptr ) },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) }, { "complete", value.complete }
    };
}

json MemoryPoolJson( const analysis::MemoryPoolDto& value )
{
    return {
        { "ref", value.ref }, { "name", value.name }, { "event_count", Decimal( value.eventCount ) },
        { "active_count", Decimal( value.activeCount ) }, { "active_bytes", Decimal( value.activeBytes ) },
        { "low", "0x" + Hex16( value.low ) }, { "high", "0x" + Hex16( value.high ) },
        { "gpu_d3d12", value.gpuD3D12 }, { "identifier_semantics", value.gpuD3D12 ? "logical_allocation_id" : "address" }
    };
}

json MemoryEventJson( const analysis::MemoryEventDto& value )
{
    return {
        { "ref", value.ref }, { "pool_ref", value.poolRef }, { "address", value.address }, { "size_bytes", Decimal( value.size ) },
        { "allocation_ns", Decimal( value.allocationNs ) }, { "free_ns", value.freeNs ? json( Decimal( *value.freeNs ) ) : json( nullptr ) },
        { "allocation_thread_ref", value.allocationThreadRef }, { "free_thread_ref", value.freeThreadRef ? json( *value.freeThreadRef ) : json( nullptr ) },
        { "allocation_callstack", value.allocationCallstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.allocationCallstack ) ) ) },
        { "free_callstack", value.freeCallstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.freeCallstack ) ) ) }, { "complete", value.complete }
    };
}

json MessageJson( const analysis::MessageDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef }, { "time_ns", Decimal( value.timeNs ) }, { "text", value.text },
        { "color", value.color }, { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "trust", "untrusted_trace_data" }
    };
}

json ProjectFields( json value, const json& params )
{
    if( !params.contains( "fields" ) ) return value;
    if( !params["fields"].is_array() ) throw QueryError( "INVALID_PARAMS", "fields must be an array of strings" );
    std::set<std::string> fields { "ref" };
    for( const auto& field : params["fields"] )
    {
        if( !field.is_string() ) throw QueryError( "INVALID_PARAMS", "fields must contain only strings" );
        fields.emplace( field.get<std::string>() );
    }
    if( value.is_array() )
    {
        for( auto& item : value ) item = ProjectFields( std::move( item ), json { { "fields", params["fields"] } } );
        return value;
    }
    if( !value.is_object() ) return value;
    for( auto it = value.begin(); it != value.end(); )
    {
        if( fields.find( it.key() ) == fields.end() ) it = value.erase( it );
        else ++it;
    }
    return value;
}

bool TextMatches( const std::string& value, const json& params )
{
    if( !params.contains( "filter" ) ) return true;
    const auto& filter = params["filter"];
    if( !filter.is_object() || !filter.contains( "text" ) || !filter["text"].is_string() ) throw QueryError( "INVALID_PARAMS", "filter.text must be a string" );
    std::string haystack = value;
    std::string needle = filter["text"].get<std::string>();
    const bool caseSensitive = filter.value( "case_sensitive", false );
    if( !caseSensitive )
    {
        haystack = Lower( std::move( haystack ) );
        needle = Lower( std::move( needle ) );
    }
    const std::string mode = filter.value( "mode", "contains" );
    if( mode == "exact" ) return haystack == needle;
    if( mode == "contains" ) return haystack.find( needle ) != std::string::npos;
    if( mode == "prefix" ) return haystack.rfind( needle, 0 ) == 0;
    throw QueryError( "INVALID_PARAMS", "filter.mode must be exact, contains, or prefix" );
}

analysis::ScanRange ScanRangeFrom( const json& params, size_t offset, size_t limit )
{
    analysis::ScanRange range;
    range.offset = offset;
    range.limit = limit;
    const auto parse = []( const json& value, const char* name ) -> int64_t {
        try
        {
            if( value.is_string() ) return std::stoll( value.get<std::string>() );
            if( value.is_number_integer() ) return value.get<int64_t>();
        }
        catch( const std::exception& ) {}
        throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be a signed decimal string" );
    };
    if( params.contains( "start_ns" ) ) range.startNs = parse( params["start_ns"], "start_ns" );
    if( params.contains( "end_ns" ) ) range.endNs = parse( params["end_ns"], "end_ns" );
    if( range.endNs <= range.startNs ) throw QueryError( "INVALID_PARAMS", "time range must satisfy start_ns < end_ns" );
    return range;
}

std::string ResolveTraceId( const json& params, const std::optional<std::string>& defaultTraceId )
{
    if( params.contains( "trace_id" ) )
    {
        if( !params["trace_id"].is_string() ) throw QueryError( "INVALID_PARAMS", "trace_id must be a string" );
        return params["trace_id"].get<std::string>();
    }
    if( defaultTraceId ) return *defaultTraceId;
    throw QueryError( "INVALID_PARAMS", "trace_id is required" );
}

size_t ResolveFrameSet( const analysis::WorkerTraceSource& source, const json& params )
{
    const auto sets = source.GetFrameSets();
    if( sets.empty() ) throw QueryError( "CAPABILITY_UNAVAILABLE", "trace contains no frame sets" );
    if( !params.contains( "frame_set" ) ) return 0;
    const auto& value = params["frame_set"];
    if( value.is_number_unsigned() || value.is_number_integer() )
    {
        const auto index = value.get<int64_t>();
        if( index < 0 || size_t( index ) >= sets.size() ) throw QueryError( "ENTITY_NOT_FOUND", "frame set index was not found" );
        return size_t( index );
    }
    if( value.is_string() )
    {
        const auto ref = value.get<std::string>();
        const auto it = std::find_if( sets.begin(), sets.end(), [&]( const auto& item ) { return item.ref == ref || item.name == ref; } );
        if( it == sets.end() ) throw QueryError( "ENTITY_NOT_FOUND", "frame set ref or name was not found" );
        return it->index;
    }
    throw QueryError( "INVALID_PARAMS", "frame_set must be an index, ref, or name" );
}

size_t TopN( const json& params )
{
    const auto value = params.value( "limit", int64_t( DefaultTopN ) );
    if( value < 1 || value > int64_t( MaximumTopN ) ) throw QueryError( "INVALID_PARAMS", "limit must be between 1 and 500" );
    return size_t( value );
}

json DescribeData()
{
    return {
        { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion },
        { "numeric_rules", {
            { "int64", "decimal string" }, { "address", "0x-prefixed hexadecimal string" },
            { "range", "half-open [start_ns,end_ns)" }, { "ratio_percentile_average", "JSON number" }
        } },
        { "limits", {
            { "default_page_size", DefaultPageSize }, { "maximum_page_size", MaximumPageSize },
            { "default_top_n", DefaultTopN }, { "maximum_top_n", MaximumTopN },
            { "request_bytes", MaximumRequestBytes }, { "response_bytes", MaximumResponseBytes },
            { "callstack_default_depth", 32 }, { "callstack_max_depth", 256 },
            { "source_default_bytes", 65536 }, { "source_max_bytes", 1048576 },
            { "frame_image_max_bytes", 16777216 }, { "frame_image_max_dimension", 4096 }
        } },
        { "filter_modes", { "exact", "contains", "prefix" } },
        { "methods", {
            "system.capabilities", "system.describe", "system.schema",
            "trace.open", "trace.status", "trace.list", "trace.close", "trace.info", "trace.overview", "trace.counts", "trace.app_info", "trace.crash",
            "thread.list", "thread.get", "frame.sets", "frame.list", "frame.get", "frame.statistics", "frame.outliers",
            "zone.cpu.search", "zone.cpu.statistics", "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.statistics",
            "memory.pools", "memory.events", "lock.list", "plot.list", "plot.points", "message.search",
            "callstack.resolve", "timeline.slice", "statistics.describe", "validation.run"
        } }
    };
}

template<typename T, typename Scan, typename Match, typename Convert>
std::pair<json, bool> ScanFiltered( const analysis::WorkerTraceSource& source, const json& params, const PageRequest& page, Scan&& scan, Match&& match, Convert&& convert )
{
    json output = json::array();
    size_t rawOffset = 0;
    size_t matchedOffset = 0;
    constexpr size_t chunk = 4096;
    bool exhausted = false;
    while( output.size() <= page.limit && !exhausted )
    {
        auto range = ScanRangeFrom( params, rawOffset, chunk );
        const std::vector<T> values = scan( source, range );
        exhausted = values.size() < chunk;
        rawOffset += values.size();
        for( const auto& value : values )
        {
            if( !match( value ) ) continue;
            if( matchedOffset++ < page.offset ) continue;
            output.emplace_back( convert( value ) );
            if( output.size() > page.limit ) break;
        }
        if( values.empty() ) exhausted = true;
    }
    const bool hasMore = output.size() > page.limit;
    if( hasMore ) output.erase( output.end() - 1 );
    return { std::move( output ), hasMore };
}

}

QueryService::QueryService( SessionManager& sessions )
    : m_sessions( sessions )
{}

json QueryService::Failure( const json& id, const QueryError& error ) const
{
    return Failure( id, error.code, error.what(), error.retryable, error.details );
}

json QueryService::Failure( const json& id, std::string code, std::string message, bool retryable, json details ) const
{
    return {
        { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion }, { "id", id }, { "ok", false },
        { "error", { { "code", std::move( code ) }, { "message", std::move( message ) }, { "retryable", retryable }, { "details", std::move( details ) } } }
    };
}

json QueryService::Execute( const json& request, const std::optional<std::string>& defaultTraceId )
{
    json id = nullptr;
    try
    {
        if( !request.is_object() ) throw QueryError( "INVALID_REQUEST", "request must be a JSON object" );
        if( request.contains( "id" ) ) id = request["id"];
        if( request.value( "protocol", "" ) != QueryProtocol ) throw QueryError( "INVALID_REQUEST", "protocol must be tracy-query/1" );
        if( !request.contains( "id" ) || !( id.is_string() || id.is_number() ) ) throw QueryError( "INVALID_REQUEST", "id must be a string or number" );
        if( !request.contains( "method" ) || !request["method"].is_string() ) throw QueryError( "INVALID_REQUEST", "method must be a string" );
        const auto params = request.value( "params", json::object() );
        if( !params.is_object() ) throw QueryError( "INVALID_PARAMS", "params must be an object" );
        std::lock_guard lock( m_queryMutex );
        auto response = Dispatch( id, request["method"].get<std::string>(), params, defaultTraceId );
        if( DumpProtocolJson( response ).size() > MaximumResponseBytes ) throw QueryError( "RESOURCE_LIMIT", "response exceeds the 8 MiB budget; use pagination or field projection" );
        return response;
    }
    catch( const SessionError& error )
    {
        return Failure( id, ToString( error.code ), error.what(), error.retryable );
    }
    catch( const QueryError& error )
    {
        return Failure( id, error );
    }
    catch( const json::exception& error )
    {
        return Failure( id, "INVALID_PARAMS", error.what() );
    }
    catch( const std::exception& error )
    {
        return Failure( id, "INTERNAL_ERROR", error.what() );
    }
}

json QueryService::Dispatch( const json& id, const std::string& method, const json& params, const std::optional<std::string>& defaultTraceId )
{
    if( method == "system.describe" || method == "system.schema" ) return Success( id, DescribeData() );
    if( method == "trace.open" )
    {
        if( !params.contains( "path" ) || !params["path"].is_string() ) throw QueryError( "INVALID_PARAMS", "path is required" );
        const auto trace = m_sessions.Open( params["path"].get<std::string>() );
        return Success( id, { { "trace_id", trace.id }, { "state", analysis::ToString( trace.state ) }, { "queued", trace.state == analysis::TraceSourceState::Queued } }, trace );
    }
    if( method == "trace.list" )
    {
        json traces = json::array();
        for( const auto& trace : m_sessions.List() ) traces.emplace_back( TraceJson( trace ) );
        return Success( id, { { "traces", std::move( traces ) } } );
    }
    if( method == "trace.status" )
    {
        const auto trace = m_sessions.Status( ResolveTraceId( params, defaultTraceId ) );
        return Success( id, { { "status", TraceJson( trace ) }, { "error_code", trace.errorCode.empty() ? json( nullptr ) : json( trace.errorCode ) }, { "error_message", trace.errorMessage.empty() ? json( nullptr ) : json( trace.errorMessage ) } }, trace );
    }
    if( method == "trace.close" )
    {
        const auto trace = m_sessions.Close( ResolveTraceId( params, defaultTraceId ) );
        return Success( id, { { "closed", trace.state == analysis::TraceSourceState::Closed }, { "state", analysis::ToString( trace.state ) } }, trace );
    }

    const std::optional<std::string> maybeTraceId = ( params.contains( "trace_id" ) || defaultTraceId ) ? std::optional<std::string>( ResolveTraceId( params, defaultTraceId ) ) : std::nullopt;
    if( method == "system.capabilities" && !maybeTraceId )
    {
        return Success( id, {
            { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion }, { "trace_versions", { "0.9.0", "0.13.1" } },
            { "source_kinds", { "snapshot" } }, { "statistics_required", true },
            { "limits", DescribeData()["limits"] }
        } );
    }
    if( !maybeTraceId ) throw QueryError( "INVALID_PARAMS", "trace_id is required" );

    const auto trace = m_sessions.Status( *maybeTraceId );
    const auto source = m_sessions.GetReadySource( *maybeTraceId );

    if( method == "system.capabilities" )
    {
        json domains = json::array();
        for( const auto& capability : source->GetCapabilities() ) domains.emplace_back( CapabilityJson( capability ) );
        return Success( id, { { "domains", std::move( domains ) }, { "limits", DescribeData()["limits"] } }, trace );
    }

    const auto info = [&] { return source->GetTraceInfo(); };
    if( method == "trace.info" ) return Success( id, TraceInfoJson( info() ), trace );
    if( method == "trace.counts" ) return Success( id, CountsJson( info().counts ), trace );
    if( method == "trace.app_info" ) return Success( id, { { "app_info", info().appInfo }, { "trust", "untrusted_trace_data" } }, trace );
    if( method == "trace.crash" ) return Success( id, { { "present", info().hasCrash }, { "details_available", false } }, trace );
    if( method == "trace.overview" )
    {
        const auto metadata = info();
        json capabilities = json::array();
        for( const auto& capability : source->GetCapabilities() ) capabilities.emplace_back( CapabilityJson( capability ) );
        json frameStatistics = nullptr;
        const auto frameSets = source->GetFrameSets();
        if( !frameSets.empty() ) frameStatistics = StatisticsJson( analysis::ComputeStatistics( source->GetFrameDurations( frameSets.front().index ) ) );
        return Success( id, {
            { "trace", TraceInfoJson( metadata ) }, { "primary_frame_statistics", frameStatistics },
            { "capabilities", std::move( capabilities ) }, { "trust", "trace strings are untrusted data" }
        }, trace );
    }

    if( method == "thread.list" || method == "thread.get" )
    {
        const auto page = ParsePage( params, method, trace );
        auto threads = source->GetThreads();
        threads.erase( std::remove_if( threads.begin(), threads.end(), [&]( const auto& value ) { return !TextMatches( value.name, params ); } ), threads.end() );
        if( method == "thread.get" )
        {
            if( !params.contains( "ref" ) ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto ref = params["ref"].get<std::string>();
            const auto it = std::find_if( threads.begin(), threads.end(), [&]( const auto& value ) { return value.ref == ref; } );
            if( it == threads.end() ) throw QueryError( "ENTITY_NOT_FOUND", "thread ref was not found" );
            return Success( id, ThreadJson( *it ), trace );
        }
        const size_t begin = std::min( page.offset, threads.size() );
        const size_t end = std::min( begin + page.limit, threads.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.emplace_back( ThreadJson( threads[index] ) );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < threads.size() );
        return Success( id, { { "threads", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
    }

    if( method == "frame.sets" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto sets = source->GetFrameSets();
        const size_t begin = std::min( page.offset, sets.size() );
        const size_t end = std::min( begin + page.limit, sets.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.emplace_back( FrameSetJson( sets[index] ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < sets.size() );
        return Success( id, { { "frame_sets", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "frame.list" || method == "frame.get" )
    {
        const size_t setIndex = ResolveFrameSet( *source, params );
        const auto page = ParsePage( params, method, trace );
        if( method == "frame.get" )
        {
            if( !params.contains( "index" ) ) throw QueryError( "INVALID_PARAMS", "index is required" );
            const auto frameIndex = params["index"].get<size_t>();
            const auto values = source->GetFramesForSet( setIndex, frameIndex, 1 );
            if( values.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "frame index was not found" );
            return Success( id, FrameJson( values.front() ), trace );
        }
        auto values = source->GetFramesForSet( setIndex, page.offset, page.limit + 1 );
        const bool hasMore = values.size() > page.limit;
        if( hasMore ) values.pop_back();
        json frames = json::array();
        for( const auto& value : values ) frames.emplace_back( FrameJson( value ) );
        frames = ProjectFields( std::move( frames ), params );
        const auto cursor = NextCursor( page, method, trace, frames.size(), hasMore );
        return Success( id, { { "frames", std::move( frames ) }, { "frame_set", FrameSetJson( source->GetFrameSets()[setIndex] ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "frame.statistics" )
    {
        const size_t setIndex = ResolveFrameSet( *source, params );
        return Success( id, { { "frame_set", FrameSetJson( source->GetFrameSets()[setIndex] ) }, { "statistics", StatisticsJson( analysis::ComputeStatistics( source->GetFrameDurations( setIndex ), params.value( "truncate_percentile", 0.90 ) ) ) } }, trace );
    }
    if( method == "frame.outliers" )
    {
        const size_t setIndex = ResolveFrameSet( *source, params );
        const size_t limit = TopN( params );
        const auto durations = source->GetFrameDurations( setIndex );
        std::vector<size_t> order( durations.size() );
        for( size_t index = 0; index < order.size(); index++ ) order[index] = index;
        std::sort( order.begin(), order.end(), [&]( size_t lhs, size_t rhs ) { return durations[lhs] != durations[rhs] ? durations[lhs] > durations[rhs] : lhs < rhs; } );
        if( order.size() > limit ) order.resize( limit );
        json outliers = json::array();
        for( const auto index : order )
        {
            const auto frame = source->GetFramesForSet( setIndex, index, 1 );
            if( !frame.empty() ) outliers.push_back( { { "frame", FrameJson( frame.front() ) }, { "duration_ns", Decimal( durations[index] ) } } );
        }
        return Success( id, { { "frame_set", FrameSetJson( source->GetFrameSets()[setIndex] ) }, { "statistics", StatisticsJson( analysis::ComputeStatistics( durations ) ) }, { "outliers", std::move( outliers ) } }, trace );
    }

    if( method == "zone.cpu.search" )
    {
        const auto page = ParsePage( params, method, trace );
        auto [values, hasMore] = ScanFiltered<analysis::CpuZoneDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanCpuZones( range ); },
            [&]( const auto& value ) { return TextMatches( value.name, params ); }, CpuZoneJson );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "zones", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "zone.gpu.contexts" )
    {
        json contexts = json::array();
        for( const auto& value : source->GetGpuContexts() )
        {
            contexts.push_back( {
                { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "thread_ref", value.threadRef },
                { "zone_count", Decimal( value.zoneCount ) }, { "period", value.period }, { "calibrated", value.calibrated }, { "type", value.type }
            } );
        }
        return Success( id, { { "contexts", std::move( contexts ) } }, trace );
    }
    if( method == "zone.gpu.search" )
    {
        const auto page = ParsePage( params, method, trace );
        auto [values, hasMore] = ScanFiltered<analysis::GpuZoneDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanGpuZones( range ); },
            [&]( const auto& value ) { return TextMatches( value.name, params ); }, GpuZoneJson );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "zones", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "zone.cpu.statistics" || method == "zone.gpu.statistics" )
    {
        const bool gpu = method == "zone.gpu.statistics";
        struct Aggregate { std::string name, file; uint32_t line = 0; std::vector<int64_t> durations; };
        std::map<std::string, Aggregate> groups;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            auto range = ScanRangeFrom( params, offset, chunk );
            if( gpu )
            {
                const auto values = source->ScanGpuZones( range );
                for( const auto& value : values ) if( value.gpuEndNs && TextMatches( value.name, params ) )
                {
                    auto& group = groups[value.sourceLocationRef];
                    group.name = value.name; group.file = value.file; group.line = value.line;
                    group.durations.emplace_back( *value.gpuEndNs - value.gpuStartNs );
                }
                offset += values.size();
                if( values.size() < chunk ) break;
            }
            else
            {
                const auto values = source->ScanCpuZones( range );
                for( const auto& value : values ) if( value.endNs && TextMatches( value.name, params ) )
                {
                    auto& group = groups[value.sourceLocationRef];
                    group.name = value.name; group.file = value.file; group.line = value.line;
                    group.durations.emplace_back( *value.endNs - value.startNs );
                }
                offset += values.size();
                if( values.size() < chunk ) break;
            }
        }
        std::vector<std::pair<std::string, Aggregate*>> order;
        for( auto& [ref, group] : groups ) order.emplace_back( ref, &group );
        std::sort( order.begin(), order.end(), []( const auto& lhs, const auto& rhs ) {
            const auto left = analysis::ComputeStatistics( lhs.second->durations ).total;
            const auto right = analysis::ComputeStatistics( rhs.second->durations ).total;
            return left != right ? left > right : lhs.first < rhs.first;
        } );
        const size_t limit = TopN( params );
        if( order.size() > limit ) order.resize( limit );
        json values = json::array();
        for( const auto& [ref, group] : order ) values.push_back( {
            { "source_location_ref", ref }, { "name", group->name }, { "file", group->file }, { "line", group->line },
            { "statistics", StatisticsJson( analysis::ComputeStatistics( group->durations ) ) }
        } );
        return Success( id, { { "groups", std::move( values ) }, { "group_count", groups.size() } }, trace );
    }

    if( method == "memory.pools" )
    {
        json pools = json::array();
        for( const auto& value : source->GetMemoryPools() ) pools.emplace_back( MemoryPoolJson( value ) );
        return Success( id, { { "pools", std::move( pools ) } }, trace );
    }
    if( method == "memory.events" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string pool = params.value( "pool_ref", "" );
        auto [values, hasMore] = ScanFiltered<analysis::MemoryEventDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMemoryEvents( range ); },
            [&]( const auto& value ) { return pool.empty() || value.poolRef == pool; }, MemoryEventJson );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "events", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "lock.list" )
    {
        json locks = json::array();
        for( const auto& value : source->GetLocks() ) locks.push_back( {
            { "ref", value.ref }, { "native_id", value.nativeId }, { "name", value.name }, { "source_location_ref", value.sourceLocationRef },
            { "event_count", Decimal( value.eventCount ) }, { "thread_count", Decimal( value.threadCount ) },
            { "valid", value.valid }, { "contended", value.contended }, { "announce_ns", Decimal( value.announceNs ) },
            { "terminate_ns", value.terminateNs ? json( Decimal( *value.terminateNs ) ) : json( nullptr ) }
        } );
        return Success( id, { { "locks", std::move( locks ) } }, trace );
    }
    if( method == "plot.list" )
    {
        json plots = json::array();
        for( const auto& value : source->GetPlotList() ) plots.push_back( {
            { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "type", value.type }, { "format", value.format },
            { "point_count", Decimal( value.pointCount ) }, { "min", value.min }, { "max", value.max }, { "sum", value.sum }
        } );
        return Success( id, { { "plots", std::move( plots ) } }, trace );
    }
    if( method == "plot.points" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string plot = params.value( "plot_ref", "" );
        auto [values, hasMore] = ScanFiltered<analysis::PlotPointDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanPlots( range ); },
            [&]( const auto& value ) { return plot.empty() || value.plotRef == plot; },
            []( const auto& value ) { return json { { "ref", value.ref }, { "plot_ref", value.plotRef }, { "time_ns", Decimal( value.timeNs ) }, { "value", value.value } }; } );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "points", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "message.search" )
    {
        const auto page = ParsePage( params, method, trace );
        auto [values, hasMore] = ScanFiltered<analysis::MessageDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMessages( range ); },
            [&]( const auto& value ) { return TextMatches( value.text, params ); }, MessageJson );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "messages", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "callstack.resolve" )
    {
        if( !params.contains( "callstacks" ) || !params["callstacks"].is_array() ) throw QueryError( "INVALID_PARAMS", "callstacks must be an array" );
        const size_t maxDepth = params.value( "max_depth", size_t( 32 ) );
        if( maxDepth < 1 || maxDepth > 256 ) throw QueryError( "INVALID_PARAMS", "max_depth must be between 1 and 256" );
        std::vector<uint32_t> callstacks;
        for( const auto& value : params["callstacks"] )
        {
            try
            {
                callstacks.emplace_back( value.is_string() ? uint32_t( std::stoul( value.get<std::string>() ) ) : value.get<uint32_t>() );
            }
            catch( const std::exception& ) { throw QueryError( "INVALID_PARAMS", "callstack ids must be unsigned integers or decimal strings" ); }
        }
        json frames = json::array();
        for( const auto& value : source->ResolveCallstacks( callstacks, maxDepth ) ) frames.push_back( {
            { "ref", value.ref }, { "name", value.name }, { "file", value.file }, { "line", value.line },
            { "address", value.address }, { "symbol_address", value.symbolAddress }, { "inline", value.inlineFrame }
        } );
        return Success( id, { { "frames", std::move( frames ) } }, trace );
    }
    if( method == "timeline.slice" )
    {
        const auto page = ParsePage( params, method, trace );
        auto range = ScanRangeFrom( params, page.offset, page.limit );
        json cpu = json::array();
        for( const auto& value : source->ScanCpuZones( range ) ) cpu.emplace_back( CpuZoneJson( value ) );
        json gpu = json::array();
        for( const auto& value : source->ScanGpuZones( range ) ) gpu.emplace_back( GpuZoneJson( value ) );
        json messages = json::array();
        for( const auto& value : source->ScanMessages( range ) ) messages.emplace_back( MessageJson( value ) );
        return Success( id, { { "range", { { "start_ns", Decimal( range.startNs ) }, { "end_ns", Decimal( range.endNs ) } } }, { "cpu_zones", std::move( cpu ) }, { "gpu_zones", std::move( gpu ) }, { "messages", std::move( messages ) } }, trace );
    }
    if( method == "statistics.describe" )
    {
        return Success( id, { { "fields", { "count", "total", "min", "max", "mean", "median", "stddev", "p50", "p90", "p95", "p99", "truncated_mean" } }, { "percentile_interpolation", "linear between adjacent sorted samples" }, { "stddev", "population" } }, trace );
    }
    if( method == "validation.run" )
    {
        json findings = json::array();
        size_t incompleteCpu = 0;
        size_t incompleteGpu = 0;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanCpuZones( range );
            incompleteCpu += std::count_if( values.begin(), values.end(), []( const auto& value ) { return !value.complete; } );
            offset += values.size();
            if( values.size() < chunk ) break;
        }
        offset = 0;
        while( true )
        {
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanGpuZones( range );
            incompleteGpu += std::count_if( values.begin(), values.end(), []( const auto& value ) { return !value.complete; } );
            offset += values.size();
            if( values.size() < chunk ) break;
        }
        if( incompleteCpu ) findings.push_back( { { "severity", "warning" }, { "code", "INCOMPLETE_CPU_ZONES" }, { "message", std::to_string( incompleteCpu ) + " CPU zones have no persisted end event" }, { "count", Decimal( uint64_t( incompleteCpu ) ) } } );
        if( incompleteGpu ) findings.push_back( { { "severity", "warning" }, { "code", "INCOMPLETE_GPU_ZONES" }, { "message", std::to_string( incompleteGpu ) + " GPU zones have incomplete timing" }, { "count", Decimal( uint64_t( incompleteGpu ) ) } } );
        if( info().samplesInconsistent ) findings.push_back( { { "severity", "warning" }, { "code", "INCONSISTENT_SAMPLES" }, { "message", "sampling data was marked inconsistent by Worker" } } );
        return Success( id, { { "valid", findings.empty() }, { "findings", std::move( findings ) }, { "checks", { "worker_load", "cpu_zone_timing", "gpu_zone_timing", "sample_consistency" } } }, trace );
    }

    static const std::set<std::string> knownButPending = {
        "thread.statistics", "thread.timeline", "thread.migration", "cpu.topology", "cpu.usage", "cpu.timeline",
        "context_switch.range", "context_switch.thread", "context_switch.statistics", "frame.range_mapping",
        "frame_image.list", "frame_image.metadata", "frame_image.resource", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.flamegraph",
        "zone.gpu.get", "zone.gpu.flamegraph", "callstack.frames", "callstack.parent", "callstack.batch",
        "sample.list", "sample.ghost_zones", "sample.symbol_statistics", "sample.flamegraph", "hardware_sample.address", "hardware_sample.counts", "hardware_sample.capabilities",
        "symbol.search", "symbol.get", "symbol.address", "symbol.raw_code", "symbol.disassembly", "source.locations", "source.statistics", "source.embedded", "source.lines",
        "memory.active_at_time", "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates",
        "memory.gpu.pools", "memory.gpu.allocations", "memory.gpu.request_scopes", "memory.gpu.pass_uses", "memory.gpu.attribution",
        "lock.get", "lock.timeline", "lock.contention_statistics", "plot.range", "plot.downsample", "plot.statistics", "message.get",
        "compare.zones", "compare.frames", "compare.source"
    };
    if( knownButPending.find( method ) != knownButPending.end() ) throw QueryError( "CAPABILITY_UNAVAILABLE", method + " is declared by v1 but not implemented in this build" );
    throw QueryError( "METHOD_NOT_FOUND", "unknown method: " + method );
}

std::string DumpProtocolJson( const json& value )
{
    return value.dump( -1, ' ', false, json::error_handler_t::replace );
}

}
