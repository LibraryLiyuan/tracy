#include "TracyQueryService.hpp"

#include "TracyAnalysis.hpp"
#include "TracyEmbeddedData.hpp"

#include "../../dtl/dtl.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>

namespace tracy::query
{

const std::vector<std::string>& QueryMethodRegistry()
{
    static const std::vector<std::string> methods = {
        "system.capabilities", "system.describe", "system.schema",
        "trace.open", "trace.status", "trace.list", "trace.close", "trace.info", "trace.overview", "trace.counts", "trace.app_info", "trace.identity", "trace.crash",
        "capture.context", "capture.coverage", "producer.list", "producer.get",
        "thread.list", "thread.get", "thread.statistics", "thread.timeline", "thread.migration",
        "cpu.topology", "cpu.usage", "cpu.timeline", "context_switch.range", "context_switch.thread", "context_switch.statistics",
        "frame.sets", "frame.list", "frame.get", "frame.statistics", "frame.outliers", "frame.range_mapping", "frame_image.list", "frame_image.metadata", "frame_image.resource", "frame_image.raw",
        "zone.cpu.search", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.statistics", "zone.cpu.flamegraph", "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.get", "zone.gpu.tree", "zone.gpu.statistics", "zone.gpu.flamegraph",
        "memory.pools", "memory.events", "memory.get", "memory.active_at_time", "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates",
        "memory.gpu.pools", "memory.gpu.allocations", "memory.gpu.request_scopes", "memory.gpu.pass_uses", "memory.gpu.attribution",
        "lock.list", "lock.get", "lock.timeline", "lock.contention_statistics",
        "plot.list", "plot.points", "plot.range", "plot.downsample", "plot.statistics", "message.search", "message.get",
        "job.search", "job.get", "job.dependencies", "job.critical_path", "job.gfx.statistics", "job.gfx_chain",
        "callstack.resolve", "callstack.frames", "callstack.parent", "callstack.batch", "sample.list", "sample.ghost_zones", "sample.symbol_statistics", "sample.flamegraph", "hardware_sample.address", "hardware_sample.counts", "hardware_sample.events", "hardware_sample.capabilities",
        "symbol.search", "symbol.get", "symbol.address", "symbol.address_map", "symbol.raw_code", "symbol.disassembly",
        "source.locations", "source.statistics", "source.embedded", "source.lines", "source.raw",
        "timeline.slice", "statistics.describe", "statistics.compute", "compare.zones", "compare.frames", "compare.source", "validation.run"
    };
    return methods;
}

bool IsPublicQueryMethod( std::string_view method )
{
    const auto& methods = QueryMethodRegistry();
    return std::find( methods.begin(), methods.end(), method ) != methods.end();
}

namespace
{

using nlohmann::json;

bool JsonDepthAllowed( const json& value, size_t depth = 0 )
{
    if( depth > 64 ) return false;
    if( value.is_array() ) for( const auto& child : value ) if( !JsonDepthAllowed( child, depth + 1 ) ) return false;
    if( value.is_object() ) for( const auto& [key, child] : value.items() ) if( !JsonDepthAllowed( child, depth + 1 ) ) return false;
    return true;
}

std::string Decimal( uint64_t value ) { return std::to_string( value ); }
std::string Decimal( int64_t value ) { return std::to_string( value ); }

uint64_t UnsignedParameter( const json& params, const char* name, uint64_t defaultValue, uint64_t maximum )
{
    if( !params.contains( name ) ) return defaultValue;
    const auto& value = params[name];
    uint64_t parsed = 0;
    if( value.is_string() )
    {
        const auto text = value.get<std::string>();
        const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed, 10 );
        if( result.ec != std::errc() || result.ptr != text.data() + text.size() ) throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be an unsigned decimal string or integer" );
    }
    else if( value.is_number_unsigned() ) parsed = value.get<uint64_t>();
    else if( value.is_number_integer() )
    {
        const auto signedValue = value.get<int64_t>();
        if( signedValue < 0 ) throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be non-negative" );
        parsed = uint64_t( signedValue );
    }
    else throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be an unsigned decimal string or integer" );
    if( parsed > maximum ) throw QueryError( "INVALID_PARAMS", std::string( name ) + " exceeds the allowed maximum" );
    return parsed;
}

uint64_t HexAddress( const std::string& value )
{
    if( value.size() < 3 || value[0] != '0' || ( value[1] != 'x' && value[1] != 'X' ) ) throw QueryError( "INVALID_PARAMS", "address must be a 0x-prefixed hexadecimal string" );
    uint64_t result = 0;
    const auto parsed = std::from_chars( value.data() + 2, value.data() + value.size(), result, 16 );
    if( parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ) throw QueryError( "INVALID_PARAMS", "address must be a 0x-prefixed hexadecimal string" );
    return result;
}

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

std::string Base64UrlEncode( const std::vector<uint8_t>& input )
{
    if( input.empty() ) return {};
    return Base64UrlEncode( std::string( reinterpret_cast<const char*>( input.data() ), input.size() ) );
}

json BinaryChunkJson( const analysis::BinaryResourceChunkDto& value )
{
    const auto nextOffset = value.offset + value.bytes.size();
    return {
        { "ref", value.ref },
        { "offset_bytes", Decimal( value.offset ) },
        { "returned_bytes", Decimal( value.bytes.size() ) },
        { "total_bytes", Decimal( value.totalBytes ) },
        { "data_base64url", Base64UrlEncode( value.bytes ) },
        { "next_offset_bytes", value.eof ? json( nullptr ) : json( Decimal( nextOffset ) ) },
        { "eof", value.eof },
        { "trust", "untrusted_trace_data" }
    };
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
        { "callstack_payloads", Decimal( value.callstackPayloads ) }, { "parent_callstack_payloads", Decimal( value.parentCallstackPayloads ) },
        { "callstack_frames", Decimal( value.callstackFrames ) }, { "parent_callstack_frames", Decimal( value.parentCallstackFrames ) },
        { "samples", Decimal( value.samples ) }, { "context_switch_samples", Decimal( value.contextSwitchSamples ) }, { "kernel_samples", Decimal( value.kernelSamples ) },
        { "ghost_zones", Decimal( value.ghostZones ) },
        { "child_sample_symbols", Decimal( value.childSampleSymbols ) }, { "child_samples", Decimal( value.childSamples ) },
        { "hardware_samples", Decimal( value.hardwareSamples ) },
        { "symbols", Decimal( value.symbols ) }, { "symbol_code_bytes", Decimal( value.symbolCodeBytes ) },
        { "source_locations", Decimal( value.sourceLocations ) }, { "source_cache_files", Decimal( value.sourceCacheFiles ) },
        { "source_cache_bytes", Decimal( value.sourceCacheBytes ) }, { "frame_images", Decimal( value.frameImages ) },
        { "job_types", Decimal( value.jobTypes ) }, { "jobs", Decimal( value.jobs ) },
        { "job_dependencies", Decimal( value.jobDependencies ) }, { "job_stages", Decimal( value.jobStages ) },
        { "gfx_dispatches", Decimal( value.gfxDispatches ) }, { "gfx_entities", Decimal( value.gfxEntities ) },
        { "gfx_links", Decimal( value.gfxLinks ) }
    };
}

json FieldAvailabilityJson( const analysis::FieldAvailabilityDto& value )
{
    return {
        { "available", value.available },
        { "reason", value.available || value.reason.empty() ? json( nullptr ) : json( value.reason ) }
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
        { "cpu_architecture", value.cpuArchitecture },
        { "timer_multiplier", value.timerMultiplier }, { "frame_offset", Decimal( value.frameOffset ) },
        { "sampling_period_ns", Decimal( value.samplingPeriodNs ) }, { "on_demand", value.onDemand },
        { "legacy_queue_delay_ns", value.legacyQueueDelayNs ? json( Decimal( *value.legacyQueueDelayNs ) ) : json( nullptr ) },
        { "field_availability", { { "legacy_queue_delay_ns", FieldAvailabilityJson( value.legacyQueueDelayAvailability ) } } },
        { "has_crash", value.hasCrash }, { "samples_inconsistent", value.samplesInconsistent },
        { "counts", CountsJson( value.counts ) }, { "app_info", value.appInfo }, { "trust", "untrusted_trace_data" }
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
        { "running_regions", value.runningRegions ? json( *value.runningRegions ) : json( nullptr ) },
        { "migrations", value.migrations },
        { "external_process_name", value.externalProcessName ? json( *value.externalProcessName ) : json( nullptr ) },
        { "external_thread_name", value.externalThreadName ? json( *value.externalThreadName ) : json( nullptr ) },
        { "local_name", value.localName ? json( *value.localName ) : json( nullptr ) },
        { "kernel_sample_count", value.kernelSampleCount ? json( Decimal( *value.kernelSampleCount ) ) : json( nullptr ) },
        { "group_hint", value.groupHint ? json( *value.groupHint ) : json( nullptr ) },
        { "field_availability", { { "group_hint", FieldAvailabilityJson( value.groupHintAvailability ) } } },
        { "trust", "untrusted_trace_data" }
    };
}

json FrameSetJson( const analysis::FrameSetDto& value )
{
    return {
        { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "continuous", value.continuous },
        { "frame_count", value.frameCount }, { "complete_frame_count", value.completeFrameCount }, { "trust", "untrusted_trace_data" }
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
        { "self_time_ns", value.selfTimeNs ? json( Decimal( *value.selfTimeNs ) ) : json( nullptr ) },
        { "running_time_ns", value.runningTimeNs ? json( Decimal( *value.runningTimeNs ) ) : json( nullptr ) },
        { "running_regions", Decimal( value.runningRegions ) }, { "child_count", value.childCount },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "callstack_ref", value.callstackRef ? json( *value.callstackRef ) : json( nullptr ) },
        { "extra_index", value.extraIndex }, { "extra_valid", value.extraValid },
        { "extra_name", value.extraName ? json( *value.extraName ) : json( nullptr ) },
        { "extra_text", value.extraText ? json( *value.extraText ) : json( nullptr ) },
        { "extra_color", value.extraColor },
        { "complete", value.complete }, { "name_resolved", value.nameResolved }, { "trust", "untrusted_trace_data" }
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
        { "self_time_ns", value.selfTimeNs ? json( Decimal( *value.selfTimeNs ) ) : json( nullptr ) }, { "child_count", value.childCount },
        { "cpu_start_ns", Decimal( value.cpuStartNs ) }, { "cpu_end_ns", value.cpuEndNs ? json( Decimal( *value.cpuEndNs ) ) : json( nullptr ) },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "callstack_ref", value.callstackRef ? json( *value.callstackRef ) : json( nullptr ) },
        { "query_id", value.queryIdAvailability.available ? json( value.queryId ) : json( nullptr ) },
        { "field_availability", { { "query_id", FieldAvailabilityJson( value.queryIdAvailability ) } } },
        { "complete", value.complete }, { "trust", "untrusted_trace_data" }
    };
}

json ContextSwitchJson( const analysis::ContextSwitchDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef }, { "start_ns", Decimal( value.startNs ) },
        { "end_ns", value.endNs ? json( Decimal( *value.endNs ) ) : json( nullptr ) },
        { "duration_ns", value.endNs ? json( Decimal( *value.endNs - value.startNs ) ) : json( nullptr ) },
        { "wakeup_ns", value.wakeupNs ? json( Decimal( *value.wakeupNs ) ) : json( nullptr ) },
        { "cpu", value.cpu }, { "wakeup_cpu", value.wakeupCpuAvailability.available ? json( value.wakeupCpu ) : json( nullptr ) },
        { "field_availability", { { "wakeup_cpu", FieldAvailabilityJson( value.wakeupCpuAvailability ) } } },
        { "reason", value.reason }, { "reason_name", value.reasonName },
        { "state", value.state }, { "state_name", value.stateName },
        { "related_thread_index", value.relatedThreadIndex },
        { "next_thread_ref", value.relatedThreadRef ? json( *value.relatedThreadRef ) : json( nullptr ) },
        { "complete", value.complete }
    };
}

json CpuContextSwitchJson( const analysis::CpuContextSwitchDto& value )
{
    return {
        { "ref", value.ref }, { "cpu", value.cpu }, { "start_ns", Decimal( value.startNs ) },
        { "end_ns", value.endNs ? json( Decimal( *value.endNs ) ) : json( nullptr ) },
        { "duration_ns", value.endNs ? json( Decimal( *value.endNs - value.startNs ) ) : json( nullptr ) },
        { "raw_thread_index", value.rawThreadIndex }, { "thread_ref", value.threadRef }, { "complete", value.complete }
    };
}

json SampleJson( const analysis::SampleDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef }, { "time_ns", Decimal( value.timeNs ) },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "callstack_ref", value.callstackRef ? json( *value.callstackRef ) : json( nullptr ) }, { "kind", value.kind }
    };
}

json GhostZoneJson( const analysis::GhostZoneDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef },
        { "parent_ref", value.parentRef ? json( *value.parentRef ) : json( nullptr ) },
        { "start_ns", Decimal( value.startNs ) }, { "end_ns", Decimal( value.endNs ) },
        { "duration_ns", Decimal( value.endNs - value.startNs ) }, { "name", value.name }, { "file", value.file },
        { "line", value.line }, { "address", value.address }, { "depth", value.depth },
        { "child_count", value.childCount }, { "inline", value.inlineFrame }, { "trust", "untrusted_trace_data" }
    };
}

json CallstackFrameJson( const analysis::CallstackFrameDto& value )
{
    return {
        { "ref", value.ref }, { "name", value.name }, { "file", value.file }, { "line", value.line },
        { "address", value.address }, { "symbol_address", value.symbolAddress }, { "inline", value.inlineFrame },
        { "callstack", Decimal( uint64_t( value.callstack ) ) }, { "depth", value.depth },
        { "image_name", value.imageName ? json( *value.imageName ) : json( nullptr ) }, { "trust", "untrusted_trace_data" }
    };
}

std::string NormalizeSourceKey( std::string value )
{
    std::replace( value.begin(), value.end(), '\\', '/' );
#ifdef _WIN32
    value = Lower( std::move( value ) );
#endif
    return value;
}

std::vector<std::string> TextLines( const std::string& text )
{
    std::vector<std::string> result;
    std::istringstream input( text );
    std::string line;
    while( std::getline( input, line ) ) result.emplace_back( std::move( line ) );
    if( !text.empty() && text.back() == '\n' ) result.emplace_back();
    return result;
}

json LockEventJson( const analysis::LockEventDto& value )
{
    return {
        { "ref", value.ref }, { "lock_ref", value.lockRef }, { "time_ns", Decimal( value.timeNs ) },
        { "thread_ref", value.threadRef }, { "type", value.type },
        { "source_location_ref", value.sourceLocationRef },
        { "owner_thread_ref", value.ownerThreadRef ? json( *value.ownerThreadRef ) : json( nullptr ) },
        { "lock_count", value.lockCount }, { "waiter_thread_refs", value.waiterThreadRefs }
    };
}

json SymbolJson( const analysis::SymbolDto& value )
{
    return {
        { "ref", value.ref }, { "address", value.address }, { "name", value.name }, { "file", value.file }, { "line", value.line },
        { "size_bytes", Decimal( value.size ) }, { "inclusive_samples", value.inclusiveSamples },
        { "exclusive_samples", value.exclusiveSamples }, { "child_samples", Decimal( value.childSamples ) },
        { "image_name", value.imageName ? json( *value.imageName ) : json( nullptr ) },
        { "call_file", value.callFile ? json( *value.callFile ) : json( nullptr ) },
        { "call_line", value.callLine }, { "inline", value.inlineFrame },
        { "has_code", value.hasCode }, { "trust", "untrusted_trace_data" }
    };
}

json SourceLocationJson( const analysis::SourceLocationDto& value )
{
    return {
        { "ref", value.ref }, { "name", value.name }, { "function", value.function }, { "file", value.file },
        { "line", value.line }, { "color", value.color }, { "native_id", value.nativeId }, { "dynamic", value.dynamic },
        { "trust", "untrusted_trace_data" }
    };
}

json MemoryPoolJson( const analysis::MemoryPoolDto& value )
{
    return {
        { "ref", value.ref }, { "native_name_id", Decimal( value.nativeNameId ) }, { "name", value.name },
        { "event_count", Decimal( value.eventCount ) }, { "free_count", Decimal( value.freeCount ) },
        { "active_count", Decimal( value.activeCount ) }, { "active_bytes", Decimal( value.activeBytes ) },
        { "persisted_usage_bytes", Decimal( value.persistedUsageBytes ) },
        { "stored_name_id", Decimal( value.storedNameId ) }, { "stored_name", value.storedName },
        { "low", "0x" + Hex16( value.low ) }, { "high", "0x" + Hex16( value.high ) },
        { "gpu_d3d12", value.gpuD3D12 }, { "identifier_semantics", value.gpuD3D12 ? "logical_allocation_id" : "address" }, { "trust", "untrusted_trace_data" }
    };
}

json MemoryEventJson( const analysis::MemoryEventDto& value )
{
    return {
        { "ref", value.ref }, { "pool_ref", value.poolRef }, { "address", value.address }, { "size_bytes", Decimal( value.size ) },
        { "allocation_ns", Decimal( value.allocationNs ) }, { "free_ns", value.freeNs ? json( Decimal( *value.freeNs ) ) : json( nullptr ) },
        { "allocation_thread_ref", value.allocationThreadRef }, { "free_thread_ref", value.freeThreadRef ? json( *value.freeThreadRef ) : json( nullptr ) },
        { "allocation_callstack", value.allocationCallstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.allocationCallstack ) ) ) },
        { "free_callstack", value.freeCallstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.freeCallstack ) ) ) },
        { "allocation_callstack_ref", value.allocationCallstackRef ? json( *value.allocationCallstackRef ) : json( nullptr ) },
        { "free_callstack_ref", value.freeCallstackRef ? json( *value.freeCallstackRef ) : json( nullptr ) },
        { "allocation_zone_ref", value.allocationZoneRef ? json( *value.allocationZoneRef ) : json( nullptr ) },
        { "free_zone_ref", value.freeZoneRef ? json( *value.freeZoneRef ) : json( nullptr ) }, { "complete", value.complete }
    };
}

json MemorySummaryJson( const analysis::TraceSource& source, const analysis::MemoryFramePoolSummary& value, bool total = false )
{
    return {
        { "pool_ref", total ? json( nullptr ) : json( source.GetMemoryPoolRef( value.pool ).value_or( "" ) ) },
        { "active_at_start", { { "bytes", Decimal( value.startBytes ) }, { "count", Decimal( value.startCount ) } } },
        { "allocated_in_frame", { { "bytes", Decimal( value.allocatedBytes ) }, { "count", Decimal( value.allocatedCount ) } } },
        { "freed_in_frame", { { "bytes", Decimal( value.freedBytes ) }, { "count", Decimal( value.freedCount ) } } },
        { "active_at_end", { { "bytes", Decimal( value.endBytes ) }, { "count", Decimal( value.endCount ) } } },
        { "peak", { { "bytes", Decimal( value.peakBytes ) }, { "count", Decimal( value.peakCount ) } } }
    };
}

json GpuPassJson( const analysis::TraceSource& source, const analysis::GpuMemoryPass& pass, bool includeUses = true, size_t useOffset = 0, size_t useLimit = MaximumPageSize )
{
    json uses = json::array();
    const size_t useEnd = includeUses ? std::min( pass.uses.size(), useOffset + useLimit ) : 0;
    for( size_t index = useOffset; index < useEnd; index++ )
    {
        const auto& use = pass.uses[index];
        uses.push_back( {
        { "allocation_id", Decimal( use.allocationId ) }, { "kind", std::string( 1, use.kind ) },
        { "usage_mask", "0x" + Hex16( use.usageMask ) }, { "usage", analysis::FormatGpuMemoryUsage( use.usageMask ) }
        } );
    }
    return {
        { "ref", source.MakeEntityRef( "gpu-memory-pass", pass.passId ) }, { "pass_id", Decimal( pass.passId ) },
        { "label_id", Decimal( pass.labelId ) }, { "taxonomy_id", Decimal( pass.labelId ) },
        { "frame", Decimal( pass.frame ) }, { "ordinal", Decimal( pass.ordinal ) }, { "thread_id", Decimal( pass.thread ) },
        { "start_ns", Decimal( pass.start ) }, { "end_ns", Decimal( pass.end ) }, { "level", pass.level },
        { "name", pass.name }, { "operations", pass.operations }, { "command_count", pass.commandCount },
        { "emitted_use_count", pass.emittedUseCount }, { "total_use_count", pass.totalUseCount },
        { "expected_chunks", pass.expectedChunks }, { "parsed_chunks", pass.parsedChunks },
        { "untracked_references", pass.untrackedReferences }, { "dropped_uses", pass.droppedUses },
        { "truncated", pass.truncated }, { "complete", pass.complete }, { "gpu_pairing", analysis::ToString( pass.gpuPairing ) },
        { "cpu_zone_ref", source.GetCpuZoneRef( pass.cpuZoneIndex ).value_or( "" ) },
        { "gpu_zone_ref", pass.gpuZoneIndex ? json( source.GetGpuZoneRef( *pass.gpuZoneIndex ).value_or( "" ) ) : json( nullptr ) },
        { "uses", std::move( uses ) }, { "uses_returned", includeUses ? useEnd - useOffset : 0 },
        { "uses_truncated", includeUses ? useEnd < pass.uses.size() : !pass.uses.empty() }, { "trust", "untrusted_trace_data" }
    };
}

json MessageJson( const analysis::MessageDto& value )
{
    return {
        { "ref", value.ref }, { "thread_ref", value.threadRef }, { "time_ns", Decimal( value.timeNs ) }, { "text", value.text },
        { "color", value.color }, { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "callstack_ref", value.callstackRef ? json( *value.callstackRef ) : json( nullptr ) },
        { "trust", "untrusted_trace_data" }
    };
}

const char* JobKindName( uint8_t kind )
{
    static constexpr const char* names[] = { "native", "managed", "burst", "gfx" };
    return kind < std::size( names ) ? names[kind] : "unknown";
}

const char* JobStageName( uint8_t stage )
{
    static constexpr const char* names[] = {
        "pre_execute_begin", "pre_execute_end", "worker_slice_begin", "worker_slice_end",
        "post_execute_begin", "post_execute_end", "completed", "wait_begin",
        "wait_active_help_begin", "wait_active_help_end", "wait_spin_yield_begin", "wait_spin_yield_end",
        "wait_sleep_begin", "wait_sleep_end", "wait_end", "flow_begin", "flow_next",
        "flow_parallel_next", "flow_end", "cancelled", "incomplete", "schedule_callstack"
    };
    return stage < std::size( names ) ? names[stage] : "unknown";
}

const char* GfxEntityKindName( uint8_t kind )
{
    static constexpr const char* names[] = { "dispatch", "gfx_job", "command_list", "submission", "gpu_segment" };
    return kind < std::size( names ) ? names[kind] : "unknown";
}

const char* GfxRelationName( uint8_t relation )
{
    static constexpr const char* names[] = { "parent", "dispatches", "executes", "produces", "submits", "runs_on_gpu", "depends_on" };
    return relation < std::size( names ) ? names[relation] : "unknown";
}

json JobJson( const analysis::TraceSource& source, const analysis::JobDto& value, bool detailed )
{
    const char* state = value.cancelled ? "cancelled" : value.incomplete ? "incomplete" : value.completedNs ? "completed" : value.truncated ? "truncated" : "scheduled";
    json result = {
        { "ref", value.ref }, { "job_id", Decimal( value.jobId ) }, { "packed_handle", Decimal( value.packedHandle ) },
        { "handle_index", uint32_t( value.packedHandle ) }, { "handle_generation", uint32_t( value.packedHandle >> 32 ) },
        { "name", value.name }, { "type_id", value.typeId }, { "kind", JobKindName( value.kind ) }, { "kind_id", value.kind },
        { "flags", value.flags }, { "state", state }, { "schedule_ns", Decimal( value.scheduleNs ) },
        { "schedule_thread_ref", value.scheduleThreadRef }, { "count", value.count }, { "grain_size", value.grainSize },
        { "unity_flow_id", value.unityFlowId }, { "expected_dependency_count", value.expectedDependencyCount },
        { "schedule_callstack", value.scheduleCallstack },
        { "schedule_callstack_ref", value.scheduleCallstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", value.scheduleCallstack ) ) },
        { "dependency_count", value.dependencies.size() }, { "stage_count", value.stages.size() },
        { "first_run_ns", value.firstRunNs ? json( Decimal( *value.firstRunNs ) ) : json( nullptr ) },
        { "completed_ns", value.completedNs ? json( Decimal( *value.completedNs ) ) : json( nullptr ) },
        { "schedule_to_first_run_ns", value.firstRunNs && !value.orphan ? json( Decimal( *value.firstRunNs - value.scheduleNs ) ) : json( nullptr ) },
        { "schedule_to_complete_ns", value.completedNs && !value.orphan ? json( Decimal( *value.completedNs - value.scheduleNs ) ) : json( nullptr ) },
        { "execution_ns", Decimal( value.executionNs ) },
        { "wait", { { "active_help_ns", Decimal( value.waitActiveHelpNs ) }, { "spin_yield_ns", Decimal( value.waitSpinYieldNs ) }, { "sleep_ns", Decimal( value.waitSleepNs ) } } },
        { "orphan", value.orphan }, { "truncated", value.truncated }, { "trust", "untrusted_trace_data" }
    };
    if( !detailed ) return result;

    json dependencies = json::array();
    for( const auto& dependency : value.dependencies ) dependencies.push_back( {
        { "prerequisite_job_ref", dependency.prerequisiteJobId == 0 ? json( nullptr ) : json( source.MakeEntityRef( "job", dependency.prerequisiteJobId ) ) },
        { "prerequisite_job_id", Decimal( dependency.prerequisiteJobId ) }, { "prerequisite_handle", Decimal( dependency.prerequisiteHandle ) },
        { "flags", dependency.flags }
    } );
    json stages = json::array();
    for( size_t index = 0; index < value.stages.size(); index++ )
    {
        const auto& stage = value.stages[index];
        stages.push_back( {
            { "ref", source.MakeEntityRef( "job-stage", ( value.jobId << 24 ) ^ index ) },
            { "time_ns", Decimal( stage.timeNs ) }, { "thread_ref", stage.threadRef }, { "stage", JobStageName( stage.stage ) },
            { "stage_id", stage.stage }, { "span_id", stage.spanId }, { "arg0", stage.arg0 }, { "arg1", stage.arg1 }, { "flags", stage.flags }
        } );
    }
    result["dependencies"] = std::move( dependencies );
    result["stages"] = std::move( stages );
    return result;
}

constexpr std::string_view CaptureIdentityPrefix = "JNCI1|";
constexpr std::string_view CaptureContextPrefix = "JNCTX1|";
constexpr std::string_view ProducerQualityPrefix = "JNQ1|";
constexpr size_t MaximumIdentityEnvelopeBytes = 48 * 1024;
constexpr size_t MaximumIdentityRecords = 4096;
constexpr size_t MaximumIdentityFields = 256;
constexpr size_t MaximumIdentityStringBytes = 8192;
constexpr size_t MaximumContextRecords = 4096;
constexpr size_t MaximumQualityRecords = 16384;

std::optional<uint64_t> DecimalStringValue( const json& value )
{
    if( !value.is_string() ) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    uint64_t parsed = 0;
    const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed, 10 );
    if( result.ec != std::errc() || result.ptr != text.data() + text.size() ) return std::nullopt;
    return parsed;
}

bool IdentityShapeAllowed( const json& value, size_t depth, size_t& fields )
{
    if( depth > 8 ) return false;
    if( value.is_string() ) return value.get_ref<const std::string&>().size() <= MaximumIdentityStringBytes;
    if( value.is_array() )
    {
        if( value.size() > 64 ) return false;
        for( const auto& child : value ) if( !IdentityShapeAllowed( child, depth + 1, fields ) ) return false;
        return true;
    }
    if( value.is_object() )
    {
        if( fields + value.size() > MaximumIdentityFields ) return false;
        fields += value.size();
        for( const auto& [key, child] : value.items() )
        {
            if( key.empty() || key.size() > 128 || !IdentityShapeAllowed( child, depth + 1, fields ) ) return false;
        }
    }
    return true;
}

std::string EscapeJsonPointerToken( const std::string& token )
{
    std::string result;
    result.reserve( token.size() );
    for( const char c : token )
    {
        if( c == '~' ) result += "~0";
        else if( c == '/' ) result += "~1";
        else result.push_back( c );
    }
    return result;
}

void MergeIdentity( json& target, const json& patch, const std::string& path, const std::string& producer,
    json& fieldSources, json& conflicts )
{
    if( patch.is_object() )
    {
        if( target.is_null() ) target = json::object();
        if( !target.is_object() )
        {
            conflicts.push_back( { { "path", path }, { "producer", producer }, { "reason", "object conflicts with an existing scalar value" } } );
            return;
        }
        for( const auto& [key, value] : patch.items() )
        {
            const auto childPath = path + '/' + EscapeJsonPointerToken( key );
            if( !target.contains( key ) ) target[key] = nullptr;
            MergeIdentity( target[key], value, childPath, producer, fieldSources, conflicts );
        }
        return;
    }

    if( target.is_null() )
    {
        target = patch;
        fieldSources[path] = producer;
    }
    else if( target != patch )
    {
        conflicts.push_back( {
            { "path", path }, { "producer", producer }, { "previous_producer", fieldSources.value( path, "unknown" ) },
            { "reason", "distinct values were emitted for the same identity field" }
        } );
    }
}

bool HasIdentityPath( const json& value, std::initializer_list<const char*> path )
{
    const json* current = &value;
    for( const auto* key : path )
    {
        if( !current->is_object() || !current->contains( key ) ) return false;
        current = &current->at( key );
    }
    return !current->is_null() && ( !current->is_string() || !current->get_ref<const std::string&>().empty() );
}

json CaptureIdentityJson( const analysis::TraceInfoDto& info )
{
    json identity = json::object();
    json sources = json::object();
    json conflicts = json::array();
    json invalid = json::array();
    std::set<std::string> documents;
    size_t seen = 0;
    size_t valid = 0;
    size_t duplicates = 0;
    size_t envelopeBytes = 0;

    for( size_t index = 0; index < info.appInfo.size(); index++ )
    {
        const auto& record = info.appInfo[index];
        const bool currentEnvelope = record.starts_with( CaptureIdentityPrefix );
        const bool identityLike = currentEnvelope || ( record.size() >= 5 && record.compare( 0, 4, "JNCI" ) == 0 && record.find( '|' ) != std::string::npos );
        if( !identityLike ) continue;
        if( ++seen > MaximumIdentityRecords )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "identity record limit exceeded" } } );
            break;
        }
        envelopeBytes += record.size();
        if( !currentEnvelope )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "unsupported capture identity envelope version" } } );
            continue;
        }
        if( record.size() <= CaptureIdentityPrefix.size() || record.size() > MaximumIdentityEnvelopeBytes )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture identity envelope has an invalid size" } } );
            continue;
        }

        const auto document = json::parse( record.begin() + CaptureIdentityPrefix.size(), record.end(), nullptr, false );
        size_t fields = 0;
        if( document.is_discarded() || !document.is_object() || !IdentityShapeAllowed( document, 0, fields ) ||
            !document.contains( "schema_version" ) || !document["schema_version"].is_number_unsigned() || document["schema_version"].get<uint64_t>() != 1 ||
            !document.contains( "kind" ) || !document["kind"].is_string() ||
            !document.contains( "producer" ) || !document["producer"].is_string() ||
            !document.contains( "identity" ) || !document["identity"].is_object() )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture identity JSON failed schema or resource-limit validation" } } );
            continue;
        }

        const auto canonical = document.dump();
        if( !documents.emplace( canonical ).second )
        {
            duplicates++;
            continue;
        }
        valid++;
        MergeIdentity( identity, document["identity"], "", document["producer"].get<std::string>(), sources, conflicts );
    }

    json missing = json::array();
    const auto require = [&]( std::initializer_list<const char*> path, const char* pointer ) {
        if( !HasIdentityPath( identity, path ) ) missing.push_back( pointer );
    };
    require( { "protocol", "jn_abi_version" }, "/protocol/jn_abi_version" );
    require( { "protocol", "jn_config_hash" }, "/protocol/jn_config_hash" );
    require( { "protocol", "tracy_protocol_version" }, "/protocol/tracy_protocol_version" );
    require( { "runtime", "target_kind" }, "/runtime/target_kind" );
    require( { "runtime", "engine_build_hash" }, "/runtime/engine_build_hash" );
    require( { "connection", "id" }, "/connection/id" );
    require( { "connection", "instance_cookie" }, "/connection/instance_cookie" );
    require( { "build", "build_id" }, "/build/build_id" );
    require( { "build", "repositories", "engine", "revision" }, "/build/repositories/engine/revision" );
    require( { "build", "repositories", "package", "revision" }, "/build/repositories/package/revision" );
    require( { "build", "repositories", "tracy", "revision" }, "/build/repositories/tracy/revision" );
    require( { "build", "artifacts", "unity", "sha256" }, "/build/artifacts/unity/sha256" );
    require( { "build", "artifacts", "jn_client", "sha256" }, "/build/artifacts/jn_client/sha256" );
    require( { "build", "artifacts", "query", "sha256" }, "/build/artifacts/query/sha256" );

    const bool present = valid != 0;
    const bool complete = present && missing.empty() && conflicts.empty() && invalid.empty();
    std::string reason;
    if( !present ) reason = seen == 0 ? "trace predates or did not emit JN Capture Identity" : "no valid JN Capture Identity document was found";
    else if( !complete ) reason = "capture identity is present but incomplete or inconsistent";

    return {
        { "present", present }, { "schema_version", 1 }, { "complete", complete },
        { "reason", reason.empty() ? json( nullptr ) : json( reason ) },
        { "identity", present ? identity : json( nullptr ) },
        { "canonical_fingerprint", present ? json( Hex16( Fnv1a( identity.dump() ) ) ) : json( nullptr ) },
        { "field_sources", sources }, { "missing_required", missing }, { "conflicts", conflicts }, { "invalid_records", invalid },
        { "records", { { "seen", seen }, { "valid", valid }, { "duplicates", duplicates }, { "envelope_bytes", Decimal( envelopeBytes ) } } },
        { "limits", { { "maximum_envelope_bytes", MaximumIdentityEnvelopeBytes }, { "maximum_records", MaximumIdentityRecords },
            { "maximum_fields", MaximumIdentityFields }, { "maximum_string_bytes", MaximumIdentityStringBytes } } },
        { "trust", "untrusted_trace_data" }
    };
}

void MergeContextPatch( json& target, const json& patch, const std::string& path,
    const std::string& producer, uint64_t generation, json& fieldSources )
{
    if( patch.is_object() )
    {
        if( !target.is_object() ) target = json::object();
        for( const auto& [key, value] : patch.items() )
        {
            const auto childPath = path + '/' + EscapeJsonPointerToken( key );
            if( value.is_object() )
            {
                if( !target.contains( key ) || !target[key].is_object() ) target[key] = json::object();
                MergeContextPatch( target[key], value, childPath, producer, generation, fieldSources );
            }
            else
            {
                target[key] = value;
                fieldSources[childPath] = { { "producer", producer }, { "generation", Decimal( generation ) } };
            }
        }
    }
    else
    {
        target = patch;
        fieldSources[path] = { { "producer", producer }, { "generation", Decimal( generation ) } };
    }
}

json CaptureContextJson( const analysis::TraceInfoDto& info )
{
    struct Entry
    {
        uint64_t generation;
        uint64_t snapshotSequence;
        uint64_t recordIndex;
        json document;
    };

    std::vector<Entry> entries;
    json invalid = json::array();
    std::set<std::string> definitions;
    std::set<uint64_t> snapshots;
    std::set<uint64_t> connectionIds;
    size_t seen = 0;
    size_t duplicates = 0;
    size_t envelopeBytes = 0;

    for( size_t index = 0; index < info.appInfo.size(); index++ )
    {
        const auto& record = info.appInfo[index];
        const bool currentEnvelope = record.starts_with( CaptureContextPrefix );
        const bool contextLike = currentEnvelope || ( record.size() >= 6 && record.compare( 0, 5, "JNCTX" ) == 0 && record.find( '|' ) != std::string::npos );
        if( !contextLike ) continue;
        if( ++seen > MaximumContextRecords )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture context record limit exceeded" } } );
            break;
        }
        envelopeBytes += record.size();
        if( !currentEnvelope )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "unsupported capture context envelope version" } } );
            continue;
        }
        if( record.size() <= CaptureContextPrefix.size() || record.size() > MaximumIdentityEnvelopeBytes )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture context envelope has an invalid size" } } );
            continue;
        }

        auto document = json::parse( record.begin() + CaptureContextPrefix.size(), record.end(), nullptr, false );
        size_t fields = 0;
        if( document.is_discarded() || !document.is_object() || !IdentityShapeAllowed( document, 0, fields ) ||
            document.value( "schema_version", 0 ) != 1 || !document.contains( "connection_id" ) ||
            !document.contains( "snapshot_sequence" ) || !document.contains( "generation" ) ||
            !document.contains( "effective_frame" ) || !document.contains( "effective_qpc" ) ||
            !document.contains( "snapshot_qpc" ) || !document.contains( "qpc_frequency" ) ||
            !document.contains( "producer" ) || !document["producer"].is_string() ||
            document["producer"].get_ref<const std::string&>().empty() ||
            !document.contains( "context" ) || !document["context"].is_object() )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture context JSON failed schema or resource-limit validation" } } );
            continue;
        }
        const auto connectionId = DecimalStringValue( document["connection_id"] );
        const auto snapshotSequence = DecimalStringValue( document["snapshot_sequence"] );
        const auto generation = DecimalStringValue( document["generation"] );
        const auto effectiveFrame = DecimalStringValue( document["effective_frame"] );
        const auto effectiveQpc = DecimalStringValue( document["effective_qpc"] );
        const auto snapshotQpc = DecimalStringValue( document["snapshot_qpc"] );
        const auto qpcFrequency = DecimalStringValue( document["qpc_frequency"] );
        if( !connectionId || !snapshotSequence || !generation || !effectiveFrame || !effectiveQpc || !snapshotQpc || !qpcFrequency || *qpcFrequency == 0 )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "capture context numeric field is invalid" } } );
            continue;
        }
        connectionIds.emplace( *connectionId );

        const auto definition = document["producer"].get<std::string>() + '\n' + std::to_string( *generation ) + '\n' + document["context"].dump();
        if( !definitions.emplace( definition ).second )
        {
            duplicates++;
            snapshots.emplace( *snapshotSequence );
            continue;
        }
        snapshots.emplace( *snapshotSequence );
        entries.push_back( { *generation, *snapshotSequence, uint64_t( index ), std::move( document ) } );
    }

    std::sort( entries.begin(), entries.end(), []( const auto& left, const auto& right ) {
        if( left.generation != right.generation ) return left.generation < right.generation;
        return left.recordIndex < right.recordIndex;
    } );

    json context = json::object();
    json sources = json::object();
    json generations = json::array();
    uint64_t latestGeneration = 0;
    for( const auto& entry : entries )
    {
        const auto& document = entry.document;
        const auto producer = document["producer"].get<std::string>();
        MergeContextPatch( context, document["context"], "", producer, entry.generation, sources );
        latestGeneration = std::max( latestGeneration, entry.generation );
        generations.push_back( {
            { "generation", Decimal( entry.generation ) }, { "producer", producer },
            { "effective_frame", document["effective_frame"] }, { "effective_qpc", document["effective_qpc"] },
            { "qpc_frequency", document["qpc_frequency"] }, { "context", document["context"] }
        } );
    }

    const auto identity = CaptureIdentityJson( info );
    if( connectionIds.size() > 1 )
        invalid.push_back( { { "record_index", nullptr }, { "reason", "capture context contains multiple connection ids" } } );
    const auto connectionId = connectionIds.size() == 1 ? std::optional<uint64_t>( *connectionIds.begin() ) : std::nullopt;
    if( connectionId && identity.value( "present", false ) && identity.contains( "identity" ) &&
        identity["identity"].is_object() && identity["identity"].contains( "connection" ) &&
        identity["identity"]["connection"].is_object() && identity["identity"]["connection"].contains( "id" ) )
    {
        const auto identityConnectionId = DecimalStringValue( identity["identity"]["connection"]["id"] );
        if( !identityConnectionId || *identityConnectionId != *connectionId )
            invalid.push_back( { { "record_index", nullptr }, { "reason", "capture context connection id does not match Capture Identity" } } );
    }
    const bool present = !entries.empty();
    json missing = json::array();
    if( !identity.value( "present", false ) ) missing.emplace_back( "build_identity" );
    if( !context.contains( "runtime" ) ) missing.emplace_back( "runtime" );
    if( !context.contains( "workload" ) ) missing.emplace_back( "workload" );
    if( !context.contains( "capture_config" ) ) missing.emplace_back( "capture_config" );
    const bool complete = present && missing.empty() && invalid.empty();
    std::string reason;
    if( !present ) reason = seen == 0 ? "trace predates or did not emit JN Capture Context" : "no valid JN Capture Context document was found";
    else if( !complete ) reason = "capture context is present but incomplete or invalid";

    return {
        { "present", present }, { "schema_version", 1 }, { "complete", complete },
        { "reason", reason.empty() ? json( nullptr ) : json( reason ) },
        { "connection_id", connectionId ? json( Decimal( *connectionId ) ) : json( nullptr ) },
        { "generation", present ? json( Decimal( latestGeneration ) ) : json( nullptr ) },
        { "context", present ? context : json( nullptr ) }, { "field_sources", sources },
        { "generations", generations }, { "missing_layers", missing }, { "invalid_records", invalid },
        { "layers", {
            { "build_identity", { { "present", identity.value( "present", false ) }, { "complete", identity.value( "complete", false ) },
                { "canonical_fingerprint", identity.value( "canonical_fingerprint", json( nullptr ) ) } } },
            { "runtime", context.contains( "runtime" ) }, { "workload", context.contains( "workload" ) },
            { "capture_config", context.contains( "capture_config" ) }
        } },
        { "records", { { "seen", seen }, { "valid", entries.size() }, { "duplicates", duplicates },
            { "snapshots", snapshots.size() }, { "envelope_bytes", Decimal( envelopeBytes ) } } },
        { "trust", "untrusted_trace_data" }
    };
}

json CaptureCoverageJson( const analysis::TraceInfoDto& info )
{
    static constexpr const char* CounterNames[] = {
        "observed", "emitted", "dropped", "filtered", "sampled_out", "overflow",
        "mismatch", "unresolved", "pre_capture", "replayed", "tail_truncated"
    };
    struct Snapshot
    {
        uint64_t sequence;
        uint64_t recordIndex;
        json document;
    };

    std::map<std::string, std::vector<Snapshot>> byProducer;
    json invalid = json::array();
    size_t seen = 0;
    size_t duplicates = 0;
    size_t envelopeBytes = 0;
    std::set<std::string> documents;
    std::set<uint64_t> connectionIds;
    for( size_t index = 0; index < info.appInfo.size(); index++ )
    {
        const auto& record = info.appInfo[index];
        const bool currentEnvelope = record.starts_with( ProducerQualityPrefix );
        const bool qualityLike = currentEnvelope || ( record.size() >= 4 && record.compare( 0, 3, "JNQ" ) == 0 && record.find( '|' ) != std::string::npos );
        if( !qualityLike ) continue;
        if( ++seen > MaximumQualityRecords )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality record limit exceeded" } } );
            break;
        }
        envelopeBytes += record.size();
        if( !currentEnvelope )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "unsupported producer quality envelope version" } } );
            continue;
        }
        if( record.size() <= ProducerQualityPrefix.size() || record.size() > MaximumIdentityEnvelopeBytes )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality envelope has an invalid size" } } );
            continue;
        }
        auto document = json::parse( record.begin() + ProducerQualityPrefix.size(), record.end(), nullptr, false );
        size_t fields = 0;
        if( document.is_discarded() || !document.is_object() || !IdentityShapeAllowed( document, 0, fields ) ||
            document.value( "schema_version", 0 ) != 1 || !document.contains( "connection_id" ) ||
            !document.contains( "snapshot_sequence" ) || !document.contains( "producer" ) || !document["producer"].is_object() )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality JSON failed schema or resource-limit validation" } } );
            continue;
        }
        auto& producer = document["producer"];
        if( !producer.contains( "key" ) || !producer["key"].is_string() || producer["key"].get_ref<const std::string&>().empty() ||
            !producer.contains( "source_mode" ) || !producer["source_mode"].is_string() ||
            !producer.contains( "requested" ) || !producer["requested"].is_boolean() ||
            !producer.contains( "compiled" ) || !producer["compiled"].is_boolean() ||
            !producer.contains( "supported" ) || !producer["supported"].is_boolean() ||
            !producer.contains( "enabled" ) || !producer["enabled"].is_boolean() ||
            !producer.contains( "effective" ) || !producer["effective"].is_boolean() ||
            !producer.contains( "permission_denied" ) || !producer["permission_denied"].is_boolean() ||
            !producer.contains( "deferred" ) || !producer["deferred"].is_boolean() ||
            !producer.contains( "counters" ) || !producer["counters"].is_object() )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality state is invalid" } } );
            continue;
        }
        const auto connectionId = DecimalStringValue( document["connection_id"] );
        const auto sequence = DecimalStringValue( document["snapshot_sequence"] );
        bool countersValid = connectionId.has_value() && sequence.has_value();
        for( const auto* counter : CounterNames )
            countersValid = countersValid && producer["counters"].contains( counter ) && DecimalStringValue( producer["counters"][counter] ).has_value();
        if( !countersValid )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality counter is invalid" } } );
            continue;
        }
        connectionIds.emplace( *connectionId );
        const auto canonical = document.dump();
        if( !documents.emplace( canonical ).second )
        {
            duplicates++;
            continue;
        }
        byProducer[producer["key"].get<std::string>()].push_back( { *sequence, uint64_t( index ), std::move( document ) } );
    }

    if( connectionIds.size() > 1 )
        invalid.push_back( { { "record_index", nullptr }, { "reason", "producer quality contains multiple connection ids" } } );
    const auto connectionId = connectionIds.size() == 1 ? std::optional<uint64_t>( *connectionIds.begin() ) : std::nullopt;
    const auto identity = CaptureIdentityJson( info );
    if( connectionId && identity.value( "present", false ) && identity.contains( "identity" ) &&
        identity["identity"].is_object() && identity["identity"].contains( "connection" ) &&
        identity["identity"]["connection"].is_object() && identity["identity"]["connection"].contains( "id" ) )
    {
        const auto identityConnectionId = DecimalStringValue( identity["identity"]["connection"]["id"] );
        if( !identityConnectionId || *identityConnectionId != *connectionId )
            invalid.push_back( { { "record_index", nullptr }, { "reason", "producer quality connection id does not match Capture Identity" } } );
    }

    json producers = json::array();
    json globalFindings = json::array();
    uint64_t globalObserved = 0;
    uint64_t globalEmitted = 0;
    bool complete = !byProducer.empty() && invalid.empty();
    for( auto& [key, snapshots] : byProducer )
    {
        std::sort( snapshots.begin(), snapshots.end(), []( const auto& left, const auto& right ) {
            if( left.sequence != right.sequence ) return left.sequence < right.sequence;
            return left.recordIndex < right.recordIndex;
        } );
        const auto& first = snapshots.front().document["producer"];
        const auto& last = snapshots.back().document["producer"];
        const bool windowComplete = snapshots.front().sequence < snapshots.back().sequence;
        complete = complete && windowComplete;
        json counters = json::object();
        std::array<uint64_t, 11> deltas {};
        bool regression = false;
        for( size_t counter = 0; counter < 11; counter++ )
        {
            const auto base = *DecimalStringValue( first["counters"][CounterNames[counter]] );
            const auto final = *DecimalStringValue( last["counters"][CounterNames[counter]] );
            if( final < base ) regression = true;
            deltas[counter] = final >= base ? final - base : 0;
            counters[CounterNames[counter]] = Decimal( deltas[counter] );
        }
        complete = complete && !regression;

        const bool requested = last["requested"].get<bool>();
        const bool compiled = last["compiled"].get<bool>();
        const bool supported = last["supported"].get<bool>();
        const bool enabled = last["enabled"].get<bool>();
        const bool effective = last["effective"].get<bool>();
        const bool permissionDenied = last["permission_denied"].get<bool>();
        const bool deferred = last["deferred"].get<bool>();
        std::string state;
        if( deferred ) state = "deferred";
        else if( !compiled ) state = "uncompiled";
        else if( permissionDenied ) state = "permission_denied";
        else if( !supported ) state = "unsupported";
        else if( !requested || !enabled || !effective ) state = "disabled";
        else if( !windowComplete || regression ) state = "unknown";
        else if( deltas[2] != 0 || deltas[5] != 0 || deltas[6] != 0 || deltas[7] != 0 || deltas[10] != 0 ) state = "degraded";
        else if( deltas[3] != 0 || deltas[4] != 0 ) state = "filtered";
        else if( deltas[0] == 0 && deltas[1] == 0 ) state = "real_zero";
        else state = "covered";

        json findings = json::array();
        const auto addFinding = [&]( const char* code, uint64_t count ) {
            if( count != 0 ) findings.push_back( { { "code", code }, { "count", Decimal( count ) } } );
        };
        addFinding( "DROPPED", deltas[2] );
        addFinding( "FILTERED", deltas[3] );
        addFinding( "SAMPLED_OUT", deltas[4] );
        addFinding( "OVERFLOW", deltas[5] );
        addFinding( "MISMATCH", deltas[6] );
        addFinding( "UNRESOLVED", deltas[7] );
        addFinding( "TAIL_TRUNCATED", deltas[10] );
        if( !windowComplete ) findings.push_back( { { "code", "NO_CLOSED_COUNTER_WINDOW" }, { "count", "1" } } );
        if( regression ) findings.push_back( { { "code", "COUNTER_REGRESSION" }, { "count", "1" } } );
        if( !findings.empty() ) globalFindings.push_back( { { "producer", key }, { "findings", findings } } );

        json ratio = nullptr;
        if( requested && compiled && supported && enabled && effective && windowComplete && !regression )
        {
            ratio = deltas[0] == 0 ? json( 1.0 ) : json( double( deltas[1] ) / double( deltas[0] ) );
            globalObserved += deltas[0];
            globalEmitted += deltas[1];
        }
        producers.push_back( {
            { "key", key }, { "id", last.value( "id", 0 ) }, { "source_mode", last["source_mode"] },
            { "producer_schema", last.value( "producer_schema", 0 ) },
            { "config_generation", last.value( "config_generation", "0" ) },
            { "requested", requested }, { "compiled", compiled }, { "supported", supported },
            { "enabled", enabled }, { "effective", effective }, { "permission_denied", permissionDenied },
            { "deferred", deferred }, { "reason", last.value( "reason", "" ) },
            { "filter", last.value( "filter", "" ) }, { "threshold", last.value( "threshold", "0" ) },
            { "budget", last.value( "budget", "0" ) }, { "sample_rate", last.value( "sample_rate", json::object() ) },
            { "state", state }, { "complete", windowComplete && !regression }, { "coverage_ratio", ratio },
            { "scanned_count", Decimal( deltas[0] ) }, { "total_count", Decimal( deltas[0] ) },
            { "omitted_count", Decimal( deltas[0] >= deltas[1] ? deltas[0] - deltas[1] : 0 ) },
            { "counters", counters }, { "quality_findings", findings },
            { "window", { { "first_sequence", Decimal( snapshots.front().sequence ) },
                { "last_sequence", Decimal( snapshots.back().sequence ) }, { "snapshot_count", snapshots.size() } } }
        } );
    }

    const bool present = !byProducer.empty();
    std::string reason;
    if( !present ) reason = seen == 0 ? "trace predates or did not emit JN Producer Quality" : "no valid JN Producer Quality document was found";
    else if( !complete ) reason = "producer quality is present but one or more counter windows are incomplete or invalid";
    json globalRatio = nullptr;
    if( globalObserved != 0 ) globalRatio = double( globalEmitted ) / double( globalObserved );
    else if( present && complete ) globalRatio = 1.0;
    return {
        { "present", present }, { "schema_version", 1 }, { "complete", complete },
        { "connection_id", connectionId ? json( Decimal( *connectionId ) ) : json( nullptr ) },
        { "reason", reason.empty() ? json( nullptr ) : json( reason ) },
        { "evidence_kind", "exact" }, { "coverage_ratio", globalRatio },
        { "scanned_count", Decimal( globalObserved ) }, { "total_count", Decimal( globalObserved ) },
        { "omitted_count", Decimal( globalObserved >= globalEmitted ? globalObserved - globalEmitted : 0 ) },
        { "producers", producers }, { "quality_findings", globalFindings }, { "invalid_records", invalid },
        { "records", { { "seen", seen }, { "valid", [&] { size_t count = 0; for( const auto& item : byProducer ) count += item.second.size(); return count; }() },
            { "duplicates", duplicates }, { "envelope_bytes", Decimal( envelopeBytes ) } } },
        { "trust", "untrusted_trace_data" }
    };
}

json GfxDispatchJson( const analysis::GfxDispatchDto& value )
{
    return {
        { "ref", value.ref }, { "dispatch_id", Decimal( value.dispatchId ) }, { "frame_index", Decimal( value.frameIndex ) },
        { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef }, { "expected_jobs", value.expectedJobs },
        { "threading_mode", value.threadingMode }, { "flags", value.flags }
    };
}

json GfxEntityJson( const analysis::GfxEntityDto& value )
{
    return {
        { "ref", value.ref }, { "entity_id", Decimal( value.entityId ) }, { "parent_id", Decimal( value.parentId ) },
        { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef }, { "kind", GfxEntityKindName( value.kind ) },
        { "kind_id", value.kind }, { "gpu_query_id", value.gpuQueryId }, { "gpu_context", value.gpuContext }, { "flags", value.flags }
    };
}

json GfxLinkJson( const analysis::GfxLinkDto& value )
{
    return {
        { "ref", value.ref }, { "source_id", Decimal( value.sourceId ) }, { "target_id", Decimal( value.targetId ) },
        { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef }, { "relation", GfxRelationName( value.relation ) },
        { "relation_id", value.relation }, { "flags", value.flags }
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

size_t ResolveFrameSet( const analysis::TraceSource& source, const json& params )
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

json DescribeData( const json& selection = json::object() )
{
    json result = {
        { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion },
        { "numeric_rules", {
            { "int64", "decimal string" }, { "address", "0x-prefixed hexadecimal string" },
            { "range", "half-open [start_ns,end_ns)" }, { "ratio_percentile_average", "JSON number" }
        } },
        { "limits", {
            { "default_page_size", DefaultPageSize }, { "maximum_page_size", MaximumPageSize },
            { "default_top_n", DefaultTopN }, { "maximum_top_n", MaximumTopN },
            { "request_bytes", MaximumRequestBytes }, { "response_bytes", MaximumResponseBytes },
            { "analysis_cache_bytes", Decimal( uint64_t( DefaultAnalysisCacheBytes ) ) },
            { "callstack_default_depth", 32 }, { "callstack_max_depth", 256 },
            { "source_default_bytes", 65536 }, { "source_max_bytes", 1048576 },
            { "frame_image_max_bytes", 16777216 }, { "frame_image_max_dimension", 4096 }
        } },
        { "filter_modes", { "exact", "contains", "prefix" } },
        { "methods", QueryMethodRegistry() }
    };

    const std::string requestedDomain = selection.value( "domain", "" );
    const std::string requestedOperation = selection.value( "operation", "" );
    json methods = json::array();
    for( const auto& value : result["methods"] )
    {
        const auto method = value.get<std::string>();
        const bool domainMatches = requestedDomain.empty() || method == requestedDomain || method.rfind( requestedDomain + '.', 0 ) == 0;
        const bool operationMatches = requestedOperation.empty() || method == requestedOperation;
        if( domainMatches && operationMatches ) methods.emplace_back( method );
    }
    if( ( !requestedDomain.empty() || !requestedOperation.empty() ) && methods.empty() ) throw QueryError( "METHOD_NOT_FOUND", "no tracy-query operation matches the requested domain/operation" );
    result["methods"] = methods;

    const auto requiredFor = []( const std::string& method ) {
        json required = json::array();
        if( method == "trace.open" ) required.emplace_back( "path" );
        else if( method.rfind( "compare.", 0 ) == 0 ) { required.emplace_back( "baseline_trace_id" ); required.emplace_back( "trace_id" ); }
        else if( method != "system.describe" && method != "system.schema" && method != "trace.list" ) required.emplace_back( "trace_id" );
        if( ( method.ends_with( ".get" ) && method != "frame.get" && method != "producer.get" ) || method == "job.dependencies" || method == "job.gfx_chain" || method == "zone.cpu.tree" || method == "zone.gpu.tree" || method == "source.lines" || method == "source.raw" ||
            method == "symbol.raw_code" || method == "symbol.disassembly" || method == "frame_image.metadata" || method == "frame_image.resource" || method == "frame_image.raw" ) required.emplace_back( "ref" );
        if( method == "memory.frame_snapshot" ) required.emplace_back( "frame_index" );
        if( method == "memory.diff" ) { required.emplace_back( "base_frame_index" ); required.emplace_back( "target_frame_index" ); }
        if( method == "memory.active_at_time" ) required.emplace_back( "time_ns" );
        if( method == "producer.get" ) required.emplace_back( "key" );
        if( method == "thread.statistics" || method == "thread.timeline" || method == "thread.migration" || method == "context_switch.thread" ) required.emplace_back( "thread_ref" );
        if( method == "plot.points" || method == "plot.range" || method == "plot.downsample" || method == "plot.statistics" ) required.emplace_back( "plot_ref" );
        if( method == "hardware_sample.address" || method == "hardware_sample.events" || method == "symbol.address" ) required.emplace_back( "address" );
        if( method == "callstack.frames" || method == "callstack.parent" ) required.emplace_back( "callstack" );
        if( method == "callstack.resolve" || method == "callstack.batch" ) required.emplace_back( "callstacks" );
        if( method == "statistics.compute" ) required.emplace_back( "values_ns" );
        return required;
    };
    json descriptors = json::array();
    for( const auto& value : methods )
    {
        const auto method = value.get<std::string>();
        const json alternatives = method == "frame.get" ? json::array( { json::array( { "ref" } ), json::array( { "frame_set", "index" } ) } ) : json::array();
        json exampleParams = json::object();
        for( const auto& required : requiredFor( method ) )
        {
            const auto name = required.get<std::string>();
            if( name == "path" ) exampleParams[name] = "C:\\\\captures\\\\capture.tracy";
            else if( name == "trace_id" ) exampleParams[name] = "trace-1";
            else if( name == "baseline_trace_id" ) exampleParams[name] = "trace-1";
            else if( name == "ref" ) exampleParams[name] = "tracy:v1:<fingerprint>:<kind>:<id>";
            else if( name == "callstacks" || name == "values_ns" ) exampleParams[name] = json::array( { "1" } );
            else if( name == "callstack" ) exampleParams[name] = "1";
            else if( name == "thread_ref" || name == "plot_ref" ) exampleParams[name] = "tracy:v1:<fingerprint>:<kind>:<id>";
            else if( name == "time_ns" ) exampleParams[name] = "0";
            else if( name == "address" ) exampleParams[name] = "0x0";
            else if( name == "key" ) exampleParams[name] = "cpu.zone.c-abi";
            else exampleParams[name] = 0;
        }
        if( method == "frame.get" ) exampleParams["index"] = 0;
        descriptors.push_back( {
            { "method", method }, { "required", requiredFor( method ) },
            { "one_of_required", alternatives },
            { "accepted_common_parameters", { "trace_id", "start_ns", "end_ns", "limit", "cursor", "filter", "fields" } },
            { "request_example", { { "protocol", QueryProtocol }, { "id", "request-1" }, { "method", method }, { "params", std::move( exampleParams ) } } },
            { "response_contract", "tracy-query/1 success or failure envelope; int64 values are decimal strings; refs are opaque" }
        } );
    }
    result["operations"] = std::move( descriptors );
    return result;
}

template<typename T, typename Scan, typename Match, typename Convert>
std::pair<json, bool> ScanFiltered( const analysis::TraceSource& source, const json& params, const PageRequest& page, Scan&& scan, Match&& match, Convert&& convert )
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

QueryService::QueryService( SessionManager& sessions, size_t analysisCacheBytes )
    : m_sessions( sessions )
    , m_cacheBudget( analysisCacheBytes )
{}

void QueryService::EvictCache( size_t incomingBytes )
{
    while( m_cacheBytes != 0 && ( incomingBytes > m_cacheBudget || m_cacheBytes > m_cacheBudget - incomingBytes ) )
    {
        bool gpu = false;
        std::string oldestKey;
        uint64_t oldestAccess = std::numeric_limits<uint64_t>::max();
        for( const auto& [key, entry] : m_gpuCache ) if( entry.value.use_count() == 1 && entry.access < oldestAccess ) { gpu = true; oldestKey = key; oldestAccess = entry.access; }
        for( const auto& [key, entry] : m_memoryCache ) if( entry.value.use_count() == 1 && entry.access < oldestAccess ) { gpu = false; oldestKey = key; oldestAccess = entry.access; }
        if( oldestKey.empty() ) break;
        if( gpu ) { m_cacheBytes -= m_gpuCache.at( oldestKey ).bytes; m_gpuCache.erase( oldestKey ); }
        else { m_cacheBytes -= m_memoryCache.at( oldestKey ).bytes; m_memoryCache.erase( oldestKey ); }
    }
}

void QueryService::EraseTraceCache( const std::string& traceId )
{
    const auto prefix = traceId + '|';
    for( auto it = m_gpuCache.begin(); it != m_gpuCache.end(); )
    {
        if( it->first.rfind( prefix, 0 ) == 0 ) { m_cacheBytes -= it->second.bytes; it = m_gpuCache.erase( it ); } else ++it;
    }
    for( auto it = m_memoryCache.begin(); it != m_memoryCache.end(); )
    {
        if( it->first.rfind( prefix, 0 ) == 0 ) { m_cacheBytes -= it->second.bytes; it = m_memoryCache.erase( it ); } else ++it;
    }
}

std::shared_ptr<const analysis::GpuMemoryAttribution> QueryService::CachedGpuAttribution( const std::string& traceId, const std::shared_ptr<analysis::TraceSource>& source )
{
    const auto key = traceId + "|gpu-attribution";
    const auto found = m_gpuCache.find( key );
    if( found != m_gpuCache.end() ) { found->second.access = ++m_cacheClock; return found->second.value; }
    auto value = std::make_shared<analysis::GpuMemoryAttribution>( source->GetGpuMemoryAttribution() );
    size_t bytes = sizeof( *value ) + value->warnings.capacity() * sizeof( std::string ) + value->requestScopes.capacity() * sizeof( analysis::GpuMemoryRequestScope ) +
        value->passes.capacity() * sizeof( analysis::GpuMemoryPass ) + value->allocations.capacity() * sizeof( analysis::GpuMemoryAllocationAttribution );
    for( const auto& warning : value->warnings ) bytes += warning.capacity();
    for( const auto& scope : value->requestScopes ) bytes += scope.name.capacity();
    for( const auto& pass : value->passes ) bytes += pass.name.capacity() + pass.operations.capacity() + pass.uses.capacity() * sizeof( analysis::GpuMemoryPassUse );
    for( const auto& allocation : value->allocations ) bytes += allocation.passIndices.capacity() * sizeof( size_t );
    EvictCache( bytes );
    if( bytes <= m_cacheBudget && m_cacheBytes <= m_cacheBudget - bytes )
    {
        m_cacheBytes += bytes;
        m_gpuCache.emplace( key, GpuCacheEntry { value, bytes, ++m_cacheClock } );
    }
    return value;
}

std::shared_ptr<const analysis::MemoryFrameSnapshot> QueryService::CachedMemorySnapshot( const std::string& traceId, const std::shared_ptr<analysis::TraceSource>& source,
    size_t frameSet, size_t frame, std::vector<std::string> poolRefs, bool allGpu )
{
    std::sort( poolRefs.begin(), poolRefs.end() );
    poolRefs.erase( std::unique( poolRefs.begin(), poolRefs.end() ), poolRefs.end() );
    std::ostringstream keyBuilder;
    keyBuilder << traceId << "|memory-frame|" << frameSet << '|' << frame << '|' << allGpu;
    for( const auto& pool : poolRefs ) keyBuilder << '|' << pool;
    const auto key = keyBuilder.str();
    const auto found = m_memoryCache.find( key );
    if( found != m_memoryCache.end() ) { found->second.access = ++m_cacheClock; return found->second.value; }
    auto value = std::make_shared<analysis::MemoryFrameSnapshot>( source->GetMemoryFrameSnapshot( frameSet, frame, poolRefs, allGpu ) );
    const size_t bytes = sizeof( *value ) + value->pools.capacity() * sizeof( analysis::MemoryFramePoolSummary ) +
        ( value->activeAtStart.capacity() + value->activeAtEnd.capacity() + value->allocated.capacity() + value->freed.capacity() + value->transitions.capacity() ) * sizeof( analysis::MemoryEventKey );
    EvictCache( bytes );
    if( bytes <= m_cacheBudget && m_cacheBytes <= m_cacheBudget - bytes )
    {
        m_cacheBytes += bytes;
        m_memoryCache.emplace( key, MemoryCacheEntry { value, bytes, ++m_cacheClock } );
    }
    return value;
}

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

json QueryService::Execute( const json& request, const std::optional<std::string>& defaultTraceId, std::stop_token stopToken )
{
    json id = nullptr;
    try
    {
        if( !request.is_object() ) throw QueryError( "INVALID_REQUEST", "request must be a JSON object" );
        if( !JsonDepthAllowed( request ) ) throw QueryError( "RESOURCE_LIMIT", "request JSON nesting exceeds 64 levels" );
        static const std::set<std::string> requestFields = { "protocol", "id", "method", "params" };
        for( const auto& [key, value] : request.items() ) if( requestFields.find( key ) == requestFields.end() )
        {
            throw QueryError( "INVALID_REQUEST", "unknown top-level request field: " + key );
        }
        if( request.contains( "id" ) ) id = request["id"];
        if( request.value( "protocol", "" ) != QueryProtocol ) throw QueryError( "INVALID_REQUEST", "protocol must be tracy-query/1" );
        if( !request.contains( "id" ) || !( id.is_string() || id.is_number() ) ) throw QueryError( "INVALID_REQUEST", "id must be a string or number" );
        if( !request.contains( "method" ) || !request["method"].is_string() ) throw QueryError( "INVALID_REQUEST", "method must be a string" );
        const auto params = request.value( "params", json::object() );
        if( !params.is_object() ) throw QueryError( "INVALID_PARAMS", "params must be an object" );
        if( stopToken.stop_requested() ) throw QueryError( "CANCELLED", "query was cancelled", true );
        std::lock_guard lock( m_queryMutex );
        if( stopToken.stop_requested() ) throw QueryError( "CANCELLED", "query was cancelled", true );
        auto response = Dispatch( id, request["method"].get<std::string>(), params, defaultTraceId, stopToken );
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

json QueryService::Dispatch( const json& id, const std::string& method, const json& params, const std::optional<std::string>& defaultTraceId, std::stop_token stopToken )
{
    const auto checkCancelled = [&] {
        if( stopToken.stop_requested() ) throw QueryError( "CANCELLED", "query was cancelled", true );
    };
    checkCancelled();
    if( method == "system.schema" ) return Success( id, {
        { "schema", json::parse( QuerySchemaJson ) },
        { "coverage", {
            { "domain", json::parse( QueryCoverageJson ) },
            { "field", json::parse( QueryFieldCoverageJson ) },
            { "mcp", json::parse( QueryMcpCoverageJson ) }
        } }
    } );
    if( method == "system.describe" )
    {
        auto data = DescribeData( params );
        data["limits"]["analysis_cache_bytes"] = Decimal( uint64_t( m_cacheBudget ) );
        data["analysis_cache"] = { { "bytes", Decimal( uint64_t( m_cacheBytes ) ) }, { "entries", m_gpuCache.size() + m_memoryCache.size() }, { "policy", "LRU; entries in use are not evicted" } };
        return Success( id, std::move( data ) );
    }
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
        return Success( id, {
            { "status", TraceJson( trace ) },
            { "load_progress", {
                { "stage", trace.loadStage.empty() ? json( nullptr ) : json( trace.loadStage ) },
                { "completed", Decimal( trace.loadCompleted ) }, { "total", Decimal( trace.loadTotal ) },
                { "sub_completed", Decimal( trace.loadSubCompleted ) }, { "sub_total", Decimal( trace.loadSubTotal ) }
            } },
            { "error_code", trace.errorCode.empty() ? json( nullptr ) : json( trace.errorCode ) },
            { "error_message", trace.errorMessage.empty() ? json( nullptr ) : json( trace.errorMessage ) }
        }, trace );
    }
    if( method == "trace.close" )
    {
        const auto traceId = ResolveTraceId( params, defaultTraceId );
        EraseTraceCache( traceId );
        const auto trace = m_sessions.Close( traceId );
        return Success( id, { { "closed", trace.state == analysis::TraceSourceState::Closed }, { "state", analysis::ToString( trace.state ) } }, trace );
    }

    const std::optional<std::string> maybeTraceId = ( params.contains( "trace_id" ) || defaultTraceId ) ? std::optional<std::string>( ResolveTraceId( params, defaultTraceId ) ) : std::nullopt;
    if( method == "system.capabilities" && !maybeTraceId )
    {
        auto limits = DescribeData()["limits"];
        limits["analysis_cache_bytes"] = Decimal( uint64_t( m_cacheBudget ) );
        return Success( id, {
            { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion }, { "trace_versions", { "0.9.0", "0.13.2-JN" } },
            { "source_kinds", { "snapshot", "segment" } }, { "statistics_required", true },
            { "limits", std::move( limits ) }
        } );
    }
    if( !maybeTraceId ) throw QueryError( "INVALID_PARAMS", "trace_id is required" );

    const auto trace = m_sessions.Status( *maybeTraceId );
    const auto source = m_sessions.GetReadySource( *maybeTraceId );
    const auto parseCallstack = [&]( const json& value, bool parent = false ) -> uint32_t {
        uint64_t parsed = 0;
        if( value.is_string() )
        {
            const auto text = value.get<std::string>();
            if( text.starts_with( "tracy:v1:" ) )
            {
                auto entity = source->ParseEntityRef( text, parent ? "parent-callstack" : "callstack" );
                if( !entity && parent ) entity = source->ParseEntityRef( text, "callstack" );
                if( !entity ) throw QueryError( "INVALID_PARAMS", "callstack ref does not belong to this trace or has the wrong kind" );
                parsed = *entity;
            }
            else
            {
                const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed, 10 );
                if( result.ec != std::errc() || result.ptr != text.data() + text.size() ) throw QueryError( "INVALID_PARAMS", "callstack must be an opaque ref or unsigned decimal string" );
            }
        }
        else if( value.is_number_unsigned() ) parsed = value.get<uint64_t>();
        else if( value.is_number_integer() )
        {
            const auto integer = value.get<int64_t>();
            if( integer < 0 ) throw QueryError( "INVALID_PARAMS", "callstack must be non-negative" );
            parsed = uint64_t( integer );
        }
        else throw QueryError( "INVALID_PARAMS", "callstack must be an opaque ref, unsigned integer, or decimal string" );
        if( parsed > std::numeric_limits<uint32_t>::max() ) throw QueryError( "INVALID_PARAMS", "callstack exceeds the uint32 range" );
        return uint32_t( parsed );
    };

    if( method == "system.capabilities" )
    {
        json domains = json::array();
        for( const auto& capability : source->GetCapabilities() ) domains.emplace_back( CapabilityJson( capability ) );
        auto limits = DescribeData()["limits"];
        limits["analysis_cache_bytes"] = Decimal( uint64_t( m_cacheBudget ) );
        return Success( id, { { "domains", std::move( domains ) }, { "limits", std::move( limits ) } }, trace );
    }

    const auto info = [&] { return source->GetTraceInfo(); };
    if( method == "trace.info" ) return Success( id, TraceInfoJson( info() ), trace );
    if( method == "trace.counts" ) return Success( id, CountsJson( info().counts ), trace );
    if( method == "trace.app_info" ) return Success( id, { { "app_info", info().appInfo }, { "trust", "untrusted_trace_data" } }, trace );
    if( method == "trace.identity" ) return Success( id, CaptureIdentityJson( info() ), trace );
    if( method == "capture.context" ) return Success( id, CaptureContextJson( info() ), trace );
    if( method == "capture.coverage" || method == "producer.list" )
        return Success( id, CaptureCoverageJson( info() ), trace );
    if( method == "producer.get" )
    {
        if( !params.contains( "key" ) || !params["key"].is_string() || params["key"].get_ref<const std::string&>().empty() )
            throw QueryError( "INVALID_PARAMS", "key is required" );
        auto coverage = CaptureCoverageJson( info() );
        for( const auto& producer : coverage["producers"] )
        {
            if( producer.value( "key", "" ) == params["key"].get<std::string>() )
            {
                auto selected = producer;
                coverage.erase( "producers" );
                coverage["producer"] = std::move( selected );
                return Success( id, std::move( coverage ), trace );
            }
        }
        throw QueryError( "ENTITY_NOT_FOUND", "producer key was not found in this trace" );
    }
    if( method == "trace.crash" )
    {
        const auto crash = source->GetCrash();
        return Success( id, {
            { "present", crash.present }, { "thread_ref", crash.present ? json( crash.threadRef ) : json( nullptr ) },
            { "time_ns", crash.timeNs ? json( Decimal( *crash.timeNs ) ) : json( nullptr ) },
            { "message", crash.present ? json( crash.message ) : json( nullptr ) },
            { "callstack", crash.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( crash.callstack ) ) ) },
            { "callstack_ref", crash.callstackRef ? json( *crash.callstackRef ) : json( nullptr ) },
            { "trust", "untrusted_trace_data" }
        }, trace );
    }
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

    const auto requiredDomain = [&]() -> std::string {
        if( method == "job.gfx.statistics" || method == "job.gfx_chain" ) return "job.gfx";
        if( method.rfind( "memory.gpu.", 0 ) == 0 ) return "memory.gpu";
        if( method.rfind( "frame_image.", 0 ) == 0 ) return "frame_image";
        if( method.rfind( "hardware_sample.", 0 ) == 0 ) return "hardware_sample";
        if( method.rfind( "context_switch.", 0 ) == 0 ) return "context_switch";
        if( method.rfind( "zone.cpu.", 0 ) == 0 ) return "zone.cpu";
        if( method.rfind( "zone.gpu.", 0 ) == 0 ) return "zone.gpu";
        for( const auto* domain : { "thread", "cpu", "frame", "timeline", "callstack", "sample", "symbol", "source", "memory", "lock", "plot", "message", "job" } )
        {
            const std::string prefix = std::string( domain ) + '.';
            if( method.rfind( prefix, 0 ) == 0 ) return domain;
        }
        return {};
    }();
    if( !requiredDomain.empty() )
    {
        const auto capabilities = source->GetCapabilities();
        const auto capability = std::find_if( capabilities.begin(), capabilities.end(), [&]( const auto& value ) { return value.domain == requiredDomain; } );
        if( capability == capabilities.end() || !capability->present )
        {
            const auto reason = capability == capabilities.end() ? "trace source does not advertise this domain" : capability->reason;
            throw QueryError( "CAPABILITY_UNAVAILABLE", requiredDomain + " is unavailable: " + reason, false, { { "domain", requiredDomain }, { "reason", reason } } );
        }
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

    if( method == "thread.statistics" )
    {
        if( !params.contains( "thread_ref" ) || !params["thread_ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "thread_ref is required" );
        const auto threadRef = params["thread_ref"].get<std::string>();
        const auto threads = source->GetThreads();
        const auto found = std::find_if( threads.begin(), threads.end(), [&]( const auto& value ) { return value.ref == threadRef; } );
        if( found == threads.end() ) throw QueryError( "ENTITY_NOT_FOUND", "thread ref was not found" );
        std::vector<int64_t> running;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto events = source->ScanContextSwitchEvents( range );
            for( const auto& event : events ) if( event.threadRef == threadRef && event.endNs ) running.emplace_back( *event.endNs - event.startNs );
            offset += events.size();
            if( events.size() < chunk ) break;
        }
        return Success( id, { { "thread", ThreadJson( *found ) }, { "running_regions", StatisticsJson( analysis::ComputeStatistics( std::move( running ) ) ) } }, trace );
    }

    if( method == "thread.timeline" || method == "thread.migration" )
    {
        if( !params.contains( "thread_ref" ) || !params["thread_ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "thread_ref is required" );
        const auto threadRef = params["thread_ref"].get<std::string>();
        const auto page = ParsePage( params, method, trace );
        if( method == "thread.migration" )
        {
            json all = json::array();
            std::optional<uint8_t> previousCpu;
            size_t scanOffset = 0;
            constexpr size_t chunk = 4096;
            while( true )
            {
                checkCancelled();
                auto range = ScanRangeFrom( params, scanOffset, chunk );
                const auto values = source->ScanContextSwitchEvents( range );
                for( const auto& event : values )
                {
                    if( event.threadRef != threadRef ) continue;
                    if( previousCpu && *previousCpu != event.cpu ) all.push_back( {
                        { "ref", source->MakeEntityRef( "thread-migration", all.size() ) },
                        { "time_ns", Decimal( event.startNs ) }, { "from_cpu", *previousCpu }, { "to_cpu", event.cpu }, { "context_switch_ref", event.ref }
                    } );
                    previousCpu = event.cpu;
                }
                scanOffset += values.size();
                if( values.size() < chunk ) break;
            }
            const size_t begin = std::min( page.offset, all.size() );
            const size_t end = std::min( begin + page.limit, all.size() );
            json migrations = json::array();
            for( size_t index = begin; index < end; index++ ) migrations.emplace_back( std::move( all[index] ) );
            const auto cursor = NextCursor( page, method, trace, end - begin, end < all.size() );
            return Success( id, { { "thread_ref", threadRef }, { "migration_count", Decimal( all.size() ) }, { "migrations", std::move( migrations ) } }, trace, PageJson( page, end - begin, cursor ) );
        }
        auto [events, hasMore] = ScanFiltered<analysis::ContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanContextSwitchEvents( range ); },
            [&]( const auto& event ) { return event.threadRef == threadRef; }, ContextSwitchJson );
        const auto cursor = NextCursor( page, method, trace, events.size(), hasMore );
        return Success( id, { { "thread_ref", threadRef }, { "context_switches", std::move( events ) } }, trace, PageJson( page, events.size(), cursor ) );
    }

    if( method == "cpu.topology" )
    {
        json topology = json::array();
        for( const auto& value : source->GetCpuTopology() ) topology.push_back( {
            { "cpu", value.cpu }, { "package", value.package },
            { "die", value.dieAvailability.available ? json( value.die ) : json( nullptr ) }, { "core", value.core },
            { "field_availability", { { "die", FieldAvailabilityJson( value.dieAvailability ) } } }
        } );
        return Success( id, { { "logical_cpus", std::move( topology ) } }, trace );
    }
    if( method == "cpu.usage" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto usage = source->GetCpuUsage();
        const size_t begin = std::min( page.offset, usage.size() );
        const size_t end = std::min( begin + page.limit, usage.size() );
        json points = json::array();
        for( size_t index = begin; index < end; index++ ) points.push_back( {
            { "ref", usage[index].ref }, { "time_ns", Decimal( usage[index].timeNs ) },
            { "own_threads", usage[index].own }, { "other_processes", usage[index].other }
        } );
        const auto cursor = NextCursor( page, method, trace, points.size(), end < usage.size() );
        return Success( id, { { "points", std::move( points ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "cpu.timeline" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto cpuFilter = params.contains( "cpu" ) ? std::optional<unsigned>( params["cpu"].get<unsigned>() ) : std::nullopt;
        auto [events, hasMore] = ScanFiltered<analysis::CpuContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanCpuContextSwitchEvents( range ); },
            [&]( const auto& event ) { return !cpuFilter || event.cpu == *cpuFilter; }, CpuContextSwitchJson );
        const auto returned = events.size();
        const auto cursor = NextCursor( page, method, trace, events.size(), hasMore );
        return Success( id, { { "segments", std::move( events ) } }, trace, PageJson( page, returned, cursor ) );
    }
    if( method == "context_switch.range" || method == "context_switch.thread" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string threadRef = params.value( "thread_ref", "" );
        if( method == "context_switch.thread" && threadRef.empty() ) throw QueryError( "INVALID_PARAMS", "thread_ref is required" );
        const auto cpuFilter = params.contains( "cpu" ) ? std::optional<unsigned>( params["cpu"].get<unsigned>() ) : std::nullopt;
        auto [events, hasMore] = ScanFiltered<analysis::ContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanContextSwitchEvents( range ); },
            [&]( const auto& event ) { return ( threadRef.empty() || event.threadRef == threadRef ) && ( !cpuFilter || event.cpu == *cpuFilter ); }, ContextSwitchJson );
        const auto cursor = NextCursor( page, method, trace, events.size(), hasMore );
        return Success( id, { { "context_switches", std::move( events ) } }, trace, PageJson( page, events.size(), cursor ) );
    }
    if( method == "context_switch.statistics" )
    {
        std::map<std::string, std::vector<int64_t>> byThread;
        std::map<std::string, std::vector<int64_t>> wakeLatency;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto events = source->ScanContextSwitchEvents( range );
            for( const auto& event : events )
            {
                if( event.endNs ) byThread[event.threadRef].emplace_back( *event.endNs - event.startNs );
                if( event.wakeupNs && event.startNs >= *event.wakeupNs ) wakeLatency[event.threadRef].emplace_back( event.startNs - *event.wakeupNs );
            }
            offset += events.size();
            if( events.size() < chunk ) break;
        }
        json groups = json::array();
        for( auto& [threadRef, durations] : byThread ) groups.push_back( {
            { "thread_ref", threadRef }, { "running", StatisticsJson( analysis::ComputeStatistics( std::move( durations ) ) ) },
            { "wakeup_latency", StatisticsJson( analysis::ComputeStatistics( std::move( wakeLatency[threadRef] ) ) ) }
        } );
        std::sort( groups.begin(), groups.end(), []( const auto& lhs, const auto& rhs ) {
            const auto left = std::stoll( lhs["running"]["total_ns"].template get<std::string>() );
            const auto right = std::stoll( rhs["running"]["total_ns"].template get<std::string>() );
            return left != right ? left > right : lhs["thread_ref"].template get<std::string>() < rhs["thread_ref"].template get<std::string>();
        } );
        const auto groupCount = groups.size();
        const auto limit = TopN( params );
        if( groups.size() > limit ) groups.erase( groups.begin() + limit, groups.end() );
        return Success( id, { { "threads", std::move( groups ) }, { "thread_count", Decimal( groupCount ) } }, trace );
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
        if( method == "frame.get" )
        {
            if( params.contains( "ref" ) && params["ref"].is_string() )
            {
                const auto requested = params["ref"].get<std::string>();
                size_t offset = 0; constexpr size_t chunk = 4096;
                while( true )
                {
                    checkCancelled();
                    analysis::ScanRange range; range.offset = offset; range.limit = chunk;
                    const auto frames = source->ScanFrames( range );
                    const auto found = std::find_if( frames.begin(), frames.end(), [&]( const auto& frame ) { return frame.ref == requested; } );
                    if( found != frames.end() ) return Success( id, FrameJson( *found ), trace );
                    offset += frames.size(); if( frames.size() < chunk ) break;
                }
                throw QueryError( "ENTITY_NOT_FOUND", "frame ref was not found" );
            }
            if( !params.contains( "index" ) ) throw QueryError( "INVALID_PARAMS", "index is required" );
            const size_t setIndex = ResolveFrameSet( *source, params );
            const auto frameIndex = params["index"].get<size_t>();
            const auto values = source->GetFramesForSet( setIndex, frameIndex, 1 );
            if( values.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "frame index was not found" );
            return Success( id, FrameJson( values.front() ), trace );
        }
        const size_t setIndex = ResolveFrameSet( *source, params );
        const auto page = ParsePage( params, method, trace );
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
    if( method == "frame.range_mapping" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto range = ScanRangeFrom( params, page.offset, page.limit + 1 );
        auto frames = source->ScanFrames( range );
        const bool hasMore = frames.size() > page.limit;
        if( hasMore ) frames.pop_back();
        json values = json::array();
        for( const auto& frame : frames ) values.emplace_back( FrameJson( frame ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "frames", std::move( values ) }, { "range", { { "start_ns", Decimal( range.startNs ) }, { "end_ns", Decimal( range.endNs ) } } } }, trace, PageJson( page, frames.size(), cursor ) );
    }
    if( method == "frame_image.list" || method == "frame_image.metadata" || method == "frame_image.resource" || method == "frame_image.raw" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto images = source->GetFrameImageResources();
        if( method == "frame_image.metadata" || method == "frame_image.resource" || method == "frame_image.raw" )
        {
            if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto ref = params["ref"].get<std::string>();
            const auto found = std::find_if( images.begin(), images.end(), [&]( const auto& item ) { return item.ref == ref; } );
            if( found == images.end() ) throw QueryError( "ENTITY_NOT_FOUND", "frame image ref was not found" );
            if( method == "frame_image.raw" )
            {
                const auto offset = size_t( UnsignedParameter( params, "offset_bytes", 0, std::numeric_limits<size_t>::max() ) );
                const auto maxBytes = size_t( UnsignedParameter( params, "max_bytes", 65536, 1024 * 1024 ) );
                if( maxBytes == 0 ) throw QueryError( "INVALID_PARAMS", "max_bytes must be between 1 and 1048576" );
                auto data = BinaryChunkJson( source->ReadFrameImageBc1( found->id, offset, maxBytes ) );
                data["format"] = "bc1_dxt1";
                data["width"] = found->width;
                data["height"] = found->height;
                data["flipped"] = found->flipped;
                return Success( id, std::move( data ), trace );
            }
            return Success( id, {
                { "ref", found->ref }, { "width", found->width }, { "height", found->height }, { "flipped", found->flipped },
                { "raw_frame_index", found->rawFrameIndex }, { "frame_ref", found->frameRef ? json( *found->frameRef ) : json( nullptr ) },
                { "raw_bc1_bytes", Decimal( found->rawBc1Bytes ) },
                { "resource_uri", "tracy://trace/" + trace.id + "/frame-image/" + std::to_string( found->id ) }, { "mime_type", "image/png" }
            }, trace );
        }
        const size_t begin = std::min( page.offset, images.size() );
        const size_t end = std::min( begin + page.limit, images.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.push_back( {
            { "ref", images[index].ref }, { "width", images[index].width }, { "height", images[index].height },
            { "flipped", images[index].flipped }, { "raw_frame_index", images[index].rawFrameIndex },
            { "frame_ref", images[index].frameRef ? json( *images[index].frameRef ) : json( nullptr ) },
            { "raw_bc1_bytes", Decimal( images[index].rawBc1Bytes ) },
            { "resource_uri", "tracy://trace/" + trace.id + "/frame-image/" + std::to_string( images[index].id ) }
        } );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < images.size() );
        return Success( id, { { "images", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
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
        const auto page = ParsePage( params, method, trace );
        const auto sourceContexts = source->GetGpuContexts();
        const size_t begin = std::min( page.offset, sourceContexts.size() ); const size_t end = std::min( begin + page.limit, sourceContexts.size() );
        json contexts = json::array();
        for( size_t index = begin; index < end; index++ )
        {
            const auto& value = sourceContexts[index];
            json noteNames = json::array();
            for( const auto& note : value.noteNames ) noteNames.push_back( {
                { "time_ns", Decimal( note.timeNs ) }, { "name", note.name }, { "trust", "untrusted_trace_data" }
            } );
            json notes = json::array();
            for( const auto& note : value.notes ) notes.push_back( {
                { "query_id", note.queryId }, { "time_ns", Decimal( note.timeNs ) }, { "value", note.value }
            } );
            contexts.push_back( {
                { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "thread_ref", value.threadRef },
                { "custom_name", value.customName ? json( *value.customName ) : json( nullptr ) },
                { "zone_count", Decimal( value.zoneCount ) }, { "period", value.period }, { "calibrated", value.calibrated },
                { "type", value.type }, { "type_name", value.typeName }, { "overflow", Decimal( value.overflow ) },
                { "note_names", std::move( noteNames ) }, { "notes", std::move( notes ) },
                { "field_availability", { { "notes", FieldAvailabilityJson( value.notesAvailability ) } } }
            } );
        }
        const auto cursor = NextCursor( page, method, trace, end - begin, end < sourceContexts.size() );
        return Success( id, { { "contexts", std::move( contexts ) } }, trace, PageJson( page, end - begin, cursor ) );
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
    if( method == "zone.cpu.get" || method == "zone.gpu.get" || method == "zone.cpu.tree" || method == "zone.gpu.tree" )
    {
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto ref = params["ref"].get<std::string>();
        const bool gpu = method.rfind( "zone.gpu", 0 ) == 0;
        const bool tree = method.ends_with( ".tree" );
        if( !tree )
        {
            if( gpu )
            {
                const auto value = source->GetGpuZone( ref );
                if( !value ) throw QueryError( "ENTITY_NOT_FOUND", "GPU zone ref was not found" );
                return Success( id, GpuZoneJson( *value ), trace );
            }
            const auto value = source->GetCpuZone( ref );
            if( !value ) throw QueryError( "ENTITY_NOT_FOUND", "CPU zone ref was not found" );
            return Success( id, CpuZoneJson( *value ), trace );
        }

        const auto page = ParsePage( params, method, trace );
        if( gpu )
        {
            const auto root = source->GetGpuZone( ref );
            if( !root ) throw QueryError( "ENTITY_NOT_FOUND", "GPU zone ref was not found" );
            auto children = source->GetGpuZoneChildren( ref, page.offset, page.limit + 1 );
            const bool hasMore = children.size() > page.limit;
            if( hasMore ) children.pop_back();
            json values = json::array(); for( const auto& child : children ) values.emplace_back( GpuZoneJson( child ) );
            const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
            return Success( id, { { "root", GpuZoneJson( *root ) }, { "children", std::move( values ) } }, trace, PageJson( page, children.size(), cursor ) );
        }
        const auto root = source->GetCpuZone( ref );
        if( !root ) throw QueryError( "ENTITY_NOT_FOUND", "CPU zone ref was not found" );
        auto children = source->GetCpuZoneChildren( ref, page.offset, page.limit + 1 );
        const bool hasMore = children.size() > page.limit;
        if( hasMore ) children.pop_back();
        json values = json::array(); for( const auto& child : children ) values.emplace_back( CpuZoneJson( child ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "root", CpuZoneJson( *root ) }, { "children", std::move( values ) } }, trace, PageJson( page, children.size(), cursor ) );
    }
    if( method == "zone.cpu.statistics" || method == "zone.gpu.statistics" )
    {
        const bool gpu = method == "zone.gpu.statistics";
        struct Aggregate
        {
            std::string name, file;
            uint32_t line = 0;
            std::vector<int64_t> inclusive;
            std::vector<int64_t> self;
            std::vector<int64_t> running;
        };
        std::map<std::string, Aggregate> groups;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            if( gpu )
            {
                const auto values = source->ScanGpuZones( range );
                for( const auto& value : values ) if( value.gpuEndNs && TextMatches( value.name, params ) )
                {
                    auto& group = groups[value.sourceLocationRef];
                    group.name = value.name; group.file = value.file; group.line = value.line;
                    group.inclusive.emplace_back( *value.gpuEndNs - value.gpuStartNs );
                    if( value.selfTimeNs ) group.self.emplace_back( *value.selfTimeNs );
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
                    group.inclusive.emplace_back( *value.endNs - value.startNs );
                    if( value.selfTimeNs ) group.self.emplace_back( *value.selfTimeNs );
                    if( value.runningTimeNs ) group.running.emplace_back( *value.runningTimeNs );
                }
                offset += values.size();
                if( values.size() < chunk ) break;
            }
        }
        std::vector<std::pair<std::string, Aggregate*>> order;
        for( auto& [ref, group] : groups ) order.emplace_back( ref, &group );
        std::sort( order.begin(), order.end(), []( const auto& lhs, const auto& rhs ) {
            const auto left = analysis::ComputeStatistics( lhs.second->inclusive ).total;
            const auto right = analysis::ComputeStatistics( rhs.second->inclusive ).total;
            return left != right ? left > right : lhs.first < rhs.first;
        } );
        const size_t limit = TopN( params );
        if( order.size() > limit ) order.resize( limit );
        json values = json::array();
        for( const auto& [ref, group] : order ) values.push_back( {
            { "source_location_ref", ref }, { "name", group->name }, { "file", group->file }, { "line", group->line },
            { "inclusive", StatisticsJson( analysis::ComputeStatistics( group->inclusive ) ) },
            { "self", StatisticsJson( analysis::ComputeStatistics( group->self ) ) },
            { "running", gpu ? json( nullptr ) : json( StatisticsJson( analysis::ComputeStatistics( group->running ) ) ) }
        } );
        return Success( id, { { "groups", std::move( values ) }, { "group_count", groups.size() } }, trace );
    }

    if( method == "zone.cpu.flamegraph" || method == "zone.gpu.flamegraph" )
    {
        const bool gpu = method == "zone.gpu.flamegraph";
        const std::string direction = params.value( "direction", "top_down" );
        if( direction != "top_down" && direction != "bottom_up" ) throw QueryError( "INVALID_PARAMS", "direction must be top_down or bottom_up" );
        struct PathStats { std::string path; uint64_t count = 0; int64_t inclusive = 0; int64_t self = 0; };
        std::unordered_map<std::string, std::string> pathsByRef;
        std::unordered_map<std::string, PathStats> groups;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            size_t received = 0;
            if( gpu )
            {
                const auto zones = source->ScanGpuZones( range ); received = zones.size();
                for( const auto& zone : zones )
                {
                    const auto parent = zone.parentRef ? pathsByRef.find( *zone.parentRef ) : pathsByRef.end();
                    const auto path = parent == pathsByRef.end() ? zone.name : parent->second + ";" + zone.name;
                    pathsByRef[zone.ref] = path;
                    if( !zone.gpuEndNs || !TextMatches( zone.name, params ) ) continue;
                    auto& stats = groups[path]; stats.path = path; stats.count++; stats.inclusive += *zone.gpuEndNs - zone.gpuStartNs;
                    if( zone.selfTimeNs ) stats.self += *zone.selfTimeNs;
                }
            }
            else
            {
                const auto zones = source->ScanCpuZones( range ); received = zones.size();
                for( const auto& zone : zones )
                {
                    const auto parent = zone.parentRef ? pathsByRef.find( *zone.parentRef ) : pathsByRef.end();
                    const auto path = parent == pathsByRef.end() ? zone.name : parent->second + ";" + zone.name;
                    pathsByRef[zone.ref] = path;
                    if( !zone.endNs || !TextMatches( zone.name, params ) ) continue;
                    auto& stats = groups[path]; stats.path = path; stats.count++; stats.inclusive += *zone.endNs - zone.startNs;
                    if( zone.selfTimeNs ) stats.self += *zone.selfTimeNs;
                }
            }
            offset += received; if( received < chunk ) break;
        }
        std::vector<PathStats*> order; order.reserve( groups.size() ); for( auto& [path, stats] : groups ) order.emplace_back( &stats );
        std::sort( order.begin(), order.end(), []( const auto* lhs, const auto* rhs ) { return lhs->inclusive != rhs->inclusive ? lhs->inclusive > rhs->inclusive : lhs->path < rhs->path; } );
        const auto limit = TopN( params ); if( order.size() > limit ) order.resize( limit );
        json paths = json::array();
        for( const auto* stats : order )
        {
            auto components = Split( stats->path, ';' ); if( direction == "bottom_up" ) std::reverse( components.begin(), components.end() );
            paths.push_back( { { "path", std::move( components ) }, { "count", Decimal( stats->count ) }, { "inclusive_ns", Decimal( stats->inclusive ) }, { "self_ns", Decimal( stats->self ) } } );
        }
        return Success( id, { { "direction", direction }, { "paths", std::move( paths ) }, { "path_count", groups.size() } }, trace );
    }

    if( method == "memory.pools" || method == "memory.gpu.pools" )
    {
        const auto page = ParsePage( params, method, trace );
        auto sourcePools = source->GetMemoryPools();
        if( method == "memory.gpu.pools" ) sourcePools.erase( std::remove_if( sourcePools.begin(), sourcePools.end(), []( const auto& pool ) { return !pool.gpuD3D12; } ), sourcePools.end() );
        const size_t begin = std::min( page.offset, sourcePools.size() ); const size_t end = std::min( begin + page.limit, sourcePools.size() );
        json pools = json::array(); for( size_t index = begin; index < end; index++ ) pools.emplace_back( MemoryPoolJson( sourcePools[index] ) );
        const auto cursor = NextCursor( page, method, trace, end - begin, end < sourcePools.size() );
        return Success( id, { { "pools", std::move( pools ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "memory.events" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string pool = params.value( "pool_ref", "" );
        auto [values, hasMore] = ScanFiltered<analysis::MemoryEventDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMemoryEvents( range ); },
            [&]( const auto& value ) { return ( pool.empty() || value.poolRef == pool ) && TextMatches( value.address, params ); }, MemoryEventJson );
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "events", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "memory.get" )
    {
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto requested = params["ref"].get<std::string>();
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto events = source->ScanMemoryEvents( range );
            const auto found = std::find_if( events.begin(), events.end(), [&]( const auto& event ) { return event.ref == requested; } );
            if( found != events.end() ) return Success( id, MemoryEventJson( *found ), trace );
            offset += events.size(); if( events.size() < chunk ) break;
        }
        throw QueryError( "ENTITY_NOT_FOUND", "memory event ref was not found" );
    }
    if( method == "memory.active_at_time" )
    {
        if( !params.contains( "time_ns" ) ) throw QueryError( "INVALID_PARAMS", "time_ns is required" );
        const auto time = ScanRangeFrom( json { { "start_ns", params["time_ns"] }, { "end_ns", Decimal( std::numeric_limits<int64_t>::max() ) } }, 0, 1 ).startNs;
        const auto page = ParsePage( params, method, trace );
        const std::string pool = params.value( "pool_ref", "" );
        auto [values, hasMore] = ScanFiltered<analysis::MemoryEventDto>( *source, json::object(), page,
            []( const auto& item, const auto& range ) { return item.ScanMemoryEvents( range ); },
            [&]( const auto& event ) { return ( pool.empty() || event.poolRef == pool ) && event.allocationNs <= time && ( !event.freeNs || *event.freeNs > time ); }, MemoryEventJson );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "time_ns", Decimal( time ) }, { "events", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "memory.frame_snapshot" )
    {
        const auto setIndex = ResolveFrameSet( *source, params );
        if( !params.contains( "frame_index" ) ) throw QueryError( "INVALID_PARAMS", "frame_index is required" );
        const auto frameIndex = params["frame_index"].get<size_t>();
        std::vector<std::string> pools;
        if( params.contains( "pool_refs" ) )
        {
            if( !params["pool_refs"].is_array() ) throw QueryError( "INVALID_PARAMS", "pool_refs must be an array" );
            for( const auto& value : params["pool_refs"] ) { if( !value.is_string() ) throw QueryError( "INVALID_PARAMS", "pool_refs must contain strings" ); pools.emplace_back( value.get<std::string>() ); }
        }
        const bool allGpu = params.value( "scope", "all" ) == "gpu_d3d12";
        if( params.value( "scope", "all" ) != "all" && !allGpu && params.value( "scope", "all" ) != "pools" ) throw QueryError( "INVALID_PARAMS", "scope must be all, pools, or gpu_d3d12" );
        const auto snapshotValue = CachedMemorySnapshot( trace.id, source, setIndex, frameIndex, pools, allGpu );
        const auto& snapshot = *snapshotValue;
        if( !snapshot.valid ) throw QueryError( "ENTITY_NOT_FOUND", "frame or selected memory pools were not found" );
        json summaries = json::array(); for( const auto& summary : snapshot.pools ) summaries.emplace_back( MemorySummaryJson( *source, summary ) );
        const std::string category = params.value( "category", "all_transitions" );
        const std::vector<analysis::MemoryEventKey>* selected = nullptr;
        if( category == "active_at_start" ) selected = &snapshot.activeAtStart;
        else if( category == "active_at_end" ) selected = &snapshot.activeAtEnd;
        else if( category == "allocated_in_frame" ) selected = &snapshot.allocated;
        else if( category == "freed_in_frame" ) selected = &snapshot.freed;
        else if( category == "all_transitions" ) selected = &snapshot.transitions;
        else throw QueryError( "INVALID_PARAMS", "unknown memory snapshot category" );
        const auto page = ParsePage( params, method, trace );
        const size_t begin = std::min( page.offset, selected->size() ); const size_t end = std::min( begin + page.limit, selected->size() );
        json events = json::array(); for( size_t index = begin; index < end; index++ ) if( const auto event = source->GetMemoryEvent( ( *selected )[index] ) ) events.emplace_back( MemoryEventJson( *event ) );
        const auto cursor = NextCursor( page, method, trace, events.size(), end < selected->size() );
        return Success( id, {
            { "frame", FrameJson( source->GetFramesForSet( setIndex, frameIndex, 1 ).front() ) },
            { "begin_ns", Decimal( snapshot.begin ) }, { "end_ns", Decimal( snapshot.end ) }, { "valid", snapshot.valid },
            { "consistent", snapshot.consistent }, { "possible_capture_baseline", snapshot.possibleCaptureBaseline },
            { "total", MemorySummaryJson( *source, snapshot.total, true ) }, { "pools", std::move( summaries ) },
            { "category", category }, { "category_count", Decimal( selected->size() ) }, { "events", std::move( events ) }
        }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "memory.diff" )
    {
        const auto setIndex = ResolveFrameSet( *source, params );
        if( !params.contains( "base_frame_index" ) || !params.contains( "target_frame_index" ) ) throw QueryError( "INVALID_PARAMS", "base_frame_index and target_frame_index are required" );
        const auto baseIndex = params["base_frame_index"].get<size_t>(); const auto targetIndex = params["target_frame_index"].get<size_t>();
        const auto baseValue = CachedMemorySnapshot( trace.id, source, setIndex, baseIndex, {}, false );
        const auto targetValue = CachedMemorySnapshot( trace.id, source, setIndex, targetIndex, {}, false );
        const auto& base = *baseValue;
        const auto& target = *targetValue;
        if( !base.valid || !target.valid ) throw QueryError( "ENTITY_NOT_FOUND", "base or target frame was not found" );
        std::set<analysis::MemoryEventKey> before( base.activeAtEnd.begin(), base.activeAtEnd.end() );
        std::set<analysis::MemoryEventKey> after( target.activeAtEnd.begin(), target.activeAtEnd.end() );
        std::vector<analysis::MemoryEventKey> added, removed, retained;
        std::set_difference( after.begin(), after.end(), before.begin(), before.end(), std::back_inserter( added ) );
        std::set_difference( before.begin(), before.end(), after.begin(), after.end(), std::back_inserter( removed ) );
        std::set_intersection( before.begin(), before.end(), after.begin(), after.end(), std::back_inserter( retained ) );
        const auto category = params.value( "category", "added" );
        const auto* selected = category == "added" ? &added : category == "removed" ? &removed : category == "retained" ? &retained : nullptr;
        if( !selected ) throw QueryError( "INVALID_PARAMS", "category must be added, removed, or retained" );
        const auto page = ParsePage( params, method, trace ); const size_t begin = std::min( page.offset, selected->size() ); const size_t end = std::min( begin + page.limit, selected->size() );
        json events = json::array(); uint64_t bytes = 0;
        for( size_t index = begin; index < end; index++ ) if( const auto event = source->GetMemoryEvent( ( *selected )[index] ) ) { bytes += event->size; events.emplace_back( MemoryEventJson( *event ) ); }
        const auto cursor = NextCursor( page, method, trace, events.size(), end < selected->size() );
        return Success( id, {
            { "base_frame_index", baseIndex }, { "target_frame_index", targetIndex },
            { "counts", { { "added", Decimal( added.size() ) }, { "removed", Decimal( removed.size() ) }, { "retained", Decimal( retained.size() ) } } },
            { "category", category }, { "returned_bytes", Decimal( bytes ) }, { "events", std::move( events ) }
        }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "memory.leak_candidates" || method == "memory.callstack_tree" )
    {
        std::vector<analysis::MemoryEventDto> active;
        size_t offset = 0; constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto events = source->ScanMemoryEvents( range );
            for( const auto& event : events ) if( !event.freeNs ) active.emplace_back( event );
            offset += events.size(); if( events.size() < chunk ) break;
        }
        if( method == "memory.callstack_tree" )
        {
            struct Group { uint64_t bytes = 0, count = 0; };
            std::map<uint32_t, Group> groups; for( const auto& event : active ) { auto& group = groups[event.allocationCallstack]; group.bytes += event.size; group.count++; }
            const auto maxDepth = params.value( "max_depth", size_t( 64 ) );
            if( maxDepth < 1 || maxDepth > 256 ) throw QueryError( "INVALID_PARAMS", "max_depth must be between 1 and 256" );
            const auto direction = params.value( "direction", "bottom_up" );
            if( direction != "top_down" && direction != "bottom_up" ) throw QueryError( "INVALID_PARAMS", "direction must be top_down or bottom_up" );
            std::vector<uint32_t> callstacks; for( const auto& [callstack, group] : groups ) if( callstack != 0 ) callstacks.emplace_back( callstack );
            std::unordered_map<uint32_t, std::vector<json>> paths;
            for( const auto& frame : source->ResolveCallstacks( callstacks, maxDepth ) ) paths[frame.callstack].emplace_back( CallstackFrameJson( frame ) );
            json values = json::array();
            for( const auto& [callstack, group] : groups )
            {
                auto path = paths[callstack]; if( direction == "top_down" ) std::reverse( path.begin(), path.end() );
                values.push_back( {
                    { "callstack", callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( callstack ) ) ) },
                    { "callstack_ref", callstack == 0 ? json( nullptr ) : json( source->MakeEntityRef( "callstack", callstack ) ) },
                    { "bytes", Decimal( group.bytes ) }, { "count", Decimal( group.count ) }, { "path", std::move( path ) }
                } );
            }
            std::sort( values.begin(), values.end(), []( const auto& lhs, const auto& rhs ) { return std::stoull( lhs["bytes"].template get<std::string>() ) > std::stoull( rhs["bytes"].template get<std::string>() ); } );
            const auto limit = TopN( params ); if( values.size() > limit ) values.erase( values.begin() + limit, values.end() );
            return Success( id, { { "active_allocations", Decimal( active.size() ) }, { "direction", direction }, { "callstacks", std::move( values ) } }, trace );
        }
        std::sort( active.begin(), active.end(), []( const auto& lhs, const auto& rhs ) { return lhs.size != rhs.size ? lhs.size > rhs.size : lhs.ref < rhs.ref; } );
        const auto limit = TopN( params ); if( active.size() > limit ) active.resize( limit );
        json values = json::array(); for( const auto& event : active ) values.emplace_back( MemoryEventJson( event ) );
        return Success( id, { { "candidates", std::move( values ) }, { "heuristic", "allocations still active at capture end, ordered by size; not proof of a leak" } }, trace );
    }
    if( method == "memory.gpu.allocations" || method == "memory.gpu.request_scopes" || method == "memory.gpu.pass_uses" || method == "memory.gpu.attribution" )
    {
        const auto attributionValue = CachedGpuAttribution( trace.id, source );
        const auto& attribution = *attributionValue;
        if( !attribution.protocolPresent && method != "memory.gpu.allocations" ) throw QueryError( "CAPABILITY_UNAVAILABLE", "trace contains no GTMEM1 relation protocol" );
        if( method == "memory.gpu.request_scopes" )
        {
            const auto page = ParsePage( params, method, trace );
            const size_t begin = std::min( page.offset, attribution.requestScopes.size() ); const size_t end = std::min( begin + page.limit, attribution.requestScopes.size() );
            json scopes = json::array(); for( size_t index = begin; index < end; index++ ) { const auto& scope = attribution.requestScopes[index]; scopes.push_back( {
                { "ref", source->MakeEntityRef( "gpu-memory-scope", index ) }, { "label_id", Decimal( scope.labelId ) }, { "frame", Decimal( scope.frame ) },
                { "thread_id", Decimal( scope.thread ) }, { "start_ns", Decimal( scope.start ) }, { "end_ns", Decimal( scope.end ) },
                { "name", scope.name }, { "cpu_zone_ref", source->GetCpuZoneRef( scope.cpuZoneIndex ).value_or( "" ) }, { "trust", "untrusted_trace_data" }
            } ); }
            const auto cursor = NextCursor( page, method, trace, end - begin, end < attribution.requestScopes.size() );
            return Success( id, { { "scopes", std::move( scopes ) } }, trace, PageJson( page, end - begin, cursor ) );
        }
        if( method == "memory.gpu.pass_uses" )
        {
            const auto page = ParsePage( params, method, trace );
            std::optional<uint64_t> requestedPass;
            if( params.contains( "pass_id" ) )
            {
                try { requestedPass = params["pass_id"].is_string() ? std::stoull( params["pass_id"].get<std::string>() ) : params["pass_id"].get<uint64_t>(); }
                catch( const std::exception& ) { throw QueryError( "INVALID_PARAMS", "pass_id must be an unsigned decimal string" ); }
            }
            if( params.contains( "pass_ref" ) && params["pass_ref"].is_string() )
            {
                const auto ref = params["pass_ref"].get<std::string>();
                const auto found = std::find_if( attribution.passes.begin(), attribution.passes.end(), [&]( const auto& pass ) { return source->MakeEntityRef( "gpu-memory-pass", pass.passId ) == ref; } );
                if( found == attribution.passes.end() ) throw QueryError( "ENTITY_NOT_FOUND", "GPU memory pass ref was not found" );
                requestedPass = found->passId;
            }
            if( requestedPass )
            {
                const auto found = attribution.passById.find( *requestedPass );
                if( found == attribution.passById.end() ) throw QueryError( "ENTITY_NOT_FOUND", "GPU memory pass id was not found" );
                const auto& pass = attribution.passes[found->second];
                const auto begin = std::min( page.offset, pass.uses.size() ); const auto end = std::min( begin + page.limit, pass.uses.size() );
                const auto cursor = NextCursor( page, method, trace, end - begin, end < pass.uses.size() );
                return Success( id, { { "pass", GpuPassJson( *source, pass, true, begin, page.limit ) }, { "complete", pass.complete } }, trace, PageJson( page, end - begin, cursor ) );
            }
            const size_t begin = std::min( page.offset, attribution.passes.size() ); const size_t end = std::min( begin + page.limit, attribution.passes.size() );
            json passes = json::array(); for( size_t index = begin; index < end; index++ ) passes.emplace_back( GpuPassJson( *source, attribution.passes[index], false ) );
            const auto cursor = NextCursor( page, method, trace, end - begin, end < attribution.passes.size() );
            return Success( id, { { "passes", std::move( passes ) }, { "complete", attribution.complete }, { "warnings", attribution.warnings } }, trace, PageJson( page, end - begin, cursor ) );
        }
        const auto page = ParsePage( params, method, trace );
        const std::string relationFilter = params.value( "relation_state", "" );
        if( !relationFilter.empty() && relationFilter != "request_and_uses" && relationFilter != "request_only" && relationFilter != "uses_only" && relationFilter != "unattributed" )
        {
            throw QueryError( "INVALID_PARAMS", "relation_state must be request_and_uses, request_only, uses_only, or unattributed" );
        }
        const std::string poolFilter = params.value( "pool_ref", "" );
        std::optional<uint64_t> allocationFilter;
        if( params.contains( "allocation_id" ) )
        {
            try { allocationFilter = params["allocation_id"].is_string() ? std::stoull( params["allocation_id"].get<std::string>() ) : params["allocation_id"].get<uint64_t>(); }
            catch( const std::exception& ) { throw QueryError( "INVALID_PARAMS", "allocation_id must be an unsigned decimal string" ); }
        }
        json allocations = json::array();
        size_t matched = 0;
        bool hasMore = false;
        for( const auto& item : attribution.allocations )
        {
            const auto event = source->GetMemoryEvent( item.allocation.key ); if( !event ) continue;
            const std::string relationState = item.requestLabelId && !item.passIndices.empty() ? "request_and_uses" : item.requestLabelId ? "request_only" : !item.passIndices.empty() ? "uses_only" : "unattributed";
            if( allocationFilter && item.allocation.allocationId != *allocationFilter ) continue;
            if( !poolFilter.empty() && event->poolRef != poolFilter ) continue;
            if( !relationFilter.empty() && relationState != relationFilter ) continue;
            if( matched++ < page.offset ) continue;
            if( allocations.size() >= page.limit ) { hasMore = true; break; }
            json passes = json::array();
            for( const auto index : item.passIndices )
            {
                if( index < attribution.passes.size() && passes.size() < DefaultPageSize ) passes.emplace_back( source->MakeEntityRef( "gpu-memory-pass", attribution.passes[index].passId ) );
            }
            json allocationJson = {
                { "allocation", MemoryEventJson( *event ) }, { "allocation_id", Decimal( item.allocation.allocationId ) },
                { "request_label_id", item.requestLabelId ? json( Decimal( *item.requestLabelId ) ) : json( nullptr ) },
                { "pass_ref_count", Decimal( item.passIndices.size() ) }, { "pass_refs", std::move( passes ) },
                { "pass_refs_truncated", item.passIndices.size() > DefaultPageSize }, { "relation_state", relationState },
                { "logical_resource", nullptr }
            };
            if( item.allocation.poolName.rfind( "GPU D3D12 Logical ", 0 ) == 0 )
            {
                const auto logical = attribution.logicalById.find( item.allocation.allocationId );
                if( logical != attribution.logicalById.end() )
                {
                    const auto& resource = attribution.logicalResources[logical->second];
                    allocationJson["logical_resource"] = {
                        { "logical_resource_id", Decimal( resource.logicalResourceId ) },
                        { "physical_allocation_id", Decimal( resource.physicalAllocationId ) },
                        { "size_bytes", Decimal( resource.size ) }, { "physical_offset_bytes", Decimal( resource.physicalOffset ) },
                        { "primary_owner_id", Decimal( uint64_t( resource.primaryOwnerId ) ) }, { "physical_owner_id", Decimal( uint64_t( resource.physicalOwnerId ) ) },
                        { "kind", std::string( 1, resource.kind ) }, { "segment", std::string( 1, resource.segment ) },
                        { "flags", resource.flags }, { "name", resource.name }, { "trust", "untrusted_trace_data" }
                    };
                }
            }
            allocations.emplace_back( std::move( allocationJson ) );
        }
        const auto returned = allocations.size();
        const auto cursor = NextCursor( page, method, trace, returned, hasMore );
        json data = { { "allocations", std::move( allocations ) }, { "protocol_present", attribution.protocolPresent }, { "complete", attribution.complete }, { "warnings", attribution.warnings } };
        if( method == "memory.gpu.attribution" )
        {
            json passes = json::array(); for( size_t index = 0; index < std::min<size_t>( attribution.passes.size(), DefaultTopN ); index++ ) passes.emplace_back( GpuPassJson( *source, attribution.passes[index], false ) );
            data["pass_count"] = Decimal( attribution.passes.size() ); data["pass_preview"] = std::move( passes );
            json logicalResources = json::array();
            for( size_t index = 0; index < std::min<size_t>( attribution.logicalResources.size(), DefaultPageSize ); index++ )
            {
                const auto& resource = attribution.logicalResources[index];
                logicalResources.push_back( {
                    { "logical_resource_id", Decimal( resource.logicalResourceId ) }, { "physical_allocation_id", Decimal( resource.physicalAllocationId ) },
                    { "size_bytes", Decimal( resource.size ) }, { "physical_offset_bytes", Decimal( resource.physicalOffset ) },
                    { "primary_owner_id", Decimal( uint64_t( resource.primaryOwnerId ) ) }, { "physical_owner_id", Decimal( uint64_t( resource.physicalOwnerId ) ) },
                    { "kind", std::string( 1, resource.kind ) }, { "segment", std::string( 1, resource.segment ) },
                    { "flags", resource.flags }, { "name", resource.name }, { "trust", "untrusted_trace_data" }
                } );
            }
            json ownerRollups = json::array();
            for( const auto& rollup : attribution.ownerRollups ) ownerRollups.push_back( {
                { "taxonomy_id", Decimal( uint64_t( rollup.taxonomyId ) ) }, { "owned_physical_bytes", Decimal( rollup.physicalBytes ) },
                { "physical_allocation_count", Decimal( rollup.physicalAllocationCount ) },
                { "logical_resource_count", Decimal( rollup.logicalResourceCount ) }
            } );
            json workingSets = json::array();
            for( size_t index = 0; index < std::min<size_t>( attribution.workingSets.size(), MaximumPageSize ); index++ )
            {
                const auto& workingSet = attribution.workingSets[index];
                workingSets.push_back( {
                    { "frame", Decimal( workingSet.frame ) }, { "taxonomy_id", Decimal( uint64_t( workingSet.taxonomyId ) ) },
                    { "referenced_working_set_bytes", Decimal( workingSet.referencedPhysicalBytes ) },
                    { "physical_allocation_count", Decimal( workingSet.physicalAllocationCount ) },
                    { "logical_resource_count", Decimal( workingSet.logicalResourceCount ) }
                } );
            }
            data["logical_resource_count"] = Decimal( attribution.logicalResources.size() );
            data["logical_resources"] = std::move( logicalResources );
            data["logical_resources_truncated"] = attribution.logicalResources.size() > DefaultPageSize;
            data["owner_rollups"] = std::move( ownerRollups );
            data["working_sets"] = std::move( workingSets );
            data["working_sets_truncated"] = attribution.workingSets.size() > MaximumPageSize;
            data["rollup_semantics"] = {
                { "owner", "each physical allocation is counted once under one primary owner" },
                { "working_set", "deduplicated by physical allocation within each frame and taxonomy node" },
                { "sibling_sum", "working sets of sibling taxonomy nodes may overlap and must not be summed as physical total" }
            };
        }
        return Success( id, std::move( data ), trace, PageJson( page, returned, cursor ) );
    }
    if( method == "lock.list" || method == "lock.get" )
    {
        auto sourceLocks = source->GetLocks();
        sourceLocks.erase( std::remove_if( sourceLocks.begin(), sourceLocks.end(), [&]( const auto& value ) { return !TextMatches( value.name, params ); } ), sourceLocks.end() );
        if( method == "lock.get" && ( !params.contains( "ref" ) || !params["ref"].is_string() ) ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto requestedRef = params.value( "ref", "" );
        json locks = json::array();
        for( const auto& value : sourceLocks )
        {
            if( !requestedRef.empty() && value.ref != requestedRef ) continue;
            locks.push_back( {
            { "ref", value.ref }, { "native_id", value.nativeId }, { "name", value.name }, { "source_location_ref", value.sourceLocationRef },
            { "custom_name", value.customName ? json( *value.customName ) : json( nullptr ) },
            { "event_count", Decimal( value.eventCount ) }, { "thread_count", Decimal( value.threadCount ) },
            { "type", value.type }, { "type_name", value.typeName },
            { "valid", value.valid }, { "contended", value.contended }, { "announce_ns", Decimal( value.announceNs ) },
            { "terminate_ns", value.terminateNs ? json( Decimal( *value.terminateNs ) ) : json( nullptr ) }, { "trust", "untrusted_trace_data" }
            } );
        }
        if( method == "lock.get" )
        {
            if( locks.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "lock ref was not found" );
            return Success( id, std::move( locks.front() ), trace );
        }
        const auto page = ParsePage( params, method, trace );
        const size_t begin = std::min( page.offset, locks.size() ); const size_t end = std::min( begin + page.limit, locks.size() );
        json pageLocks = json::array(); for( size_t index = begin; index < end; index++ ) pageLocks.emplace_back( std::move( locks[index] ) );
        const auto cursor = NextCursor( page, method, trace, end - begin, end < locks.size() );
        return Success( id, { { "locks", std::move( pageLocks ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "lock.timeline" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string lockRef = params.value( "lock_ref", "" );
        auto [events, hasMore] = ScanFiltered<analysis::LockEventDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanLockEvents( range ); },
            [&]( const auto& event ) { return lockRef.empty() || event.lockRef == lockRef; }, LockEventJson );
        const auto cursor = NextCursor( page, method, trace, events.size(), hasMore );
        return Success( id, { { "events", std::move( events ) } }, trace, PageJson( page, events.size(), cursor ) );
    }
    if( method == "lock.contention_statistics" )
    {
        struct LockStats { uint64_t waits = 0, obtains = 0, releases = 0; std::vector<int64_t> waitDurations; std::unordered_map<std::string, int64_t> waiting; };
        std::map<std::string, LockStats> groups;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto events = source->ScanLockEvents( range );
            for( const auto& event : events )
            {
                auto& stats = groups[event.lockRef];
                const bool wait = event.type == "wait" || event.type == "wait_shared";
                const bool obtain = event.type == "obtain" || event.type == "obtain_shared";
                const bool release = event.type == "release" || event.type == "release_shared";
                if( wait ) { stats.waits++; stats.waiting[event.threadRef] = event.timeNs; }
                if( obtain )
                {
                    stats.obtains++;
                    const auto found = stats.waiting.find( event.threadRef );
                    if( found != stats.waiting.end() && event.timeNs >= found->second ) { stats.waitDurations.emplace_back( event.timeNs - found->second ); stats.waiting.erase( found ); }
                }
                if( release ) stats.releases++;
            }
            offset += events.size();
            if( events.size() < chunk ) break;
        }
        json values = json::array();
        for( auto& [lockRef, stats] : groups ) values.push_back( {
            { "lock_ref", lockRef }, { "waits", Decimal( stats.waits ) }, { "obtains", Decimal( stats.obtains ) }, { "releases", Decimal( stats.releases ) },
            { "wait_time", StatisticsJson( analysis::ComputeStatistics( std::move( stats.waitDurations ) ) ) }
        } );
        std::sort( values.begin(), values.end(), []( const auto& lhs, const auto& rhs ) {
            const auto left = std::stoll( lhs["wait_time"]["total_ns"].template get<std::string>() );
            const auto right = std::stoll( rhs["wait_time"]["total_ns"].template get<std::string>() );
            return left != right ? left > right : lhs["lock_ref"].template get<std::string>() < rhs["lock_ref"].template get<std::string>();
        } );
        const auto groupCount = values.size();
        const auto limit = TopN( params );
        if( values.size() > limit ) values.erase( values.begin() + limit, values.end() );
        return Success( id, { { "locks", std::move( values ) }, { "lock_count", Decimal( groupCount ) } }, trace );
    }
    if( method == "plot.list" )
    {
        const auto page = ParsePage( params, method, trace );
        auto sourcePlots = source->GetPlotList();
        sourcePlots.erase( std::remove_if( sourcePlots.begin(), sourcePlots.end(), [&]( const auto& value ) { return !TextMatches( value.name, params ); } ), sourcePlots.end() );
        const size_t begin = std::min( page.offset, sourcePlots.size() ); const size_t end = std::min( begin + page.limit, sourcePlots.size() );
        json plots = json::array();
        for( size_t index = begin; index < end; index++ ) { const auto& value = sourcePlots[index]; plots.push_back( {
            { "ref", value.ref }, { "index", value.index }, { "name", value.name }, { "type", value.type }, { "format", value.format },
            { "show_steps", value.showSteps }, { "fill", value.fill }, { "color", value.color },
            { "point_count", Decimal( value.pointCount ) }, { "min", value.min }, { "max", value.max }, { "sum", value.sum }, { "trust", "untrusted_trace_data" }
        } ); }
        const auto cursor = NextCursor( page, method, trace, end - begin, end < sourcePlots.size() );
        return Success( id, { { "plots", std::move( plots ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "plot.points" || method == "plot.range" )
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
    if( method == "plot.statistics" || method == "plot.downsample" )
    {
        const std::string plot = params.value( "plot_ref", "" );
        if( plot.empty() ) throw QueryError( "INVALID_PARAMS", "plot_ref is required" );
        std::vector<analysis::PlotPointDto> points;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto values = source->ScanPlots( range );
            for( const auto& point : values ) if( point.plotRef == plot ) points.emplace_back( point );
            offset += values.size();
            if( values.size() < chunk ) break;
        }
        if( method == "plot.statistics" )
        {
            if( points.empty() ) return Success( id, { { "present", false }, { "reason", "no points in the requested range" } }, trace );
            double sum = 0;
            double min = std::numeric_limits<double>::max();
            double max = std::numeric_limits<double>::lowest();
            for( const auto& point : points ) { sum += point.value; min = std::min( min, point.value ); max = std::max( max, point.value ); }
            return Success( id, { { "present", true }, { "count", Decimal( points.size() ) }, { "min", min }, { "max", max }, { "mean", sum / points.size() }, { "sum", sum } }, trace );
        }
        const auto buckets = params.value( "buckets", size_t( 200 ) );
        if( buckets < 1 || buckets > 1000 ) throw QueryError( "INVALID_PARAMS", "buckets must be between 1 and 1000" );
        json output = json::array();
        if( !points.empty() )
        {
            const int64_t first = points.front().timeNs;
            const int64_t last = std::max( first + 1, points.back().timeNs + 1 );
            const int64_t width = std::max<int64_t>( 1, ( last - first + int64_t( buckets ) - 1 ) / int64_t( buckets ) );
            size_t cursor = 0;
            while( cursor < points.size() )
            {
                const auto bucketIndex = std::min<size_t>( buckets - 1, size_t( ( points[cursor].timeNs - first ) / width ) );
                const auto begin = first + int64_t( bucketIndex ) * width;
                const auto end = begin + width;
                double min = points[cursor].value, max = points[cursor].value, sum = 0;
                size_t count = 0;
                while( cursor < points.size() && points[cursor].timeNs < end ) { min = std::min( min, points[cursor].value ); max = std::max( max, points[cursor].value ); sum += points[cursor].value; count++; cursor++; }
                output.push_back( { { "begin_ns", Decimal( begin ) }, { "end_ns", Decimal( end ) }, { "count", count }, { "min", min }, { "max", max }, { "mean", sum / count } } );
            }
        }
        return Success( id, { { "plot_ref", plot }, { "buckets", std::move( output ) } }, trace );
    }
    if( method == "message.search" || method == "message.get" )
    {
        if( method == "message.get" && ( !params.contains( "ref" ) || !params["ref"].is_string() ) ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const std::string requestedRef = params.value( "ref", "" );
        const std::string threadRef = params.value( "thread_ref", "" );
        std::optional<uint32_t> callstack;
        if( params.contains( "callstack" ) ) callstack = parseCallstack( params["callstack"] );
        const auto page = ParsePage( params, method, trace );
        auto [values, hasMore] = ScanFiltered<analysis::MessageDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMessages( range ); },
            [&]( const auto& value ) { return ( requestedRef.empty() || value.ref == requestedRef ) && ( threadRef.empty() || value.threadRef == threadRef ) &&
                ( !callstack || value.callstack == *callstack ) && TextMatches( value.text, params ); }, MessageJson );
        if( method == "message.get" )
        {
            if( values.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "message ref was not found" );
            return Success( id, std::move( values.front() ), trace );
        }
        values = ProjectFields( std::move( values ), params );
        const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
        return Success( id, { { "messages", std::move( values ) } }, trace, PageJson( page, values.size(), cursor ) );
    }
    if( method == "job.search" || method == "job.get" || method == "job.dependencies" || method == "job.critical_path" || method == "job.gfx.statistics" || method == "job.gfx_chain" )
    {
        auto jobs = source->GetJobs();
        const auto findJob = [&]( uint64_t jobId ) { return std::find_if( jobs.begin(), jobs.end(), [&]( const auto& job ) { return job.jobId == jobId; } ); };
        const auto parseJobRef = [&]( const char* parameter = "ref" ) -> uint64_t {
            if( !params.contains( parameter ) || !params[parameter].is_string() ) throw QueryError( "INVALID_PARAMS", std::string( parameter ) + " is required" );
            const auto value = params[parameter].get<std::string>();
            const auto parsed = source->ParseEntityRef( value, "job" );
            if( !parsed ) throw QueryError( "INVALID_PARAMS", std::string( parameter ) + " is not a Job ref from this trace" );
            return *parsed;
        };

        if( method == "job.search" )
        {
            const auto page = ParsePage( params, method, trace );
            const std::string kind = params.value( "kind", "" );
            const std::string state = params.value( "state", "" );
            jobs.erase( std::remove_if( jobs.begin(), jobs.end(), [&]( const auto& job ) {
                const char* currentState = job.cancelled ? "cancelled" : job.incomplete ? "incomplete" : job.completedNs ? "completed" : job.truncated ? "truncated" : "scheduled";
                return !TextMatches( job.name, params ) || ( !kind.empty() && kind != JobKindName( job.kind ) ) || ( !state.empty() && state != currentState );
            } ), jobs.end() );
            std::sort( jobs.begin(), jobs.end(), []( const auto& lhs, const auto& rhs ) { return lhs.scheduleNs != rhs.scheduleNs ? lhs.scheduleNs < rhs.scheduleNs : lhs.jobId < rhs.jobId; } );
            const auto begin = std::min( page.offset, jobs.size() );
            const auto end = std::min( begin + page.limit, jobs.size() );
            json values = json::array();
            for( size_t index = begin; index < end; index++ ) values.push_back( JobJson( *source, jobs[index], false ) );
            values = ProjectFields( std::move( values ), params );
            const auto cursor = NextCursor( page, method, trace, end - begin, end < jobs.size() );
            return Success( id, { { "jobs", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
        }

        if( method == "job.get" )
        {
            const auto jobId = parseJobRef();
            const auto job = findJob( jobId );
            if( job == jobs.end() ) throw QueryError( "ENTITY_NOT_FOUND", "Job ref was not found" );
            return Success( id, JobJson( *source, *job, true ), trace );
        }

        if( method == "job.dependencies" )
        {
            const auto jobId = parseJobRef();
            const auto job = findJob( jobId );
            if( job == jobs.end() ) throw QueryError( "ENTITY_NOT_FOUND", "Job ref was not found" );
            json upstream = json::array();
            for( const auto& dependency : job->dependencies )
            {
                const auto prerequisite = findJob( dependency.prerequisiteJobId );
                upstream.push_back( prerequisite == jobs.end() ? json {
                    { "ref", source->MakeEntityRef( "job", dependency.prerequisiteJobId ) }, { "job_id", Decimal( dependency.prerequisiteJobId ) }, { "missing", true }
                } : JobJson( *source, *prerequisite, false ) );
            }
            json downstream = json::array();
            for( const auto& candidate : jobs )
            {
                if( std::any_of( candidate.dependencies.begin(), candidate.dependencies.end(), [&]( const auto& dependency ) { return dependency.prerequisiteJobId == jobId; } ) )
                    downstream.push_back( JobJson( *source, candidate, false ) );
            }
            return Success( id, { { "job", JobJson( *source, *job, false ) }, { "upstream", std::move( upstream ) }, { "downstream", std::move( downstream ) } }, trace );
        }

        if( method == "job.critical_path" )
        {
            if( jobs.empty() ) return Success( id, { { "jobs", json::array() }, { "total_execution_ns", "0" }, { "has_cycle", false } }, trace );
            std::unordered_map<uint64_t, size_t> indexById;
            indexById.reserve( jobs.size() );
            for( size_t index = 0; index < jobs.size(); index++ ) indexById.emplace( jobs[index].jobId, index );
            std::vector<std::vector<size_t>> outgoing( jobs.size() );
            std::vector<size_t> indegree( jobs.size(), 0 );
            for( size_t index = 0; index < jobs.size(); index++ )
            {
                for( const auto& dependency : jobs[index].dependencies )
                {
                    const auto prerequisite = indexById.find( dependency.prerequisiteJobId );
                    if( prerequisite == indexById.end() ) continue;
                    outgoing[prerequisite->second].push_back( index );
                    indegree[index]++;
                }
            }
            std::queue<size_t> ready;
            for( size_t index = 0; index < indegree.size(); index++ ) if( indegree[index] == 0 ) ready.push( index );
            std::vector<int64_t> cost( jobs.size(), 0 );
            std::vector<std::optional<size_t>> parent( jobs.size() );
            size_t processed = 0;
            while( !ready.empty() )
            {
                checkCancelled();
                const auto current = ready.front();
                ready.pop();
                processed++;
                cost[current] += std::max<int64_t>( jobs[current].executionNs, 0 );
                for( const auto next : outgoing[current] )
                {
                    if( cost[next] < cost[current] )
                    {
                        cost[next] = cost[current];
                        parent[next] = current;
                    }
                    if( --indegree[next] == 0 ) ready.push( next );
                }
            }
            size_t endIndex;
            if( params.contains( "ref" ) )
            {
                const auto requested = parseJobRef();
                const auto found = indexById.find( requested );
                if( found == indexById.end() ) throw QueryError( "ENTITY_NOT_FOUND", "Job ref was not found" );
                endIndex = found->second;
            }
            else
            {
                endIndex = size_t( std::distance( cost.begin(), std::max_element( cost.begin(), cost.end() ) ) );
            }
            std::vector<size_t> path;
            for( std::optional<size_t> current = endIndex; current; current = parent[*current] ) path.push_back( *current );
            std::reverse( path.begin(), path.end() );
            json values = json::array();
            for( const auto index : path ) values.push_back( JobJson( *source, jobs[index], false ) );
            return Success( id, {
                { "jobs", std::move( values ) }, { "total_execution_ns", Decimal( cost[endIndex] ) },
                { "has_cycle", processed != jobs.size() }, { "processed_jobs", processed }, { "total_jobs", jobs.size() }
            }, trace );
        }

        if( method == "job.gfx.statistics" )
        {
            const auto dispatches = source->GetGfxDispatches();
            const auto entities = source->GetGfxEntities();
            const auto links = source->GetGfxLinks();
            std::set<uint64_t> dispatchIds;
            std::set<uint64_t> entityIds;
            std::map<uint64_t, std::string> jobRefs;
            json entitiesByKind = json::object();
            json linksByRelation = json::object();
            json sampleDispatchRef = nullptr;
            json sampleEntityRef = nullptr;
            json sampleLinkedJobRef = nullptr;
            for( const auto& dispatch : dispatches )
            {
                dispatchIds.emplace( dispatch.dispatchId );
                if( sampleDispatchRef.is_null() ) sampleDispatchRef = dispatch.ref;
            }
            for( const auto& entity : entities )
            {
                entityIds.emplace( entity.entityId );
                const auto name = GfxEntityKindName( entity.kind );
                entitiesByKind[name] = Decimal( entitiesByKind.contains( name ) ? std::stoull( entitiesByKind[name].get<std::string>() ) + 1 : 1 );
                if( sampleEntityRef.is_null() ) sampleEntityRef = entity.ref;
            }
            for( const auto& job : jobs ) jobRefs.emplace( job.jobId, job.ref );

            uint64_t danglingParents = 0;
            for( const auto& entity : entities )
            {
                if( entity.parentId != 0 && !dispatchIds.contains( entity.parentId ) && !entityIds.contains( entity.parentId ) ) danglingParents++;
            }
            uint64_t danglingSources = 0;
            uint64_t danglingTargets = 0;
            uint64_t capturedExecuteLinks = 0;
            uint64_t uncapturedExecuteLinks = 0;
            for( const auto& link : links )
            {
                const auto relationName = GfxRelationName( link.relation );
                linksByRelation[relationName] = Decimal( linksByRelation.contains( relationName ) ? std::stoull( linksByRelation[relationName].get<std::string>() ) + 1 : 1 );
                const bool sourceKnown = dispatchIds.contains( link.sourceId ) || entityIds.contains( link.sourceId ) || jobRefs.contains( link.sourceId );
                const bool targetKnown = dispatchIds.contains( link.targetId ) || entityIds.contains( link.targetId ) || jobRefs.contains( link.targetId );
                if( !sourceKnown ) danglingSources++;
                if( !targetKnown && link.relation != 5 ) danglingTargets++;
                if( link.relation == 2 )
                {
                    const auto sourceJob = jobRefs.find( link.sourceId );
                    const auto targetJob = jobRefs.find( link.targetId );
                    if( sourceJob != jobRefs.end() || targetJob != jobRefs.end() )
                    {
                        capturedExecuteLinks++;
                        if( sampleLinkedJobRef.is_null() ) sampleLinkedJobRef = sourceJob != jobRefs.end() ? sourceJob->second : targetJob->second;
                    }
                    else
                    {
                        uncapturedExecuteLinks++;
                    }
                }
            }
            return Success( id, {
                { "counts", {
                    { "dispatches", Decimal( dispatches.size() ) }, { "entities", Decimal( entities.size() ) },
                    { "links", Decimal( links.size() ) }, { "jobs", Decimal( jobs.size() ) }
                } },
                { "entities_by_kind", std::move( entitiesByKind ) },
                { "links_by_relation", std::move( linksByRelation ) },
                { "integrity", {
                    { "dangling_parent_entities", Decimal( danglingParents ) },
                    { "dangling_link_sources", Decimal( danglingSources ) },
                    { "dangling_link_targets", Decimal( danglingTargets ) },
                    { "captured_execute_links", Decimal( capturedExecuteLinks ) },
                    { "uncaptured_execute_links", Decimal( uncapturedExecuteLinks ) }
                } },
                { "samples", {
                    { "dispatch_ref", std::move( sampleDispatchRef ) },
                    { "entity_ref", std::move( sampleEntityRef ) },
                    { "linked_job_ref", std::move( sampleLinkedJobRef ) }
                } }
            }, trace );
        }

        uint64_t rootId = 0;
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto rootRef = params["ref"].get<std::string>();
        for( const auto* kind : { "job", "gfx-dispatch", "gfx-entity" } )
        {
            const auto parsed = source->ParseEntityRef( rootRef, kind );
            if( parsed ) { rootId = *parsed; break; }
        }
        if( rootId == 0 ) throw QueryError( "INVALID_PARAMS", "ref must identify a Job, GfxDispatch, or GfxEntity in this trace" );
        const auto dispatches = source->GetGfxDispatches();
        const auto entities = source->GetGfxEntities();
        const auto links = source->GetGfxLinks();
        std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
        for( const auto& link : links )
        {
            adjacency[link.sourceId].push_back( link.targetId );
            adjacency[link.targetId].push_back( link.sourceId );
        }
        for( const auto& entity : entities ) if( entity.parentId != 0 )
        {
            adjacency[entity.entityId].push_back( entity.parentId );
            adjacency[entity.parentId].push_back( entity.entityId );
        }
        const auto maxNodes = size_t( UnsignedParameter( params, "max_nodes", 10000, 100000 ) );
        std::set<uint64_t> visited;
        std::queue<uint64_t> frontier;
        visited.emplace( rootId );
        frontier.push( rootId );
        bool truncated = false;
        while( !frontier.empty() )
        {
            const auto current = frontier.front();
            frontier.pop();
            for( const auto next : adjacency[current] )
            {
                if( visited.size() >= maxNodes ) { truncated = true; break; }
                if( visited.emplace( next ).second ) frontier.push( next );
            }
            if( truncated ) break;
        }
        json dispatchJson = json::array();
        for( const auto& dispatch : dispatches ) if( visited.contains( dispatch.dispatchId ) ) dispatchJson.push_back( GfxDispatchJson( dispatch ) );
        json entityJson = json::array();
        for( const auto& entity : entities ) if( visited.contains( entity.entityId ) ) entityJson.push_back( GfxEntityJson( entity ) );
        json linkJson = json::array();
        for( const auto& link : links ) if( visited.contains( link.sourceId ) && visited.contains( link.targetId ) ) linkJson.push_back( GfxLinkJson( link ) );
        json jobJson = json::array();
        for( const auto& job : jobs ) if( visited.contains( job.jobId ) ) jobJson.push_back( JobJson( *source, job, false ) );
        return Success( id, {
            { "root_ref", rootRef }, { "jobs", std::move( jobJson ) }, { "dispatches", std::move( dispatchJson ) },
            { "entities", std::move( entityJson ) }, { "links", std::move( linkJson ) }, { "visited_nodes", visited.size() }, { "truncated", truncated }
        }, trace );
    }
    if( method == "callstack.resolve" || method == "callstack.frames" || method == "callstack.parent" || method == "callstack.batch" )
    {
        json callstackValues;
        if( method == "callstack.frames" || method == "callstack.parent" )
        {
            if( !params.contains( "callstack" ) ) throw QueryError( "INVALID_PARAMS", "callstack is required" );
            callstackValues = json::array( { params["callstack"] } );
        }
        else
        {
            if( !params.contains( "callstacks" ) || !params["callstacks"].is_array() ) throw QueryError( "INVALID_PARAMS", "callstacks must be an array" );
            callstackValues = params["callstacks"];
        }
        if( callstackValues.size() > MaximumPageSize ) throw QueryError( "RESOURCE_LIMIT", "at most 1000 callstacks may be resolved per request" );
        const size_t maxDepth = params.value( "max_depth", size_t( 32 ) );
        if( maxDepth < 1 || maxDepth > 256 ) throw QueryError( "INVALID_PARAMS", "max_depth must be between 1 and 256" );
        std::vector<uint32_t> callstacks;
        for( const auto& value : callstackValues ) callstacks.emplace_back( parseCallstack( value, method == "callstack.parent" ) );
        const auto resolved = method == "callstack.parent" ? source->ResolveParentCallstacks( callstacks, maxDepth ) : source->ResolveCallstacks( callstacks, maxDepth );
        if( resolved.empty() && !callstacks.empty() ) throw QueryError( "ENTITY_NOT_FOUND", method == "callstack.parent" ? "parent callstack was not found" : "callstack was not found" );
        json frames = json::array();
        for( const auto& value : resolved )
        {
            auto frame = CallstackFrameJson( value );
            frame["callstack_ref"] = source->MakeEntityRef( method == "callstack.parent" ? "parent-callstack" : "callstack", value.callstack );
            frames.emplace_back( std::move( frame ) );
        }
        return Success( id, { { "kind", method == "callstack.parent" ? "parent" : "event" }, { "frames", std::move( frames ) } }, trace );
    }
    if( method == "sample.list" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string threadRef = params.value( "thread_ref", "" );
        const std::string kind = params.value( "kind", "" );
        auto [samples, hasMore] = ScanFiltered<analysis::SampleDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanSampleEvents( range ); },
            [&]( const auto& sample ) { return ( threadRef.empty() || sample.threadRef == threadRef ) && ( kind.empty() || sample.kind == kind ); }, SampleJson );
        const auto cursor = NextCursor( page, method, trace, samples.size(), hasMore );
        return Success( id, { { "samples", std::move( samples ) } }, trace, PageJson( page, samples.size(), cursor ) );
    }
    if( method == "sample.ghost_zones" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string threadRef = params.value( "thread_ref", "" );
        auto [zones, hasMore] = ScanFiltered<analysis::GhostZoneDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanGhostZones( range ); },
            [&]( const auto& zone ) { return ( threadRef.empty() || zone.threadRef == threadRef ) && TextMatches( zone.name + " " + zone.file, params ); }, GhostZoneJson );
        const auto cursor = NextCursor( page, method, trace, zones.size(), hasMore );
        return Success( id, { { "ready", true }, { "ghost_zones", std::move( zones ) } }, trace, PageJson( page, zones.size(), cursor ) );
    }
    if( method == "sample.flamegraph" )
    {
        const std::string direction = params.value( "direction", "top_down" );
        if( direction != "top_down" && direction != "bottom_up" ) throw QueryError( "INVALID_PARAMS", "direction must be top_down or bottom_up" );
        const auto maxDepth = params.value( "max_depth", size_t( 64 ) );
        if( maxDepth < 1 || maxDepth > 256 ) throw QueryError( "INVALID_PARAMS", "max_depth must be between 1 and 256" );
        const std::string threadRef = params.value( "thread_ref", "" );
        const std::string kind = params.value( "kind", "sample" );
        if( kind != "sample" && kind != "context_switch" && kind != "all" ) throw QueryError( "INVALID_PARAMS", "kind must be sample, context_switch, or all" );
        std::unordered_map<uint32_t, uint64_t> counts;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto samples = source->ScanSampleEvents( range );
            for( const auto& sample : samples ) if( sample.callstack != 0 && ( threadRef.empty() || sample.threadRef == threadRef ) && ( kind == "all" || sample.kind == kind ) ) counts[sample.callstack]++;
            offset += samples.size(); if( samples.size() < chunk ) break;
        }
        std::vector<uint32_t> callstacks; callstacks.reserve( counts.size() ); for( const auto& [callstack, count] : counts ) callstacks.emplace_back( callstack );
        std::unordered_map<uint32_t, std::vector<std::string>> paths;
        for( const auto& frame : source->ResolveCallstacks( callstacks, maxDepth ) )
        {
            paths[frame.callstack].emplace_back( frame.name.empty() ? frame.address : frame.name );
        }
        std::vector<std::pair<uint32_t, uint64_t>> order( counts.begin(), counts.end() );
        std::sort( order.begin(), order.end(), []( const auto& lhs, const auto& rhs ) { return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first; } );
        const auto limit = TopN( params ); if( order.size() > limit ) order.resize( limit );
        json output = json::array();
        for( const auto& [callstack, count] : order )
        {
            auto path = paths[callstack]; if( direction == "top_down" ) std::reverse( path.begin(), path.end() );
            output.push_back( { { "callstack", Decimal( uint64_t( callstack ) ) }, { "callstack_ref", source->MakeEntityRef( "callstack", callstack ) }, { "samples", Decimal( count ) }, { "path", std::move( path ) } } );
        }
        return Success( id, { { "direction", direction }, { "sample_kind", kind }, { "paths", std::move( output ) }, { "unique_callstacks", counts.size() } }, trace );
    }
    if( method == "sample.symbol_statistics" || method == "symbol.search" || method == "symbol.get" || method == "symbol.address" || method == "symbol.address_map" )
    {
        if( method == "symbol.address_map" )
        {
            const auto page = ParsePage( params, method, trace );
            auto mappings = source->GetSymbolAddressMappings( page.offset, page.limit + 1 );
            const bool hasMore = mappings.size() > page.limit;
            if( hasMore ) mappings.pop_back();
            json values = json::array();
            for( const auto& mapping : mappings ) values.push_back( {
                { "ref", mapping.ref }, { "address", mapping.address }, { "symbol_ref", mapping.symbolRef },
                { "symbol_address", mapping.symbolAddress }, { "offset_bytes", Decimal( uint64_t( mapping.offset ) ) }, { "inline_mapping", mapping.inlineMapping }
            } );
            const auto returned = mappings.size();
            const auto cursor = NextCursor( page, method, trace, values.size(), hasMore );
            return Success( id, { { "mappings", std::move( values ) } }, trace, PageJson( page, returned, cursor ) );
        }
        auto symbols = source->GetSymbols();
        if( method == "symbol.get" )
        {
            if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto ref = params["ref"].get<std::string>();
            const auto found = std::find_if( symbols.begin(), symbols.end(), [&]( const auto& symbol ) { return symbol.ref == ref; } );
            if( found == symbols.end() ) throw QueryError( "ENTITY_NOT_FOUND", "symbol ref was not found" );
            return Success( id, SymbolJson( *found ), trace );
        }
        if( method == "symbol.address" )
        {
            if( !params.contains( "address" ) || !params["address"].is_string() ) throw QueryError( "INVALID_PARAMS", "address is required as 0x-prefixed hexadecimal string" );
            const auto address = params["address"].get<std::string>();
            const auto mapping = source->ResolveSymbolAddress( HexAddress( address ) );
            if( !mapping ) throw QueryError( "ENTITY_NOT_FOUND", "symbol address was not found" );
            const auto found = std::find_if( symbols.begin(), symbols.end(), [&]( const auto& symbol ) { return symbol.ref == mapping->symbolRef; } );
            if( found == symbols.end() ) throw QueryError( "ENTITY_NOT_FOUND", "resolved symbol metadata was not found" );
            auto data = SymbolJson( *found );
            data["query_address"] = mapping->address;
            data["address_mapping_ref"] = mapping->ref;
            data["offset_bytes"] = Decimal( uint64_t( mapping->offset ) );
            data["inline_mapping"] = mapping->inlineMapping;
            return Success( id, std::move( data ), trace );
        }
        symbols.erase( std::remove_if( symbols.begin(), symbols.end(), [&]( const auto& symbol ) {
            return method == "symbol.search" && !TextMatches( symbol.name + " " + symbol.file, params );
        } ), symbols.end() );
        if( method == "sample.symbol_statistics" )
        {
            symbols.erase( std::remove_if( symbols.begin(), symbols.end(), []( const auto& symbol ) { return symbol.inclusiveSamples == 0 && symbol.exclusiveSamples == 0; } ), symbols.end() );
            std::sort( symbols.begin(), symbols.end(), []( const auto& lhs, const auto& rhs ) {
                const auto left = uint64_t( lhs.inclusiveSamples ) + lhs.exclusiveSamples;
                const auto right = uint64_t( rhs.inclusiveSamples ) + rhs.exclusiveSamples;
                return left != right ? left > right : lhs.ref < rhs.ref;
            } );
            const auto limit = TopN( params );
            if( symbols.size() > limit ) symbols.resize( limit );
            json values = json::array(); for( const auto& symbol : symbols ) values.emplace_back( SymbolJson( symbol ) );
            return Success( id, { { "symbols", std::move( values ) } }, trace );
        }
        const auto page = ParsePage( params, method, trace );
        const size_t begin = std::min( page.offset, symbols.size() );
        const size_t end = std::min( begin + page.limit, symbols.size() );
        json values = json::array(); for( size_t index = begin; index < end; index++ ) values.emplace_back( SymbolJson( symbols[index] ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < symbols.size() );
        return Success( id, { { "symbols", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "symbol.raw_code" || method == "symbol.disassembly" )
    {
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto ref = params["ref"].get<std::string>();
        const auto resources = source->GetSymbolResources();
        const auto found = std::find_if( resources.begin(), resources.end(), [&]( const auto& value ) { return value.ref == ref; } );
        if( found == resources.end() || found->codeBytes == 0 ) throw QueryError( "CAPABILITY_UNAVAILABLE", "symbol has no persisted machine code" );
        const auto maxBytes = size_t( UnsignedParameter( params, "max_bytes", 65536, 1024 * 1024 ) );
        if( maxBytes < 1 || maxBytes > 1024 * 1024 ) throw QueryError( "INVALID_PARAMS", "max_bytes must be between 1 and 1048576" );
        if( method == "symbol.disassembly" )
        {
            const auto maxInstructions = params.value( "max_instructions", size_t( 1000 ) );
            if( maxInstructions < 1 || maxInstructions > 10000 ) throw QueryError( "INVALID_PARAMS", "max_instructions must be between 1 and 10000" );
            const auto instructions = source->DisassembleSymbol( ref, maxBytes, maxInstructions );
            if( instructions.empty() ) throw QueryError( "CAPABILITY_UNAVAILABLE", "trace CPU architecture is unknown or persisted code could not be disassembled" );
            json output = json::array();
            for( const auto& instruction : instructions ) output.push_back( {
                { "ref", instruction.ref }, { "address", instruction.address }, { "bytes", instruction.bytes },
                { "mnemonic", instruction.mnemonic }, { "operands", instruction.operands }, { "size", instruction.size },
                { "trust", "untrusted_trace_data" }
            } );
            return Success( id, {
                { "symbol_ref", found->ref }, { "architecture", info().cpuArchitecture },
                { "persisted_code_bytes", Decimal( found->codeBytes ) }, { "decoded_instructions", std::move( output ) },
                { "truncated", instructions.size() == maxInstructions || found->codeBytes > maxBytes }
            }, trace );
        }
        const auto offset = size_t( UnsignedParameter( params, "offset_bytes", 0, std::numeric_limits<size_t>::max() ) );
        auto data = BinaryChunkJson( source->ReadSymbolCodeBytes( found->id, offset, maxBytes ) );
        data["symbol_ref"] = found->ref;
        data["address"] = "0x" + Hex16( found->id );
        data["representation"] = "base64url machine-code chunk";
        data["resource_uri"] = "tracy://trace/" + trace.id + "/symbol-code/" + Hex16( found->id );
        return Success( id, std::move( data ), trace );
    }
    if( method == "hardware_sample.capabilities" )
    {
        const auto values = source->GetHardwareSamples();
        const bool branch = std::any_of( values.begin(), values.end(), []( const auto& item ) { return item.branchRetired != 0 || item.branchMisses != 0; } );
        return Success( id, { { "present", !values.empty() }, { "address_count", Decimal( values.size() ) }, { "branch_retirement", branch } }, trace );
    }
    if( method == "hardware_sample.counts" || method == "hardware_sample.address" )
    {
        auto values = source->GetHardwareSamples();
        if( method == "hardware_sample.address" )
        {
            if( !params.contains( "address" ) || !params["address"].is_string() ) throw QueryError( "INVALID_PARAMS", "address is required" );
            const auto address = Lower( params["address"].get<std::string>() );
            values.erase( std::remove_if( values.begin(), values.end(), [&]( const auto& item ) { return Lower( item.address ) != address; } ), values.end() );
            if( values.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "hardware sample address was not found" );
        }
        const auto page = ParsePage( params, method, trace );
        const size_t begin = method == "hardware_sample.address" ? 0 : std::min( page.offset, values.size() );
        const size_t end = method == "hardware_sample.address" ? values.size() : std::min( begin + page.limit, values.size() );
        json output = json::array();
        for( size_t index = begin; index < end; index++ )
        {
            const auto& value = values[index];
            output.push_back( {
            { "ref", value.ref }, { "address", value.address }, { "cycles", Decimal( value.cycles ) }, { "retired", Decimal( value.retired ) },
            { "cache_references", Decimal( value.cacheReferences ) }, { "cache_misses", Decimal( value.cacheMisses ) },
            { "branch_retired", Decimal( value.branchRetired ) }, { "branch_misses", Decimal( value.branchMisses ) }
            } );
        }
        if( method == "hardware_sample.address" ) return Success( id, std::move( output.front() ), trace );
        const auto cursor = NextCursor( page, method, trace, end - begin, end < values.size() );
        return Success( id, { { "addresses", std::move( output ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "hardware_sample.events" )
    {
        if( !params.contains( "address" ) || !params["address"].is_string() ) throw QueryError( "INVALID_PARAMS", "address is required" );
        const auto addressText = params["address"].get<std::string>();
        const auto address = HexAddress( addressText );
        const auto summaries = source->GetHardwareSamples();
        if( std::none_of( summaries.begin(), summaries.end(), [&]( const auto& item ) { return Lower( item.address ) == Lower( addressText ); } ) )
        {
            throw QueryError( "ENTITY_NOT_FOUND", "hardware sample address was not found" );
        }
        const auto kind = params.value( "kind", std::string( "all" ) );
        static const std::set<std::string> kinds = { "all", "cycles", "retired", "cache_references", "cache_misses", "branch_retired", "branch_misses" };
        if( !kinds.contains( kind ) ) throw QueryError( "INVALID_PARAMS", "kind is not a supported hardware sample event type" );
        const auto page = ParsePage( params, method, trace );
        auto events = source->GetHardwareSampleEvents( address, kind, page.offset, page.limit + 1 );
        const bool hasMore = events.size() > page.limit;
        if( hasMore ) events.pop_back();
        json output = json::array();
        for( const auto& event : events ) output.push_back( {
            { "ref", event.ref }, { "address", event.address }, { "kind", event.kind },
            { "event_index", event.eventIndex }, { "time_ns", Decimal( event.timeNs ) }
        } );
        const auto cursor = NextCursor( page, method, trace, output.size(), hasMore );
        return Success( id, { { "events", std::move( output ) } }, trace, PageJson( page, events.size(), cursor ) );
    }
    if( method == "source.locations" )
    {
        auto locations = source->GetSourceLocations();
        locations.erase( std::remove_if( locations.begin(), locations.end(), [&]( const auto& value ) {
            return !TextMatches( value.name + " " + value.function + " " + value.file, params );
        } ), locations.end() );
        const auto page = ParsePage( params, method, trace );
        const size_t begin = std::min( page.offset, locations.size() );
        const size_t end = std::min( begin + page.limit, locations.size() );
        json values = json::array(); for( size_t index = begin; index < end; index++ ) values.emplace_back( SourceLocationJson( locations[index] ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < locations.size() );
        return Success( id, { { "source_locations", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "source.statistics" )
    {
        struct SourceStats { uint64_t cpuCount = 0, gpuCount = 0; int64_t cpuInclusive = 0, cpuSelf = 0, cpuRunning = 0, gpuInclusive = 0, gpuSelf = 0; };
        std::map<std::string, SourceStats> groups;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto zones = source->ScanCpuZones( range );
            for( const auto& zone : zones ) if( zone.endNs )
            {
                auto& stats = groups[zone.sourceLocationRef]; stats.cpuCount++; stats.cpuInclusive += *zone.endNs - zone.startNs;
                if( zone.selfTimeNs ) stats.cpuSelf += *zone.selfTimeNs; if( zone.runningTimeNs ) stats.cpuRunning += *zone.runningTimeNs;
            }
            offset += zones.size(); if( zones.size() < chunk ) break;
        }
        offset = 0;
        while( true )
        {
            checkCancelled();
            auto range = ScanRangeFrom( params, offset, chunk );
            const auto zones = source->ScanGpuZones( range );
            for( const auto& zone : zones ) if( zone.gpuEndNs )
            {
                auto& stats = groups[zone.sourceLocationRef]; stats.gpuCount++; stats.gpuInclusive += *zone.gpuEndNs - zone.gpuStartNs;
                if( zone.selfTimeNs ) stats.gpuSelf += *zone.selfTimeNs;
            }
            offset += zones.size(); if( zones.size() < chunk ) break;
        }
        const auto locations = source->GetSourceLocations();
        std::unordered_map<std::string, analysis::SourceLocationDto> metadata;
        for( const auto& location : locations ) metadata.emplace( location.ref, location );
        json values = json::array();
        for( const auto& [ref, stats] : groups )
        {
            json item = {
                { "source_location_ref", ref }, { "cpu_count", Decimal( stats.cpuCount ) }, { "gpu_count", Decimal( stats.gpuCount ) },
                { "cpu_inclusive_ns", Decimal( stats.cpuInclusive ) }, { "cpu_self_ns", Decimal( stats.cpuSelf ) },
                { "cpu_running_ns", Decimal( stats.cpuRunning ) }, { "gpu_inclusive_ns", Decimal( stats.gpuInclusive ) }, { "gpu_self_ns", Decimal( stats.gpuSelf ) }
            };
            const auto found = metadata.find( ref ); if( found != metadata.end() ) item["source"] = SourceLocationJson( found->second );
            values.emplace_back( std::move( item ) );
        }
        std::sort( values.begin(), values.end(), []( const auto& lhs, const auto& rhs ) {
            const auto left = std::stoll( lhs["cpu_inclusive_ns"].template get<std::string>() ) + std::stoll( lhs["gpu_inclusive_ns"].template get<std::string>() );
            const auto right = std::stoll( rhs["cpu_inclusive_ns"].template get<std::string>() ) + std::stoll( rhs["gpu_inclusive_ns"].template get<std::string>() );
            return left != right ? left > right : lhs["source_location_ref"].template get<std::string>() < rhs["source_location_ref"].template get<std::string>();
        } );
        const auto limit = TopN( params ); if( values.size() > limit ) values.erase( values.begin() + limit, values.end() );
        return Success( id, { { "locations", std::move( values ) }, { "group_count", groups.size() } }, trace );
    }
    if( method == "source.embedded" )
    {
        const auto page = ParsePage( params, method, trace );
        const auto resources = source->GetSourceResources();
        const size_t begin = std::min( page.offset, resources.size() );
        const size_t end = std::min( begin + page.limit, resources.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.push_back( {
            { "ref", resources[index].ref }, { "path", resources[index].path }, { "bytes", Decimal( resources[index].bytes ) },
            { "path_base64url", Base64UrlEncode( resources[index].pathBytes ) },
            { "resource_uri", "tracy://trace/" + trace.id + "/source/" + std::to_string( resources[index].id ) }, { "trust", "untrusted_trace_data" }
        } );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < resources.size() );
        return Success( id, { { "files", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
    }
    if( method == "source.raw" )
    {
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto ref = params["ref"].get<std::string>();
        const auto resources = source->GetSourceResources();
        auto found = std::find_if( resources.begin(), resources.end(), [&]( const auto& value ) { return value.ref == ref; } );
        if( found == resources.end() )
        {
            const auto locations = source->GetSourceLocations();
            const auto location = std::find_if( locations.begin(), locations.end(), [&]( const auto& value ) { return value.ref == ref; } );
            if( location != locations.end() ) found = std::find_if( resources.begin(), resources.end(), [&]( const auto& value ) { return value.path == location->file; } );
        }
        if( found == resources.end() ) throw QueryError( "CAPABILITY_UNAVAILABLE", "source is not embedded in the trace" );
        const auto offset = size_t( UnsignedParameter( params, "offset_bytes", 0, std::numeric_limits<size_t>::max() ) );
        const auto maxBytes = size_t( UnsignedParameter( params, "max_bytes", 65536, 1024 * 1024 ) );
        if( maxBytes == 0 ) throw QueryError( "INVALID_PARAMS", "max_bytes must be between 1 and 1048576" );
        auto data = BinaryChunkJson( source->ReadEmbeddedSourceBytes( found->id, offset, maxBytes ) );
        data["path"] = found->path;
        data["path_base64url"] = Base64UrlEncode( found->pathBytes );
        data["representation"] = "base64url source byte chunk";
        return Success( id, std::move( data ), trace );
    }
    if( method == "source.lines" )
    {
        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto ref = params["ref"].get<std::string>();
        const auto maxBytes = params.value( "max_bytes", size_t( 65536 ) );
        if( maxBytes < 1 || maxBytes > 1024 * 1024 ) throw QueryError( "INVALID_PARAMS", "max_bytes must be between 1 and 1048576" );
        const auto startLine = params.value( "start_line", size_t( 1 ) );
        const auto lineCount = params.value( "line_count", size_t( 200 ) );
        if( startLine < 1 || lineCount < 1 || lineCount > 5000 ) throw QueryError( "INVALID_PARAMS", "start_line and line_count are out of range" );
        const auto resources = source->GetSourceResources();
        auto found = std::find_if( resources.begin(), resources.end(), [&]( const auto& value ) { return value.ref == ref; } );
        if( found == resources.end() )
        {
            const auto locations = source->GetSourceLocations();
            const auto location = std::find_if( locations.begin(), locations.end(), [&]( const auto& value ) { return value.ref == ref; } );
            if( location != locations.end() ) found = std::find_if( resources.begin(), resources.end(), [&]( const auto& value ) { return value.path == location->file; } );
        }
        if( found == resources.end() ) throw QueryError( "CAPABILITY_UNAVAILABLE", "source is not embedded in the trace" );
        const auto text = source->ReadEmbeddedSource( found->id, maxBytes );
        std::istringstream stream( text.text );
        json lines = json::array();
        std::string line;
        size_t current = 1;
        while( std::getline( stream, line ) )
        {
            if( current >= startLine && lines.size() < lineCount ) lines.push_back( { { "line", current }, { "text", line }, { "trust", "untrusted_trace_data" } } );
            if( lines.size() >= lineCount ) break;
            current++;
        }
        return Success( id, { { "ref", found->ref }, { "path", found->path }, { "lines", std::move( lines ) }, { "truncated", text.truncated || !stream.eof() } }, trace );
    }
    if( method == "timeline.slice" )
    {
        const auto page = ParsePage( params, method, trace );
        auto range = ScanRangeFrom( params, page.offset, page.limit + 1 );
        std::set<std::string> tracks;
        if( params.contains( "tracks" ) )
        {
            if( !params["tracks"].is_array() ) throw QueryError( "INVALID_PARAMS", "tracks must be an array" );
            for( const auto& value : params["tracks"] ) { if( !value.is_string() ) throw QueryError( "INVALID_PARAMS", "tracks entries must be strings" ); tracks.emplace( value.get<std::string>() ); }
        }
        const auto wanted = [&]( const char* track ) { return tracks.empty() || tracks.find( track ) != tracks.end(); };
        static const std::set<std::string> validTracks = { "cpu_zones", "gpu_zones", "frames", "context_switches", "lock_events", "plot_points", "messages" };
        for( const auto& track : tracks ) if( validTracks.find( track ) == validTracks.end() ) throw QueryError( "INVALID_PARAMS", "unknown timeline track: " + track );
        const auto resolution = params.value( "resolution_ns", int64_t( 0 ) );
        if( resolution < 0 ) throw QueryError( "INVALID_PARAMS", "resolution_ns must be non-negative" );
        bool hasMore = false;
        size_t returned = 0;
        const auto cap = [&]( auto& values ) { if( values.size() > page.limit ) { values.resize( page.limit ); hasMore = true; } returned = std::max( returned, values.size() ); };
        json cpu = json::array();
        if( wanted( "cpu_zones" ) ) { auto values = source->ScanCpuZones( range ); cap( values ); for( const auto& value : values ) cpu.emplace_back( CpuZoneJson( value ) ); }
        json gpu = json::array();
        if( wanted( "gpu_zones" ) ) { auto values = source->ScanGpuZones( range ); cap( values ); for( const auto& value : values ) gpu.emplace_back( GpuZoneJson( value ) ); }
        json frames = json::array();
        if( wanted( "frames" ) ) { auto values = source->ScanFrames( range ); cap( values ); for( const auto& value : values ) frames.emplace_back( FrameJson( value ) ); }
        json contextSwitches = json::array();
        if( wanted( "context_switches" ) ) { auto values = source->ScanContextSwitchEvents( range ); cap( values ); for( const auto& value : values ) contextSwitches.emplace_back( ContextSwitchJson( value ) ); }
        json locks = json::array();
        if( wanted( "lock_events" ) ) { auto values = source->ScanLockEvents( range ); cap( values ); for( const auto& value : values ) locks.emplace_back( LockEventJson( value ) ); }
        json plots = json::array();
        if( wanted( "plot_points" ) )
        {
            auto values = source->ScanPlots( range ); cap( values );
            if( resolution == 0 ) for( const auto& value : values ) plots.push_back( { { "ref", value.ref }, { "plot_ref", value.plotRef }, { "time_ns", Decimal( value.timeNs ) }, { "value", value.value } } );
            else
            {
                struct Bucket { int64_t begin = 0; double min = 0, max = 0, sum = 0; size_t count = 0; };
                std::map<std::pair<std::string, int64_t>, Bucket> buckets;
                for( const auto& value : values )
                {
                    const auto begin = value.timeNs - value.timeNs % resolution;
                    auto& bucket = buckets[{ value.plotRef, begin }];
                    if( bucket.count == 0 ) { bucket.begin = begin; bucket.min = bucket.max = value.value; }
                    bucket.min = std::min( bucket.min, value.value ); bucket.max = std::max( bucket.max, value.value ); bucket.sum += value.value; bucket.count++;
                }
                for( const auto& [key, bucket] : buckets ) plots.push_back( { { "plot_ref", key.first }, { "begin_ns", Decimal( bucket.begin ) }, { "end_ns", Decimal( bucket.begin + resolution ) },
                    { "count", bucket.count }, { "min", bucket.min }, { "max", bucket.max }, { "mean", bucket.sum / bucket.count } } );
            }
        }
        json messages = json::array();
        if( wanted( "messages" ) ) { auto values = source->ScanMessages( range ); cap( values ); for( const auto& value : values ) messages.emplace_back( MessageJson( value ) ); }
        const auto cursor = NextCursor( page, method, trace, hasMore ? page.limit : returned, hasMore );
        return Success( id, {
            { "range", { { "start_ns", Decimal( range.startNs ) }, { "end_ns", Decimal( range.endNs ) } } },
            { "resolution_ns", Decimal( resolution ) }, { "pagination", "cursor offset is applied independently to each selected track" },
            { "cpu_zones", std::move( cpu ) }, { "gpu_zones", std::move( gpu ) }, { "frames", std::move( frames ) },
            { "context_switches", std::move( contextSwitches ) }, { "lock_events", std::move( locks ) },
            { "plot_points", std::move( plots ) }, { "messages", std::move( messages ) }
        }, trace, PageJson( page, returned, cursor ) );
    }
    if( method == "statistics.describe" )
    {
        return Success( id, { { "fields", { "count", "total", "min", "max", "mean", "median", "stddev", "p50", "p90", "p95", "p99", "truncated_mean" } }, { "percentile_interpolation", "linear between adjacent sorted samples" }, { "stddev", "population" } }, trace );
    }
    if( method == "statistics.compute" )
    {
        if( !params.contains( "values_ns" ) || !params["values_ns"].is_array() ) throw QueryError( "INVALID_PARAMS", "values_ns must be an array of decimal strings" );
        if( params["values_ns"].size() > 100000 ) throw QueryError( "RESOURCE_LIMIT", "values_ns is limited to 100000 entries" );
        std::vector<int64_t> values;
        values.reserve( params["values_ns"].size() );
        for( const auto& value : params["values_ns"] )
        {
            try { values.emplace_back( value.is_string() ? std::stoll( value.get<std::string>() ) : value.get<int64_t>() ); }
            catch( const std::exception& ) { throw QueryError( "INVALID_PARAMS", "values_ns entries must be signed decimal strings" ); }
        }
        return Success( id, { { "statistics", StatisticsJson( analysis::ComputeStatistics( values, params.value( "truncate_percentile", 0.90 ) ) ) } }, trace );
    }
    if( method == "compare.zones" || method == "compare.frames" || method == "compare.source" )
    {
        if( !params.contains( "baseline_trace_id" ) || !params["baseline_trace_id"].is_string() ) throw QueryError( "INVALID_PARAMS", "baseline_trace_id is required" );
        const auto baselineId = params["baseline_trace_id"].get<std::string>();
        if( baselineId == trace.id ) throw QueryError( "INVALID_PARAMS", "baseline and candidate trace sessions must be different" );
        const auto baselineTrace = m_sessions.Status( baselineId );
        const auto baseline = m_sessions.GetReadySource( baselineId );
        const auto limit = TopN( params );

        const json tracePair = {
            { "baseline", { { "trace_id", baselineTrace.id }, { "fingerprint", baselineTrace.fingerprint } } },
            { "candidate", { { "trace_id", trace.id }, { "fingerprint", trace.fingerprint } } }
        };

        if( method == "compare.zones" )
        {
            struct ZoneAggregate
            {
                std::string name;
                std::string function;
                std::string file;
                uint32_t line = 0;
                std::vector<int64_t> inclusive;
                std::vector<int64_t> self;
                std::vector<int64_t> running;
            };
            using ZoneMap = std::map<std::string, ZoneAggregate>;
            const auto domain = params.value( "zone_domain", "cpu" );
            if( domain != "cpu" && domain != "gpu" ) throw QueryError( "INVALID_PARAMS", "zone_domain must be cpu or gpu" );
            const auto collect = [&]( const std::shared_ptr<analysis::TraceSource>& item ) {
                ZoneMap groups;
                size_t offset = 0;
                constexpr size_t chunk = 4096;
                while( true )
                {
                    checkCancelled();
                    auto range = ScanRangeFrom( params, offset, chunk );
                    size_t count = 0;
                    if( domain == "gpu" )
                    {
                        const auto zones = item->ScanGpuZones( range );
                        count = zones.size();
                        for( const auto& zone : zones ) if( zone.gpuEndNs && TextMatches( zone.name + " " + zone.function + " " + zone.file, params ) )
                        {
                            const auto key = NormalizeSourceKey( zone.file ) + ':' + std::to_string( zone.line ) + '|' + zone.name + '|' + zone.function;
                            auto& group = groups[key]; group.name = zone.name; group.function = zone.function; group.file = zone.file; group.line = zone.line;
                            group.inclusive.emplace_back( *zone.gpuEndNs - zone.gpuStartNs );
                            if( zone.selfTimeNs ) group.self.emplace_back( *zone.selfTimeNs );
                        }
                    }
                    else
                    {
                        const auto zones = item->ScanCpuZones( range );
                        count = zones.size();
                        for( const auto& zone : zones ) if( zone.endNs && TextMatches( zone.name + " " + zone.function + " " + zone.file, params ) )
                        {
                            const auto key = NormalizeSourceKey( zone.file ) + ':' + std::to_string( zone.line ) + '|' + zone.name + '|' + zone.function;
                            auto& group = groups[key]; group.name = zone.name; group.function = zone.function; group.file = zone.file; group.line = zone.line;
                            group.inclusive.emplace_back( *zone.endNs - zone.startNs );
                            if( zone.selfTimeNs ) group.self.emplace_back( *zone.selfTimeNs );
                            if( zone.runningTimeNs ) group.running.emplace_back( *zone.runningTimeNs );
                        }
                    }
                    offset += count;
                    if( count < chunk ) break;
                }
                return groups;
            };

            auto before = collect( baseline );
            auto after = collect( source );
            std::set<std::string> keys;
            for( const auto& [key, value] : before ) keys.emplace( key );
            for( const auto& [key, value] : after ) keys.emplace( key );
            struct ZoneComparison { std::string key; double magnitude = 0; json value; };
            std::vector<ZoneComparison> comparisons;
            comparisons.reserve( keys.size() );
            for( const auto& key : keys )
            {
                const auto left = before.find( key );
                const auto right = after.find( key );
                const auto leftStats = analysis::ComputeStatistics( left == before.end() ? std::vector<int64_t>{} : left->second.inclusive );
                const auto rightStats = analysis::ComputeStatistics( right == after.end() ? std::vector<int64_t>{} : right->second.inclusive );
                const auto& metadata = right != after.end() ? right->second : left->second;
                const double deltaMean = rightStats.mean - leftStats.mean;
                const double deltaP95 = rightStats.p95 - leftStats.p95;
                const auto ratio = leftStats.mean == 0 ? json( nullptr ) : json( rightStats.mean / leftStats.mean );
                const auto leftSelf = analysis::ComputeStatistics( left == before.end() ? std::vector<int64_t>{} : left->second.self );
                const auto rightSelf = analysis::ComputeStatistics( right == after.end() ? std::vector<int64_t>{} : right->second.self );
                const auto leftRunning = analysis::ComputeStatistics( left == before.end() ? std::vector<int64_t>{} : left->second.running );
                const auto rightRunning = analysis::ComputeStatistics( right == after.end() ? std::vector<int64_t>{} : right->second.running );
                comparisons.push_back( { key, std::max( std::abs( deltaMean ), std::abs( deltaP95 ) ), {
                    { "match_key", key }, { "name", metadata.name }, { "function", metadata.function }, { "file", metadata.file }, { "line", metadata.line },
                    { "presence", left == before.end() ? "candidate_only" : right == after.end() ? "baseline_only" : "both" },
                    { "baseline", { { "inclusive", StatisticsJson( leftStats ) }, { "self", StatisticsJson( leftSelf ) }, { "running", domain == "cpu" ? json( StatisticsJson( leftRunning ) ) : json( nullptr ) } } },
                    { "candidate", { { "inclusive", StatisticsJson( rightStats ) }, { "self", StatisticsJson( rightSelf ) }, { "running", domain == "cpu" ? json( StatisticsJson( rightRunning ) ) : json( nullptr ) } } },
                    { "delta", { { "mean_ns", deltaMean }, { "p95_ns", deltaP95 }, { "total_ns", Decimal( rightStats.total - leftStats.total ) }, { "mean_ratio", ratio } } }
                } } );
            }
            std::sort( comparisons.begin(), comparisons.end(), []( const auto& lhs, const auto& rhs ) { return lhs.magnitude != rhs.magnitude ? lhs.magnitude > rhs.magnitude : lhs.key < rhs.key; } );
            if( comparisons.size() > limit ) comparisons.resize( limit );
            json output = json::array(); for( auto& comparison : comparisons ) output.emplace_back( std::move( comparison.value ) );
            return Success( id, { { "traces", tracePair }, { "zone_domain", domain }, { "matched_group_count", keys.size() }, { "groups", std::move( output ) } }, trace );
        }

        if( method == "compare.frames" )
        {
            struct FrameComparison { std::string key; double magnitude = 0; json value; };
            std::map<std::string, std::pair<analysis::FrameSetDto, analysis::Statistics>> before;
            std::map<std::string, std::pair<analysis::FrameSetDto, analysis::Statistics>> after;
            for( const auto& set : baseline->GetFrameSets() ) before[NormalizeSourceKey( set.name )] = { set, analysis::ComputeStatistics( baseline->GetFrameDurations( set.index ) ) };
            for( const auto& set : source->GetFrameSets() ) after[NormalizeSourceKey( set.name )] = { set, analysis::ComputeStatistics( source->GetFrameDurations( set.index ) ) };
            std::set<std::string> keys; for( const auto& [key, value] : before ) keys.emplace( key ); for( const auto& [key, value] : after ) keys.emplace( key );
            std::vector<FrameComparison> comparisons;
            for( const auto& key : keys )
            {
                const auto left = before.find( key ); const auto right = after.find( key );
                const analysis::Statistics empty;
                const auto& leftStats = left == before.end() ? empty : left->second.second;
                const auto& rightStats = right == after.end() ? empty : right->second.second;
                const auto& metadata = right != after.end() ? right->second.first : left->second.first;
                const double meanDelta = rightStats.mean - leftStats.mean;
                const double p95Delta = rightStats.p95 - leftStats.p95;
                comparisons.push_back( { key, std::max( std::abs( meanDelta ), std::abs( p95Delta ) ), {
                    { "match_key", key }, { "name", metadata.name }, { "presence", left == before.end() ? "candidate_only" : right == after.end() ? "baseline_only" : "both" },
                    { "baseline", StatisticsJson( leftStats ) }, { "candidate", StatisticsJson( rightStats ) },
                    { "delta", { { "count", Decimal( int64_t( rightStats.count ) - int64_t( leftStats.count ) ) }, { "mean_ns", meanDelta },
                        { "p95_ns", p95Delta }, { "p99_ns", rightStats.p99 - leftStats.p99 },
                        { "mean_ratio", leftStats.mean == 0 ? json( nullptr ) : json( rightStats.mean / leftStats.mean ) } } }
                } } );
            }
            std::sort( comparisons.begin(), comparisons.end(), []( const auto& lhs, const auto& rhs ) { return lhs.magnitude != rhs.magnitude ? lhs.magnitude > rhs.magnitude : lhs.key < rhs.key; } );
            if( comparisons.size() > limit ) comparisons.resize( limit );
            json output = json::array(); for( auto& comparison : comparisons ) output.emplace_back( std::move( comparison.value ) );
            return Success( id, { { "traces", tracePair }, { "matched_frame_set_count", keys.size() }, { "frame_sets", std::move( output ) } }, trace );
        }

        const auto maxBytes = params.value( "max_bytes", size_t( 65536 ) );
        if( maxBytes < 1 || maxBytes > 1024 * 1024 ) throw QueryError( "INVALID_PARAMS", "max_bytes must be between 1 and 1048576" );
        const auto pathFilter = params.value( "path", "" );
        std::map<std::string, analysis::SourceResourceDto> before;
        std::map<std::string, analysis::SourceResourceDto> after;
        for( const auto& resource : baseline->GetSourceResources() ) before.emplace( NormalizeSourceKey( resource.path ), resource );
        for( const auto& resource : source->GetSourceResources() ) after.emplace( NormalizeSourceKey( resource.path ), resource );
        json baselineOnly = json::array(), candidateOnly = json::array(), changed = json::array(), inconclusive = json::array();
        for( const auto& [key, resource] : before )
        {
            if( !pathFilter.empty() && key.find( NormalizeSourceKey( pathFilter ) ) == std::string::npos ) continue;
            const auto candidate = after.find( key );
            if( candidate == after.end() ) { if( baselineOnly.size() < limit ) baselineOnly.emplace_back( resource.path ); continue; }
            const auto left = baseline->ReadEmbeddedSource( resource.id, maxBytes );
            const auto right = source->ReadEmbeddedSource( candidate->second.id, maxBytes );
            if( left.text == right.text )
            {
                if( resource.bytes != candidate->second.bytes )
                {
                    if( changed.size() < limit ) changed.push_back( {
                        { "path", resource.path }, { "unified_diff", "" }, { "truncated", left.truncated || right.truncated },
                        { "reason", "byte_size_changed" }, { "baseline_bytes", Decimal( resource.bytes ) },
                        { "candidate_bytes", Decimal( candidate->second.bytes ) }, { "trust", "untrusted_trace_data" }
                    } );
                    continue;
                }
                if( left.truncated || right.truncated )
                {
                    if( inconclusive.size() < limit ) inconclusive.push_back( {
                        { "path", resource.path }, { "reason", "bounded_prefix_equal" },
                        { "compared_bytes", Decimal( std::min( left.text.size(), right.text.size() ) ) },
                        { "total_bytes", Decimal( resource.bytes ) }, { "trust", "untrusted_trace_data" }
                    } );
                    continue;
                }
                continue;
            }
            auto leftLines = TextLines( left.text ); auto rightLines = TextLines( right.text );
            dtl::Diff<std::string, std::vector<std::string>> diff( leftLines, rightLines );
            diff.compose(); diff.composeUnifiedHunks();
            std::ostringstream formatted; diff.printUnifiedFormat( formatted );
            auto unified = formatted.str();
            constexpr size_t DiffBudget = 256 * 1024;
            const bool truncated = unified.size() > DiffBudget || left.truncated || right.truncated;
            if( unified.size() > DiffBudget ) unified.resize( DiffBudget );
            if( changed.size() < limit ) changed.push_back( { { "path", resource.path }, { "unified_diff", std::move( unified ) }, { "truncated", truncated }, { "trust", "untrusted_trace_data" } } );
        }
        for( const auto& [key, resource] : after )
        {
            if( !pathFilter.empty() && key.find( NormalizeSourceKey( pathFilter ) ) == std::string::npos ) continue;
            if( before.find( key ) == before.end() && candidateOnly.size() < limit ) candidateOnly.emplace_back( resource.path );
        }
        return Success( id, {
            { "traces", tracePair }, { "baseline_only", std::move( baselineOnly ) }, { "candidate_only", std::move( candidateOnly ) },
            { "changed", std::move( changed ) }, { "inconclusive", std::move( inconclusive ) }, { "bounded", true }
        }, trace );
    }
    if( method == "validation.run" )
    {
        json findings = json::array();
        const auto metadata = info();
        const auto addFinding = [&]( const char* severity, const char* code, std::string message, uint64_t count, json refs = json::array() ) {
            findings.push_back( { { "severity", severity }, { "code", code }, { "message", std::move( message ) },
                { "count", Decimal( count ) }, { "refs", std::move( refs ) } } );
        };
        const auto addRef = []( json& refs, const std::string& ref ) { if( refs.size() < 20 ) refs.emplace_back( ref ); };
        struct ReferenceFinding { uint64_t count = 0; json refs = json::array(); };
        std::map<std::string, ReferenceFinding> referenceFindings;
        const auto noteReference = [&]( const char* kind, const std::string& ownerRef ) {
            auto& finding = referenceFindings[kind]; finding.count++; addRef( finding.refs, ownerRef );
        };
        std::set<std::string> threadRefs, sourceRefs, frameSetRefs, frameImageRefs, memoryPoolRefs, gpuContextRefs, lockRefs, symbolRefs;
        for( const auto& value : source->GetThreads() ) threadRefs.emplace( value.ref );
        for( const auto& value : source->GetSourceLocations() ) sourceRefs.emplace( value.ref );
        for( const auto& value : source->GetFrameSets() ) frameSetRefs.emplace( value.ref );
        for( const auto& value : source->GetFrameImageResources() ) frameImageRefs.emplace( value.ref );
        for( const auto& value : source->GetMemoryPools() ) memoryPoolRefs.emplace( value.ref );
        for( const auto& value : source->GetGpuContexts() ) gpuContextRefs.emplace( value.ref );
        for( const auto& value : source->GetLocks() ) lockRefs.emplace( value.ref );
        for( const auto& value : source->GetSymbols() ) symbolRefs.emplace( value.ref );
        std::set<std::string> cpuZoneRefs, gpuZoneRefs;
        std::vector<std::pair<std::string, std::string>> cpuParents, gpuParents;
        std::set<uint32_t> referencedCallstacks;
        size_t incompleteCpu = 0, invalidCpu = 0, unresolvedCpuNames = 0;
        size_t incompleteGpu = 0, invalidGpu = 0;
        json incompleteCpuRefs = json::array(), invalidCpuRefs = json::array(), unresolvedCpuNameRefs = json::array(), incompleteGpuRefs = json::array(), invalidGpuRefs = json::array();
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanCpuZones( range );
            for( const auto& value : values )
            {
                cpuZoneRefs.emplace( value.ref );
                if( value.parentRef ) cpuParents.emplace_back( value.ref, *value.parentRef );
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( !value.sourceLocationRef.empty() && sourceRefs.find( value.sourceLocationRef ) == sourceRefs.end() ) noteReference( "SOURCE_LOCATION", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
                if( !value.nameResolved ) { unresolvedCpuNames++; addRef( unresolvedCpuNameRefs, value.ref ); }
                if( !value.complete ) { incompleteCpu++; addRef( incompleteCpuRefs, value.ref ); }
                if( value.endNs && *value.endNs < value.startNs ) { invalidCpu++; addRef( invalidCpuRefs, value.ref ); }
            }
            offset += values.size();
            if( values.size() < chunk ) break;
        }
        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanGpuZones( range );
            for( const auto& value : values )
            {
                gpuZoneRefs.emplace( value.ref );
                if( value.parentRef ) gpuParents.emplace_back( value.ref, *value.parentRef );
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( !value.contextRef.empty() && gpuContextRefs.find( value.contextRef ) == gpuContextRefs.end() ) noteReference( "GPU_CONTEXT", value.ref );
                if( !value.sourceLocationRef.empty() && sourceRefs.find( value.sourceLocationRef ) == sourceRefs.end() ) noteReference( "SOURCE_LOCATION", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
                if( !value.complete ) { incompleteGpu++; addRef( incompleteGpuRefs, value.ref ); }
                if( ( value.gpuEndNs && *value.gpuEndNs < value.gpuStartNs ) || ( value.cpuEndNs && *value.cpuEndNs < value.cpuStartNs ) ) { invalidGpu++; addRef( invalidGpuRefs, value.ref ); }
            }
            offset += values.size();
            if( values.size() < chunk ) break;
        }
        if( incompleteCpu ) addFinding( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", incompleteCpu, std::move( incompleteCpuRefs ) );
        if( invalidCpu ) addFinding( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", invalidCpu, std::move( invalidCpuRefs ) );
        if( unresolvedCpuNames ) addFinding( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", unresolvedCpuNames, std::move( unresolvedCpuNameRefs ) );
        if( incompleteGpu ) addFinding( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", incompleteGpu, std::move( incompleteGpuRefs ) );
        if( invalidGpu ) addFinding( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", invalidGpu, std::move( invalidGpuRefs ) );
        for( const auto& [owner, parent] : cpuParents ) if( cpuZoneRefs.find( parent ) == cpuZoneRefs.end() ) noteReference( "CPU_ZONE_PARENT", owner );
        for( const auto& [owner, parent] : gpuParents ) if( gpuZoneRefs.find( parent ) == gpuZoneRefs.end() ) noteReference( "GPU_ZONE_PARENT", owner );

        size_t incompleteFrames = 0, invalidFrames = 0;
        json incompleteFrameRefs = json::array(), invalidFrameRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanFrames( range );
            for( const auto& value : values )
            {
                if( !value.frameSetRef.empty() && frameSetRefs.find( value.frameSetRef ) == frameSetRefs.end() ) noteReference( "FRAME_SET", value.ref );
                if( value.imageRef && frameImageRefs.find( *value.imageRef ) == frameImageRefs.end() ) noteReference( "FRAME_IMAGE", value.ref );
                if( !value.complete ) { incompleteFrames++; addRef( incompleteFrameRefs, value.ref ); }
                if( value.endNs && *value.endNs < value.beginNs ) { invalidFrames++; addRef( invalidFrameRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }
        if( incompleteFrames ) addFinding( "info", "INCOMPLETE_FRAMES", "frame sets contain an open final frame", incompleteFrames, std::move( incompleteFrameRefs ) );
        if( invalidFrames ) addFinding( "error", "INVALID_FRAME_TIMING", "frames end before they begin", invalidFrames, std::move( invalidFrameRefs ) );

        size_t invalidMemory = 0, unresolvedMemoryCallstacks = 0;
        json invalidMemoryRefs = json::array(), memoryCallstackRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanMemoryEvents( range );
            for( const auto& value : values )
            {
                if( !value.poolRef.empty() && memoryPoolRefs.find( value.poolRef ) == memoryPoolRefs.end() ) noteReference( "MEMORY_POOL", value.ref );
                if( !value.allocationThreadRef.empty() && threadRefs.find( value.allocationThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.freeThreadRef && threadRefs.find( *value.freeThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.allocationZoneRef && cpuZoneRefs.find( *value.allocationZoneRef ) == cpuZoneRefs.end() ) noteReference( "CPU_ZONE", value.ref );
                if( value.freeZoneRef && cpuZoneRefs.find( *value.freeZoneRef ) == cpuZoneRefs.end() ) noteReference( "CPU_ZONE", value.ref );
                if( value.allocationCallstack != 0 ) referencedCallstacks.emplace( value.allocationCallstack );
                if( value.freeCallstack != 0 ) referencedCallstacks.emplace( value.freeCallstack );
                if( value.freeNs && *value.freeNs < value.allocationNs ) { invalidMemory++; addRef( invalidMemoryRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }
        if( invalidMemory ) addFinding( "error", "INVALID_MEMORY_LIFETIME", "memory events are freed before allocation", invalidMemory, std::move( invalidMemoryRefs ) );
        size_t invalidContextSwitches = 0, invalidWakeups = 0;
        json invalidContextRefs = json::array(), invalidWakeupRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanContextSwitchEvents( range );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.endNs && *value.endNs < value.startNs ) { invalidContextSwitches++; addRef( invalidContextRefs, value.ref ); }
                if( value.wakeupNs && *value.wakeupNs > value.startNs ) { invalidWakeups++; addRef( invalidWakeupRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }
        if( invalidContextSwitches ) addFinding( "error", "INVALID_CONTEXT_SWITCH_TIMING", "context-switch running intervals are reversed", invalidContextSwitches, std::move( invalidContextRefs ) );
        if( invalidWakeups ) addFinding( "warning", "INVALID_WAKEUP_ORDER", "thread wakeup occurs after its running interval begins", invalidWakeups, std::move( invalidWakeupRefs ) );

        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanSampleEvents( range );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }
        if( metadata.samplesInconsistent ) addFinding( "warning", "INCONSISTENT_SAMPLES", "sampling data was marked inconsistent by Worker", 1 );

        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanMessages( range );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }
        offset = 0;
        while( true )
        {
            checkCancelled();
            analysis::ScanRange range; range.offset = offset; range.limit = chunk;
            const auto values = source->ScanLockEvents( range );
            for( const auto& value : values )
            {
                if( !value.lockRef.empty() && lockRefs.find( value.lockRef ) == lockRefs.end() ) noteReference( "LOCK", value.ref );
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.ownerThreadRef && threadRefs.find( *value.ownerThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                for( const auto& waiter : value.waiterThreadRefs ) if( threadRefs.find( waiter ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
            }
            offset += values.size(); if( values.size() < chunk ) break;
        }

        if( metadata.hasCrash )
        {
            const auto crash = source->GetCrash();
            if( !crash.threadRef.empty() && threadRefs.find( crash.threadRef ) == threadRefs.end() ) noteReference( "THREAD", crash.threadRef );
            if( crash.callstack != 0 ) referencedCallstacks.emplace( crash.callstack );
        }
        if( !referencedCallstacks.empty() )
        {
            std::vector<uint32_t> ids( referencedCallstacks.begin(), referencedCallstacks.end() );
            std::set<uint32_t> resolved;
            for( const auto& frame : source->ResolveCallstacks( ids, 1 ) ) resolved.emplace( frame.callstack );
            for( const auto callstack : referencedCallstacks ) if( resolved.find( callstack ) == resolved.end() )
            {
                unresolvedMemoryCallstacks++;
                addRef( memoryCallstackRefs, source->MakeEntityRef( "callstack", callstack ) );
            }
        }
        if( unresolvedMemoryCallstacks ) addFinding( "warning", "UNRESOLVED_CALLSTACK", "persisted events reference callstacks that cannot be resolved", unresolvedMemoryCallstacks, std::move( memoryCallstackRefs ) );
        for( const auto& resource : source->GetSymbolResources() ) if( symbolRefs.find( resource.ref ) == symbolRefs.end() ) noteReference( "SYMBOL", resource.ref );
        for( auto& [kind, finding] : referenceFindings )
        {
            addFinding( "warning", ( "UNRESOLVED_" + kind + "_REFERENCE" ).c_str(), "persisted entity reference cannot be resolved in this trace", finding.count, std::move( finding.refs ) );
        }

        const auto gpuPools = source->GetMemoryPools();
        if( std::any_of( gpuPools.begin(), gpuPools.end(), []( const auto& pool ) { return pool.gpuD3D12; } ) )
        {
            const auto attributionValue = CachedGpuAttribution( trace.id, source );
            const auto& attribution = *attributionValue;
            if( attribution.protocolPresent )
            {
                size_t incompletePasses = 0, missingGpu = 0, ambiguousGpu = 0, unknownUses = 0;
                json incompleteRefs = json::array(), missingRefs = json::array(), ambiguousRefs = json::array();
                for( const auto& pass : attribution.passes )
                {
                    const auto ref = source->MakeEntityRef( "gpu-memory-pass", pass.passId );
                    if( !pass.complete ) { incompletePasses++; addRef( incompleteRefs, ref ); }
                    if( pass.gpuPairing == analysis::GpuZonePairing::Missing ) { missingGpu++; addRef( missingRefs, ref ); }
                    if( pass.gpuPairing == analysis::GpuZonePairing::Ambiguous ) { ambiguousGpu++; addRef( ambiguousRefs, ref ); }
                    for( const auto& use : pass.uses ) if( attribution.allocationById.find( use.allocationId ) == attribution.allocationById.end() ) unknownUses++;
                }
                if( incompletePasses ) addFinding( "warning", "INCOMPLETE_GTMEM_PASS", "GTMEM1 pass payload chunks or use counts are incomplete", incompletePasses, std::move( incompleteRefs ) );
                if( missingGpu ) addFinding( "warning", "MISSING_GTMEM_GPU_ZONE", "GTMEM1 passes have no matching GPU zone", missingGpu, std::move( missingRefs ) );
                if( ambiguousGpu ) addFinding( "warning", "AMBIGUOUS_GTMEM_GPU_ZONE", "GTMEM1 passes match more than one GPU zone", ambiguousGpu, std::move( ambiguousRefs ) );
                if( unknownUses ) addFinding( "warning", "UNKNOWN_GTMEM_ALLOCATION", "GTMEM1 pass uses reference allocation IDs absent from D3D12 pools", unknownUses );
            }
        }

        for( const auto& capability : source->GetCapabilities() ) if( !capability.present )
        {
            findings.push_back( { { "severity", "info" }, { "code", "CAPABILITY_ABSENT" }, { "message", capability.reason.empty() ? capability.domain + " is absent" : capability.reason },
                { "count", "0" }, { "refs", json::array() }, { "domain", capability.domain } } );
        }
        const auto errors = std::count_if( findings.begin(), findings.end(), []( const auto& finding ) { return finding.value( "severity", "" ) == "error"; } );
        return Success( id, {
            { "valid", errors == 0 }, { "error_count", errors }, { "finding_count", findings.size() }, { "findings", std::move( findings ) },
            { "checks", { "worker_load", "cpu_zone_timing", "gpu_zone_timing", "zone_parent_references", "frame_boundaries", "frame_image_references", "memory_lifetimes", "entity_references", "callstack_references", "symbol_references", "context_switch_timing", "sample_consistency", "gtmem1_protocol", "gpu_pass_pairing", "capability_presence" } }
        }, trace );
    }

    throw QueryError( "METHOD_NOT_FOUND", "unknown method: " + method );
}

std::string DumpProtocolJson( const json& value )
{
    return value.dump( -1, ' ', false, json::error_handler_t::replace );
}

}
