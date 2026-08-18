#include "TracyQueryService.hpp"

#include "TracyAnalysis.hpp"
#include "TracyEmbeddedData.hpp"
#include "../../public/common/TracyQueue.hpp"

#include "../../dtl/dtl.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
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

namespace
{

const std::vector<std::string>& RawQueryMethodRegistry()
{
    static const std::vector<std::string> methods = {
        "system.capabilities", "system.describe", "system.schema",
        "trace.open", "trace.status", "trace.list", "trace.close", "trace.info", "trace.overview", "trace.counts", "trace.app_info", "trace.identity", "trace.crash",
        "capture.context", "capture.coverage", "trace.telemetry_cost", "producer.list", "producer.get",
        "catalog.kinds", "catalog.list", "catalog.get", "catalog.entities", "catalog.quality",
        "relation.search", "relation.get", "runtime.domain.states",
        "gpu.taxonomy.tree", "gpu.taxonomy.coverage", "gpu.pass.search", "gpu.pass.get",
        "thread.list", "thread.get", "thread.statistics", "thread.timeline", "thread.migration",
        "cpu.topology", "cpu.usage", "cpu.timeline", "context_switch.range", "context_switch.thread", "context_switch.statistics",
        "frame.sets", "frame.list", "frame.get", "frame.statistics", "frame.outliers", "frame.range_mapping", "frame.identity", "entity.related", "correlation.chain", "timeline.correlated_slice", "evidence.graph", "frame.critical_path", "frame.explain", "frame_image.list", "frame_image.metadata", "frame_image.resource", "frame_image.raw",
        "zone.cpu.search", "zone.cpu.get", "zone.cpu.tree", "zone.cpu.statistics", "zone.cpu.flamegraph", "zone.gpu.contexts", "zone.gpu.search", "zone.gpu.get", "zone.gpu.tree", "zone.gpu.statistics", "zone.gpu.flamegraph",
        "memory.pools", "memory.events", "memory.get", "memory.active_at_time", "memory.frame_snapshot", "memory.diff", "memory.callstack_tree", "memory.leak_candidates",
        "memory.gpu.pools", "memory.gpu.allocations", "memory.gpu.request_scopes", "memory.gpu.pass_uses", "memory.gpu.attribution",
        "memory.gpu.summary", "memory.gpu.residency", "memory.gpu.fragmentation", "memory.gpu.churn",
        "runtime.script.summary", "runtime.script.frames", "runtime.script.stacks", "runtime.script.zones",
        "memory.gc.summary", "memory.gc.events",
        "lock.list", "lock.get", "lock.timeline", "lock.contention_statistics",
        "plot.list", "plot.points", "plot.range", "plot.downsample", "plot.statistics", "message.search", "message.get",
        "job.search", "job.get", "job.dependencies", "job.critical_path", "job.statistics", "job.gfx.statistics", "job.gfx_chain",
        "io.search", "io.get", "io.statistics", "io.chain", "network.capabilities",
        "callstack.resolve", "callstack.frames", "callstack.parent", "callstack.batch", "sample.list", "sample.ghost_zones", "sample.symbol_statistics", "sample.flamegraph", "hardware_sample.address", "hardware_sample.counts", "hardware_sample.events", "hardware_sample.capabilities",
        "symbol.search", "symbol.get", "symbol.address", "symbol.address_map", "symbol.raw_code", "symbol.disassembly",
        "source.locations", "source.callsite", "source.callsite.search", "source.statistics", "source.embedded", "source.lines", "source.raw",
        "timeline.slice", "statistics.describe", "statistics.compute", "compare.compatibility", "compare.normalized", "compare.zones", "compare.frames", "compare.source", "validation.run"
    };
    return methods;
}

nlohmann::json RequiredParametersFor( const std::string& method )
{
    nlohmann::json required = nlohmann::json::array();
    if( method == "trace.open" ) required.emplace_back( "path" );
    else if( method.rfind( "compare.", 0 ) == 0 ) { required.emplace_back( "baseline_trace_id" ); required.emplace_back( "trace_id" ); }
    else if( method != "system.describe" && method != "system.schema" && method != "trace.list" ) required.emplace_back( "trace_id" );
    if( ( method.ends_with( ".get" ) && method != "frame.get" && method != "producer.get" && method != "catalog.get" ) || method == "job.dependencies" || method == "job.gfx_chain" || method == "io.chain" || method == "entity.related" || method == "correlation.chain" || method == "timeline.correlated_slice" || method == "zone.cpu.tree" || method == "zone.gpu.tree" || method == "source.lines" || method == "source.raw" ||
        method == "symbol.raw_code" || method == "symbol.disassembly" || method == "frame_image.metadata" || method == "frame_image.resource" || method == "frame_image.raw" ) required.emplace_back( "ref" );
    if( method == "memory.frame_snapshot" ) required.emplace_back( "frame_index" );
    if( method == "source.callsite" ) required.emplace_back( "callsite_id" );
    if( method == "memory.diff" ) { required.emplace_back( "base_frame_index" ); required.emplace_back( "target_frame_index" ); }
    if( method == "memory.active_at_time" ) required.emplace_back( "time_ns" );
    if( method == "producer.get" ) required.emplace_back( "key" );
    if( method == "catalog.get" ) required.emplace_back( "definition_key" );
    if( method == "thread.statistics" || method == "thread.timeline" || method == "thread.migration" || method == "context_switch.thread" ) required.emplace_back( "thread_ref" );
    if( method == "plot.points" || method == "plot.range" || method == "plot.downsample" || method == "plot.statistics" ) required.emplace_back( "plot_ref" );
    if( method == "hardware_sample.address" || method == "hardware_sample.events" || method == "symbol.address" ) required.emplace_back( "address" );
    if( method == "callstack.frames" || method == "callstack.parent" ) required.emplace_back( "callstack" );
    if( method == "callstack.resolve" || method == "callstack.batch" ) required.emplace_back( "callstacks" );
    if( method == "statistics.compute" ) required.emplace_back( "values_ns" );
    return required;
}

nlohmann::json ParameterSchemaFor( const std::string& name )
{
    using nlohmann::json;
    if( name == "limit" ) return { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumPageSize } };
    if( name == "frame_index" || name == "base_frame_index" || name == "target_frame_index" || name == "index" || name == "warmup_frames" || name == "window_frames" ) return { { "type", "integer" }, { "minimum", 0 }, { "maximum", 1000000 } };
    if( name == "callsite_id" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", std::numeric_limits<uint32_t>::max() } }, json { { "type", "string" }, { "pattern", "^[1-9][0-9]*$" } } } ) } };
    if( name == "allow_warnings" ) return { { "type", "boolean" } };
    if( name == "comparison_mode" ) return { { "type", "string" }, { "enum", { "performance", "contract" } } };
    if( name == "max_scan_events" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumMaxScanEvents } }, json { { "type", "string" }, { "pattern", "^[0-9]+$" } } } ) } };
    if( name == "max_cpu_ms" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumMaxCpuMs } }, json { { "type", "string" }, { "pattern", "^[0-9]+$" } } } ) } };
    if( name == "max_nodes" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumMaxNodes } }, json { { "type", "string" }, { "pattern", "^[0-9]+$" } } } ) } };
    if( name == "max_edges" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumMaxEdges } }, json { { "type", "string" }, { "pattern", "^[0-9]+$" } } } ) } };
    if( name == "max_groups" ) return { { "oneOf", json::array( { json { { "type", "integer" }, { "minimum", 1 }, { "maximum", MaximumMaxGroups } }, json { { "type", "string" }, { "pattern", "^[0-9]+$" } } } ) } };
    if( name == "filter" ) return { { "type", "object" } };
    if( name == "callstacks" || name == "values_ns" || name == "fields" ) return { { "type", "array" }, { "items", { { "type", { "string", "integer" } } } } };
    return { { "type", "string" } };
}

std::string MethodDomain( const std::string& method )
{
    if( method.rfind( "source.callsite", 0 ) == 0 ) return "source.callsite";
    if( method == "evidence.graph" || method == "frame.critical_path" || method == "frame.explain" ) return "evidence";
    if( method.rfind( "runtime.domain.", 0 ) == 0 ) return "runtime.domain";
    if( method.rfind( "runtime.script.", 0 ) == 0 ) return "runtime.script";
    if( method.rfind( "memory.gc.", 0 ) == 0 ) return "memory.gc";
    if( method.rfind( "memory.gpu.", 0 ) == 0 ) return "memory.gpu";
    if( method.rfind( "gpu.pass.", 0 ) == 0 ) return "gpu.pass";
    if( method.rfind( "gpu.taxonomy.", 0 ) == 0 ) return "gpu.taxonomy";
    if( method.rfind( "job.gfx", 0 ) == 0 ) return "job.gfx";
    const auto separator = method.find( '.' );
    return separator == std::string::npos ? method : method.substr( 0, separator );
}

}

const nlohmann::json& QueryEnvelopeOutputSchema()
{
    static const nlohmann::json schema = {
        { "type", "object" }, { "required", { "protocol", "schema_version", "id", "ok" } },
        { "properties", {
            { "protocol", { { "const", QueryProtocol } } }, { "schema_version", { { "const", QuerySchemaVersion } } },
            { "id", {} }, { "ok", { { "type", "boolean" } } }, { "data", {} }, { "trace", { { "type", "object" } } },
            { "page", { { "type", "object" } } }, { "partial", { { "type", "boolean" } } },
            { "omitted_count", { { "type", { "string", "null" } } } }, { "budget", { { "type", "object" } } },
            { "warnings", { { "type", "array" } } }, { "error", { { "type", "object" } } }
        } },
        { "allOf", nlohmann::json::array( {
            nlohmann::json { { "if", { { "properties", { { "ok", { { "const", true } } } } } } },
                { "then", { { "required", { "data", "partial", "omitted_count", "budget", "warnings" } } } } },
            nlohmann::json { { "if", { { "properties", { { "ok", { { "const", false } } } } } } },
                { "then", { { "required", { "error" } } } } }
        } ) },
        { "additionalProperties", true }
    };
    return schema;
}

const nlohmann::json& QueryOperationSchemaRegistry()
{
    using nlohmann::json;
    static const json registry = [] {
        json operations = json::array();
        const std::vector<std::string> common = {
            "trace_id", "baseline_trace_id", "start_ns", "end_ns", "limit", "cursor", "filter", "fields",
            "max_scan_events", "max_cpu_ms", "max_nodes", "max_groups", "max_edges",
            "comparison_mode", "frame_set", "warmup_frames", "window_frames", "allow_warnings"
        };
        for( const auto& method : RawQueryMethodRegistry() )
        {
            const auto required = RequiredParametersFor( method );
            json properties = json::object();
            for( const auto& name : common ) properties[name] = ParameterSchemaFor( name );
            for( const auto& value : required )
            {
                const auto name = value.get<std::string>();
                properties[name] = ParameterSchemaFor( name );
            }
            json alternatives = json::array();
            if( method == "frame.get" )
            {
                alternatives = json::array( { json::array( { "ref" } ), json::array( { "frame_set", "index" } ) } );
                properties["ref"] = ParameterSchemaFor( "ref" );
                properties["frame_set"] = ParameterSchemaFor( "frame_set" );
                properties["index"] = { { "type", "integer" }, { "minimum", 0 } };
            }
            else if( method == "evidence.graph" || method == "frame.critical_path" || method == "frame.explain" )
            {
                alternatives = json::array( { json::array( { "ref" } ), json::array( { "frame_id" } ) } );
                properties["ref"] = ParameterSchemaFor( "ref" );
                properties["frame_id"] = ParameterSchemaFor( "frame_id" );
                properties["include_exact"] = { { "type", "boolean" } };
                properties["include_derived"] = { { "type", "boolean" } };
                properties["include_heuristic"] = { { "type", "boolean" } };
                properties["domains"] = { { "type", "array" }, { "items", { { "type", "string" } } } };
            }
            else if( method == "gpu.pass.search" )
            {
                properties["source_mode"] = { { "type", "string" },
                    { "enum", { "cpp-marker-command-list", "managed-command-buffer" } } };
                properties["pass_source_id"] = ParameterSchemaFor( "pass_source_id" );
                properties["taxonomy_id"] = ParameterSchemaFor( "taxonomy_id" );
                properties["name"] = ParameterSchemaFor( "name" );
            }
            else if( method == "relation.search" )
            {
                properties["namespace"] = ParameterSchemaFor( "namespace" );
                properties["source_kind"] = ParameterSchemaFor( "source_kind" );
                properties["target_kind"] = ParameterSchemaFor( "target_kind" );
                properties["relation"] = ParameterSchemaFor( "relation" );
            }
            else if( method == "runtime.domain.states" )
            {
                properties["domain"] = ParameterSchemaFor( "domain" );
            }
            else if( method == "source.callsite" || method == "source.callsite.search" )
            {
                properties["callsite_id"] = ParameterSchemaFor( "callsite_id" );
                properties["provenance"] = ParameterSchemaFor( "provenance" );
                properties["domain"] = ParameterSchemaFor( "domain" );
            }
            json inputSchema = { { "type", "object" }, { "properties", std::move( properties ) }, { "required", required }, { "additionalProperties", true } };
            if( !alternatives.empty() ) inputSchema["oneOf"] = json::array( {
                json { { "required", alternatives[0] } }, json { { "required", alternatives[1] } }
            } );
            json exampleParams = json::object();
            for( const auto& value : required )
            {
                const auto name = value.get<std::string>();
                if( name == "path" ) exampleParams[name] = "C:\\\\captures\\\\capture.tracy";
                else if( name == "trace_id" || name == "baseline_trace_id" ) exampleParams[name] = "trace-1";
                else if( name == "ref" || name == "thread_ref" || name == "plot_ref" ) exampleParams[name] = "tracy:v1:<fingerprint>:<kind>:<id>";
                else if( name == "callstacks" || name == "values_ns" ) exampleParams[name] = json::array( { "1" } );
                else if( name == "callstack" ) exampleParams[name] = "1";
                else if( name == "time_ns" ) exampleParams[name] = "0";
                else if( name == "address" ) exampleParams[name] = "0x0";
                else if( name == "key" ) exampleParams[name] = "cpu.zone.c-abi";
                else if( name == "definition_key" ) exampleParams[name] = "jn-def:v1:source:0000000000000000";
                else exampleParams[name] = 0;
            }
            if( method == "frame.get" ) exampleParams["index"] = 0;
            if( method == "evidence.graph" || method == "frame.critical_path" || method == "frame.explain" )
                exampleParams["ref"] = "tracy:v1:<fingerprint>:frame-identity:<id>";
            operations.push_back( {
                { "method", method }, { "schema_version", QuerySchemaVersion }, { "domain", MethodDomain( method ) },
                { "required", required }, { "one_of_required", alternatives }, { "pagination", true },
                { "budget_parameters", { "max_scan_events", "max_cpu_ms", "max_nodes", "max_groups", "max_edges" } },
                { "input_schema", std::move( inputSchema ) }, { "output_schema", QueryEnvelopeOutputSchema() },
                { "request_example", { { "protocol", QueryProtocol }, { "id", "request-1" }, { "method", method }, { "params", std::move( exampleParams ) } } }
            } );
        }
        return operations;
    }();
    return registry;
}

const std::vector<std::string>& QueryMethodRegistry()
{
    static const std::vector<std::string> methods = [] {
        std::vector<std::string> result;
        result.reserve( QueryOperationSchemaRegistry().size() );
        for( const auto& operation : QueryOperationSchemaRegistry() ) result.emplace_back( operation.at( "method" ).get<std::string>() );
        return result;
    }();
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
    size_t rawOffset = 0;
    bool rawOffsetBound = false;
    size_t limit = DefaultPageSize;
    std::string binding;
};

bool BudgetPartial();

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
    bound.erase( "max_scan_events" );
    bound.erase( "max_cpu_ms" );
    bound.erase( "max_nodes" );
    bound.erase( "max_groups" );
    page.binding = Hex16( Fnv1a( bound.dump() ) );
    if( params.contains( "cursor" ) )
    {
        if( !params["cursor"].is_string() ) throw QueryError( "INVALID_PARAMS", "cursor must be a string" );
        const auto parts = Split( Base64UrlDecode( params["cursor"].get<std::string>() ), '|' );
        const bool legacy = parts.size() == 7 && parts[0] == "v1";
        const bool current = parts.size() == 8 && parts[0] == "v2";
        if( ( !legacy && !current ) || parts[1] != trace.id || parts[2] != Decimal( trace.revision ) || parts[3] != method || parts[4] != page.binding )
        {
            throw QueryError( "STALE_CURSOR", "cursor does not match the trace, revision, method, or filters" );
        }
        try
        {
            page.offset = size_t( std::stoull( parts[5] ) );
            if( current )
            {
                page.rawOffset = size_t( std::stoull( parts[6] ) );
                page.rawOffsetBound = true;
                if( parts[7] != "stable" ) throw std::invalid_argument( "tie breaker" );
            }
            else if( parts[6] != "stable" ) throw std::invalid_argument( "tie breaker" );
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
    const auto nextOffset = page.offset + returned;
    const std::string value = "v2|" + trace.id + '|' + Decimal( trace.revision ) + '|' + method + '|' + page.binding + '|' + std::to_string( nextOffset ) + '|' + std::to_string( nextOffset ) + "|stable";
    return Base64UrlEncode( value );
}

std::string NextCursorAt( const PageRequest& page, const std::string& method, const TraceSessionSnapshot& trace, size_t nextOffset, size_t nextRawOffset, bool hasMore )
{
    if( !hasMore ) return {};
    const std::string value = "v2|" + trace.id + '|' + Decimal( trace.revision ) + '|' + method + '|' + page.binding + '|' + std::to_string( nextOffset ) + '|' + std::to_string( nextRawOffset ) + "|stable";
    return Base64UrlEncode( value );
}

json PageJson( const PageRequest& page, size_t returned, const std::string& nextCursor, bool truncated = false )
{
    return {
        { "limit", page.limit }, { "returned", returned },
        { "next_cursor", nextCursor.empty() ? json( nullptr ) : json( nextCursor ) }, { "truncated", truncated },
        { "partial", BudgetPartial() }, { "omitted_count", BudgetPartial() ? json( nullptr ) : json( "0" ) },
        { "omitted_count_exact", !BudgetPartial() }
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
        { "gfx_links", Decimal( value.gfxLinks ) }, { "correlated_frame_events", Decimal( value.correlatedFrameEvents ) },
        { "io_requests", Decimal( value.ioRequests ) }, { "io_configs", Decimal( value.ioConfigs ) }, { "io_stages", Decimal( value.ioStages ) },
        { "relations", Decimal( value.relations ) }, { "runtime_domain_states", Decimal( value.runtimeDomainStates ) },
        { "gpu_reference_passes", Decimal( value.gpuReferencePasses ) },
        { "gpu_reference_uses", Decimal( value.gpuReferenceUses ) },
        { "gpu_reference_ends", Decimal( value.gpuReferenceEnds ) },
        { "callsites", Decimal( value.callsites ) }
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
        { "callsite_id", value.callsiteId ? json( *value.callsiteId ) : json( nullptr ) },
        { "stack_ref", value.stackRef ? json( *value.stackRef ) : json( nullptr ) },
        { "provenance", value.stackProvenance },
        { "unavailable_reason", value.stackUnavailableReason ? json( *value.stackUnavailableReason ) : json( nullptr ) },
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
        { "callsite_id", value.callsiteId ? json( *value.callsiteId ) : json( nullptr ) },
        { "stack_ref", value.stackRef ? json( *value.stackRef ) : json( nullptr ) },
        { "provenance", value.stackProvenance },
        { "unavailable_reason", value.stackUnavailableReason ? json( *value.stackUnavailableReason ) : json( nullptr ) },
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
    size_t resourceSetV2ResourceCount = 0;
    size_t perUseV1ResourceCount = 0;
    std::set<uint32_t> resourceSetIds;
    for( const auto& use : pass.uses )
    {
        if( use.encoding == 2 )
        {
            resourceSetV2ResourceCount++;
            if( use.resourceSetId != 0 ) resourceSetIds.emplace( use.resourceSetId );
        }
        else
        {
            perUseV1ResourceCount++;
        }
    }
    const char* resourceEncoding = pass.uses.empty() ? "none" :
        ( resourceSetV2ResourceCount == pass.uses.size() ? "ResourceSetV2" :
        ( perUseV1ResourceCount == pass.uses.size() ? "PerUseV1" : "Mixed" ) );
    const size_t useEnd = includeUses ? std::min( pass.uses.size(), useOffset + useLimit ) : 0;
    for( size_t index = useOffset; index < useEnd; index++ )
    {
        const auto& use = pass.uses[index];
        uses.push_back( {
        { "allocation_id", Decimal( use.allocationId ) }, { "kind", std::string( 1, use.kind ) },
        { "usage_mask", "0x" + Hex16( use.usageMask ) }, { "usage", analysis::FormatGpuMemoryUsage( use.usageMask ) },
        { "resource_set_id", use.resourceSetId == 0 ? json( nullptr ) : json( Decimal( uint64_t( use.resourceSetId ) ) ) },
        { "encoding", use.encoding == 2 ? "ResourceSetV2" : "PerUseV1" }
        } );
    }
    return {
        { "ref", source.MakeEntityRef( "gpu-memory-pass", pass.passId ) }, { "pass_id", Decimal( pass.passId ) },
        { "parent_pass_id", pass.parentPassId == 0 ? json( nullptr ) : json( Decimal( pass.parentPassId ) ) },
        { "label_id", Decimal( pass.labelId ) }, { "taxonomy_id", Decimal( pass.labelId ) },
        { "frame", Decimal( pass.frame ) }, { "ordinal", Decimal( pass.ordinal ) }, { "thread_id", Decimal( pass.thread ) },
        { "command_list_id", Decimal( pass.commandListId ) },
        { "start_ns", Decimal( pass.start ) }, { "end_ns", Decimal( pass.end ) }, { "level", pass.level },
        { "name", pass.name }, { "operations", pass.operations }, { "command_count", pass.commandCount },
        { "emitted_use_count", pass.emittedUseCount }, { "total_use_count", pass.totalUseCount },
        { "expected_chunks", pass.expectedChunks }, { "parsed_chunks", pass.parsedChunks },
        { "untracked_references", pass.untrackedReferences }, { "dropped_uses", pass.droppedUses },
        { "truncated", pass.truncated }, { "complete", pass.complete },
        { "resource_encoding", resourceEncoding },
        { "resource_set_v2_resource_count", resourceSetV2ResourceCount },
        { "per_use_v1_resource_count", perUseV1ResourceCount },
        { "resource_set_id_count", resourceSetIds.size() },
        { "resource_set_id_min", resourceSetIds.empty() ? json( nullptr ) : json( Decimal( uint64_t( *resourceSetIds.begin() ) ) ) },
        { "resource_set_id_max", resourceSetIds.empty() ? json( nullptr ) : json( Decimal( uint64_t( *resourceSetIds.rbegin() ) ) ) },
        { "provenance", pass.structuredBinary ? "exact-binary" : "legacy-zone-text" },
        { "event_flags", pass.flags }, { "gpu_pairing", analysis::ToString( pass.gpuPairing ) },
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
        "flow_parallel_next", "flow_end", "cancelled", "incomplete", "schedule_callstack",
        "ready", "queue_enter", "dispatch", "steal", "wait_callstack", "continuation",
        "schedule_callsite", "wait_callsite"
    };
    return stage < std::size( names ) ? names[stage] : "unknown";
}

std::string JobStageRef( const analysis::TraceSource& source, uint64_t jobId, size_t index )
{
    return source.MakeEntityRef( "job-stage", ( jobId << 24 ) ^ index );
}

const char* GfxEntityKindName( uint8_t kind )
{
    static constexpr const char* names[] = { "dispatch", "gfx_job", "command_list", "submission", "gpu_segment", "explicit_gpu_pass" };
    return kind < std::size( names ) ? names[kind] : "unknown";
}

const char* GfxRelationName( uint8_t relation )
{
    static constexpr const char* names[] = { "parent", "dispatches", "executes", "produces", "submits", "runs_on_gpu", "depends_on",
        "recorded_on_command_list", "belongs_to_frame", "belongs_to_camera", "belongs_to_view", "references_resources", "classifies_as_taxonomy",
        "gpu_segment_references_resources" };
    return relation < std::size( names ) ? names[relation] : "unknown";
}

const char* RelationNamespaceName( uint8_t value )
{
    static constexpr const char* names[] = { "generic", "gfx", "job", "gpu_reference", "script", "io" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* EntityKindName( uint8_t value )
{
    static constexpr const char* names[] = {
        "unknown", "frame", "cpu_zone", "job", "gfx_entity", "gpu_pass", "gpu_segment", "gpu_taxonomy",
        "gpu_resource", "gpu_allocation", "io_request", "script_zone", "camera", "view", "command_list", "submission"
    };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* RuntimeDomainName( uint8_t value )
{
    static constexpr const char* names[] = { "unknown", "gpu_reference", "script_stack", "job", "gpu_pass", "io" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* RuntimeModeName( uint8_t value )
{
    static constexpr const char* names[] = { "follow_profile", "disabled", "enabled", "validation_dual" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* RuntimeStateReasonName( uint8_t value )
{
    static constexpr const char* names[] = {
        "requested", "connection_snapshot", "profile_change", "performance_gate", "overflow", "unsupported", "disconnect", "shutdown"
    };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* RuntimeCapabilityStatus( const analysis::RuntimeDomainStateDto& value )
{
    if( value.domain == uint8_t( JnRuntimeDomain::GpuReference ) )
    {
        if( value.requestedMode == uint8_t( JnRuntimeMode::FollowProfile ) &&
            value.effectiveMode == uint8_t( JnRuntimeMode::Disabled ) && value.reason == 3 )
            return "TriggeredOnly_PerformanceGateFailed";
        if( value.effectiveMode == uint8_t( JnRuntimeMode::ValidationDual ) ) return "ValidationOnly";
        if( value.requestedMode == uint8_t( JnRuntimeMode::Disabled ) ) return "DisabledByRequest";
        if( value.effectiveMode == uint8_t( JnRuntimeMode::Enabled ) )
            return value.requestedMode == uint8_t( JnRuntimeMode::FollowProfile ) ? "Available" : "TriggeredExplicitly";
        return "DisabledByProfile";
    }
    return value.effectiveMode == uint8_t( JnRuntimeMode::Disabled ) ? "Unavailable" : "Available";
}

const char* RelationName( uint8_t relationNamespace, uint8_t relation )
{
    if( relationNamespace == uint8_t( JnRelationNamespace::Gfx ) ) return GfxRelationName( relation );
    static constexpr const char* generic[] = { "related_to", "parent", "uses_resource", "executes_pass", "owned_by", "depends_on", "continues_as" };
    return relation < std::size( generic ) ? generic[relation] : "unknown";
}

json RelationJson( const analysis::TraceSource& source, const analysis::RelationDto& value )
{
    return {
        { "ref", value.ref }, { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef },
        { "source_id", Decimal( value.sourceId ) }, { "target_id", Decimal( value.targetId ) },
        { "source_kind", EntityKindName( value.sourceKind ) }, { "source_kind_id", value.sourceKind },
        { "target_kind", EntityKindName( value.targetKind ) }, { "target_kind_id", value.targetKind },
        { "source_ref", source.MakeEntityRef( EntityKindName( value.sourceKind ), value.sourceId ) },
        { "target_ref", source.MakeEntityRef( EntityKindName( value.targetKind ), value.targetId ) },
        { "namespace", RelationNamespaceName( value.relationNamespace ) }, { "namespace_id", value.relationNamespace },
        { "relation", RelationName( value.relationNamespace, value.relation ) }, { "relation_id", value.relation },
        { "flags", value.flags }, { "provenance", "exact-binary" }, { "evidence_kind", "exact" }
    };
}

json RuntimeDomainStateJson( const analysis::RuntimeDomainStateDto& value )
{
    return {
        { "ref", value.ref }, { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef },
        { "generation", Decimal( value.generation ) }, { "requested_frame", Decimal( value.requestedFrame ) },
        { "domain", RuntimeDomainName( value.domain ) }, { "domain_id", value.domain },
        { "requested_mode", RuntimeModeName( value.requestedMode ) }, { "requested_mode_id", value.requestedMode },
        { "effective_mode", RuntimeModeName( value.effectiveMode ) }, { "effective_mode_id", value.effectiveMode },
        { "reason", RuntimeStateReasonName( value.reason ) }, { "reason_id", value.reason },
        { "capability_status", RuntimeCapabilityStatus( value ) },
        { "flags", value.flags }, { "provenance", "exact-binary" }
    };
}

const char* CorrelatedFrameDomainName( uint8_t domain )
{
    static constexpr const char* names[] = { "editor", "player", "render", "present", "gpu_memory" };
    return domain < std::size( names ) ? names[domain] : "unknown";
}

const char* CorrelatedFramePhaseName( uint8_t phase )
{
    static constexpr const char* names[] = { "begin", "end", "boundary" };
    return phase < std::size( names ) ? names[phase] : "unknown";
}

json CorrelatedFrameEventJson( const analysis::TraceSource& source, const analysis::CorrelatedFrameEventDto& value )
{
    return {
        { "ref", value.ref }, { "frame_ref", source.MakeEntityRef( "frame-identity", value.frameId ) },
        { "frame_id", Decimal( value.frameId ) }, { "domain_index", Decimal( value.domainIndex ) },
        { "time_ns", Decimal( value.timeNs ) }, { "thread_ref", value.threadRef },
        { "domain", CorrelatedFrameDomainName( value.domain ) }, { "domain_id", value.domain },
        { "phase", CorrelatedFramePhaseName( value.phase ) }, { "phase_id", value.phase },
        { "canonical", ( value.flags & 1 ) != 0 }, { "alias", ( value.flags & 2 ) != 0 },
        { "flags", value.flags }, { "evidence_kind", "exact" }
    };
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
        { "origin_frame_sequence", value.originFrameSequence },
        { "origin_frame_ref", value.originFrameId == 0 ? json( nullptr ) : json( source.MakeEntityRef( "frame-identity", value.originFrameId ) ) },
        { "job_schema_version", value.jobSchemaVersion },
        { "schedule_callstack", value.scheduleCallstack },
        { "schedule_callstack_ref", value.scheduleCallstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", value.scheduleCallstack ) ) },
        { "schedule_callsite_id", value.scheduleCallsiteId == 0 ? json( nullptr ) : json( value.scheduleCallsiteId ) },
        { "schedule_stack_ref", value.scheduleCallstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", value.scheduleCallstack ) ) },
        { "schedule_stack_provenance", value.scheduleStackProvenance.empty() ? json( nullptr ) : json( value.scheduleStackProvenance ) },
        { "schedule_stack_unavailable_reason", value.scheduleStackUnavailableReason ? json( *value.scheduleStackUnavailableReason ) : json( nullptr ) },
        { "dependency_count", value.dependencies.size() }, { "stage_count", value.stages.size() },
        { "ready_ns", value.readyNs ? json( Decimal( *value.readyNs ) ) : json( nullptr ) },
        { "ready_lane", value.readyNs ? json( value.readyLane ) : json( nullptr ) },
        { "ready_flags", value.readyFlags },
        { "queue_enter_ns", value.queueEnterNs ? json( Decimal( *value.queueEnterNs ) ) : json( nullptr ) },
        { "queue_lane", value.queueEnterNs ? json( value.queueLane ) : json( nullptr ) },
        { "first_run_ns", value.firstRunNs ? json( Decimal( *value.firstRunNs ) ) : json( nullptr ) },
        { "completed_ns", value.completedNs ? json( Decimal( *value.completedNs ) ) : json( nullptr ) },
        { "schedule_to_first_run_ns", value.firstRunNs && !value.orphan ? json( Decimal( *value.firstRunNs - value.scheduleNs ) ) : json( nullptr ) },
        { "schedule_to_complete_ns", value.completedNs && !value.orphan ? json( Decimal( *value.completedNs - value.scheduleNs ) ) : json( nullptr ) },
        { "schedule_to_ready_ns", value.readyNs && !value.orphan ? json( Decimal( *value.readyNs - value.scheduleNs ) ) : json( nullptr ) },
        { "ready_to_queue_ns", value.readyNs && value.queueEnterNs ? json( Decimal( *value.queueEnterNs - *value.readyNs ) ) : json( nullptr ) },
        { "queue_to_first_run_ns", value.queueEnterNs && value.firstRunNs ? json( Decimal( *value.firstRunNs - *value.queueEnterNs ) ) : json( nullptr ) },
        { "dependency_ready_latency_ns", value.dependencyReadyLatencyNs ? json( Decimal( *value.dependencyReadyLatencyNs ) ) : json( nullptr ) },
        { "execution_ns", Decimal( value.executionNs ) },
        { "dispatch_count", value.dispatchCount }, { "scheduler_steal_count", value.schedulerStealCount },
        { "range_steal_slice_count", value.rangeStealSliceCount }, { "active_help_dispatch_count", value.activeHelpDispatchCount },
        { "queue_retry_count", value.queueRetryCount }, { "execution_lanes", value.executionLanes },
        { "wait", { { "total_ns", Decimal( value.waitNs ) }, { "active_help_ns", Decimal( value.waitActiveHelpNs ) },
            { "spin_yield_ns", Decimal( value.waitSpinYieldNs ) }, { "sleep_ns", Decimal( value.waitSleepNs ) },
            { "callstack_count", value.waitCallstacks.size() }, { "end_count", value.waitEndCount },
            { "continuation_count", value.continuationCount } } },
        { "capture_boundary", value.captureBoundary }, { "orphan", value.orphan },
        { "truncated", value.truncated }, { "trust", "untrusted_trace_data" }
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
        json stageJson = {
            { "ref", JobStageRef( source, value.jobId, index ) },
            { "time_ns", Decimal( stage.timeNs ) }, { "thread_ref", stage.threadRef }, { "stage", JobStageName( stage.stage ) },
            { "stage_id", stage.stage }, { "span_id", stage.spanId }, { "arg0", stage.arg0 }, { "arg1", stage.arg1 }, { "flags", stage.flags }
        };
        if( stage.stage == uint8_t( JnJobStage::Ready ) )
        {
            stageJson["source_lane"] = stage.arg0;
            stageJson["reason"] = ( stage.flags & uint8_t( 1 << 3 ) ) != 0 ? "dependency" :
                ( stage.flags & uint8_t( 1 << 4 ) ) != 0 ? "manual" :
                ( stage.flags & uint8_t( 1 << 5 ) ) != 0 ? "immediate" : "unknown";
        }
        else if( stage.stage == uint8_t( JnJobStage::QueueEnter ) )
        {
            stageJson["queue_lane"] = stage.arg0;
            stageJson["participant_count"] = stage.arg1;
            stageJson["retry"] = ( stage.flags & uint8_t( 1 << 6 ) ) != 0;
        }
        else if( stage.stage == uint8_t( JnJobStage::Dispatch ) )
        {
            stageJson["executor_lane"] = stage.arg0;
            stageJson["source_lane"] = stage.arg1;
            stageJson["stolen"] = ( stage.flags & uint8_t( 1 << 0 ) ) != 0;
            stageJson["wait_active_help"] = ( stage.flags & uint8_t( 1 << 1 ) ) != 0;
            stageJson["worker_lane"] = ( stage.flags & uint8_t( 1 << 2 ) ) != 0;
        }
        else if( stage.stage == uint8_t( JnJobStage::Steal ) )
        {
            stageJson["thief_lane"] = stage.arg0;
            stageJson["victim_lane"] = stage.arg1;
            stageJson["range_partition"] = ( stage.flags & uint8_t( 1 << 7 ) ) != 0;
        }
        else if( stage.stage == uint8_t( JnJobStage::WaitCallstack ) )
        {
            stageJson["callstack_ref"] = stage.spanId == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", stage.spanId ) );
            stageJson["wait_span_id"] = stage.arg1;
            stageJson["callstack_kind"] = "native";
        }
        else if( stage.stage == uint8_t( JnJobStage::ScheduleCallsite ) ||
            stage.stage == uint8_t( JnJobStage::WaitCallsite ) )
        {
            stageJson["callsite_id"] = stage.callsiteId;
            stageJson["stack_ref"] = stage.callstack == 0 ? json( nullptr ) :
                json( source.MakeEntityRef( "callstack", stage.callstack ) );
            stageJson["provenance"] = stage.stackProvenance.empty() ? json( nullptr ) : json( stage.stackProvenance );
            stageJson["unavailable_reason"] = stage.stackUnavailableReason ?
                json( *stage.stackUnavailableReason ) : json( nullptr );
            if( stage.stage == uint8_t( JnJobStage::WaitCallsite ) ) stageJson["wait_span_id"] = stage.arg1;
        }
        stages.push_back( std::move( stageJson ) );
    }
    json waitCallstacks = json::array();
    for( const auto& callstack : value.waitCallstacks ) waitCallstacks.push_back( {
        { "time_ns", Decimal( callstack.timeNs ) }, { "thread_ref", callstack.threadRef },
        { "wait_span_id", callstack.waitSpanId }, { "callstack", callstack.callstack },
        { "callstack_ref", callstack.callstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", callstack.callstack ) ) },
        { "callsite_id", callstack.callsiteId == 0 ? json( nullptr ) : json( callstack.callsiteId ) },
        { "stack_ref", callstack.callstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", callstack.callstack ) ) },
        { "provenance", callstack.stackProvenance.empty() ? json( nullptr ) : json( callstack.stackProvenance ) },
        { "unavailable_reason", callstack.stackUnavailableReason ? json( *callstack.stackUnavailableReason ) : json( nullptr ) },
        { "callstack_kind", "native" }
    } );
    result["dependencies"] = std::move( dependencies );
    result["stages"] = std::move( stages );
    result["wait_callstacks"] = std::move( waitCallstacks );
    return result;
}

const char* IoOperationName( uint8_t value )
{
    static constexpr const char* names[] = { "read", "stat", "open", "close", "jnfs_load", "decompress", "deserialize", "integrate", "upload", "resource_load" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* IoSourceName( uint8_t value )
{
    static constexpr const char* names[] = { "async_read_manager", "jnfs_native", "jnfs_managed", "async_upload_manager", "lua" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* IoStageName( uint8_t value )
{
    static constexpr const char* names[] = { "start", "complete", "error", "cancel", "requeue", "request_callstack" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* IoStatusName( uint8_t value )
{
    static constexpr const char* names[] = { "unknown", "success", "failure", "truncated", "cancelled", "requeued" };
    return value < std::size( names ) ? names[value] : "unknown";
}

const char* IoParentKindName( uint8_t value )
{
    static constexpr const char* names[] = { "none", "io_request", "unity_flow", "job", "resource" };
    return value < std::size( names ) ? names[value] : "unknown";
}

json IoRequestJson( const analysis::TraceSource& source, const analysis::IoRequestDto& value, bool detailed )
{
    const bool sync = ( value.flags & uint8_t( JnIoFlags::Sync ) ) != 0;
    const bool async = ( value.flags & uint8_t( JnIoFlags::Async ) ) != 0;
    const bool rightCensored = !value.endNs.has_value();
    const char* captureEndState = !rightCensored ? "terminal" : value.startNs ? "running" : "queued";
    json result = {
        { "ref", value.ref }, { "request_id", Decimal( value.requestId ) }, { "resource_id", Decimal( value.resourceId ) },
        { "resource_identity", ( value.flags & uint8_t( JnIoFlags::ResourcePathHash ) ) != 0 ? "path_hash" : "stable_id" },
        { "parent_id", Decimal( value.parentId ) }, { "parent_kind", IoParentKindName( value.parentKind ) }, { "parent_kind_id", value.parentKind },
        { "parent_ref", value.parentKind == uint8_t( JnIoParentKind::IoRequest ) && value.parentId != 0 ? json( source.MakeEntityRef( "io-request", value.parentId ) ) : json( nullptr ) },
        { "operation", IoOperationName( value.operation ) }, { "operation_id", value.operation },
        { "source", IoSourceName( value.source ) }, { "source_id", value.source },
        { "sync", sync }, { "async", async }, { "priority", value.priority }, { "subsystem", value.subsystem },
        { "queue_ns", value.orphan ? json( nullptr ) : json( Decimal( value.queueNs ) ) }, { "queue_thread_ref", value.queueThreadRef },
        { "start_ns", value.startNs ? json( Decimal( *value.startNs ) ) : json( nullptr ) },
        { "end_ns", value.endNs ? json( Decimal( *value.endNs ) ) : json( nullptr ) },
        { "queue_latency_ns", value.startNs && !value.orphan ? json( Decimal( *value.startNs - value.queueNs ) ) : json( nullptr ) },
        { "execution_ns", value.startNs && value.endNs ? json( Decimal( *value.endNs - *value.startNs ) ) : json( nullptr ) },
        { "total_ns", value.endNs && !value.orphan ? json( Decimal( *value.endNs - value.queueNs ) ) : json( nullptr ) },
        { "requested_bytes", Decimal( value.requestedBytes ) }, { "transferred_bytes", Decimal( value.transferredBytes ) },
        { "status", IoStatusName( value.status ) }, { "status_id", value.status },
        { "origin_frame_sequence", value.originFrameSequence }, { "request_callstack", value.requestCallstack },
        { "request_callstack_ref", value.requestCallstack == 0 ? json( nullptr ) : json( source.MakeEntityRef( "callstack", value.requestCallstack ) ) },
        { "callstack_kind", value.requestCallstack == 0 ? json( nullptr ) : json( "native" ) },
        { "flags", value.flags }, { "config_flags", value.configFlags }, { "stage_count", value.stages.size() },
        { "terminal_count", value.terminalCount }, { "orphan", value.orphan }, { "truncated", value.truncated },
        { "right_censored", rightCensored }, { "capture_end_state", captureEndState },
        { "capture_boundary", value.captureBoundary }, { "evidence_kind", "exact" }, { "trust", "untrusted_trace_data" }
    };
    if( !detailed ) return result;
    json stages = json::array();
    for( size_t index = 0; index < value.stages.size(); index++ )
    {
        const auto& stage = value.stages[index];
        stages.push_back( {
            { "ref", source.MakeEntityRef( "io-stage", ( value.requestId << 16 ) ^ index ) },
            { "time_ns", Decimal( stage.timeNs ) }, { "thread_ref", stage.threadRef },
            { "bytes", Decimal( stage.bytes ) }, { "detail", stage.detail },
            { "stage", IoStageName( stage.stage ) }, { "stage_id", stage.stage },
            { "status", IoStatusName( stage.status ) }, { "status_id", stage.status }, { "flags", stage.flags },
            { "callstack_ref", stage.stage == uint8_t( JnIoStage::RequestCallstack ) && stage.detail != 0 ? json( source.MakeEntityRef( "callstack", stage.detail ) ) : json( nullptr ) }
        } );
    }
    result["stages"] = std::move( stages );
    return result;
}

constexpr std::string_view CaptureIdentityPrefix = "JNCI1|";
constexpr std::string_view CaptureContextPrefix = "JNCTX1|";
constexpr std::string_view ProducerQualityPrefix = "JNQ1|";
constexpr std::string_view CatalogDefinitionPrefix = "JNCAT1|";
constexpr std::string_view CatalogEntityPrefix = "JNENT1|";
constexpr size_t MaximumIdentityEnvelopeBytes = 48 * 1024;
constexpr size_t MaximumIdentityRecords = 4096;
constexpr size_t MaximumIdentityFields = 256;
constexpr size_t MaximumIdentityStringBytes = 8192;
constexpr size_t MaximumContextRecords = 4096;
constexpr size_t MaximumQualityRecords = 16384;
constexpr size_t MaximumCatalogRecords = 4096;
constexpr size_t MaximumCatalogDefinitions = 32768;
constexpr size_t MaximumCatalogEntities = 65536;

std::optional<uint64_t> DecimalStringValue( const json& value )
{
    if( !value.is_string() ) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    uint64_t parsed = 0;
    const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed, 10 );
    if( result.ec != std::errc() || result.ptr != text.data() + text.size() ) return std::nullopt;
    return parsed;
}

uint64_t PositiveBudgetParameter( const json& params, const char* name, uint64_t defaultValue, uint64_t maximum )
{
    const auto value = UnsignedParameter( params, name, defaultValue, maximum );
    if( value == 0 ) throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be greater than zero" );
    return value;
}

struct QueryBudgetState
{
    explicit QueryBudgetState( const json& params, std::stop_token token )
        : stopToken( token )
        , started( std::chrono::steady_clock::now() )
        , maxScanEvents( PositiveBudgetParameter( params, "max_scan_events", DefaultMaxScanEvents, MaximumMaxScanEvents ) )
        , maxCpuMs( PositiveBudgetParameter( params, "max_cpu_ms", DefaultMaxCpuMs, MaximumMaxCpuMs ) )
        , maxNodes( PositiveBudgetParameter( params, "max_nodes", DefaultMaxNodes, MaximumMaxNodes ) )
        , maxEdges( PositiveBudgetParameter( params, "max_edges", DefaultMaxEdges, MaximumMaxEdges ) )
        , maxGroups( PositiveBudgetParameter( params, "max_groups", DefaultMaxGroups, MaximumMaxGroups ) )
    {}

    void CheckCancelled() const
    {
        if( stopToken.stop_requested() ) throw QueryError( "CANCELLED", "query was cancelled", true );
    }

    uint64_t ElapsedMs() const
    {
        return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count() );
    }

    void Exhaust( const char* reason )
    {
        partial = true;
        exhaustedBy.emplace( reason );
    }

    size_t ScanAllowance( size_t requested )
    {
        CheckCancelled();
        if( ElapsedMs() >= maxCpuMs ) { Exhaust( "max_cpu_ms" ); return 0; }
        if( scannedEvents >= maxScanEvents ) { Exhaust( "max_scan_events" ); return 0; }
        return size_t( std::min<uint64_t>( requested, maxScanEvents - scannedEvents ) );
    }

    void Scanned( size_t actual, size_t requested, size_t allowed )
    {
        (void)requested;
        (void)allowed;
        scannedEvents += actual;
    }

    bool ConsumeNodes( size_t count = 1 )
    {
        CheckCancelled();
        if( ElapsedMs() >= maxCpuMs ) { Exhaust( "max_cpu_ms" ); return false; }
        if( nodes + count > maxNodes ) { Exhaust( "max_nodes" ); return false; }
        nodes += count;
        return true;
    }

    bool ConsumeGroups( size_t count = 1 )
    {
        CheckCancelled();
        if( ElapsedMs() >= maxCpuMs ) { Exhaust( "max_cpu_ms" ); return false; }
        if( groups + count > maxGroups ) { Exhaust( "max_groups" ); return false; }
        groups += count;
        return true;
    }

    bool ConsumeEdges( size_t count = 1 )
    {
        CheckCancelled();
        if( ElapsedMs() >= maxCpuMs ) { Exhaust( "max_cpu_ms" ); return false; }
        if( edges + count > maxEdges ) { Exhaust( "max_edges" ); return false; }
        edges += count;
        return true;
    }

    void Attach( json& response ) const
    {
        if( !response.value( "ok", false ) ) return;
        response["partial"] = partial;
        response["omitted_count"] = partial ? json( nullptr ) : json( "0" );
        response["budget"] = {
            { "limits", {
                { "max_scan_events", Decimal( maxScanEvents ) }, { "max_cpu_ms", Decimal( maxCpuMs ) },
                { "max_nodes", Decimal( maxNodes ) }, { "max_edges", Decimal( maxEdges ) },
                { "max_groups", Decimal( maxGroups ) }
            } },
            { "consumed", {
                { "scan_events", Decimal( scannedEvents ) }, { "cpu_ms", Decimal( ElapsedMs() ) },
                { "nodes", Decimal( nodes ) }, { "edges", Decimal( edges ) }, { "groups", Decimal( groups ) }
            } },
            { "exhausted_by", json( exhaustedBy ) }, { "omitted_count_exact", !partial }
        };
        if( response.contains( "page" ) )
        {
            response["page"]["partial"] = partial;
            response["page"]["omitted_count"] = partial ? json( nullptr ) : json( "0" );
            response["page"]["omitted_count_exact"] = !partial;
        }
        if( partial ) response["warnings"].emplace_back( "query budget exhausted; resume with next_cursor when available" );
    }

    std::stop_token stopToken;
    std::chrono::steady_clock::time_point started;
    uint64_t maxScanEvents;
    uint64_t maxCpuMs;
    uint64_t maxNodes;
    uint64_t maxEdges;
    uint64_t maxGroups;
    uint64_t scannedEvents = 0;
    uint64_t nodes = 0;
    uint64_t edges = 0;
    uint64_t groups = 0;
    bool partial = false;
    std::set<std::string> exhaustedBy;
};

thread_local QueryBudgetState* ActiveBudget = nullptr;

struct QueryBudgetScope
{
    explicit QueryBudgetScope( QueryBudgetState& budget ) : previous( ActiveBudget ) { ActiveBudget = &budget; }
    ~QueryBudgetScope() { ActiveBudget = previous; }
    QueryBudgetState* previous;
};

size_t BudgetScanAllowance( size_t requested ) { return ActiveBudget ? ActiveBudget->ScanAllowance( requested ) : requested; }
void BudgetScanned( size_t actual, size_t requested, size_t allowed ) { if( ActiveBudget ) ActiveBudget->Scanned( actual, requested, allowed ); }
bool BudgetConsumeNode( size_t count = 1 ) { return !ActiveBudget || ActiveBudget->ConsumeNodes( count ); }
bool BudgetConsumeEdge( size_t count = 1 ) { return !ActiveBudget || ActiveBudget->ConsumeEdges( count ); }
bool BudgetConsumeGroup( size_t count = 1 ) { return !ActiveBudget || ActiveBudget->ConsumeGroups( count ); }
bool BudgetPartial() { return ActiveBudget && ActiveBudget->partial; }

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

    // The producer can report only that Sampling/Context Switch passed its
    // startup gates. ETW privilege and kernel-session success are observable
    // only in the persisted trace. Publish the authoritative result next to
    // the requested configuration so automation never mistakes a request for
    // captured data.
    if( context.contains( "capture_config" ) && context["capture_config"].is_object() )
    {
        const bool samplingPresent = info.counts.samples != 0 && info.samplingPeriodNs > 0;
        const bool contextSwitchPresent = info.counts.contextSwitches != 0;
        const uint64_t samplingHz = samplingPresent ?
            uint64_t( ( 1000000000ll + info.samplingPeriodNs / 2 ) / info.samplingPeriodNs ) : 0;
        context["capture_config"]["actual_capabilities"] = {
            { "verification", "persisted_trace" },
            { "sampling", {
                { "present", samplingPresent },
                { "period_ns", Decimal( samplingPresent ? uint64_t( info.samplingPeriodNs ) : 0 ) },
                { "frequency_hz", samplingHz },
                { "sample_count", Decimal( info.counts.samples ) },
                { "reason", samplingPresent ? json( nullptr ) : json( "absent_in_persisted_trace" ) }
            } },
            { "context_switch", {
                { "present", contextSwitchPresent },
                { "event_count", Decimal( info.counts.contextSwitches ) },
                { "reason", contextSwitchPresent ? json( nullptr ) : json( "absent_in_persisted_trace" ) }
            } }
        };
        sources["capture_config.actual_capabilities"] = {
            { "producer", "tracy-query" }, { "generation", Decimal( uint64_t( 0 ) ) }
        };
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
        "mismatch", "unresolved", "pre_capture", "replayed", "tail_truncated",
        "cpu_time_ns", "event_bytes", "dictionary_hit", "dictionary_miss",
        "dictionary_bytes", "degrade"
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
            ( document.value( "schema_version", 0 ) < 1 || document.value( "schema_version", 0 ) > 2 ) ||
            !document.contains( "connection_id" ) ||
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
            ( producer.contains( "runtime_policy" ) && !producer["runtime_policy"].is_object() ) ||
            !producer.contains( "counters" ) || !producer["counters"].is_object() )
        {
            if( invalid.size() < 64 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", "producer quality state is invalid" } } );
            continue;
        }
        const auto connectionId = DecimalStringValue( document["connection_id"] );
        const auto sequence = DecimalStringValue( document["snapshot_sequence"] );
        bool countersValid = connectionId.has_value() && sequence.has_value();
        const size_t requiredCounterCount = document.value( "schema_version", 1 ) >= 2 ?
            std::size( CounterNames ) : 11;
        for( size_t counter = 0; counter < requiredCounterCount; counter++ )
            countersValid = countersValid && producer["counters"].contains( CounterNames[counter] ) &&
                DecimalStringValue( producer["counters"][CounterNames[counter]] ).has_value();
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
        const auto producerKey = producer["key"].get<std::string>();
        byProducer[producerKey].push_back( { *sequence, uint64_t( index ), std::move( document ) } );
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
        // Keep cold-path value copies instead of references into vector-owned
        // JSON documents. This query may be built with LTCG; explicit values
        // avoid optimizer-sensitive lifetime/evaluation-order coupling with
        // the later output initializer list.
        const auto first = snapshots.front().document["producer"];
        const auto last = snapshots.back().document["producer"];
        const bool windowComplete = snapshots.front().sequence < snapshots.back().sequence;
        complete = complete && windowComplete;
        json counters = json::object();
        std::array<uint64_t, std::size( CounterNames )> deltas {};
        bool regression = false;
        for( size_t counter = 0; counter < std::size( CounterNames ); counter++ )
        {
            const auto baseValue = first["counters"].contains( CounterNames[counter] ) ?
                DecimalStringValue( first["counters"][CounterNames[counter]] ) : std::optional<uint64_t>( 0 );
            const auto finalValue = last["counters"].contains( CounterNames[counter] ) ?
                DecimalStringValue( last["counters"][CounterNames[counter]] ) : std::optional<uint64_t>( 0 );
            const auto base = baseValue.value_or( 0 );
            const auto final = finalValue.value_or( 0 );
            if( final < base ) regression = true;
            deltas[counter] = final >= base ? final - base : 0;
            counters[CounterNames[counter]] = Decimal( deltas[counter] );
        }
        complete = complete && !regression;

        const bool requested = last["requested"].get<bool>();
        const bool compiled = last["compiled"].get<bool>();
        const bool supported = last["supported"].get<bool>();
        bool enabled = last["enabled"].get<bool>();
        bool effective = last["effective"].get<bool>();
        const bool permissionDenied = last["permission_denied"].get<bool>();
        bool deferred = last["deferred"].get<bool>();
        std::string producerReason = last.value( "reason", "" );
        const bool persistedSampling = info.counts.samples != 0 && info.samplingPeriodNs > 0;
        const bool persistedContextSwitch = info.counts.contextSwitches != 0;
        const bool systemTracingProducer = key == "sampling.context-switch";
        const bool persistedSystemTracing = persistedSampling && persistedContextSwitch;
        const bool persistedSystemTracingPartial = systemTracingProducer && persistedSampling != persistedContextSwitch;
        if( systemTracingProducer && ( persistedSystemTracing || persistedSystemTracingPartial ) )
        {
            // Startup can only report a deferred privilege check. Persisted
            // samples/context switches are the authoritative evidence that
            // the Windows tracing session actually ran.
            enabled = true;
            effective = true;
            deferred = false;
            producerReason = persistedSystemTracing ? "persisted_trace_data_verified" : "persisted_trace_data_partial";
        }
        std::string state;
        if( deferred ) state = "deferred";
        else if( !compiled ) state = "uncompiled";
        else if( permissionDenied ) state = "permission_denied";
        else if( !supported ) state = "unsupported";
        else if( !requested || !enabled || !effective ) state = "disabled";
        else if( !windowComplete || regression ) state = "unknown";
        else if( persistedSystemTracingPartial || deltas[2] != 0 || deltas[5] != 0 || deltas[6] != 0 || deltas[7] != 0 || deltas[10] != 0 ) state = "degraded";
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
        if( persistedSystemTracingPartial ) findings.push_back( { { "code", "PERSISTED_SYSTEM_TRACING_PARTIAL" }, { "count", "1" } } );
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
            { "deferred", deferred }, { "reason", producerReason },
            { "persisted_evidence", systemTracingProducer ? json( {
                { "verification", "persisted_trace" },
                { "sampling", { { "present", persistedSampling }, { "count", Decimal( info.counts.samples ) } } },
                { "context_switch", { { "present", persistedContextSwitch }, { "count", Decimal( info.counts.contextSwitches ) } } }
            } ) : json( nullptr ) },
            { "filter", last.value( "filter", "" ) }, { "threshold", last.value( "threshold", "0" ) },
            { "budget", last.value( "budget", "0" ) }, { "runtime_policy", last.value( "runtime_policy", json::object() ) },
            { "sample_rate", last.value( "sample_rate", json::object() ) },
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

json CallsiteJson( const analysis::CallsiteDto& value )
{
    return {
        { "ref", value.ref }, { "callsite_id", value.callsiteId },
        { "thread_ref", value.threadRef }, { "source_location_ref", value.sourceLocationRef },
        { "stack_ref", value.stackRef ? json( *value.stackRef ) : json( nullptr ) },
        { "callstack", value.callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( value.callstack ) ) ) },
        { "domain", value.domain }, { "provenance", value.provenance }, { "flags", value.flags },
        { "unavailable_reason", value.unavailableReason ? json( *value.unavailableReason ) : json( nullptr ) },
        { "trust", "untrusted_trace_data" }
    };
}

json TelemetryCostJson( const analysis::TraceInfoDto& info )
{
    const auto coverage = CaptureCoverageJson( info );
    json producers = json::array();
    uint64_t totalCpuTimeNs = 0;
    uint64_t totalEvents = 0;
    uint64_t totalBytes = 0;
    uint64_t totalDictionaryHit = 0;
    uint64_t totalDictionaryMiss = 0;
    uint64_t totalDictionaryBytes = 0;
    uint64_t totalOverflow = 0;
    uint64_t totalDegrade = 0;

    if( coverage.contains( "producers" ) && coverage["producers"].is_array() )
    {
        for( const auto& producer : coverage["producers"] )
        {
            const auto& counters = producer.value( "counters", json::object() );
            const auto counter = [&]( const char* name ) {
                if( !counters.contains( name ) ) return uint64_t( 0 );
                return DecimalStringValue( counters[name] ).value_or( 0 );
            };
            const auto cpuTimeNs = counter( "cpu_time_ns" );
            const auto events = counter( "emitted" );
            const auto bytes = counter( "event_bytes" );
            const auto dictionaryHit = counter( "dictionary_hit" );
            const auto dictionaryMiss = counter( "dictionary_miss" );
            const auto dictionaryBytes = counter( "dictionary_bytes" );
            const auto overflow = counter( "overflow" );
            const auto degrade = counter( "degrade" );
            totalCpuTimeNs += cpuTimeNs;
            totalEvents += events;
            totalBytes += bytes;
            totalDictionaryHit += dictionaryHit;
            totalDictionaryMiss += dictionaryMiss;
            totalDictionaryBytes += dictionaryBytes;
            totalOverflow += overflow;
            totalDegrade += degrade;
            producers.push_back( {
                { "key", producer.value( "key", "" ) },
                { "state", producer.value( "state", "unknown" ) },
                { "complete", producer.value( "complete", false ) },
                { "cpu_time_ns", Decimal( cpuTimeNs ) },
                { "event_count", Decimal( events ) },
                { "event_bytes", Decimal( bytes ) },
                { "cpu_ns_per_event", events == 0 ? json( nullptr ) : json( double( cpuTimeNs ) / double( events ) ) },
                { "bytes_per_event", events == 0 ? json( nullptr ) : json( double( bytes ) / double( events ) ) },
                { "dictionary_hit", Decimal( dictionaryHit ) },
                { "dictionary_miss", Decimal( dictionaryMiss ) },
                { "dictionary_bytes", Decimal( dictionaryBytes ) },
                { "overflow", Decimal( overflow ) },
                { "degrade", Decimal( degrade ) }
            } );
        }
    }

    return {
        { "present", coverage.value( "present", false ) },
        { "schema_version", 1 },
        { "complete", coverage.value( "complete", false ) },
        { "reason", coverage.value( "reason", json( nullptr ) ) },
        { "measurement", "producer_self_reported_cpu_time" },
        { "totals", {
            { "cpu_time_ns", Decimal( totalCpuTimeNs ) },
            { "event_count", Decimal( totalEvents ) },
            { "event_bytes", Decimal( totalBytes ) },
            { "cpu_ns_per_event", totalEvents == 0 ? json( nullptr ) : json( double( totalCpuTimeNs ) / double( totalEvents ) ) },
            { "bytes_per_event", totalEvents == 0 ? json( nullptr ) : json( double( totalBytes ) / double( totalEvents ) ) },
            { "dictionary_hit", Decimal( totalDictionaryHit ) },
            { "dictionary_miss", Decimal( totalDictionaryMiss ) },
            { "dictionary_bytes", Decimal( totalDictionaryBytes ) },
            { "overflow", Decimal( totalOverflow ) },
            { "degrade", Decimal( totalDegrade ) }
        } },
        { "producers", std::move( producers ) },
        { "trust", "untrusted_trace_data" }
    };
}

json ProducerDomainQualityJson( const analysis::TraceInfoDto& info, std::string_view producerKey )
{
    json result = {
        { "present", false }, { "complete", false }, { "producer_key", producerKey },
        { "source_mode", nullptr }, { "state", "missing" }, { "counters", json::object() },
        { "optional_filtering_allowed", true }
    };
    const auto coverage = CaptureCoverageJson( info );
    if( !coverage.contains( "producers" ) || !coverage["producers"].is_array() ) return result;
    const auto producer = std::find_if( coverage["producers"].begin(), coverage["producers"].end(),
        [&]( const auto& value ) { return value.value( "key", "" ) == producerKey; } );
    if( producer == coverage["producers"].end() ) return result;

    const auto& counters = producer->at( "counters" );
    const auto counterIsZero = [&]( const char* name ) {
        if( !counters.contains( name ) ) return false;
        const auto value = DecimalStringValue( counters[name] );
        return value.has_value() && *value == 0;
    };
    const bool clean = producer->value( "complete", false ) &&
        counterIsZero( "dropped" ) && counterIsZero( "overflow" ) &&
        counterIsZero( "mismatch" ) && counterIsZero( "unresolved" ) &&
        counterIsZero( "tail_truncated" );
    return {
        { "present", true }, { "complete", clean }, { "producer_key", producerKey },
        { "source_mode", producer->value( "source_mode", "" ) },
        { "state", producer->value( "state", "unknown" ) }, { "counters", counters },
        { "optional_filtering_allowed", true }
    };
}

bool CatalogDefinitionKeyValid( const std::string& key, const std::string& kind )
{
    const auto prefix = "jn-def:v1:" + kind + ":";
    if( key.rfind( prefix, 0 ) != 0 || key.size() != prefix.size() + 16 ) return false;
    return std::all_of( key.begin() + ptrdiff_t( prefix.size() ), key.end(), []( const char c ) {
        return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' );
    } );
}

std::string CatalogCanonicalMaterial( const json& definition )
{
    std::string material = "jn-catalog-definition-v1";
    material.push_back( char( definition["kind_id"].get<uint8_t>() ) );
    const auto append = [&material]( const std::string& value ) {
        material.push_back( '\0' );
        material += std::to_string( value.size() );
        material.push_back( ':' );
        material += value;
    };
    append( Lower( definition["namespace"].get<std::string>() ) );
    append( definition["canonical_name"].get<std::string>() );
    append( definition["source"]["file_id"].get<std::string>() );
    append( definition["source"]["function"].get<std::string>() );
    material.push_back( '\0' );
    material += std::to_string( definition["source"]["line"].get<uint32_t>() );
    return material;
}

bool CatalogPrivacySafe( const json& definition )
{
    const auto unsafeText = []( const std::string& value ) {
        const auto lower = Lower( value );
        for( const auto* pattern : { "password=", "passwd=", "payload=", "account=", "user=",
            "username=", "token=", "authorization:", "bearer " } )
            if( lower.find( pattern ) != std::string::npos ) return true;
        uint32_t dots = 0;
        uint32_t digits = 0;
        for( const char c : lower )
        {
            if( c >= '0' && c <= '9' ) digits++;
            else if( c == '.' && digits != 0 ) { dots++; digits = 0; }
            else { dots = 0; digits = 0; }
            if( dots >= 3 && digits != 0 ) return true;
        }
        return false;
    };
    const auto file = definition["source"]["file_id"].get<std::string>();
    const auto lowerFile = Lower( file );
    if( file.find( '\\' ) != std::string::npos || ( lowerFile.size() >= 2 && lowerFile[1] == ':' ) ||
        lowerFile.rfind( "/", 0 ) == 0 || lowerFile.find( "/users/" ) != std::string::npos ) return false;
    return !unsafeText( definition["canonical_name"].get<std::string>() ) &&
        !unsafeText( definition["namespace"].get<std::string>() ) &&
        !unsafeText( definition["source"]["function"].get<std::string>() );
}

json CatalogJson( const analysis::TraceInfoDto& info )
{
    std::optional<uint64_t> activeConnectionId;
    const auto captureIdentity = CaptureIdentityJson( info );
    if( captureIdentity.value( "present", false ) && captureIdentity.contains( "identity" ) &&
        captureIdentity["identity"].is_object() && captureIdentity["identity"].contains( "connection" ) &&
        captureIdentity["identity"]["connection"].is_object() &&
        captureIdentity["identity"]["connection"].contains( "id" ) )
        activeConnectionId = DecimalStringValue( captureIdentity["identity"]["connection"]["id"] );

    std::map<std::string, json> definitions;
    std::map<std::string, std::string> catalogIds;
    std::map<uint64_t, json> entities;
    json invalid = json::array();
    std::set<uint64_t> connectionIds;
    size_t seen = 0;
    size_t validRecords = 0;
    size_t duplicates = 0;
    size_t envelopeBytes = 0;
    size_t privacyViolations = 0;
    size_t unresolvedEntities = 0;
    size_t staleConnectionRecords = 0;

    const auto addInvalid = [&]( size_t index, const std::string& reason ) {
        if( invalid.size() < 128 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", reason } } );
    };
    for( size_t recordIndex = 0; recordIndex < info.appInfo.size(); recordIndex++ )
    {
        const auto& record = info.appInfo[recordIndex];
        const bool definitionRecord = record.starts_with( CatalogDefinitionPrefix );
        const bool entityRecord = record.starts_with( CatalogEntityPrefix );
        const bool catalogLike = definitionRecord || entityRecord ||
            ( record.size() >= 6 && ( record.compare( 0, 5, "JNCAT" ) == 0 || record.compare( 0, 5, "JNENT" ) == 0 ) &&
                record.find( '|' ) != std::string::npos );
        if( !catalogLike ) continue;
        if( ++seen > MaximumCatalogRecords )
        {
            addInvalid( recordIndex, "catalog record limit exceeded" );
            break;
        }
        envelopeBytes += record.size();
        if( !definitionRecord && !entityRecord )
        {
            addInvalid( recordIndex, "unsupported catalog envelope version" );
            continue;
        }
        const auto prefix = definitionRecord ? CatalogDefinitionPrefix : CatalogEntityPrefix;
        if( record.size() <= prefix.size() || record.size() > MaximumIdentityEnvelopeBytes )
        {
            addInvalid( recordIndex, "catalog envelope has an invalid size" );
            continue;
        }
        const auto document = json::parse( record.begin() + ptrdiff_t( prefix.size() ), record.end(), nullptr, false );
        if( document.is_discarded() || !document.is_object() || document.value( "schema_version", 0 ) != 1 ||
            !document.contains( "connection_id" ) )
        {
            addInvalid( recordIndex, "catalog envelope failed schema validation" );
            continue;
        }
        const auto connectionId = DecimalStringValue( document["connection_id"] );
        if( !connectionId )
        {
            addInvalid( recordIndex, "catalog connection_id is invalid" );
            continue;
        }
        // Tracy retains incremental AppInfo across on-demand reconnects. The
        // connection snapshot identifies the active capture generation; old
        // catalog/entity envelopes are history from an earlier capture and
        // must not fabricate entities in the current capture-local namespace.
        if( activeConnectionId && *connectionId != *activeConnectionId )
        {
            staleConnectionRecords++;
            continue;
        }
        connectionIds.emplace( *connectionId );

        if( definitionRecord )
        {
            if( !document.contains( "definitions" ) || !document["definitions"].is_array() || document["definitions"].size() > 512 )
            {
                addInvalid( recordIndex, "catalog definitions array is invalid" );
                continue;
            }
            bool recordValid = true;
            for( auto definition : document["definitions"] )
            {
                if( definitions.size() >= MaximumCatalogDefinitions )
                {
                    addInvalid( recordIndex, "catalog definition limit exceeded" );
                    recordValid = false;
                    break;
                }
                if( !definition.is_object() || !definition.contains( "catalog_id" ) || !DecimalStringValue( definition["catalog_id"] ) ||
                    !definition.contains( "definition_key" ) || !definition["definition_key"].is_string() ||
                    !definition.contains( "kind" ) || !definition["kind"].is_string() ||
                    !definition.contains( "kind_id" ) || !definition["kind_id"].is_number_unsigned() ||
                    definition["kind_id"].get<uint64_t>() == 0 || definition["kind_id"].get<uint64_t>() > 9 ||
                    !definition.contains( "flags" ) || !definition["flags"].is_number_unsigned() ||
                    !definition.contains( "canonical_name" ) || !definition["canonical_name"].is_string() ||
                    definition["canonical_name"].get_ref<const std::string&>().empty() ||
                    definition["canonical_name"].get_ref<const std::string&>().size() > 192 ||
                    !definition.contains( "namespace" ) || !definition["namespace"].is_string() ||
                    !definition.contains( "source" ) || !definition["source"].is_object() ||
                    !definition["source"].contains( "file_id" ) || !definition["source"]["file_id"].is_string() ||
                    !definition["source"].contains( "function" ) || !definition["source"]["function"].is_string() ||
                    !definition["source"].contains( "line" ) || !definition["source"]["line"].is_number_unsigned() )
                {
                    addInvalid( recordIndex, "catalog definition failed field validation" );
                    recordValid = false;
                    continue;
                }
                const auto key = definition["definition_key"].get<std::string>();
                const auto kind = definition["kind"].get<std::string>();
                if( !CatalogDefinitionKeyValid( key, kind ) ||
                    key.substr( key.size() - 16 ) != Hex16( Fnv1a( CatalogCanonicalMaterial( definition ) ) ) )
                {
                    addInvalid( recordIndex, "catalog definition_key does not match canonical fields" );
                    recordValid = false;
                    continue;
                }
                definition["connection_id"] = Decimal( *connectionId );
                definition["trust"] = "untrusted_trace_data";
                if( !CatalogPrivacySafe( definition ) )
                {
                    privacyViolations++;
                    addInvalid( recordIndex, "catalog definition violates privacy policy" );
                    recordValid = false;
                    continue;
                }
                const auto idKey = Decimal( *connectionId ) + ":" + definition["catalog_id"].get<std::string>();
                const auto existingId = catalogIds.find( idKey );
                if( existingId != catalogIds.end() && existingId->second != key )
                {
                    addInvalid( recordIndex, "catalog_id maps to multiple definition keys" );
                    recordValid = false;
                    continue;
                }
                const auto existing = definitions.find( key );
                if( existing != definitions.end() )
                {
                    auto comparable = definition;
                    comparable.erase( "connection_id" );
                    comparable.erase( "trust" );
                    auto oldComparable = existing->second;
                    oldComparable.erase( "connection_id" );
                    oldComparable.erase( "trust" );
                    if( comparable != oldComparable )
                    {
                        addInvalid( recordIndex, "definition_key maps to conflicting canonical fields" );
                        recordValid = false;
                    }
                    else duplicates++;
                    continue;
                }
                catalogIds[idKey] = key;
                definitions.emplace( key, std::move( definition ) );
            }
            if( recordValid ) validRecords++;
        }
        else
        {
            if( !document.contains( "entities" ) || !document["entities"].is_array() || document["entities"].size() > 1024 )
            {
                addInvalid( recordIndex, "catalog entities array is invalid" );
                continue;
            }
            bool recordValid = true;
            for( auto entity : document["entities"] )
            {
                if( entities.size() >= MaximumCatalogEntities )
                {
                    addInvalid( recordIndex, "catalog entity limit exceeded" );
                    recordValid = false;
                    break;
                }
                if( !entity.is_object() || entity.contains( "name" ) || !entity.contains( "entity_id" ) ||
                    !entity.contains( "catalog_id" ) || !entity.contains( "definition_key" ) ||
                    !entity.contains( "connection_generation" ) || !entity["connection_generation"].is_number_unsigned() ||
                    !entity.contains( "parent_entity_id" ) || !entity.contains( "flags" ) || !entity["flags"].is_number_unsigned() )
                {
                    addInvalid( recordIndex, "catalog entity failed field validation" );
                    recordValid = false;
                    continue;
                }
                const auto entityId = DecimalStringValue( entity["entity_id"] );
                const auto catalogId = DecimalStringValue( entity["catalog_id"] );
                const auto parentId = DecimalStringValue( entity["parent_entity_id"] );
                if( !entityId || !catalogId || !parentId || !entity["definition_key"].is_string() ||
                    entity["connection_generation"].get<uint64_t>() == 0 ||
                    entity["connection_generation"].get<uint64_t>() != ( *entityId >> 48 ) ||
                    ( *parentId != 0 && ( *parentId >> 48 ) != ( *entityId >> 48 ) ) )
                {
                    addInvalid( recordIndex, "catalog entity identifiers are invalid" );
                    recordValid = false;
                    continue;
                }
                entity["connection_id"] = Decimal( *connectionId );
                entity["trust"] = "untrusted_trace_data";
                const auto existing = entities.find( *entityId );
                if( existing != entities.end() )
                {
                    if( existing->second != entity )
                    {
                        addInvalid( recordIndex, "entity_id maps to conflicting fields" );
                        recordValid = false;
                    }
                    else duplicates++;
                    continue;
                }
                entities.emplace( *entityId, std::move( entity ) );
            }
            if( recordValid ) validRecords++;
        }
    }

    for( const auto& [entityId, entity] : entities )
    {
        const auto idKey = entity["connection_id"].get<std::string>() + ":" + entity["catalog_id"].get<std::string>();
        const auto definition = catalogIds.find( idKey );
        if( definition == catalogIds.end() || definition->second != entity["definition_key"].get<std::string>() )
        {
            unresolvedEntities++;
            if( invalid.size() < 128 ) invalid.push_back( {
                { "record_index", nullptr }, { "reason", "catalog entity references an unresolved definition" },
                { "entity_id", Decimal( entityId ) }
            } );
        }
    }

    json definitionArray = json::array();
    std::map<std::string, size_t> kindCounts;
    for( auto& [key, definition] : definitions )
    {
        kindCounts[definition["kind"].get<std::string>()]++;
        definitionArray.push_back( definition );
    }
    json entityArray = json::array();
    for( const auto& [entityId, entity] : entities ) entityArray.push_back( entity );
    json kinds = json::array();
    for( const auto& [kind, count] : kindCounts ) kinds.push_back( { { "kind", kind }, { "definition_count", count } } );
    json connections = json::array();
    for( const auto value : connectionIds ) connections.push_back( Decimal( value ) );

    const bool present = !definitions.empty();
    const bool complete = present && invalid.empty();
    std::string reason;
    if( !present ) reason = seen == 0 ? "trace predates or did not emit JN Catalog" : "no valid JN Catalog definitions were found";
    else if( !complete ) reason = "catalog is present but contains invalid, unresolved, or privacy-unsafe records";
    return {
        { "present", present }, { "schema_version", 1 }, { "complete", complete },
        { "reason", reason.empty() ? json( nullptr ) : json( reason ) },
        { "active_connection_id", activeConnectionId ? json( Decimal( *activeConnectionId ) ) : json( nullptr ) },
        { "connection_ids", std::move( connections ) }, { "kinds", std::move( kinds ) },
        { "definitions", std::move( definitionArray ) }, { "entities", std::move( entityArray ) },
        { "quality", { { "invalid_count", invalid.size() }, { "privacy_violation_count", privacyViolations },
            { "unresolved_entity_count", unresolvedEntities }, { "duplicate_count", duplicates } } },
        { "invalid_records", std::move( invalid ) },
        { "records", { { "seen", seen }, { "valid", validRecords }, { "duplicates", duplicates },
            { "stale_connection", staleConnectionRecords },
            { "envelope_bytes", Decimal( envelopeBytes ) } } },
        { "limits", { { "maximum_records", MaximumCatalogRecords }, { "maximum_definitions", MaximumCatalogDefinitions },
            { "maximum_entities", MaximumCatalogEntities } } },
        { "trust", "untrusted_trace_data" }
    };
}

constexpr std::string_view GpuTaxonomyPrefix = "JNGT1|";
constexpr size_t MaximumGpuTaxonomyDefinitions = 256;

json GpuTaxonomyCatalogJson( const analysis::TraceInfoDto& info )
{
    std::optional<uint64_t> activeConnectionId;
    const auto captureIdentity = CaptureIdentityJson( info );
    if( captureIdentity.value( "present", false ) && captureIdentity.contains( "identity" ) &&
        captureIdentity["identity"].is_object() && captureIdentity["identity"].contains( "connection" ) &&
        captureIdentity["identity"]["connection"].is_object() &&
        captureIdentity["identity"]["connection"].contains( "id" ) )
        activeConnectionId = DecimalStringValue( captureIdentity["identity"]["connection"]["id"] );

    std::map<uint32_t, json> definitions;
    json invalid = json::array();
    json statusCapabilities = json::object();
    std::string sourceMode;
    size_t seen = 0;
    size_t validRecords = 0;
    size_t duplicates = 0;
    size_t staleConnectionRecords = 0;
    size_t catalogUnresolved = 0;
    size_t duplicatePartRecords = 0;
    std::optional<uint32_t> expectedPartCount;
    std::set<uint32_t> receivedParts;
    const auto addInvalid = [&]( size_t index, const std::string& reason ) {
        if( invalid.size() < 128 ) invalid.push_back( { { "record_index", Decimal( index ) }, { "reason", reason } } );
    };

    for( size_t recordIndex = 0; recordIndex < info.appInfo.size(); recordIndex++ )
    {
        const auto& record = info.appInfo[recordIndex];
        const bool currentEnvelope = record.starts_with( GpuTaxonomyPrefix );
        const bool taxonomyLike = currentEnvelope || ( record.size() >= 5 && record.compare( 0, 4, "JNGT" ) == 0 &&
            record.find( '|' ) != std::string::npos );
        if( !taxonomyLike ) continue;
        seen++;
        if( !currentEnvelope )
        {
            addInvalid( recordIndex, "unsupported GPU taxonomy envelope version" );
            continue;
        }
        if( record.size() <= GpuTaxonomyPrefix.size() || record.size() > MaximumIdentityEnvelopeBytes )
        {
            addInvalid( recordIndex, "GPU taxonomy envelope has an invalid size" );
            continue;
        }
        const auto document = json::parse( record.begin() + ptrdiff_t( GpuTaxonomyPrefix.size() ), record.end(), nullptr, false );
        if( document.is_discarded() || !document.is_object() || document.value( "schema_version", 0 ) != 2 ||
            !document.contains( "connection_id" ) || !document.contains( "source_mode" ) ||
            !document["source_mode"].is_string() || !document.contains( "status_capabilities" ) ||
            !document["status_capabilities"].is_object() || !document.contains( "definitions" ) ||
            !document["definitions"].is_array() || document["definitions"].size() > MaximumGpuTaxonomyDefinitions )
        {
            addInvalid( recordIndex, "GPU taxonomy envelope failed schema validation" );
            continue;
        }
        const auto connectionId = DecimalStringValue( document["connection_id"] );
        if( !connectionId )
        {
            addInvalid( recordIndex, "GPU taxonomy connection_id is invalid" );
            continue;
        }
        if( activeConnectionId && *connectionId != *activeConnectionId )
        {
            staleConnectionRecords++;
            continue;
        }
        const bool hasPartIndex = document.contains( "part_index" );
        const bool hasPartCount = document.contains( "part_count" );
        if( hasPartIndex != hasPartCount ||
            ( hasPartIndex && ( !document["part_index"].is_number_unsigned() ||
                !document["part_count"].is_number_unsigned() ) ) )
        {
            addInvalid( recordIndex, "GPU taxonomy part metadata is invalid" );
            continue;
        }
        const uint64_t partIndex64 = hasPartIndex ? document["part_index"].get<uint64_t>() : 0;
        const uint64_t partCount64 = hasPartCount ? document["part_count"].get<uint64_t>() : 1;
        if( partCount64 == 0 || partCount64 > 64 || partIndex64 >= partCount64 )
        {
            addInvalid( recordIndex, "GPU taxonomy part index/count is out of range" );
            continue;
        }
        const uint32_t partIndex = uint32_t( partIndex64 );
        const uint32_t partCount = uint32_t( partCount64 );
        if( !expectedPartCount ) expectedPartCount = partCount;
        else if( *expectedPartCount != partCount )
        {
            addInvalid( recordIndex, "GPU taxonomy part_count changed within one connection" );
            continue;
        }
        if( !receivedParts.emplace( partIndex ).second ) duplicatePartRecords++;
        if( sourceMode.empty() ) sourceMode = document["source_mode"].get<std::string>();
        else if( sourceMode != document["source_mode"].get<std::string>() )
            addInvalid( recordIndex, "GPU taxonomy source_mode changed within one connection" );
        if( statusCapabilities.empty() ) statusCapabilities = document["status_capabilities"];
        else if( statusCapabilities != document["status_capabilities"] )
            addInvalid( recordIndex, "GPU taxonomy status capabilities changed within one connection" );

        bool recordValid = true;
        for( auto definition : document["definitions"] )
        {
            if( !definition.is_object() || !definition.contains( "taxonomy_id" ) ||
                !definition.contains( "parent_id" ) || !definition.contains( "level" ) ||
                !definition["level"].is_number_unsigned() || definition["level"].get<uint64_t>() > 2 ||
                !definition.contains( "queue_mask" ) || !definition["queue_mask"].is_number_unsigned() ||
                definition["queue_mask"].get<uint64_t>() == 0 || definition["queue_mask"].get<uint64_t>() > 7 ||
                !definition.contains( "canonical_name" ) || !definition["canonical_name"].is_string() ||
                definition["canonical_name"].get_ref<const std::string&>().empty() ||
                definition["canonical_name"].get_ref<const std::string&>().size() > 192 ||
                !definition.contains( "catalog_definition_key" ) || !definition["catalog_definition_key"].is_string() )
            {
                addInvalid( recordIndex, "GPU taxonomy definition failed field validation" );
                recordValid = false;
                continue;
            }
            const auto taxonomyId64 = DecimalStringValue( definition["taxonomy_id"] );
            const auto parentId64 = DecimalStringValue( definition["parent_id"] );
            if( !taxonomyId64 || !parentId64 || *taxonomyId64 == 0 || *taxonomyId64 > std::numeric_limits<uint32_t>::max() ||
                *parentId64 > std::numeric_limits<uint32_t>::max() )
            {
                addInvalid( recordIndex, "GPU taxonomy identifier is invalid" );
                recordValid = false;
                continue;
            }
            const uint32_t taxonomyId = uint32_t( *taxonomyId64 );
            const uint32_t parentId = uint32_t( *parentId64 );
            const uint8_t level = definition["level"].get<uint8_t>();
            const bool encodedLevelValid = ( level == 0 && taxonomyId >= 0x00010001u && taxonomyId <= 0x00010003u ) ||
                ( level == 1 && ( taxonomyId & 0xF0000000u ) == 0x10000000u ) ||
                ( level == 2 && ( taxonomyId & 0xF0000000u ) == 0x20000000u );
            if( !encodedLevelValid || ( level == 0 ) != ( parentId == 0 ) )
            {
                addInvalid( recordIndex, "GPU taxonomy level/parent encoding is invalid" );
                recordValid = false;
                continue;
            }
            definition["taxonomy_id"] = Decimal( uint64_t( taxonomyId ) );
            definition["parent_id"] = Decimal( uint64_t( parentId ) );
            definition["evidence_kind"] = "producer_definition";
            const auto existing = definitions.find( taxonomyId );
            if( existing != definitions.end() )
            {
                if( existing->second != definition )
                {
                    addInvalid( recordIndex, "taxonomy_id maps to conflicting definitions" );
                    recordValid = false;
                }
                else duplicates++;
                continue;
            }
            definitions.emplace( taxonomyId, std::move( definition ) );
        }
        if( recordValid ) validRecords++;
    }

    for( const auto& [taxonomyId, definition] : definitions )
    {
        const uint8_t level = definition["level"].get<uint8_t>();
        const uint32_t parentId = uint32_t( *DecimalStringValue( definition["parent_id"] ) );
        if( level == 0 ) continue;
        const auto parent = definitions.find( parentId );
        if( parent == definitions.end() || parent->second["level"].get<uint8_t>() + 1 != level )
            invalid.push_back( { { "record_index", nullptr }, { "reason", "GPU taxonomy parent is unresolved or has the wrong level" },
                { "taxonomy_id", Decimal( uint64_t( taxonomyId ) ) }, { "parent_id", Decimal( uint64_t( parentId ) ) } } );
    }

    const auto catalog = CatalogJson( info );
    std::map<std::string, json> catalogDefinitions;
    if( catalog.value( "present", false ) )
        for( const auto& definition : catalog["definitions"] )
            catalogDefinitions.emplace( definition.value( "definition_key", "" ), definition );
    for( auto& [taxonomyId, definition] : definitions )
    {
        const auto key = definition.value( "catalog_definition_key", "" );
        const auto linked = catalogDefinitions.find( key );
        const bool catalogLinked = !key.empty() && linked != catalogDefinitions.end() &&
            linked->second.value( "kind", "" ) == "gpu_taxonomy" &&
            linked->second.value( "canonical_name", "" ) == definition.value( "canonical_name", "" ) &&
            linked->second["source"].value( "line", 0u ) == taxonomyId;
        definition["catalog_linked"] = catalogLinked;
        if( !catalogLinked ) catalogUnresolved++;
    }

    json definitionArray = json::array();
    json edges = json::array();
    std::array<size_t, 3> levels {};
    for( const auto& [taxonomyId, definition] : definitions )
    {
        definitionArray.push_back( definition );
        levels[definition["level"].get<uint8_t>()]++;
        const auto parentId = *DecimalStringValue( definition["parent_id"] );
        if( parentId != 0 ) edges.push_back( { { "parent_id", Decimal( parentId ) },
            { "child_id", Decimal( uint64_t( taxonomyId ) ) }, { "evidence_kind", "producer_definition" } } );
    }
    const size_t expectedParts = expectedPartCount.value_or( seen == 0 ? 0u : 1u );
    const size_t missingParts = expectedParts > receivedParts.size() ? expectedParts - receivedParts.size() : 0;
    const bool present = !definitions.empty();
    const bool complete = present && invalid.empty() && catalogUnresolved == 0 && missingParts == 0;
    std::string reason;
    if( !present ) reason = seen == 0 ? "trace predates or did not emit JN GPU Taxonomy" : "no valid JN GPU Taxonomy definition was found";
    else if( !complete ) reason = "GPU taxonomy is present but contains invalid hierarchy, missing parts or unresolved catalog links";
    return {
        { "present", present }, { "schema_version", 2 }, { "complete", complete },
        { "reason", reason.empty() ? json( nullptr ) : json( reason ) },
        { "active_connection_id", activeConnectionId ? json( Decimal( *activeConnectionId ) ) : json( nullptr ) },
        { "source_mode", sourceMode.empty() ? json( nullptr ) : json( sourceMode ) },
        { "status_capabilities", std::move( statusCapabilities ) }, { "definitions", std::move( definitionArray ) },
        { "logical_edges", std::move( edges ) },
        { "level_counts", { { "level0", levels[0] }, { "level1", levels[1] }, { "level2", levels[2] } } },
        { "quality", { { "invalid_count", invalid.size() }, { "duplicate_count", duplicates },
            { "catalog_unresolved_count", catalogUnresolved }, { "missing_part_count", missingParts } } },
        { "invalid_records", std::move( invalid ) },
        { "records", { { "seen", seen }, { "valid", validRecords }, { "duplicates", duplicates },
            { "stale_connection", staleConnectionRecords }, { "expected_parts", expectedParts },
            { "received_parts", receivedParts.size() }, { "missing_parts", missingParts },
            { "duplicate_part_records", duplicatePartRecords } } },
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

struct ExplicitGpuPassMatch
{
    analysis::GfxEntityDto pass;
    std::optional<analysis::GfxEntityDto> segment;
    std::optional<analysis::GpuZoneDto> zone;
    uint64_t commandListId = 0;
    uint64_t frameId = 0;
    uint64_t cameraId = 0;
    uint64_t viewId = 0;
    uint64_t referenceToken = 0;
    uint32_t taxonomyId = 0;
    bool taxonomyStableId = false;
    uint64_t bestDistance = std::numeric_limits<uint64_t>::max();
};

struct ExplicitGpuPassSet
{
    std::vector<ExplicitGpuPassMatch> matches;
    std::unordered_map<uint64_t, std::vector<size_t>> byGpuQuery;
};

uint64_t ExplicitGpuQueryKey( uint8_t context, uint32_t queryId )
{
    return ( uint64_t( context ) << 32 ) | queryId;
}

ExplicitGpuPassSet BuildExplicitGpuPassSet( const analysis::TraceSource& source, const json& taxonomy,
    const std::vector<analysis::GfxEntityDto>* entityOverride = nullptr,
    const std::vector<analysis::GfxLinkDto>* linkOverride = nullptr )
{
    ExplicitGpuPassSet result;
    std::vector<uint32_t> taxonomyIds;
    if( taxonomy.contains( "definitions" ) && taxonomy["definitions"].is_array() )
    {
        taxonomyIds.reserve( taxonomy["definitions"].size() );
        for( const auto& definition : taxonomy["definitions"] )
        {
            const auto value = definition.contains( "taxonomy_id" ) ? DecimalStringValue( definition["taxonomy_id"] ) : std::nullopt;
            taxonomyIds.emplace_back( value && *value <= std::numeric_limits<uint32_t>::max() ? uint32_t( *value ) : 0 );
        }
    }

    std::vector<analysis::GfxEntityDto> ownedEntities;
    std::vector<analysis::GfxLinkDto> ownedLinks;
    if( !entityOverride ) ownedEntities = source.GetGfxEntities();
    if( !linkOverride ) ownedLinks = source.GetGfxLinks();
    const auto& entities = entityOverride ? *entityOverride : ownedEntities;
    const auto& links = linkOverride ? *linkOverride : ownedLinks;
    std::unordered_map<uint64_t, analysis::GfxEntityDto> entityById;
    entityById.reserve( entities.size() );
    for( const auto& entity : entities ) entityById.emplace( entity.entityId, entity );
    std::unordered_map<uint64_t, size_t> passById;
    for( const auto& entity : entities )
    {
        if( entity.kind != 5 ) continue;
        ExplicitGpuPassMatch match;
        match.pass = entity;
        if( entity.gpuContext < taxonomyIds.size() ) match.taxonomyId = taxonomyIds[entity.gpuContext];
        passById.emplace( entity.entityId, result.matches.size() );
        result.matches.emplace_back( std::move( match ) );
    }
    for( const auto& link : links )
    {
        const auto found = passById.find( link.sourceId );
        if( found == passById.end() ) continue;
        auto& match = result.matches[found->second];
        switch( link.relation )
        {
        case 5:
        {
            const auto target = entityById.find( link.targetId );
            if( target != entityById.end() && target->second.kind == 4 ) match.segment = target->second;
            break;
        }
        case 7: match.commandListId = link.targetId; break;
        case 8: match.frameId = link.targetId; break;
        case 9: match.cameraId = link.targetId; break;
        case 10: match.viewId = link.targetId; break;
        case 11: match.referenceToken = link.targetId; break;
        case 12:
            if( link.targetId != 0 && link.targetId <= std::numeric_limits<uint32_t>::max() )
            {
                match.taxonomyId = uint32_t( link.targetId );
                match.taxonomyStableId = true;
            }
            break;
        default: break;
        }
    }
    for( size_t index = 0; index < result.matches.size(); ++index )
    {
        const auto& match = result.matches[index];
        if( !match.segment ) continue;
        result.byGpuQuery[ExplicitGpuQueryKey( match.segment->gpuContext, match.segment->gpuQueryId )].emplace_back( index );
    }
    return result;
}

void MatchExplicitGpuPassZone( const analysis::TraceSource& source, ExplicitGpuPassSet& passes,
    const analysis::GpuZoneDto& zone )
{
    const auto context = source.ParseEntityRef( zone.contextRef, "gpu-context" );
    if( !context || *context > std::numeric_limits<uint8_t>::max() ) return;
    const auto candidates = passes.byGpuQuery.find( ExplicitGpuQueryKey( uint8_t( *context ), zone.queryId ) );
    if( candidates == passes.byGpuQuery.end() ) return;
    for( const auto index : candidates->second )
    {
        auto& match = passes.matches[index];
        if( !match.segment ) continue;
        const uint64_t distance = zone.cpuStartNs >= match.segment->timeNs ?
            uint64_t( zone.cpuStartNs - match.segment->timeNs ) : uint64_t( match.segment->timeNs - zone.cpuStartNs );
        if( distance >= match.bestDistance ) continue;
        match.bestDistance = distance;
        match.zone = zone;
    }
}

json ExplicitGpuPassJson( const analysis::TraceSource& source, const ExplicitGpuPassMatch& match,
    const std::map<uint32_t, json>& taxonomyDefinitions )
{
    const char* sourceMode = ( match.pass.flags & 2 ) != 0 ? "managed-command-buffer" : "cpp-marker-command-list";
    json result = {
        { "ref", match.pass.ref }, { "pass_instance_id", Decimal( match.pass.entityId ) },
        { "pass_source_id", match.pass.gpuQueryId }, { "source_mode", sourceMode },
        { "taxonomy_index", match.pass.gpuContext }, { "taxonomy_id", Decimal( uint64_t( match.taxonomyId ) ) },
        { "taxonomy_evidence", match.taxonomyStableId ? "stable_id_relation" : "legacy_catalog_index" },
        { "logical_parent_id", Decimal( match.pass.parentId ) },
        { "logical_parent_ref", match.pass.parentId == 0 ? json( nullptr ) : json( source.MakeEntityRef( "gfx-entity", match.pass.parentId ) ) },
        { "command_list_id", Decimal( match.commandListId ) },
        { "command_list_ref", match.commandListId == 0 ? json( nullptr ) : json( source.MakeEntityRef( "gfx-entity", match.commandListId ) ) },
        { "frame_id", Decimal( match.frameId ) }, { "camera_id", Decimal( match.cameraId ) },
        { "view_id", Decimal( match.viewId ) }, { "reference_token", Decimal( match.referenceToken ) },
        { "gpu_segment_ref", match.segment ? json( match.segment->ref ) : json( nullptr ) },
        { "gpu_zone_ref", match.zone ? json( match.zone->ref ) : json( nullptr ) },
        { "complete", match.zone && match.zone->complete }, { "flags", match.pass.flags }
    };
    const auto definition = taxonomyDefinitions.find( match.taxonomyId );
    result["taxonomy_name"] = definition == taxonomyDefinitions.end() ? json( nullptr ) :
        json( definition->second.value( "canonical_name", "" ) );
    if( match.zone )
    {
        result["name"] = match.zone->name;
        result["source"] = { { "source_location_ref", match.zone->sourceLocationRef },
            { "file", match.zone->file }, { "function", match.zone->function }, { "line", match.zone->line } };
        result["gpu"] = { { "context_ref", match.zone->contextRef }, { "query_id", match.zone->queryId },
            { "gpu_start_ns", Decimal( match.zone->gpuStartNs ) },
            { "gpu_end_ns", match.zone->gpuEndNs ? json( Decimal( *match.zone->gpuEndNs ) ) : json( nullptr ) },
            { "cpu_record_begin_ns", Decimal( match.zone->cpuStartNs ) },
            { "cpu_record_end_ns", match.zone->cpuEndNs ? json( Decimal( *match.zone->cpuEndNs ) ) : json( nullptr ) } };
        result["callstack"] = match.zone->callstack == 0 ? json( nullptr ) : json( Decimal( uint64_t( match.zone->callstack ) ) );
        result["callstack_ref"] = match.zone->callstackRef ? json( *match.zone->callstackRef ) : json( nullptr );
    }
    else
    {
        result["name"] = nullptr;
        result["source"] = nullptr;
        result["gpu"] = nullptr;
        result["callstack"] = nullptr;
        result["callstack_ref"] = nullptr;
    }
    return result;
}

struct EvidenceNodeData
{
    std::string ref;
    std::string sourceRef;
    std::string key;
    std::string kind;
    std::string domain;
    std::string name;
    std::string threadRef;
    std::string contextRef;
    std::optional<std::string> sourceLocationRef;
    std::optional<std::string> callstackRef;
    int64_t sourceStartNs = 0;
    int64_t sourceEndNs = 0;
    int64_t startNs = 0;
    int64_t endNs = 0;
    bool complete = true;
    bool criticalEligible = true;
    json details = json::object();
};

struct EvidenceEdgeData
{
    std::string ref;
    size_t source = 0;
    size_t target = 0;
    std::string relation;
    std::string evidenceKind;
    std::string ruleId;
    double confidence = 0;
    int64_t timeDeltaNs = 0;
    bool complete = true;
    bool criticalEligible = false;
    json sourceFields = json::array();
};

struct EvidenceGraphBuilder
{
    static constexpr size_t InvalidIndex = std::numeric_limits<size_t>::max();

    EvidenceGraphBuilder( const analysis::TraceSource& traceSource, std::string frameEntityRef,
        int64_t beginNs, int64_t endNs, size_t nodeLimit, size_t edgeLimit,
        bool exact, bool derived, bool heuristic, std::set<std::string> selectedDomains )
        : source( traceSource )
        , frameRef( std::move( frameEntityRef ) )
        , frameBeginNs( beginNs )
        , frameEndNs( std::max( beginNs, endNs ) )
        , maxNodes( nodeLimit )
        , maxEdges( edgeLimit )
        , includeExact( exact )
        , includeDerived( derived )
        , includeHeuristic( heuristic )
        , domains( std::move( selectedDomains ) )
    {}

    bool DomainAllowed( const std::string& domain ) const
    {
        return domain == "frame" || domains.empty() || domains.contains( domain );
    }

    size_t AddNode( std::string key, std::string sourceRef, std::string kind, std::string domain,
        std::string name, int64_t sourceStartNs, int64_t sourceEndNs, std::string threadRef = {},
        std::string contextRef = {}, std::optional<std::string> sourceLocationRef = std::nullopt,
        std::optional<std::string> callstackRef = std::nullopt, bool complete = true,
        bool criticalEligible = true, json details = json::object(), bool preserveSourceRef = false )
    {
        if( !DomainAllowed( domain ) ) return InvalidIndex;
        if( const auto found = byKey.find( key ); found != byKey.end() ) return found->second;
        if( !BudgetConsumeNode() || nodes.size() >= maxNodes )
        {
            truncated = true;
            omittedNodes++;
            return InvalidIndex;
        }
        EvidenceNodeData value;
        value.key = std::move( key );
        value.sourceRef = std::move( sourceRef );
        value.ref = preserveSourceRef && !value.sourceRef.empty() ? value.sourceRef :
            source.MakeEntityRef( "evidence-node", nodes.size() + 1 );
        value.kind = std::move( kind );
        value.domain = std::move( domain );
        value.name = std::move( name );
        value.threadRef = std::move( threadRef );
        value.contextRef = std::move( contextRef );
        value.sourceLocationRef = std::move( sourceLocationRef );
        value.callstackRef = std::move( callstackRef );
        value.sourceStartNs = sourceStartNs;
        value.sourceEndNs = std::max( sourceStartNs, sourceEndNs );
        value.startNs = std::clamp( value.sourceStartNs, frameBeginNs, frameEndNs );
        value.endNs = std::clamp( value.sourceEndNs, frameBeginNs, frameEndNs );
        if( value.endNs < value.startNs ) value.endNs = value.startNs;
        value.complete = complete;
        value.criticalEligible = criticalEligible;
        value.details = std::move( details );
        const auto index = nodes.size();
        byKey.emplace( value.key, index );
        if( !value.sourceRef.empty() && !bySourceRef.contains( value.sourceRef ) ) bySourceRef.emplace( value.sourceRef, index );
        domainCounts[value.domain]++;
        nodes.emplace_back( std::move( value ) );
        return index;
    }

    size_t FindKey( std::string_view key ) const
    {
        const auto found = byKey.find( std::string( key ) );
        return found == byKey.end() ? InvalidIndex : found->second;
    }

    size_t FindSource( std::string_view ref ) const
    {
        const auto found = bySourceRef.find( std::string( ref ) );
        return found == bySourceRef.end() ? InvalidIndex : found->second;
    }

    void AddEdge( size_t sourceIndex, size_t targetIndex, std::string relation,
        std::string evidenceKind, std::string ruleId, double confidence,
        bool criticalEligible, json sourceFields = json::array(), bool complete = true )
    {
        if( sourceIndex == InvalidIndex || targetIndex == InvalidIndex || sourceIndex >= nodes.size() || targetIndex >= nodes.size() ) return;
        if( evidenceKind == "exact" && !includeExact ) return;
        if( evidenceKind == "derived" && !includeDerived ) return;
        if( evidenceKind == "heuristic" && !includeHeuristic ) return;
        const auto key = std::to_string( sourceIndex ) + '|' + std::to_string( targetIndex ) + '|' + relation + '|' + evidenceKind;
        if( !edgeKeys.emplace( key ).second ) return;
        if( !BudgetConsumeEdge() || edges.size() >= maxEdges )
        {
            truncated = true;
            omittedEdges++;
            return;
        }
        EvidenceEdgeData value;
        value.ref = source.MakeEntityRef( "evidence-edge", edges.size() + 1 );
        value.source = sourceIndex;
        value.target = targetIndex;
        value.relation = std::move( relation );
        value.evidenceKind = std::move( evidenceKind );
        value.ruleId = std::move( ruleId );
        value.confidence = confidence;
        value.timeDeltaNs = nodes[targetIndex].startNs - nodes[sourceIndex].endNs;
        value.complete = complete;
        value.criticalEligible = criticalEligible;
        value.sourceFields = std::move( sourceFields );
        evidenceCounts[value.evidenceKind]++;
        edges.emplace_back( std::move( value ) );
    }

    json NodeJson( size_t index ) const
    {
        const auto& value = nodes.at( index );
        return {
            { "ref", value.ref }, { "source_ref", value.sourceRef.empty() ? json( nullptr ) : json( value.sourceRef ) },
            { "kind", value.kind }, { "domain", value.domain }, { "name", value.name },
            { "frame_ref", frameRef }, { "thread_ref", value.threadRef.empty() ? json( nullptr ) : json( value.threadRef ) },
            { "context_ref", value.contextRef.empty() ? json( nullptr ) : json( value.contextRef ) },
            { "source_location_ref", value.sourceLocationRef ? json( *value.sourceLocationRef ) : json( nullptr ) },
            { "callstack_ref", value.callstackRef ? json( *value.callstackRef ) : json( nullptr ) },
            { "source_start_ns", Decimal( value.sourceStartNs ) }, { "source_end_ns", Decimal( value.sourceEndNs ) },
            { "start_ns", Decimal( value.startNs ) }, { "end_ns", Decimal( value.endNs ) },
            { "duration_ns", Decimal( std::max<int64_t>( 0, value.endNs - value.startNs ) ) },
            { "outside_frame", value.sourceEndNs < frameBeginNs || value.sourceStartNs > frameEndNs },
            { "complete", value.complete }, { "critical_eligible", value.criticalEligible },
            { "details", value.details }, { "trust", "untrusted_trace_data" }
        };
    }

    json EdgeJson( size_t index ) const
    {
        const auto& value = edges.at( index );
        return {
            { "ref", value.ref }, { "source_ref", nodes[value.source].ref }, { "target_ref", nodes[value.target].ref },
            { "relation", value.relation }, { "evidence_kind", value.evidenceKind },
            { "confidence", value.confidence }, { "rule_id", value.ruleId },
            { "source_fields", value.sourceFields }, { "time_delta_ns", Decimal( value.timeDeltaNs ) },
            { "complete", value.complete }, { "critical_eligible", value.criticalEligible }
        };
    }

    const analysis::TraceSource& source;
    std::string frameRef;
    int64_t frameBeginNs = 0;
    int64_t frameEndNs = 0;
    size_t maxNodes = 0;
    size_t maxEdges = 0;
    bool includeExact = true;
    bool includeDerived = true;
    bool includeHeuristic = false;
    bool truncated = false;
    uint64_t omittedNodes = 0;
    uint64_t omittedEdges = 0;
    size_t root = InvalidIndex;
    std::set<std::string> domains;
    std::vector<EvidenceNodeData> nodes;
    std::vector<EvidenceEdgeData> edges;
    std::unordered_map<std::string, size_t> byKey;
    std::unordered_map<std::string, size_t> bySourceRef;
    std::set<std::string> edgeKeys;
    std::map<std::string, uint64_t> domainCounts;
    std::map<std::string, uint64_t> evidenceCounts;
};

json EvidenceCriticalPathJson( const EvidenceGraphBuilder& graph )
{
    const auto count = graph.nodes.size();
    std::vector<std::vector<size_t>> outgoing( count );
    std::vector<size_t> indegree( count, 0 );
    for( size_t index = 0; index < graph.edges.size(); index++ )
    {
        const auto& edge = graph.edges[index];
        if( !edge.criticalEligible || !graph.nodes[edge.source].criticalEligible || !graph.nodes[edge.target].criticalEligible ) continue;
        outgoing[edge.source].emplace_back( index );
        indegree[edge.target]++;
    }
    std::queue<size_t> ready;
    for( size_t index = 0; index < count; index++ ) if( indegree[index] == 0 ) ready.push( index );
    constexpr int64_t Unreachable = std::numeric_limits<int64_t>::min() / 4;
    std::vector<int64_t> cost( count, Unreachable );
    std::vector<int64_t> coveredEnd( count, graph.frameBeginNs );
    std::vector<std::optional<size_t>> parentNode( count );
    std::vector<std::optional<size_t>> parentEdge( count );
    if( graph.root != EvidenceGraphBuilder::InvalidIndex && graph.root < count ) cost[graph.root] = 0;
    size_t processed = 0;
    while( !ready.empty() )
    {
        const auto current = ready.front();
        ready.pop();
        processed++;
        for( const auto edgeIndex : outgoing[current] )
        {
            const auto& edge = graph.edges[edgeIndex];
            const auto next = edge.target;
            if( cost[current] != Unreachable )
            {
                const auto& node = graph.nodes[next];
                const auto contribution = std::max<int64_t>( 0, node.endNs - std::max( node.startNs, coveredEnd[current] ) );
                const auto candidate = cost[current] + contribution;
                const auto candidateCoveredEnd = std::max( coveredEnd[current], node.endNs );
                if( candidate > cost[next] || ( candidate == cost[next] && candidateCoveredEnd < coveredEnd[next] ) )
                {
                    cost[next] = candidate;
                    coveredEnd[next] = candidateCoveredEnd;
                    parentNode[next] = current;
                    parentEdge[next] = edgeIndex;
                }
            }
            if( --indegree[next] == 0 ) ready.push( next );
        }
    }
    size_t end = graph.root;
    for( size_t index = 0; index < count; index++ ) if( cost[index] > ( end < count ? cost[end] : Unreachable ) ) end = index;
    std::vector<size_t> path;
    if( end < count && cost[end] != Unreachable )
        for( std::optional<size_t> current = end; current; current = parentNode[*current] ) path.emplace_back( *current );
    std::reverse( path.begin(), path.end() );
    json values = json::array();
    int64_t pathCoveredEnd = graph.frameBeginNs;
    int64_t total = 0;
    for( const auto index : path )
    {
        if( index == graph.root ) continue;
        const auto& node = graph.nodes[index];
        const auto raw = std::max<int64_t>( 0, node.endNs - node.startNs );
        const auto contribution = std::max<int64_t>( 0, node.endNs - std::max( node.startNs, pathCoveredEnd ) );
        pathCoveredEnd = std::max( pathCoveredEnd, node.endNs );
        total += contribution;
        auto value = graph.NodeJson( index );
        value["raw_duration_ns"] = Decimal( raw );
        value["wall_clock_contribution_ns"] = Decimal( contribution );
        value["overlap_excluded_ns"] = Decimal( raw - contribution );
        value["entering_edge_ref"] = parentEdge[index] ? json( graph.edges[*parentEdge[index]].ref ) : json( nullptr );
        value["entering_evidence_kind"] = parentEdge[index] ? json( graph.edges[*parentEdge[index]].evidenceKind ) : json( nullptr );
        values.emplace_back( std::move( value ) );
    }
    const auto frameDuration = std::max<int64_t>( 0, graph.frameEndNs - graph.frameBeginNs );
    const auto boundedTotal = std::min( total, frameDuration );
    return {
        { "nodes", std::move( values ) }, { "total_wall_clock_contribution_ns", Decimal( boundedTotal ) },
        { "frame_duration_ns", Decimal( frameDuration ) },
        { "temporal_coverage_ratio", frameDuration == 0 ? 0.0 : double( boundedTotal ) / double( frameDuration ) },
        { "overlap_accounting", "incremental_wall_clock_union_v1" },
        { "has_cycle", processed != count }, { "processed_nodes", processed }, { "total_nodes", count },
        { "valid_contribution", total <= frameDuration }
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
            { "default_max_scan_events", Decimal( DefaultMaxScanEvents ) }, { "maximum_max_scan_events", Decimal( MaximumMaxScanEvents ) },
            { "default_max_cpu_ms", Decimal( DefaultMaxCpuMs ) }, { "maximum_max_cpu_ms", Decimal( MaximumMaxCpuMs ) },
            { "default_max_nodes", Decimal( DefaultMaxNodes ) }, { "maximum_max_nodes", Decimal( MaximumMaxNodes ) },
            { "default_max_edges", Decimal( DefaultMaxEdges ) }, { "maximum_max_edges", Decimal( MaximumMaxEdges ) },
            { "default_max_groups", Decimal( DefaultMaxGroups ) }, { "maximum_max_groups", Decimal( MaximumMaxGroups ) },
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

    json descriptors = json::array();
    for( const auto& descriptor : QueryOperationSchemaRegistry() )
    {
        const auto method = descriptor.at( "method" ).get<std::string>();
        if( std::find( methods.begin(), methods.end(), method ) != methods.end() ) descriptors.emplace_back( descriptor );
    }
    result["operations"] = std::move( descriptors );
    return result;
}

struct FilteredScanPage
{
    json values = json::array();
    bool hasMore = false;
    size_t nextOffset = 0;
    size_t nextRawOffset = 0;
};

template<typename T, typename Scan, typename Match, typename Convert>
FilteredScanPage ScanFiltered( const analysis::TraceSource& source, const json& params, const PageRequest& page, Scan&& scan, Match&& match, Convert&& convert )
{
    FilteredScanPage result;
    size_t rawOffset = page.rawOffsetBound ? page.rawOffset : 0;
    size_t matchedOffset = page.rawOffsetBound ? page.offset : 0;
    constexpr size_t chunk = 4096;
    bool exhausted = false;
    while( !result.hasMore && !exhausted )
    {
        const auto allowed = BudgetScanAllowance( chunk );
        if( allowed == 0 )
        {
            result.hasMore = BudgetPartial();
            break;
        }
        auto range = ScanRangeFrom( params, rawOffset, allowed );
        const std::vector<T> values = scan( source, range );
        BudgetScanned( values.size(), chunk, allowed );
        exhausted = values.size() < allowed;
        for( size_t index = 0; index < values.size(); index++ )
        {
            const auto& value = values[index];
            if( !match( value ) ) continue;
            if( matchedOffset++ < page.offset ) continue;
            if( result.values.size() == page.limit )
            {
                result.hasMore = true;
                result.nextRawOffset = rawOffset + index;
                break;
            }
            result.values.emplace_back( convert( value ) );
        }
        rawOffset += values.size();
        if( values.empty() ) exhausted = true;
    }
    if( !result.hasMore && BudgetPartial() ) result.hasMore = true;
    result.nextOffset = page.offset + result.values.size();
    if( result.nextRawOffset == 0 || !result.hasMore ) result.nextRawOffset = rawOffset;
    return result;
}

const json* JsonPointerValue( const json& value, const char* pointer )
{
    try
    {
        const auto& result = value.at( json::json_pointer( pointer ) );
        return result.is_null() ? nullptr : &result;
    }
    catch( const std::exception& )
    {
        return nullptr;
    }
}

std::optional<uint64_t> NonNegativeJsonInteger( const json* value )
{
    if( value == nullptr ) return std::nullopt;
    if( value->is_number_unsigned() ) return value->get<uint64_t>();
    if( value->is_number_integer() )
    {
        const auto result = value->get<int64_t>();
        return result < 0 ? std::nullopt : std::optional<uint64_t>( uint64_t( result ) );
    }
    if( value->is_string() ) return DecimalStringValue( *value );
    return std::nullopt;
}

struct ComparisonFrameWindow
{
    bool valid = false;
    std::string reason;
    analysis::FrameSetDto baselineSet;
    analysis::FrameSetDto candidateSet;
    std::vector<analysis::FrameDto> baselineFrames;
    std::vector<analysis::FrameDto> candidateFrames;
    size_t warmupFrames = 0;
    size_t frameCount = 0;
    analysis::ScanRange baselineRange;
    analysis::ScanRange candidateRange;
};

ComparisonFrameWindow SelectComparisonFrameWindow( const analysis::TraceSource& baseline,
    const analysis::TraceSource& candidate, const json& params, const json& baselineContext,
    const json& candidateContext )
{
    ComparisonFrameWindow result;
    const auto baselineSets = baseline.GetFrameSets();
    const auto candidateSets = candidate.GetFrameSets();
    if( baselineSets.empty() || candidateSets.empty() )
    {
        result.reason = "one or both traces contain no frame sets";
        return result;
    }

    const auto normalized = []( const std::string& value ) { return Lower( NormalizeSourceKey( value ) ); };
    const analysis::FrameSetDto* selectedBaseline = nullptr;
    const analysis::FrameSetDto* selectedCandidate = nullptr;
    const auto selectByName = [&]( const std::string& name ) {
        const auto key = normalized( name );
        const auto left = std::find_if( baselineSets.begin(), baselineSets.end(), [&]( const auto& value ) { return normalized( value.name ) == key; } );
        const auto right = std::find_if( candidateSets.begin(), candidateSets.end(), [&]( const auto& value ) { return normalized( value.name ) == key; } );
        if( left == baselineSets.end() || right == candidateSets.end() ) return false;
        selectedBaseline = &*left;
        selectedCandidate = &*right;
        return true;
    };

    if( params.contains( "frame_set" ) )
    {
        const auto& requested = params["frame_set"];
        if( requested.is_string() )
        {
            const auto text = requested.get<std::string>();
            if( !selectByName( text ) )
            {
                const auto left = std::find_if( baselineSets.begin(), baselineSets.end(), [&]( const auto& value ) { return value.ref == text; } );
                if( left == baselineSets.end() || !selectByName( left->name ) )
                {
                    result.reason = "requested frame set is not present in both traces";
                    return result;
                }
            }
        }
        else if( requested.is_number_integer() || requested.is_number_unsigned() )
        {
            const auto index = requested.get<int64_t>();
            if( index < 0 || size_t( index ) >= baselineSets.size() || !selectByName( baselineSets[size_t( index )].name ) )
            {
                result.reason = "requested frame set index is invalid or has no candidate match";
                return result;
            }
        }
        else
        {
            result.reason = "frame_set must be a name, ref, or non-negative index";
            return result;
        }
    }
    else if( !selectByName( "Player.Frame" ) && !selectByName( "Editor.Frame" ) )
    {
        for( const auto& left : baselineSets )
        {
            if( !left.continuous ) continue;
            const auto right = std::find_if( candidateSets.begin(), candidateSets.end(), [&]( const auto& value ) {
                return value.continuous && normalized( value.name ) == normalized( left.name );
            } );
            if( right == candidateSets.end() ) continue;
            selectedBaseline = &left;
            selectedCandidate = &*right;
            break;
        }
    }

    if( selectedBaseline == nullptr || selectedCandidate == nullptr )
    {
        result.reason = "no common Player.Frame, Editor.Frame, or continuous frame set was found";
        return result;
    }
    result.baselineSet = *selectedBaseline;
    result.candidateSet = *selectedCandidate;

    const auto completeFrames = []( const analysis::TraceSource& source, const analysis::FrameSetDto& set ) {
        std::vector<analysis::FrameDto> output;
        const auto frames = source.GetFramesForSet( set.index, 0, set.frameCount );
        output.reserve( frames.size() );
        for( const auto& frame : frames ) if( frame.complete && frame.endNs && *frame.endNs >= frame.beginNs ) output.emplace_back( frame );
        return output;
    };
    auto baselineFrames = completeFrames( baseline, *selectedBaseline );
    auto candidateFrames = completeFrames( candidate, *selectedCandidate );

    size_t warmup = 0;
    if( params.contains( "warmup_frames" ) )
    {
        if( !params["warmup_frames"].is_number_integer() && !params["warmup_frames"].is_number_unsigned() )
        {
            result.reason = "warmup_frames must be a non-negative integer";
            return result;
        }
        const auto requested = params["warmup_frames"].get<int64_t>();
        if( requested < 0 )
        {
            result.reason = "warmup_frames must be a non-negative integer";
            return result;
        }
        warmup = size_t( requested );
    }
    else
    {
        const auto left = NonNegativeJsonInteger( JsonPointerValue( baselineContext, "/context/workload/warmup_frames" ) ).value_or( 0 );
        const auto right = NonNegativeJsonInteger( JsonPointerValue( candidateContext, "/context/workload/warmup_frames" ) ).value_or( 0 );
        warmup = size_t( std::max( left, right ) );
    }
    if( baselineFrames.size() <= warmup || candidateFrames.size() <= warmup )
    {
        result.reason = "warmup removes every complete frame from one or both traces";
        return result;
    }
    baselineFrames.erase( baselineFrames.begin(), baselineFrames.begin() + ptrdiff_t( warmup ) );
    candidateFrames.erase( candidateFrames.begin(), candidateFrames.begin() + ptrdiff_t( warmup ) );
    const size_t available = std::min( baselineFrames.size(), candidateFrames.size() );
    size_t count = std::min<size_t>( available, 300 );
    if( params.contains( "window_frames" ) )
    {
        if( !params["window_frames"].is_number_integer() && !params["window_frames"].is_number_unsigned() )
        {
            result.reason = "window_frames must be a non-negative integer";
            return result;
        }
        const auto requested = params["window_frames"].get<int64_t>();
        if( requested < 0 )
        {
            result.reason = "window_frames must be a non-negative integer";
            return result;
        }
        if( requested != 0 )
        {
            if( uint64_t( requested ) > available )
            {
                result.reason = "requested frame window exceeds the common complete-frame count";
                return result;
            }
            count = size_t( requested );
        }
    }
    if( count == 0 )
    {
        result.reason = "the common complete-frame window is empty";
        return result;
    }
    baselineFrames.resize( count );
    candidateFrames.resize( count );
    result.baselineRange.startNs = baselineFrames.front().beginNs;
    result.baselineRange.endNs = *baselineFrames.back().endNs + 1;
    result.candidateRange.startNs = candidateFrames.front().beginNs;
    result.candidateRange.endNs = *candidateFrames.back().endNs + 1;
    result.baselineRange.limit = std::numeric_limits<size_t>::max();
    result.candidateRange.limit = std::numeric_limits<size_t>::max();
    result.baselineFrames = std::move( baselineFrames );
    result.candidateFrames = std::move( candidateFrames );
    result.warmupFrames = warmup;
    result.frameCount = count;
    result.valid = true;
    return result;
}

json ComparisonWindowJson( const ComparisonFrameWindow& value )
{
    if( !value.valid ) return { { "valid", false }, { "reason", value.reason } };
    return {
        { "valid", true }, { "alignment", "complete-frame ordinal" }, { "frame_set", value.baselineSet.name },
        { "warmup_frames", value.warmupFrames }, { "window_frames", value.frameCount },
        { "baseline", { { "frame_set_ref", value.baselineSet.ref }, { "first_index", value.baselineFrames.front().index },
            { "last_index", value.baselineFrames.back().index }, { "start_ns", Decimal( value.baselineRange.startNs ) },
            { "end_ns", Decimal( value.baselineRange.endNs ) } } },
        { "candidate", { { "frame_set_ref", value.candidateSet.ref }, { "first_index", value.candidateFrames.front().index },
            { "last_index", value.candidateFrames.back().index }, { "start_ns", Decimal( value.candidateRange.startNs ) },
            { "end_ns", Decimal( value.candidateRange.endNs ) } } }
    };
}

struct N11TraceData
{
    bool scriptPresent = false;
    bool scriptCapability = false;
    uint8_t scriptSchema = 0;
    bool gcPresent = false;
    bool gcCapability = false;
    uint64_t scriptInvalid = 0;
    uint64_t scriptUnresolved = 0;
    uint64_t scriptOrphanEnds = 0;
    uint64_t gcInvalid = 0;
    std::map<uint32_t, json> frames;
    std::map<uint64_t, json> stacks;
    std::map<uint32_t, json> markers;
    std::vector<json> zones;
    std::vector<json> gcEvents;
};

bool JsonUnsigned( const json& document, const char* key, uint64_t& value, uint64_t maximum = std::numeric_limits<uint64_t>::max() )
{
    if( !document.contains( key ) ) return false;
    const auto& item = document[key];
    if( item.is_number_unsigned() ) value = item.get<uint64_t>();
    else if( item.is_number_integer() )
    {
        const auto signedValue = item.get<int64_t>();
        if( signedValue < 0 ) return false;
        value = uint64_t( signedValue );
    }
    else return false;
    return value <= maximum;
}

bool JsonDecimalString( const json& document, const char* key, uint64_t& value )
{
    if( !document.contains( key ) || !document[key].is_string() ) return false;
    const auto& text = document[key].get_ref<const std::string&>();
    if( text.empty() ) return false;
    const auto parsed = std::from_chars( text.data(), text.data() + text.size(), value, 10 );
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

json ParseN11Record( const std::string& text, std::string_view prefix )
{
    if( !text.starts_with( prefix ) ) return json();
    auto document = json::parse( text.begin() + prefix.size(), text.end(), nullptr, false );
    if( document.is_discarded() || !document.is_object() ) return json();
    uint64_t schema = 0;
    if( !JsonUnsigned( document, "schema_version", schema, 1 ) || schema != 1 ) return json();
    return document;
}

const char* ScriptRuntimeName( uint64_t runtime )
{
    return runtime == 1 ? "managed" : runtime == 2 ? "lua" : "unknown";
}

bool ScriptRuntimeValid( uint64_t runtime )
{
    return runtime == 1 || runtime == 2;
}

const char* GcKindName( uint64_t kind )
{
    switch( kind )
    {
    case 1: return "managed_heap_used";
    case 2: return "managed_heap_reserved";
    case 3: return "managed_allocation_bytes";
    case 4: return "managed_collection_begin";
    case 5: return "managed_collection_end";
    case 6: return "managed_collection_observed";
    case 7: return "managed_incremental_slice_begin";
    case 8: return "managed_incremental_slice_end";
    case 16: return "lua_heap_used";
    case 17: return "lua_cycle_begin";
    case 18: return "lua_cycle_end";
    case 19: return "lua_slice";
    case 20: return "lua_pause";
    case 21: return "lua_reload_generation";
    default: return "unknown";
    }
}

N11TraceData ParseN11Trace( const analysis::TraceSource& source, const analysis::TraceInfoDto& info )
{
    N11TraceData result;
    const auto binaryFrames = source.GetScriptFrames();
    const auto binaryEvents = source.GetScriptStackEvents();
    const bool binaryScript = !binaryFrames.empty() || !binaryEvents.empty();
    std::map<uint64_t, size_t> openZones;
    if( binaryScript )
    {
        result.scriptPresent = true;
        result.scriptCapability = true;
        result.scriptSchema = 2;
        std::unordered_map<uint64_t, uint64_t> frameByScriptZone;
        for( const auto& relation : source.GetRelations() )
        {
            if( relation.relationNamespace == 4 && relation.relation == 1 &&
                relation.sourceKind == 1 && relation.targetKind == 11 )
                frameByScriptZone[relation.targetId] = relation.sourceId;
        }
        for( const auto& frame : binaryFrames )
        {
            if( frame.frameId == 0 || !ScriptRuntimeValid( frame.runtime ) || frame.function.empty() ||
                result.frames.contains( frame.frameId ) )
            {
                result.scriptInvalid++;
                continue;
            }
            result.frames.emplace( frame.frameId, json {
                { "schema_version", 2 }, { "record", "frame" }, { "frame_id", frame.frameId },
                { "runtime", ScriptRuntimeName( frame.runtime ) }, { "function", frame.function },
                { "file", frame.file }, { "line", frame.line }, { "flags", frame.flags },
                { "source_mode", "binary-script-schema-2" }, { "trust", "untrusted_trace_data" }
            } );
        }

        struct PendingStack
        {
            uint8_t runtime = 0;
            uint8_t flags = 0;
            std::vector<uint32_t> frames;
            bool header = false;
        };
        std::map<uint64_t, PendingStack> pendingStacks;
        for( const auto& event : binaryEvents )
        {
            if( event.primaryId == 0 || ( event.kind != 5 && !ScriptRuntimeValid( event.runtime ) ) )
            {
                result.scriptInvalid++;
                continue;
            }
            switch( event.kind )
            {
            case 1:
            {
                if( event.value == 0 || event.value > 64 || pendingStacks.contains( event.primaryId ) )
                {
                    result.scriptInvalid++;
                    break;
                }
                auto& stack = pendingStacks[event.primaryId];
                stack.runtime = event.runtime;
                stack.flags = event.flags;
                stack.frames.resize( event.value );
                stack.header = true;
                break;
            }
            case 2:
            {
                auto found = pendingStacks.find( event.primaryId );
                if( found == pendingStacks.end() || !found->second.header || event.secondaryId == 0 ||
                    event.secondaryId > std::numeric_limits<uint32_t>::max() || event.value >= found->second.frames.size() ||
                    found->second.frames[event.value] != 0 )
                {
                    result.scriptInvalid++;
                    break;
                }
                found->second.frames[event.value] = uint32_t( event.secondaryId );
                break;
            }
            case 3:
            {
                const auto markerId = uint32_t( event.primaryId );
                const auto color = uint32_t( event.primaryId >> 32 );
                if( markerId == 0 || event.value == 0 || event.text.empty() )
                {
                    result.scriptInvalid++;
                    break;
                }
                json marker = {
                    { "schema_version", 2 }, { "record", "marker" }, { "marker_id", markerId },
                    { "runtime", ScriptRuntimeName( event.runtime ) }, { "name", event.text },
                    { "source_frame_id", event.value }, { "color", color }, { "flags", event.flags },
                    { "source_mode", "binary-script-schema-2" }, { "trust", "untrusted_trace_data" }
                };
                const auto found = result.markers.find( markerId );
                if( found != result.markers.end() )
                {
                    const auto& existing = found->second;
                    const bool identical = existing.value( "runtime", "" ) == marker.value( "runtime", "" ) &&
                        existing.value( "source_frame_id", 0u ) == event.value &&
                        existing.value( "color", 0u ) == color && existing.value( "flags", 0u ) == uint32_t( event.flags ) &&
                        existing.value( "name", "" ) == event.text;
                    if( !identical ) result.scriptInvalid++;
                    break;
                }
                result.markers.emplace( markerId, std::move( marker ) );
                break;
            }
            case 4:
            {
                if( event.secondaryId == 0 || event.value == 0 || openZones.contains( event.primaryId ) )
                {
                    result.scriptInvalid++;
                    break;
                }
                json zone = {
                    { "schema_version", 2 }, { "zone_id", std::to_string( event.primaryId ) },
                    { "marker_id", event.value }, { "stack_id", std::to_string( event.secondaryId ) },
                    { "runtime_id", event.runtime }, { "runtime", ScriptRuntimeName( event.runtime ) },
                    { "phase", "begin" }, { "frame_id", std::to_string( frameByScriptZone[event.primaryId] ) }, { "flags", event.flags },
                    { "start_ns", Decimal( event.timeNs ) }, { "end_ns", nullptr }, { "duration_ns", nullptr },
                    { "thread_ref", event.threadRef }, { "event_ref", event.ref }, { "complete", false },
                    { "source_mode", "binary-script-schema-2" }, { "trust", "untrusted_trace_data" }
                };
                openZones.emplace( event.primaryId, result.zones.size() );
                result.zones.emplace_back( std::move( zone ) );
                break;
            }
            case 5:
            {
                const auto found = openZones.find( event.primaryId );
                if( found == openZones.end() )
                {
                    result.scriptOrphanEnds++;
                    break;
                }
                auto& zone = result.zones[found->second];
                const auto start = std::stoll( zone["start_ns"].get<std::string>() );
                if( event.timeNs < start )
                {
                    result.scriptInvalid++;
                    break;
                }
                zone["end_ns"] = Decimal( event.timeNs );
                zone["duration_ns"] = Decimal( event.timeNs - start );
                zone["complete"] = true;
                openZones.erase( found );
                break;
            }
            default:
                result.scriptInvalid++;
                break;
            }
        }
        for( auto& [stackId, stack] : pendingStacks )
        {
            if( !stack.header || std::any_of( stack.frames.begin(), stack.frames.end(), []( uint32_t value ) { return value == 0; } ) )
            {
                result.scriptUnresolved++;
                continue;
            }
            json frameIds = json::array();
            for( const auto frameId : stack.frames ) frameIds.push_back( frameId );
            result.stacks.emplace( stackId, json {
                { "schema_version", 2 }, { "record", "stack" }, { "stack_id", std::to_string( stackId ) },
                { "runtime", ScriptRuntimeName( stack.runtime ) }, { "flags", stack.flags },
                { "frame_ids", std::move( frameIds ) }, { "source_mode", "binary-script-schema-2" },
                { "trust", "untrusted_trace_data" }
            } );
        }
    }
    for( const auto& record : info.appInfo )
    {
        if( record.starts_with( "JNSTK1|" ) )
        {
            result.scriptPresent = true;
            if( result.scriptSchema == 0 ) result.scriptSchema = 1;
            auto document = ParseN11Record( record, "JNSTK1|" );
            if( document.empty() || !document.contains( "record" ) || !document["record"].is_string() )
            {
                result.scriptInvalid++;
                continue;
            }
            const auto kind = document["record"].get<std::string>();
            if( kind == "capability" )
            {
                result.scriptCapability = true;
                continue;
            }
            if( kind == "frame" )
            {
                if( binaryScript ) continue;
                uint64_t id = 0, line = 0, flags = 0;
                const auto runtime = document.value( "runtime", "" );
                if( !JsonUnsigned( document, "frame_id", id, std::numeric_limits<uint32_t>::max() ) || id == 0 ||
                    !JsonUnsigned( document, "line", line, std::numeric_limits<uint32_t>::max() ) ||
                    !JsonUnsigned( document, "flags", flags, 255 ) ||
                    ( runtime != "managed" && runtime != "lua" ) ||
                    !document.contains( "function" ) || !document["function"].is_string() || document["function"].get_ref<const std::string&>().empty() ||
                    !document.contains( "file" ) || !document["file"].is_string() || result.frames.contains( uint32_t( id ) ) )
                {
                    result.scriptInvalid++;
                    continue;
                }
                document["trust"] = "untrusted_trace_data";
                result.frames.emplace( uint32_t( id ), std::move( document ) );
            }
            else if( kind == "stack" )
            {
                if( binaryScript ) continue;
                uint64_t id = 0, flags = 0;
                const auto runtime = document.value( "runtime", "" );
                if( !JsonDecimalString( document, "stack_id", id ) || id == 0 ||
                    !JsonUnsigned( document, "flags", flags, 255 ) ||
                    ( runtime != "managed" && runtime != "lua" ) ||
                    !document.contains( "frame_ids" ) || !document["frame_ids"].is_array() ||
                    document["frame_ids"].empty() || document["frame_ids"].size() > 64 || result.stacks.contains( id ) )
                {
                    result.scriptInvalid++;
                    continue;
                }
                bool validFrames = true;
                for( const auto& frame : document["frame_ids"] )
                    validFrames &= frame.is_number_unsigned() || ( frame.is_number_integer() && frame.get<int64_t>() > 0 );
                if( !validFrames )
                {
                    result.scriptInvalid++;
                    continue;
                }
                document["trust"] = "untrusted_trace_data";
                result.stacks.emplace( id, std::move( document ) );
            }
            else if( kind == "marker" )
            {
                uint64_t id = 0, sourceFrame = 0, color = 0, flags = 0;
                const auto runtime = document.value( "runtime", "" );
                if( !JsonUnsigned( document, "marker_id", id, std::numeric_limits<uint32_t>::max() ) || id == 0 ||
                    !JsonUnsigned( document, "source_frame_id", sourceFrame, std::numeric_limits<uint32_t>::max() ) || sourceFrame == 0 ||
                    !JsonUnsigned( document, "color", color, std::numeric_limits<uint32_t>::max() ) ||
                    !JsonUnsigned( document, "flags", flags, 255 ) ||
                    ( runtime != "managed" && runtime != "lua" ) ||
                    !document.contains( "name" ) || !document["name"].is_string() || document["name"].get_ref<const std::string&>().empty() )
                {
                    result.scriptInvalid++;
                    continue;
                }
                const auto markerId = uint32_t( id );
                const auto found = result.markers.find( markerId );
                if( found != result.markers.end() )
                {
                    const auto& existing = found->second;
                    const bool identical = existing.value( "runtime", "" ) == runtime &&
                        existing.value( "source_frame_id", 0u ) == uint32_t( sourceFrame ) &&
                        existing.value( "color", 0u ) == uint32_t( color ) &&
                        existing.value( "flags", 0u ) == uint32_t( flags ) &&
                        existing.value( "name", "" ) == document.value( "name", "" );
                    if( identical ) continue;
                    result.scriptInvalid++;
                    continue;
                }
                document["trust"] = "untrusted_trace_data";
                result.markers.emplace( markerId, std::move( document ) );
            }
            else result.scriptInvalid++;
        }
        else if( record.starts_with( "JNGC1|" ) )
        {
            result.gcPresent = true;
            auto document = ParseN11Record( record, "JNGC1|" );
            if( document.empty() || document.value( "record", "" ) != "capability" ) result.gcInvalid++;
            else result.gcCapability = true;
        }
    }

    size_t rawOffset = 0;
    constexpr size_t chunk = 4096;
    while( true )
    {
        const auto allowed = BudgetScanAllowance( chunk );
        if( allowed == 0 ) break;
        analysis::ScanRange range;
        range.offset = rawOffset;
        range.limit = allowed;
        const auto messages = source.ScanMessages( range );
        BudgetScanned( messages.size(), chunk, allowed );
        for( const auto& message : messages )
        {
            if( message.text.starts_with( "JNSZ1|" ) )
            {
                if( binaryScript ) continue;
                result.scriptPresent = true;
                if( result.scriptSchema == 0 ) result.scriptSchema = 1;
                auto document = ParseN11Record( message.text, "JNSZ1|" );
                uint64_t zoneId = 0;
                if( document.empty() || !JsonDecimalString( document, "zone_id", zoneId ) || zoneId == 0 ||
                    !document.contains( "phase" ) || !document["phase"].is_string() )
                {
                    result.scriptInvalid++;
                    continue;
                }
                const auto phase = document["phase"].get<std::string>();
                if( phase == "begin" )
                {
                    uint64_t markerId = 0, stackId = 0, runtime = 0, flags = 0, frameId = 0;
                    if( !JsonUnsigned( document, "marker_id", markerId, std::numeric_limits<uint32_t>::max() ) || markerId == 0 ||
                        !JsonDecimalString( document, "stack_id", stackId ) || stackId == 0 ||
                        !JsonUnsigned( document, "runtime", runtime, 2 ) || !ScriptRuntimeValid( runtime ) ||
                        !JsonUnsigned( document, "flags", flags, 255 ) || !JsonDecimalString( document, "frame_id", frameId ) ||
                        openZones.contains( zoneId ) )
                    {
                        result.scriptInvalid++;
                        continue;
                    }
                    document["runtime_id"] = runtime;
                    document["runtime"] = ScriptRuntimeName( runtime );
                    document["start_ns"] = Decimal( message.timeNs );
                    document["end_ns"] = nullptr;
                    document["duration_ns"] = nullptr;
                    document["thread_ref"] = message.threadRef;
                    document["message_ref"] = message.ref;
                    document["complete"] = false;
                    document["trust"] = "untrusted_trace_data";
                    openZones.emplace( zoneId, result.zones.size() );
                    result.zones.emplace_back( std::move( document ) );
                }
                else if( phase == "end" )
                {
                    const auto found = openZones.find( zoneId );
                    if( found == openZones.end() )
                    {
                        result.scriptOrphanEnds++;
                        continue;
                    }
                    auto& zone = result.zones[found->second];
                    if( !zone["end_ns"].is_null() || message.timeNs < std::stoll( zone["start_ns"].get<std::string>() ) )
                    {
                        result.scriptInvalid++;
                        continue;
                    }
                    const auto start = std::stoll( zone["start_ns"].get<std::string>() );
                    zone["end_ns"] = Decimal( message.timeNs );
                    zone["duration_ns"] = Decimal( message.timeNs - start );
                    zone["complete"] = true;
                    openZones.erase( found );
                }
                else result.scriptInvalid++;
            }
            else if( message.text.starts_with( "JNGC1|" ) )
            {
                result.gcPresent = true;
                auto document = ParseN11Record( message.text, "JNGC1|" );
                uint64_t eventId = 0, runtime = 0, kind = 0, generation = 0, flags = 0, value = 0, frameId = 0;
                if( document.empty() || !JsonDecimalString( document, "event_id", eventId ) || eventId == 0 ||
                    !JsonUnsigned( document, "runtime", runtime, 2 ) || !ScriptRuntimeValid( runtime ) ||
                    !JsonUnsigned( document, "kind", kind, 21 ) ||
                    !JsonUnsigned( document, "generation", generation, 255 ) ||
                    !JsonUnsigned( document, "flags", flags, 31 ) ||
                    !JsonDecimalString( document, "value", value ) || !JsonDecimalString( document, "frame_id", frameId ) ||
                    ( runtime == 1 && ( kind < 1 || kind > 8 ) ) || ( runtime == 2 && ( kind < 16 || kind > 21 ) ) )
                {
                    result.gcInvalid++;
                    continue;
                }
                document["runtime_id"] = runtime;
                document["runtime"] = ScriptRuntimeName( runtime );
                document["kind_name"] = GcKindName( kind );
                document["time_ns"] = Decimal( message.timeNs );
                document["thread_ref"] = message.threadRef;
                document["message_ref"] = message.ref;
                document["trust"] = "untrusted_trace_data";
                result.gcEvents.emplace_back( std::move( document ) );
            }
        }
        rawOffset += messages.size();
        if( messages.size() < allowed ) break;
    }

    for( auto& [stackId, stack] : result.stacks )
    {
        json expanded = json::array();
        for( const auto& frameValue : stack["frame_ids"] )
        {
            const auto frameId = uint32_t( frameValue.get<uint64_t>() );
            const auto found = result.frames.find( frameId );
            if( found == result.frames.end() || found->second.value( "runtime", "" ) != stack.value( "runtime", "" ) ) result.scriptUnresolved++;
            else expanded.emplace_back( found->second );
        }
        stack["frames"] = std::move( expanded );
        stack["complete"] = stack["frames"].size() == stack["frame_ids"].size();
    }
    for( auto& [markerId, marker] : result.markers )
    {
        const auto frameId = uint32_t( marker.value( "source_frame_id", 0u ) );
        const auto found = result.frames.find( frameId );
        if( found == result.frames.end() || found->second.value( "runtime", "" ) != marker.value( "runtime", "" ) ) result.scriptUnresolved++;
        else marker["source_frame"] = found->second;
    }
    for( auto& zone : result.zones )
    {
        const auto markerId = uint32_t( zone.value( "marker_id", 0u ) );
        uint64_t stackId = 0;
        JsonDecimalString( zone, "stack_id", stackId );
        const auto marker = result.markers.find( markerId );
        const auto stack = result.stacks.find( stackId );
        if( marker == result.markers.end() || stack == result.stacks.end() ||
            marker->second.value( "runtime", "" ) != zone.value( "runtime", "" ) ||
            stack->second.value( "runtime", "" ) != zone.value( "runtime", "" ) ) result.scriptUnresolved++;
        else
        {
            zone["marker"] = marker->second;
            zone["stack"] = stack->second;
        }
    }
    return result;
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

std::shared_ptr<const analysis::GpuMemoryAttribution> QueryService::CachedGpuAttribution( const std::string& traceId,
    const std::shared_ptr<analysis::TraceSource>& source, bool summaryOnly )
{
    const auto key = traceId + ( summaryOnly ? "|gpu-summary-attribution" : "|gpu-attribution" );
    const auto found = m_gpuCache.find( key );
    if( found != m_gpuCache.end() ) { found->second.access = ++m_cacheClock; return found->second.value; }
    auto value = std::make_shared<analysis::GpuMemoryAttribution>( summaryOnly ? source->GetGpuMemorySummaryAttribution() : source->GetGpuMemoryAttribution() );
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
        QueryBudgetState budget( params, stopToken );
        QueryBudgetScope budgetScope( budget );
        auto response = Dispatch( id, request["method"].get<std::string>(), params, defaultTraceId, stopToken );
        budget.Attach( response );
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
        { "operations", QueryOperationSchemaRegistry() },
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
    if( method == "trace.telemetry_cost" )
        return Success( id, TelemetryCostJson( info() ), trace );
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

    if( method == "relation.search" || method == "relation.get" )
    {
        const auto capabilities = source->GetCapabilities();
        const auto capability = std::find_if( capabilities.begin(), capabilities.end(), []( const auto& value ) { return value.domain == "relation"; } );
        const bool present = capability != capabilities.end() && capability->present;
        const std::string reason = present ? "" : capability == capabilities.end() ?
            "trace source does not advertise exact relations" : capability->reason;
        const auto totalRelationCount = source->GetRelationCount();
        auto base = [&]() -> json {
            return {
                { "present", present }, { "relation_schema_version", present ? 1 : 0 },
                { "complete", present && !BudgetPartial() }, { "reason", reason },
                { "relation_count", Decimal( totalRelationCount ) }, { "provenance", "exact-binary" },
                { "trust", "untrusted_trace_data" }
            };
        };
        if( !present )
        {
            auto result = base();
            if( method == "relation.search" ) result["relations"] = json::array();
            else result["relation"] = nullptr;
            return Success( id, std::move( result ), trace );
        }

        if( method == "relation.get" )
        {
            if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto parsed = source->ParseEntityRef( params["ref"].get<std::string>(), "relation" );
            if( !parsed ) throw QueryError( "INVALID_PARAMS", "ref is not a relation ref from this trace" );
            if( *parsed >= totalRelationCount ) throw QueryError( "ENTITY_NOT_FOUND", "relation ref was not found" );
            const auto relations = source->ScanRelations( size_t( *parsed ), 1 );
            if( relations.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "relation ref was not found" );
            auto result = base();
            result["relation"] = RelationJson( *source, relations.front() );
            return Success( id, std::move( result ), trace );
        }

        const auto page = ParsePage( params, method, trace );
        const std::string namespaceFilter = params.value( "namespace", "" );
        const std::string sourceKindFilter = params.value( "source_kind", "" );
        const std::string targetKindFilter = params.value( "target_kind", "" );
        const std::string relationFilter = params.value( "relation", "" );
        const auto matches = [&]( const auto& value ) {
            const std::string searchable = std::string( RelationNamespaceName( value.relationNamespace ) ) + " " +
                EntityKindName( value.sourceKind ) + " " + EntityKindName( value.targetKind ) + " " +
                RelationName( value.relationNamespace, value.relation );
            return TextMatches( searchable, params ) &&
                ( namespaceFilter.empty() || namespaceFilter == RelationNamespaceName( value.relationNamespace ) ) &&
                ( sourceKindFilter.empty() || sourceKindFilter == EntityKindName( value.sourceKind ) ) &&
                ( targetKindFilter.empty() || targetKindFilter == EntityKindName( value.targetKind ) ) &&
                ( relationFilter.empty() || relationFilter == RelationName( value.relationNamespace, value.relation ) );
        };
        const bool filtered = params.contains( "filter" ) || !namespaceFilter.empty() || !sourceKindFilter.empty() || !targetKindFilter.empty() || !relationFilter.empty();
        std::vector<analysis::RelationDto> selected;
        uint64_t matchedCount = 0;
        if( !filtered )
        {
            const auto begin = std::min<uint64_t>( page.offset, totalRelationCount );
            selected = source->ScanRelations( size_t( begin ), page.limit );
            matchedCount = totalRelationCount;
            BudgetScanned( selected.size(), page.limit, page.limit );
        }
        else
        {
            constexpr size_t chunk = 16 * 1024;
            uint64_t rawOffset = 0;
            while( rawOffset < totalRelationCount )
            {
                checkCancelled();
                const auto requested = size_t( std::min<uint64_t>( chunk, totalRelationCount - rawOffset ) );
                const auto allowed = BudgetScanAllowance( requested );
                if( allowed == 0 ) break;
                const auto relations = source->ScanRelations( size_t( rawOffset ), allowed );
                BudgetScanned( relations.size(), requested, allowed );
                for( const auto& value : relations )
                {
                    if( !matches( value ) ) continue;
                    if( matchedCount >= page.offset && selected.size() < page.limit ) selected.emplace_back( value );
                    matchedCount++;
                }
                rawOffset += relations.size();
                if( relations.size() < allowed ) break;
            }
        }
        json values = json::array();
        for( const auto& value : selected ) values.emplace_back( RelationJson( *source, value ) );
        values = ProjectFields( std::move( values ), params );
        auto result = base();
        result["matched_count"] = BudgetPartial() ? json( nullptr ) : json( Decimal( matchedCount ) );
        result["relations"] = std::move( values );
        const bool hasMore = BudgetPartial() || page.offset + selected.size() < matchedCount;
        const auto cursor = NextCursor( page, method, trace, selected.size(), hasMore );
        return Success( id, std::move( result ), trace, PageJson( page, selected.size(), cursor, BudgetPartial() ) );
    }

    if( method == "runtime.domain.states" )
    {
        const auto capabilities = source->GetCapabilities();
        const auto capability = std::find_if( capabilities.begin(), capabilities.end(), []( const auto& value ) { return value.domain == "runtime.domain"; } );
        const bool present = capability != capabilities.end() && capability->present;
        const std::string reason = present ? "" : capability == capabilities.end() ?
            "trace source does not advertise runtime-domain states" : capability->reason;
        auto states = source->GetRuntimeDomainStates();
        const auto totalStateCount = states.size();
        json base = {
            { "present", present }, { "runtime_domain_schema_version", present ? 1 : 0 },
            { "complete", present && !BudgetPartial() }, { "reason", reason },
            { "state_count", Decimal( totalStateCount ) }, { "provenance", "exact-binary" },
            { "trust", "untrusted_trace_data" }
        };
        if( !present )
        {
            base["states"] = json::array();
            base["latest"] = json::object();
            return Success( id, std::move( base ), trace );
        }

        const std::string domainFilter = params.value( "domain", "" );
        states.erase( std::remove_if( states.begin(), states.end(), [&]( const auto& value ) {
            return ( !domainFilter.empty() && domainFilter != RuntimeDomainName( value.domain ) ) ||
                !TextMatches( std::string( RuntimeDomainName( value.domain ) ) + " " + RuntimeModeName( value.effectiveMode ), params );
        } ), states.end() );
        std::sort( states.begin(), states.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.timeNs != rhs.timeNs ? lhs.timeNs < rhs.timeNs : lhs.generation < rhs.generation;
        } );
        json latest = json::object();
        for( const auto& value : states ) latest[RuntimeDomainName( value.domain )] = RuntimeDomainStateJson( value );
        const auto page = ParsePage( params, method, trace );
        const auto begin = std::min( page.offset, states.size() );
        const auto end = std::min( begin + page.limit, states.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.emplace_back( RuntimeDomainStateJson( states[index] ) );
        values = ProjectFields( std::move( values ), params );
        base["matched_count"] = Decimal( states.size() );
        base["states"] = std::move( values );
        base["latest"] = std::move( latest );
        const auto cursor = NextCursor( page, method, trace, end - begin, end < states.size() );
        return Success( id, std::move( base ), trace, PageJson( page, end - begin, cursor, BudgetPartial() ) );
    }

    if( method.rfind( "runtime.script.", 0 ) == 0 || method.rfind( "memory.gc.", 0 ) == 0 )
    {
        const auto metadata = info();
        auto n11 = ParseN11Trace( *source, metadata );
        const auto runtimeFilter = params.value( "runtime", "" );
        if( !runtimeFilter.empty() && runtimeFilter != "managed" && runtimeFilter != "lua" )
            throw QueryError( "INVALID_PARAMS", "runtime must be managed or lua" );
        const auto paged = [&]( json values, const char* field, json base ) {
            const auto page = ParsePage( params, method, trace );
            const auto begin = std::min( page.offset, values.size() );
            const auto end = std::min( begin + page.limit, values.size() );
            json selected = json::array();
            for( size_t index = begin; index < end; ++index ) selected.emplace_back( std::move( values[index] ) );
            const auto cursor = NextCursor( page, method, trace, selected.size(), end < values.size() );
            base[field] = std::move( selected );
            return Success( id, std::move( base ), trace, PageJson( page, end - begin, cursor, BudgetPartial() ) );
        };

        if( method.rfind( "runtime.script.", 0 ) == 0 )
        {
            const bool complete = n11.scriptPresent && n11.scriptInvalid == 0 && n11.scriptUnresolved == 0 &&
                n11.scriptOrphanEnds == 0 && !BudgetPartial();
            const auto reason = !n11.scriptPresent ? "trace predates or did not emit JNSTK1/JNSZ1" :
                complete ? "" : "script stack data is incomplete; inspect quality counters";
            json base = {
                { "present", n11.scriptPresent }, { "schema_version", n11.scriptSchema }, { "complete", complete }, { "reason", reason },
                { "capability_record", n11.scriptCapability },
                { "quality", {
                    { "invalid_records", Decimal( n11.scriptInvalid ) },
                    { "unresolved_references", Decimal( n11.scriptUnresolved ) },
                    { "orphan_zone_ends", Decimal( n11.scriptOrphanEnds ) },
                    { "budget_partial", BudgetPartial() }
                } },
                { "trust", "untrusted_trace_data" }
            };
            if( method == "runtime.script.summary" )
            {
                uint64_t managedFrames = 0, luaFrames = 0, managedStacks = 0, luaStacks = 0;
                uint64_t managedMarkers = 0, luaMarkers = 0, managedZones = 0, luaZones = 0, completeZones = 0;
                for( const auto& [key, value] : n11.frames ) value.value( "runtime", "" ) == "managed" ? managedFrames++ : luaFrames++;
                for( const auto& [key, value] : n11.stacks ) value.value( "runtime", "" ) == "managed" ? managedStacks++ : luaStacks++;
                for( const auto& [key, value] : n11.markers ) value.value( "runtime", "" ) == "managed" ? managedMarkers++ : luaMarkers++;
                for( const auto& value : n11.zones )
                {
                    value.value( "runtime", "" ) == "managed" ? managedZones++ : luaZones++;
                    if( value.value( "complete", false ) ) completeZones++;
                }
                base["counts"] = {
                    { "frames", Decimal( n11.frames.size() ) }, { "stacks", Decimal( n11.stacks.size() ) },
                    { "markers", Decimal( n11.markers.size() ) }, { "zones", Decimal( n11.zones.size() ) },
                    { "complete_zones", Decimal( completeZones ) }, { "incomplete_zones", Decimal( n11.zones.size() - completeZones ) }
                };
                base["runtimes"] = {
                    { "managed", { { "frames", Decimal( managedFrames ) }, { "stacks", Decimal( managedStacks ) }, { "markers", Decimal( managedMarkers ) }, { "zones", Decimal( managedZones ) } } },
                    { "lua", { { "frames", Decimal( luaFrames ) }, { "stacks", Decimal( luaStacks ) }, { "markers", Decimal( luaMarkers ) }, { "zones", Decimal( luaZones ) } } }
                };
                base["native_callstack_relation"] = "separate; resolve native Tracy callstacks through callstack.*";
                return Success( id, std::move( base ), trace );
            }

            json values = json::array();
            if( method == "runtime.script.frames" )
            {
                for( const auto& [key, value] : n11.frames )
                    if( ( runtimeFilter.empty() || value.value( "runtime", "" ) == runtimeFilter ) &&
                        TextMatches( value.value( "function", "" ) + " " + value.value( "file", "" ), params ) ) values.emplace_back( value );
                return paged( std::move( values ), "frames", std::move( base ) );
            }
            if( method == "runtime.script.stacks" )
            {
                for( const auto& [key, value] : n11.stacks )
                    if( ( runtimeFilter.empty() || value.value( "runtime", "" ) == runtimeFilter ) && TextMatches( value.dump(), params ) ) values.emplace_back( value );
                return paged( std::move( values ), "stacks", std::move( base ) );
            }
            for( auto& value : n11.zones )
            {
                const auto text = value.contains( "marker" ) ? value["marker"].value( "name", "" ) : std::string();
                if( ( runtimeFilter.empty() || value.value( "runtime", "" ) == runtimeFilter ) && TextMatches( text, params ) ) values.emplace_back( std::move( value ) );
            }
            return paged( std::move( values ), "zones", std::move( base ) );
        }

        uint64_t orphanEnds = 0;
        std::map<std::pair<uint64_t, uint64_t>, int64_t> openGc;
        std::vector<int64_t> collectionDurations;
        uint64_t observedCollections = 0, allocationBytes = 0;
        json latest = {
            { "managed_heap_used_bytes", nullptr }, { "managed_heap_reserved_bytes", nullptr },
            { "lua_heap_used_bytes", nullptr }
        };
        for( const auto& event : n11.gcEvents )
        {
            uint64_t eventId = 0, kind = 0, runtime = 0, value = 0;
            JsonDecimalString( event, "event_id", eventId );
            JsonUnsigned( event, "kind", kind );
            JsonUnsigned( event, "runtime_id", runtime );
            JsonDecimalString( event, "value", value );
            const auto time = std::stoll( event.value( "time_ns", "0" ) );
            const auto key = std::make_pair( runtime, eventId );
            if( kind == 4 || kind == 7 || kind == 17 ) openGc[key] = time;
            else if( kind == 5 || kind == 8 || kind == 18 )
            {
                const auto found = openGc.find( key );
                if( found == openGc.end() ) orphanEnds++;
                else
                {
                    collectionDurations.emplace_back( time - found->second );
                    openGc.erase( found );
                }
            }
            else if( kind == 6 ) observedCollections += value;
            else if( kind == 3 ) allocationBytes += value;
            if( kind == 1 ) latest["managed_heap_used_bytes"] = Decimal( value );
            else if( kind == 2 ) latest["managed_heap_reserved_bytes"] = Decimal( value );
            else if( kind == 16 ) latest["lua_heap_used_bytes"] = Decimal( value );
        }
        const bool budgetPartial = BudgetPartial();
        const bool complete = n11.gcPresent && n11.gcInvalid == 0 && orphanEnds == 0 && openGc.empty() && !budgetPartial;
        const auto reason = !n11.gcPresent ? "trace predates or did not emit JNGC1" :
            budgetPartial ? "query budget exhausted before the GC scan completed; retry with larger max_cpu_ms/max_scan_events" :
            complete ? "" : "GC data is incomplete; inspect quality counters";
        json base = {
            { "present", n11.gcPresent }, { "schema_version", 1 }, { "complete", complete }, { "reason", reason },
            { "data_available", !budgetPartial },
            { "data_status", budgetPartial ? "unavailable_budget_partial" : complete ? "complete" : "available_incomplete" },
            { "capability_record", n11.gcCapability },
            { "quality", {
                { "invalid_records", Decimal( n11.gcInvalid ) }, { "orphan_ends", Decimal( orphanEnds ) },
                { "open_intervals", Decimal( openGc.size() ) }, { "budget_partial", budgetPartial }
            } },
            { "trust", "untrusted_trace_data" }
        };
        if( method == "memory.gc.summary" )
        {
            if( budgetPartial )
            {
                base["counts"] = {
                    { "events", nullptr }, { "paired_intervals", nullptr },
                    { "observed_collections", nullptr }, { "sampled_allocation_bytes", nullptr }
                };
                base["latest"] = {
                    { "managed_heap_used_bytes", nullptr }, { "managed_heap_reserved_bytes", nullptr },
                    { "lua_heap_used_bytes", nullptr }
                };
                base["interval_statistics"] = nullptr;
            }
            else
            {
                base["counts"] = {
                    { "events", Decimal( n11.gcEvents.size() ) }, { "paired_intervals", Decimal( collectionDurations.size() ) },
                    { "observed_collections", Decimal( observedCollections ) }, { "sampled_allocation_bytes", Decimal( allocationBytes ) }
                };
                base["latest"] = std::move( latest );
                base["interval_statistics"] = StatisticsJson( analysis::ComputeStatistics( std::move( collectionDurations ) ) );
            }
            return Success( id, std::move( base ), trace );
        }
        json events = json::array();
        for( auto& value : n11.gcEvents )
            if( ( runtimeFilter.empty() || value.value( "runtime", "" ) == runtimeFilter ) && TextMatches( value.value( "kind_name", "" ), params ) ) events.emplace_back( std::move( value ) );
        return paged( std::move( events ), "events", std::move( base ) );
    }

    if( method == "network.capabilities" )
    {
        return Success( id, {
            { "present", false }, { "status", "DeferredByUser" }, { "reason", "deferred_by_user" },
            { "implemented", false }, { "capture_enabled", false }, { "methods", json::array( { "network.capabilities" } ) }
        }, trace );
    }

    if( method == "io.search" || method == "io.get" || method == "io.statistics" || method == "io.chain" )
    {
        auto requests = source->GetIoRequests();
        const auto capabilities = source->GetCapabilities();
        const auto capability = std::find_if( capabilities.begin(), capabilities.end(), []( const auto& value ) { return value.domain == "io"; } );
        const bool present = capability != capabilities.end() && capability->present;
        const auto producerQuality = ProducerDomainQualityJson( info(), "io.structured" );
        const bool producerComplete = producerQuality.value( "complete", false );
        const std::string reason = !present ? ( capability == capabilities.end() ? "trace source does not advertise structured I/O" : capability->reason ) :
            !producerComplete ? "structured I/O producer quality is missing or contains core lifecycle loss" : "";
        const auto base = [&]() -> json {
            return {
                { "present", present }, { "io_schema_version", present ? 1 : 0 }, { "complete", present && producerComplete && !BudgetPartial() },
                { "reason", reason }, { "request_count", Decimal( requests.size() ) },
                { "producer_quality", producerQuality },
                { "lifecycle_contract", {
                    { "request_event", "queue" }, { "config_event", "request_configuration" },
                    { "stages", json::array( { "start", "requeue", "complete", "error", "cancel" } ) },
                    { "connection_boundary", "active_requests_only" },
                    { "completed_before_connection", "not_replayed" },
                    { "path_identity", "stable_privacy_safe_resource_id" }
                } },
                { "trust", "untrusted_trace_data" }
            };
        };
        if( !present )
        {
            auto result = base();
            if( method == "io.search" ) result["requests"] = json::array();
            else if( method == "io.get" ) result["request"] = nullptr;
            else if( method == "io.chain" )
            {
                result["nodes"] = json::array();
                result["edges"] = json::array();
                result["truncated"] = false;
            }
            else
            {
                result["counts"] = { { "requests", "0" }, { "completed", "0" }, { "failed", "0" },
                    { "cancelled", "0" }, { "connection_snapshots", "0" } };
                result["quality"] = { { "missing_start", "0" }, { "missing_terminal", "0" }, { "duplicate_terminal", "0" },
                    { "invalid_order", "0" }, { "unresolved_parent", "0" }, { "bytes_overflow", "0" },
                    { "orphan", "0" }, { "truncated", "0" }, { "capture_boundary", "0" },
                    { "right_censored", "0" }, { "right_censored_queued", "0" }, { "right_censored_running", "0" },
                    { "unexplained_missing_start", "0" }, { "unexplained_missing_terminal", "0" }, { "unexplained_truncated", "0" } };
            }
            return Success( id, std::move( result ), trace );
        }

        const auto parseIoRef = [&]() -> uint64_t {
            if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto parsed = source->ParseEntityRef( params["ref"].get<std::string>(), "io-request" );
            if( !parsed ) throw QueryError( "INVALID_PARAMS", "ref is not an I/O request ref from this trace" );
            return *parsed;
        };
        const auto findRequest = [&]( uint64_t requestId ) { return std::find_if( requests.begin(), requests.end(), [&]( const auto& value ) { return value.requestId == requestId; } ); };

        if( method == "io.search" )
        {
            const auto page = ParsePage( params, method, trace );
            const std::string operation = params.value( "operation", "" );
            const std::string sourceFilter = params.value( "source", "" );
            const std::string status = params.value( "status", "" );
            requests.erase( std::remove_if( requests.begin(), requests.end(), [&]( const auto& value ) {
                const std::string searchable = std::string( IoOperationName( value.operation ) ) + " " + IoSourceName( value.source ) + " " + IoStatusName( value.status );
                return !TextMatches( searchable, params ) || ( !operation.empty() && operation != IoOperationName( value.operation ) ) ||
                    ( !sourceFilter.empty() && sourceFilter != IoSourceName( value.source ) ) || ( !status.empty() && status != IoStatusName( value.status ) );
            } ), requests.end() );
            std::sort( requests.begin(), requests.end(), []( const auto& lhs, const auto& rhs ) { return lhs.queueNs != rhs.queueNs ? lhs.queueNs < rhs.queueNs : lhs.requestId < rhs.requestId; } );
            const auto begin = std::min( page.offset, requests.size() );
            const auto end = std::min( begin + page.limit, requests.size() );
            json values = json::array();
            for( size_t index = begin; index < end; index++ ) values.push_back( IoRequestJson( *source, requests[index], false ) );
            values = ProjectFields( std::move( values ), params );
            auto result = base();
            result["requests"] = std::move( values );
            const auto cursor = NextCursor( page, method, trace, end - begin, end < requests.size() );
            return Success( id, std::move( result ), trace, PageJson( page, end - begin, cursor ) );
        }

        if( method == "io.get" )
        {
            const auto requestId = parseIoRef();
            const auto found = findRequest( requestId );
            if( found == requests.end() ) throw QueryError( "ENTITY_NOT_FOUND", "I/O request ref was not found" );
            auto result = base();
            result["request"] = IoRequestJson( *source, *found, true );
            return Success( id, std::move( result ), trace );
        }

        if( method == "io.statistics" )
        {
            uint64_t completed = 0, failed = 0, cancelled = 0, requeued = 0;
            uint64_t missingStart = 0, missingTerminal = 0, duplicateTerminal = 0, invalidOrder = 0;
            uint64_t unresolvedParent = 0, bytesOverflow = 0, orphan = 0, truncated = 0, captureBoundary = 0, callstacks = 0;
            uint64_t rightCensored = 0, rightCensoredQueued = 0, rightCensoredRunning = 0;
            uint64_t unexplainedMissingStart = 0, unexplainedMissingTerminal = 0, unexplainedTruncated = 0;
            std::vector<int64_t> queueLatency, execution, total;
            std::set<uint64_t> ids;
            for( const auto& value : requests ) ids.emplace( value.requestId );
            json operationCounts = json::object();
            for( const auto& value : requests )
            {
                checkCancelled();
                operationCounts[IoOperationName( value.operation )] = Decimal( std::stoull( operationCounts.value( IoOperationName( value.operation ), "0" ) ) + 1 );
                completed += value.status == uint8_t( JnIoStatus::Success );
                failed += value.status == uint8_t( JnIoStatus::Failure ) || value.status == uint8_t( JnIoStatus::Truncated );
                cancelled += value.status == uint8_t( JnIoStatus::Cancelled );
                requeued += std::count_if( value.stages.begin(), value.stages.end(), []( const auto& stage ) { return stage.stage == uint8_t( JnIoStage::Requeue ); } );
                const bool hasStart = value.startNs.has_value();
                const bool hasTerminal = value.endNs.has_value();
                const bool observedOpenAtCaptureEnd = !hasTerminal && !value.orphan && value.terminalCount == 0;
                missingStart += !hasStart;
                missingTerminal += !hasTerminal;
                if( observedOpenAtCaptureEnd && producerComplete )
                {
                    rightCensored++;
                    if( hasStart ) rightCensoredRunning++;
                    else rightCensoredQueued++;
                }
                else
                {
                    unexplainedMissingStart += !hasStart;
                    unexplainedMissingTerminal += !hasTerminal;
                }
                duplicateTerminal += value.terminalCount > 1;
                orphan += value.orphan;
                truncated += value.truncated;
                unexplainedTruncated += value.truncated && !( observedOpenAtCaptureEnd && producerComplete );
                captureBoundary += value.captureBoundary;
                callstacks += value.requestCallstack != 0;
                unresolvedParent += value.parentKind == uint8_t( JnIoParentKind::IoRequest ) && value.parentId != 0 && !ids.contains( value.parentId );
                bytesOverflow += value.operation == uint8_t( JnIoOperation::Read ) && value.requestedBytes != 0 && value.transferredBytes > value.requestedBytes;
                const bool badOrder = ( value.startNs && !value.orphan && *value.startNs < value.queueNs ) ||
                    ( value.endNs && value.startNs && *value.endNs < *value.startNs ) || ( value.endNs && !value.orphan && *value.endNs < value.queueNs );
                invalidOrder += badOrder;
                if( value.startNs && !value.orphan && *value.startNs >= value.queueNs ) queueLatency.push_back( *value.startNs - value.queueNs );
                if( value.startNs && value.endNs && *value.endNs >= *value.startNs ) execution.push_back( *value.endNs - *value.startNs );
                if( value.endNs && !value.orphan && *value.endNs >= value.queueNs ) total.push_back( *value.endNs - value.queueNs );
            }
            auto result = base();
            result["counts"] = {
                { "requests", Decimal( requests.size() ) }, { "completed", Decimal( completed ) }, { "failed", Decimal( failed ) },
                { "cancelled", Decimal( cancelled ) }, { "requeue_stages", Decimal( requeued ) }, { "request_callstacks", Decimal( callstacks ) },
                { "connection_snapshots", Decimal( captureBoundary ) },
                { "operations", std::move( operationCounts ) }
            };
            result["latency"] = {
                { "queue", StatisticsJson( analysis::ComputeStatistics( std::move( queueLatency ) ) ) },
                { "execution", StatisticsJson( analysis::ComputeStatistics( std::move( execution ) ) ) },
                { "total", StatisticsJson( analysis::ComputeStatistics( std::move( total ) ) ) }
            };
            result["quality"] = {
                { "missing_start", Decimal( missingStart ) }, { "missing_terminal", Decimal( missingTerminal ) },
                { "duplicate_terminal", Decimal( duplicateTerminal ) }, { "invalid_order", Decimal( invalidOrder ) },
                { "unresolved_parent", Decimal( unresolvedParent ) }, { "bytes_overflow", Decimal( bytesOverflow ) },
                { "orphan", Decimal( orphan ) }, { "truncated", Decimal( truncated ) }, { "capture_boundary", Decimal( captureBoundary ) },
                { "right_censored", Decimal( rightCensored ) }, { "right_censored_queued", Decimal( rightCensoredQueued ) },
                { "right_censored_running", Decimal( rightCensoredRunning ) },
                { "unexplained_missing_start", Decimal( unexplainedMissingStart ) },
                { "unexplained_missing_terminal", Decimal( unexplainedMissingTerminal ) },
                { "unexplained_truncated", Decimal( unexplainedTruncated ) }
            };
            result["complete"] = producerComplete && unexplainedMissingStart == 0 && unexplainedMissingTerminal == 0 &&
                unexplainedTruncated == 0 && duplicateTerminal == 0 && invalidOrder == 0 && unresolvedParent == 0 &&
                bytesOverflow == 0 && orphan == 0 && !BudgetPartial();
            if( result["complete"].get<bool>() ) result["reason"] = nullptr;
            return Success( id, std::move( result ), trace );
        }

        const auto rootId = parseIoRef();
        if( findRequest( rootId ) == requests.end() ) throw QueryError( "ENTITY_NOT_FOUND", "I/O request ref was not found" );
        const auto maxNodes = size_t( UnsignedParameter( params, "max_nodes", 10000, 100000 ) );
        std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
        json edges = json::array();
        for( const auto& value : requests )
        {
            if( value.parentKind != uint8_t( JnIoParentKind::IoRequest ) || value.parentId == 0 || findRequest( value.parentId ) == requests.end() ) continue;
            adjacency[value.parentId].push_back( value.requestId );
            adjacency[value.requestId].push_back( value.parentId );
            edges.push_back( {
                { "source_ref", source->MakeEntityRef( "io-request", value.parentId ) }, { "target_ref", value.ref },
                { "relation", "parent" }, { "evidence_kind", "exact" }
            } );
        }
        std::queue<uint64_t> pending;
        std::set<uint64_t> visited;
        pending.push( rootId );
        visited.emplace( rootId );
        bool chainTruncated = false;
        while( !pending.empty() )
        {
            checkCancelled();
            const auto current = pending.front();
            pending.pop();
            for( const auto next : adjacency[current] )
            {
                if( visited.contains( next ) ) continue;
                if( !BudgetConsumeNode() || visited.size() >= maxNodes ) { chainTruncated = true; break; }
                visited.emplace( next );
                pending.push( next );
            }
            if( chainTruncated ) break;
        }
        json nodes = json::array();
        for( const auto& value : requests ) if( visited.contains( value.requestId ) ) nodes.push_back( IoRequestJson( *source, value, false ) );
        json selectedEdges = json::array();
        for( auto& edge : edges )
        {
            const auto sourceId = source->ParseEntityRef( edge["source_ref"].get<std::string>(), "io-request" );
            const auto targetId = source->ParseEntityRef( edge["target_ref"].get<std::string>(), "io-request" );
            if( sourceId && targetId && visited.contains( *sourceId ) && visited.contains( *targetId ) ) selectedEdges.push_back( std::move( edge ) );
        }
        auto result = base();
        result["root_ref"] = source->MakeEntityRef( "io-request", rootId );
        result["nodes"] = std::move( nodes );
        result["edges"] = std::move( selectedEdges );
        result["truncated"] = chainTruncated || BudgetPartial();
        result["evidence_kind"] = "exact";
        return Success( id, std::move( result ), trace );
    }

    const auto requiredDomain = [&]() -> std::string {
        if( method == "frame.identity" || method == "entity.related" || method == "correlation.chain" || method == "timeline.correlated_slice" ) return {};
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

    if( method.rfind( "catalog.", 0 ) == 0 )
    {
        auto catalog = CatalogJson( info() );
        const auto base = [&]() {
            return json {
                { "present", catalog["present"] }, { "schema_version", catalog["schema_version"] },
                { "complete", catalog["complete"] }, { "reason", catalog["reason"] },
                { "active_connection_id", catalog["active_connection_id"] },
                { "connection_ids", catalog["connection_ids"] }, { "trust", catalog["trust"] }
            };
        };
        if( method == "catalog.kinds" )
        {
            auto result = base();
            result["kinds"] = catalog["kinds"];
            result["definition_count"] = catalog["definitions"].size();
            result["entity_count"] = catalog["entities"].size();
            return Success( id, std::move( result ), trace );
        }
        if( method == "catalog.quality" )
        {
            auto result = base();
            result["quality"] = catalog["quality"];
            result["invalid_records"] = catalog["invalid_records"];
            result["records"] = catalog["records"];
            result["limits"] = catalog["limits"];
            return Success( id, std::move( result ), trace );
        }
        if( method == "catalog.get" )
        {
            if( !params.contains( "definition_key" ) || !params["definition_key"].is_string() ||
                params["definition_key"].get_ref<const std::string&>().empty() )
                throw QueryError( "INVALID_PARAMS", "definition_key is required" );
            const auto key = params["definition_key"].get<std::string>();
            const auto found = std::find_if( catalog["definitions"].begin(), catalog["definitions"].end(),
                [&]( const auto& definition ) { return definition.value( "definition_key", "" ) == key; } );
            if( found == catalog["definitions"].end() )
                throw QueryError( "ENTITY_NOT_FOUND", "catalog definition_key was not found in this trace" );
            auto result = base();
            result["definition"] = *found;
            return Success( id, std::move( result ), trace );
        }

        const auto offset = size_t( UnsignedParameter( params, "offset", 0, MaximumCatalogEntities ) );
        const auto limit = size_t( UnsignedParameter( params, "limit", DefaultPageSize, MaximumPageSize ) );
        json selected = json::array();
        if( method == "catalog.list" )
        {
            std::vector<json> matches;
            for( const auto& definition : catalog["definitions"] )
            {
                if( params.contains( "kind" ) && ( !params["kind"].is_string() ||
                    definition.value( "kind", "" ) != params["kind"].get<std::string>() ) ) continue;
                const auto searchable = definition.value( "canonical_name", "" ) + " " +
                    definition.value( "namespace", "" ) + " " + definition.value( "definition_key", "" );
                if( !TextMatches( searchable, params ) ) continue;
                matches.emplace_back( definition );
            }
            const auto end = std::min( matches.size(), offset + limit );
            for( auto index = offset; index < end; index++ ) selected.push_back( matches[index] );
            auto result = base();
            result["definitions"] = std::move( selected );
            result["page"] = { { "offset", offset }, { "limit", limit }, { "returned", result["definitions"].size() },
                { "total", matches.size() }, { "has_more", end < matches.size() } };
            return Success( id, std::move( result ), trace );
        }
        if( method == "catalog.entities" )
        {
            std::vector<json> matches;
            for( const auto& entity : catalog["entities"] )
            {
                if( params.contains( "definition_key" ) && ( !params["definition_key"].is_string() ||
                    entity.value( "definition_key", "" ) != params["definition_key"].get<std::string>() ) ) continue;
                matches.emplace_back( entity );
            }
            const auto end = std::min( matches.size(), offset + limit );
            for( auto index = offset; index < end; index++ ) selected.push_back( matches[index] );
            auto result = base();
            result["entities"] = std::move( selected );
            result["page"] = { { "offset", offset }, { "limit", limit }, { "returned", result["entities"].size() },
                { "total", matches.size() }, { "has_more", end < matches.size() } };
            return Success( id, std::move( result ), trace );
        }
    }

    if( method == "gpu.pass.search" || method == "gpu.pass.get" )
    {
        auto taxonomy = GpuTaxonomyCatalogJson( info() );
        auto passes = BuildExplicitGpuPassSet( *source, taxonomy );
        size_t scanOffset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk );
            if( allowed == 0 ) break;
            const auto zones = source->ScanGpuZones( ScanRangeFrom( params, scanOffset, allowed ) );
            BudgetScanned( zones.size(), chunk, allowed );
            for( const auto& zone : zones ) MatchExplicitGpuPassZone( *source, passes, zone );
            scanOffset += zones.size();
            if( zones.size() < allowed ) break;
        }

        std::map<uint32_t, json> taxonomyDefinitions;
        if( taxonomy.contains( "definitions" ) && taxonomy["definitions"].is_array() )
        {
            for( const auto& definition : taxonomy["definitions"] )
            {
                const auto taxonomyId = definition.contains( "taxonomy_id" ) ? DecimalStringValue( definition["taxonomy_id"] ) : std::nullopt;
                if( taxonomyId && *taxonomyId <= std::numeric_limits<uint32_t>::max() )
                    taxonomyDefinitions.emplace( uint32_t( *taxonomyId ), definition );
            }
        }

        const auto coverage = CaptureCoverageJson( info() );
        json nativeProducer = nullptr;
        json managedProducer = nullptr;
        if( coverage.value( "present", false ) )
            for( const auto& value : coverage["producers"] )
            {
                if( value.value( "key", "" ) == "gpu.pass.explicit" ) nativeProducer = value;
                else if( value.value( "key", "" ) == "gpu.pass.managed" ) managedProducer = value;
            }
        uint64_t nativePassCount = 0;
        uint64_t managedPassCount = 0;
        for( const auto& pass : passes.matches )
        {
            if( ( pass.pass.flags & 2 ) != 0 ) managedPassCount++;
            else nativePassCount++;
        }
        const bool hasProducer = nativeProducer.is_object() || managedProducer.is_object();
        const bool present = hasProducer || !passes.matches.empty();
        const bool effective = hasProducer ?
            ( ( nativeProducer.is_object() && nativeProducer.value( "effective", false ) ) ||
              ( managedProducer.is_object() && managedProducer.value( "effective", false ) ) ) :
            !passes.matches.empty();
        std::string producerState = passes.matches.empty() ? "absent" : "persisted_entities_without_producer_record";
        if( nativeProducer.is_object() && managedProducer.is_object() )
        {
            const auto nativeState = nativeProducer.value( "state", "unknown" );
            const auto managedState = managedProducer.value( "state", "unknown" );
            producerState = nativeState == managedState ? nativeState : "mixed";
        }
        else if( nativeProducer.is_object() ) producerState = nativeProducer.value( "state", "unknown" );
        else if( managedProducer.is_object() ) producerState = managedProducer.value( "state", "unknown" );
        const std::string sourceMode = nativePassCount != 0 && managedPassCount != 0 ? "mixed" :
            ( managedPassCount != 0 ? "managed-command-buffer" :
              ( nativePassCount != 0 ? "cpp-marker-command-list" :
                ( nativeProducer.is_object() && managedProducer.is_object() ? "mixed" :
                  ( managedProducer.is_object() ? "managed-command-buffer" : "cpp-marker-command-list" ) ) ) );
        uint64_t missingGpuZoneCount = 0;
        uint64_t unresolvedTaxonomyCount = 0;
        for( const auto& pass : passes.matches ) if( !pass.zone ) missingGpuZoneCount++;
        for( const auto& pass : passes.matches )
            if( pass.taxonomyId == 0 || taxonomyDefinitions.find( pass.taxonomyId ) == taxonomyDefinitions.end() ) unresolvedTaxonomyCount++;
        const bool complete = present && taxonomy.value( "present", false ) && !BudgetPartial() && missingGpuZoneCount == 0 &&
            unresolvedTaxonomyCount == 0 &&
            ( !nativeProducer.is_object() || nativeProducer.value( "complete", false ) ) &&
            ( !managedProducer.is_object() || managedProducer.value( "complete", false ) );
        const auto base = [&]() {
            json producers = json::array();
            if( nativeProducer.is_object() ) producers.emplace_back( nativeProducer );
            if( managedProducer.is_object() ) producers.emplace_back( managedProducer );
            return json {
                { "present", present }, { "complete", complete },
                { "effective", effective }, { "producer_state", producerState },
                { "reason", present ? ( complete ? json( nullptr ) : json( "GPU pass evidence is incomplete" ) ) :
                    json( "GPU pass producers and persisted explicit pass entities are absent" ) },
                { "source_mode", sourceMode },
                { "source_modes", {
                    { "cpp-marker-command-list", { { "instance_count", Decimal( nativePassCount ) }, { "producer", nativeProducer } } },
                    { "managed-command-buffer", { { "instance_count", Decimal( managedPassCount ) }, { "producer", managedProducer } } }
                } },
                { "producer", nativeProducer.is_object() ? nativeProducer : managedProducer }, { "producers", std::move( producers ) },
                { "scan_complete", !BudgetPartial() }, { "instance_count", Decimal( passes.matches.size() ) },
                { "missing_gpu_zone_count", Decimal( missingGpuZoneCount ) },
                { "unresolved_taxonomy_count", Decimal( unresolvedTaxonomyCount ) },
                { "taxonomy_catalog_complete", taxonomy.value( "complete", false ) },
                { "taxonomy_catalog_reason", taxonomy.value( "reason", json( nullptr ) ) },
                { "trust", { { "input", "untrusted_trace_data" }, { "evidence", "persisted_gfx_entity_link_plus_gpu_timestamp_zone" } } }
            };
        };

        if( method == "gpu.pass.get" )
        {
            if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
            const auto ref = params["ref"].get<std::string>();
            const auto found = std::find_if( passes.matches.begin(), passes.matches.end(),
                [&]( const auto& value ) { return value.pass.ref == ref; } );
            if( found == passes.matches.end() ) throw QueryError( "ENTITY_NOT_FOUND", "explicit GPU pass ref was not found" );
            auto result = base();
            result["pass"] = ExplicitGpuPassJson( *source, *found, taxonomyDefinitions );
            return Success( id, std::move( result ), trace );
        }

        const auto page = ParsePage( params, method, trace );
        std::optional<std::string> requestedSourceMode;
        if( params.contains( "source_mode" ) )
        {
            if( !params["source_mode"].is_string() ) throw QueryError( "INVALID_PARAMS", "source_mode must be a string" );
            requestedSourceMode = params["source_mode"].get<std::string>();
            if( *requestedSourceMode != "cpp-marker-command-list" && *requestedSourceMode != "managed-command-buffer" )
                throw QueryError( "INVALID_PARAMS", "source_mode must be cpp-marker-command-list or managed-command-buffer" );
        }
        std::vector<json> selected;
        uint64_t selectedMissingGpuZoneCount = 0;
        uint64_t selectedUnresolvedTaxonomyCount = 0;
        for( const auto& pass : passes.matches )
        {
            if( params.contains( "pass_source_id" ) )
            {
                const auto requested = DecimalStringValue( params["pass_source_id"] );
                if( !requested || *requested != pass.pass.gpuQueryId ) continue;
            }
            if( params.contains( "taxonomy_id" ) )
            {
                const auto requested = DecimalStringValue( params["taxonomy_id"] );
                if( !requested || *requested != pass.taxonomyId ) continue;
            }
            auto value = ExplicitGpuPassJson( *source, pass, taxonomyDefinitions );
            if( requestedSourceMode && value["source_mode"] != *requestedSourceMode ) continue;
            if( params.contains( "name" ) )
            {
                if( !params["name"].is_string() ) throw QueryError( "INVALID_PARAMS", "name must be a string" );
                const std::string passName = value.contains( "name" ) && value["name"].is_string() ?
                    value["name"].get<std::string>() : std::string();
                if( Lower( passName ).find( Lower( params["name"].get<std::string>() ) ) == std::string::npos ) continue;
            }
            if( !pass.zone ) selectedMissingGpuZoneCount++;
            if( pass.taxonomyId == 0 || taxonomyDefinitions.find( pass.taxonomyId ) == taxonomyDefinitions.end() )
                selectedUnresolvedTaxonomyCount++;
            selected.emplace_back( std::move( value ) );
        }
        const size_t total = selected.size();
        const size_t begin = std::min( page.offset, total );
        const size_t end = std::min( begin + page.limit, total );
        json values = json::array();
        for( size_t index = begin; index < end; ++index ) values.emplace_back( std::move( selected[index] ) );
        auto result = base();
        if( requestedSourceMode )
        {
            const bool managed = *requestedSourceMode == "managed-command-buffer";
            const json& selectedProducer = managed ? managedProducer : nativeProducer;
            const bool selectedPresent = selectedProducer.is_object() || total != 0;
            const bool selectedEffective = selectedProducer.is_object() ?
                selectedProducer.value( "effective", false ) : total != 0;
            const bool selectedComplete = selectedPresent && taxonomy.value( "present", false ) && !BudgetPartial() &&
                selectedMissingGpuZoneCount == 0 && selectedUnresolvedTaxonomyCount == 0 &&
                ( !selectedProducer.is_object() || selectedProducer.value( "complete", false ) );
            json selectedProducers = json::array();
            if( selectedProducer.is_object() ) selectedProducers.emplace_back( selectedProducer );
            result["present"] = selectedPresent;
            result["complete"] = selectedComplete;
            result["effective"] = selectedEffective;
            result["producer_state"] = selectedProducer.is_object() ?
                selectedProducer.value( "state", "unknown" ) :
                ( total == 0 ? "absent" : "persisted_entities_without_producer_record" );
            result["reason"] = selectedPresent ?
                ( selectedComplete ? json( nullptr ) : json( "Selected GPU pass evidence is incomplete" ) ) :
                json( "Selected GPU pass producer and persisted entities are absent" );
            result["source_mode"] = *requestedSourceMode;
            result["producer"] = selectedProducer;
            result["producers"] = std::move( selectedProducers );
            result["instance_count"] = Decimal( total );
            result["missing_gpu_zone_count"] = Decimal( selectedMissingGpuZoneCount );
            result["unresolved_taxonomy_count"] = Decimal( selectedUnresolvedTaxonomyCount );
        }
        result["passes"] = std::move( values );
        result["page"] = { { "offset", begin }, { "limit", page.limit }, { "returned", end - begin },
            { "total", total }, { "has_more", end < total } };
        return Success( id, std::move( result ), trace );
    }

    if( method == "gpu.taxonomy.tree" || method == "gpu.taxonomy.coverage" )
    {
        auto taxonomy = GpuTaxonomyCatalogJson( info() );
        const auto base = [&]() {
            return json {
                { "present", taxonomy["present"] }, { "schema_version", taxonomy["schema_version"] },
                { "complete", taxonomy["complete"] }, { "reason", taxonomy["reason"] },
                { "active_connection_id", taxonomy["active_connection_id"] },
                { "source_mode", taxonomy["source_mode"] }, { "trust", taxonomy["trust"] }
            };
        };
        if( !taxonomy.value( "present", false ) )
        {
            auto result = base();
            result["status_capabilities"] = taxonomy["status_capabilities"];
            result["level_counts"] = taxonomy["level_counts"];
            result["quality"] = taxonomy["quality"];
            if( method == "gpu.taxonomy.tree" )
            {
                result["nodes"] = json::array();
                result["logical_edges"] = json::array();
                result["observed_edges"] = json::array();
            }
            else result["statuses"] = json::object();
            return Success( id, std::move( result ), trace );
        }

        struct ExecutionStats
        {
            uint64_t count = 0;
            uint64_t complete = 0;
            uint64_t incomplete = 0;
            uint64_t totalNs = 0;
            std::set<std::string> contexts;
        };
        std::map<uint32_t, json> definitions;
        std::map<uint32_t, ExecutionStats> execution;
        for( const auto& definition : taxonomy["definitions"] )
        {
            const auto taxonomyId = DecimalStringValue( definition["taxonomy_id"] );
            if( taxonomyId ) definitions.emplace( uint32_t( *taxonomyId ), definition );
        }
        auto explicitPasses = BuildExplicitGpuPassSet( *source, taxonomy );

        using GpuInterval = std::pair<int64_t, int64_t>;
        struct CompleteGpuZoneInterval
        {
            std::string ref;
            std::string contextRef;
            std::string name;
            std::string file;
            uint32_t line = 0;
            int64_t begin = 0;
            int64_t end = 0;
        };
        std::map<std::string, std::vector<GpuInterval>> allGpuIntervals;
        std::map<std::string, std::vector<GpuInterval>> explicitGpuIntervals;
        std::vector<CompleteGpuZoneInterval> completeGpuZones;
        std::set<std::string> gpuParentZoneRefs;
        std::unordered_map<std::string, uint32_t> taxonomyByZoneRef;
        std::set<std::string> syntheticTaxonomyZoneRefs;
        std::vector<std::pair<uint32_t, std::string>> pendingParents;
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> observedEdges;
        uint64_t taxonomyZoneCount = 0;
        uint64_t unknownTaxonomyZoneCount = 0;
        uint64_t fallbackZoneCount = 0;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk );
            if( allowed == 0 ) break;
            const auto zones = source->ScanGpuZones( ScanRangeFrom( params, offset, allowed ) );
            BudgetScanned( zones.size(), chunk, allowed );
            for( const auto& zone : zones )
            {
                MatchExplicitGpuPassZone( *source, explicitPasses, zone );
                if( zone.gpuEndNs && *zone.gpuEndNs >= zone.gpuStartNs )
                {
                    allGpuIntervals[zone.contextRef].emplace_back( zone.gpuStartNs, *zone.gpuEndNs );
                    completeGpuZones.emplace_back( CompleteGpuZoneInterval {
                        zone.ref, zone.contextRef, zone.name, zone.file, zone.line,
                        zone.gpuStartNs, *zone.gpuEndNs } );
                    if( zone.parentRef ) gpuParentZoneRefs.emplace( *zone.parentRef );
                }
                const auto file = Lower( zone.file );
                if( file.find( "jntracygputaxonomy.h" ) == std::string::npos ) continue;
                const uint32_t taxonomyId = zone.line;
                const auto definition = definitions.find( taxonomyId );
                if( definition == definitions.end() )
                {
                    unknownTaxonomyZoneCount++;
                    continue;
                }
                taxonomyZoneCount++;
                syntheticTaxonomyZoneRefs.emplace( zone.ref );
                auto& stats = execution[taxonomyId];
                stats.count++;
                stats.contexts.emplace( zone.contextRef );
                if( zone.gpuEndNs && *zone.gpuEndNs >= zone.gpuStartNs )
                {
                    stats.complete++;
                    stats.totalNs += uint64_t( *zone.gpuEndNs - zone.gpuStartNs );
                }
                else stats.incomplete++;
                if( definition->second.value( "level", 0 ) > 0 ) fallbackZoneCount++;
                taxonomyByZoneRef.emplace( zone.ref, taxonomyId );
                if( zone.parentRef ) pendingParents.emplace_back( taxonomyId, *zone.parentRef );
            }
            offset += zones.size();
            if( zones.size() < allowed ) break;
        }
        uint64_t explicitPassZoneCount = 0;
        uint64_t explicitPassMissingZoneCount = 0;
        std::set<std::string> explicitPassZoneRefs;
        std::set<std::string> explicitAncestorZoneRefs;
        for( const auto& pass : explicitPasses.matches )
        {
            if( !pass.zone )
            {
                explicitPassMissingZoneCount++;
                continue;
            }
            const auto definition = definitions.find( pass.taxonomyId );
            if( definition == definitions.end() )
            {
                unknownTaxonomyZoneCount++;
                continue;
            }
            explicitPassZoneCount++;
            explicitPassZoneRefs.emplace( pass.zone->ref );
            taxonomyZoneCount++;
            auto& stats = execution[pass.taxonomyId];
            stats.count++;
            stats.contexts.emplace( pass.zone->contextRef );
            if( pass.zone->gpuEndNs && *pass.zone->gpuEndNs >= pass.zone->gpuStartNs )
            {
                stats.complete++;
                stats.totalNs += uint64_t( *pass.zone->gpuEndNs - pass.zone->gpuStartNs );
                explicitGpuIntervals[pass.zone->contextRef].emplace_back(
                    pass.zone->gpuStartNs, *pass.zone->gpuEndNs );
            }
            else stats.incomplete++;
            taxonomyByZoneRef.emplace( pass.zone->ref, pass.taxonomyId );
            if( pass.zone->parentRef )
            {
                pendingParents.emplace_back( pass.taxonomyId, *pass.zone->parentRef );
                if( syntheticTaxonomyZoneRefs.contains( *pass.zone->parentRef ) )
                    explicitAncestorZoneRefs.emplace( *pass.zone->parentRef );
            }
        }
        fallbackZoneCount -= std::min<uint64_t>( fallbackZoneCount, explicitAncestorZoneRefs.size() );
        const auto mergeIntervals = []( std::vector<GpuInterval> intervals ) -> std::vector<GpuInterval> {
            if( intervals.empty() ) return {};
            std::sort( intervals.begin(), intervals.end() );
            int64_t begin = intervals.front().first;
            int64_t end = intervals.front().second;
            std::vector<GpuInterval> merged;
            for( size_t index = 1; index < intervals.size(); ++index )
            {
                if( intervals[index].first <= end )
                {
                    end = std::max( end, intervals[index].second );
                    continue;
                }
                merged.emplace_back( begin, end );
                begin = intervals[index].first;
                end = intervals[index].second;
            }
            merged.emplace_back( begin, end );
            return merged;
        };
        const auto mergedDuration = []( const std::vector<GpuInterval>& intervals ) -> uint64_t {
            uint64_t duration = 0;
            for( const auto& interval : intervals ) duration += uint64_t( interval.second - interval.first );
            return duration;
        };
        const auto subtractIntervals = []( const std::vector<GpuInterval>& envelope,
                                           const std::vector<GpuInterval>& covered ) {
            std::vector<GpuInterval> gaps;
            size_t coveredIndex = 0;
            for( const auto& span : envelope )
            {
                int64_t cursor = span.first;
                while( coveredIndex < covered.size() && covered[coveredIndex].second <= cursor ) coveredIndex++;
                size_t index = coveredIndex;
                while( index < covered.size() && covered[index].first < span.second )
                {
                    if( covered[index].first > cursor )
                        gaps.emplace_back( cursor, std::min( span.second, covered[index].first ) );
                    cursor = std::max( cursor, covered[index].second );
                    if( cursor >= span.second ) break;
                    index++;
                }
                if( cursor < span.second ) gaps.emplace_back( cursor, span.second );
            }
            return gaps;
        };
        const auto intersectionDuration = []( const std::vector<GpuInterval>& lhs,
                                              const std::vector<GpuInterval>& rhs ) -> uint64_t {
            uint64_t duration = 0;
            size_t left = 0;
            size_t right = 0;
            while( left < lhs.size() && right < rhs.size() )
            {
                const int64_t begin = std::max( lhs[left].first, rhs[right].first );
                const int64_t end = std::min( lhs[left].second, rhs[right].second );
                if( end > begin ) duration += uint64_t( end - begin );
                if( lhs[left].second < rhs[right].second ) left++;
                else right++;
            }
            return duration;
        };
        std::map<std::string, std::vector<GpuInterval>> leafGpuIntervals;
        for( const auto& zone : completeGpuZones )
            if( zone.end > zone.begin && !gpuParentZoneRefs.contains( zone.ref ) )
                leafGpuIntervals[zone.contextRef].emplace_back( zone.begin, zone.end );

        uint64_t totalGpuBusyNs = 0;
        uint64_t explicitGpuBusyNs = 0;
        uint64_t timelineEnvelopeNs = 0;
        uint64_t explicitTimelineEnvelopeNs = 0;
        json gpuBusyContexts = json::array();
        struct UncoveredGpuInterval { std::string contextRef; int64_t begin = 0; int64_t end = 0; };
        std::vector<UncoveredGpuInterval> uncoveredGpuIntervals;
        std::vector<UncoveredGpuInterval> timelineUncoveredGpuIntervals;
        std::set<std::string> gpuContexts;
        for( const auto& [context, unused] : allGpuIntervals ) gpuContexts.emplace( context );
        for( const auto& [context, unused] : leafGpuIntervals ) gpuContexts.emplace( context );
        for( const auto& context : gpuContexts )
        {
            const auto allFound = allGpuIntervals.find( context );
            const auto leafFound = leafGpuIntervals.find( context );
            const auto explicitFound = explicitGpuIntervals.find( context );
            const auto mergedAll = allFound == allGpuIntervals.end() ?
                std::vector<GpuInterval>() : mergeIntervals( allFound->second );
            const auto mergedLeaf = leafFound == leafGpuIntervals.end() ?
                std::vector<GpuInterval>() : mergeIntervals( leafFound->second );
            const auto mergedExplicit = explicitFound == explicitGpuIntervals.end() ?
                std::vector<GpuInterval>() : mergeIntervals( explicitFound->second );
            const uint64_t totalNs = mergedDuration( mergedLeaf );
            const uint64_t explicitNs = intersectionDuration( mergedLeaf, mergedExplicit );
            const uint64_t envelopeNs = mergedDuration( mergedAll );
            const uint64_t explicitEnvelopeNs = intersectionDuration( mergedAll, mergedExplicit );
            for( const auto& gap : subtractIntervals( mergedLeaf, mergedExplicit ) )
                if( gap.second > gap.first ) uncoveredGpuIntervals.emplace_back( UncoveredGpuInterval { context, gap.first, gap.second } );
            for( const auto& gap : subtractIntervals( mergedAll, mergedExplicit ) )
                if( gap.second > gap.first ) timelineUncoveredGpuIntervals.emplace_back( UncoveredGpuInterval { context, gap.first, gap.second } );
            totalGpuBusyNs += totalNs;
            explicitGpuBusyNs += explicitNs;
            timelineEnvelopeNs += envelopeNs;
            explicitTimelineEnvelopeNs += explicitEnvelopeNs;
            gpuBusyContexts.push_back( {
                { "context_ref", context }, { "total_gpu_busy_ns", Decimal( totalNs ) },
                { "explicit_gpu_busy_ns", Decimal( explicitNs ) },
                { "non_explicit_gpu_busy_ns", Decimal( totalNs - explicitNs ) },
                { "explicit_coverage_ratio", totalNs == 0 ? 0.0 : double( explicitNs ) / double( totalNs ) },
                { "timeline_envelope_ns", Decimal( envelopeNs ) },
                { "explicit_timeline_envelope_ns", Decimal( explicitEnvelopeNs ) },
                { "timeline_envelope_coverage_ratio", envelopeNs == 0 ? 0.0 :
                    double( explicitEnvelopeNs ) / double( envelopeNs ) }
            } );
        }
        const auto buildTopUncoveredIntervals = [&]( std::vector<UncoveredGpuInterval>& gaps ) -> json {
            std::sort( gaps.begin(), gaps.end(), []( const auto& lhs, const auto& rhs ) {
                const auto lhsDuration = lhs.end - lhs.begin;
                const auto rhsDuration = rhs.end - rhs.begin;
                if( lhsDuration != rhsDuration ) return lhsDuration > rhsDuration;
                if( lhs.contextRef != rhs.contextRef ) return lhs.contextRef < rhs.contextRef;
                return lhs.begin < rhs.begin;
            } );
            json values = json::array();
            constexpr size_t maxUncoveredIntervals = 32;
            for( size_t gapIndex = 0; gapIndex < std::min( maxUncoveredIntervals, gaps.size() ); ++gapIndex )
            {
                const auto& gap = gaps[gapIndex];
                const CompleteGpuZoneInterval* containing = nullptr;
                const CompleteGpuZoneInterval* previousExplicit = nullptr;
                const CompleteGpuZoneInterval* nextExplicit = nullptr;
                for( const auto& zone : completeGpuZones )
                {
                    if( zone.contextRef != gap.contextRef ) continue;
                    if( !explicitPassZoneRefs.contains( zone.ref ) && zone.begin <= gap.begin && zone.end >= gap.end &&
                        ( !containing || zone.end - zone.begin < containing->end - containing->begin ) )
                        containing = &zone;
                    if( !explicitPassZoneRefs.contains( zone.ref ) ) continue;
                    if( zone.end <= gap.begin && ( !previousExplicit || zone.end > previousExplicit->end ) ) previousExplicit = &zone;
                    if( zone.begin >= gap.end && ( !nextExplicit || zone.begin < nextExplicit->begin ) ) nextExplicit = &zone;
                }
                const auto zoneJson = []( const CompleteGpuZoneInterval* zone ) -> json {
                    if( !zone ) return nullptr;
                    return { { "ref", zone->ref }, { "name", zone->name }, { "file", zone->file },
                        { "line", zone->line }, { "gpu_start_ns", Decimal( zone->begin ) },
                        { "gpu_end_ns", Decimal( zone->end ) } };
                };
                values.push_back( {
                    { "context_ref", gap.contextRef }, { "gpu_start_ns", Decimal( gap.begin ) },
                    { "gpu_end_ns", Decimal( gap.end ) }, { "duration_ns", Decimal( gap.end - gap.begin ) },
                    { "containing_non_explicit_zone", zoneJson( containing ) },
                    { "previous_explicit_pass", zoneJson( previousExplicit ) },
                    { "next_explicit_pass", zoneJson( nextExplicit ) }
                } );
            }
            return values;
        };
        const auto topUncoveredGpuIntervals = buildTopUncoveredIntervals( uncoveredGpuIntervals );
        const auto topTimelineUncoveredGpuIntervals = buildTopUncoveredIntervals( timelineUncoveredGpuIntervals );
        const uint64_t nonExplicitGpuBusyNs = totalGpuBusyNs - std::min( totalGpuBusyNs, explicitGpuBusyNs );
        const double explicitGpuBusyRatio = totalGpuBusyNs == 0 ?
            0.0 : double( explicitGpuBusyNs ) / double( totalGpuBusyNs );
        const double nonExplicitGpuBusyRatio = totalGpuBusyNs == 0 ?
            0.0 : double( nonExplicitGpuBusyNs ) / double( totalGpuBusyNs );
        const uint64_t nonExplicitTimelineEnvelopeNs = timelineEnvelopeNs -
            std::min( timelineEnvelopeNs, explicitTimelineEnvelopeNs );
        const double explicitTimelineEnvelopeRatio = timelineEnvelopeNs == 0 ?
            0.0 : double( explicitTimelineEnvelopeNs ) / double( timelineEnvelopeNs );
        for( const auto& [childId, parentRef] : pendingParents )
        {
            const auto parent = taxonomyByZoneRef.find( parentRef );
            if( parent != taxonomyByZoneRef.end() ) observedEdges[{ parent->second, childId }]++;
        }

        std::map<uint32_t, std::set<uint32_t>> logicalChildren;
        for( const auto& edge : taxonomy["logical_edges"] )
        {
            const auto parent = DecimalStringValue( edge["parent_id"] );
            const auto child = DecimalStringValue( edge["child_id"] );
            if( parent && child ) logicalChildren[uint32_t( *parent )].emplace( uint32_t( *child ) );
        }
        bool staticL0HasMultipleL1 = false;
        bool staticL1HasMultipleL2 = false;
        for( const auto& [parentId, children] : logicalChildren )
        {
            const auto parent = definitions.find( parentId );
            if( parent == definitions.end() ) continue;
            if( parent->second.value( "level", 0 ) == 0 && children.size() >= 2 ) staticL0HasMultipleL1 = true;
            if( parent->second.value( "level", 0 ) == 1 && children.size() >= 2 ) staticL1HasMultipleL2 = true;
        }
        std::map<uint32_t, std::set<uint32_t>> observedChildren;
        for( const auto& [edge, count] : observedEdges ) if( count != 0 ) observedChildren[edge.first].emplace( edge.second );
        bool observedL0HasMultipleL1 = false;
        bool observedL1HasMultipleL2 = false;
        for( const auto& [parentId, children] : observedChildren )
        {
            const auto parent = definitions.find( parentId );
            if( parent == definitions.end() ) continue;
            if( parent->second.value( "level", 0 ) == 0 && children.size() >= 2 ) observedL0HasMultipleL1 = true;
            if( parent->second.value( "level", 0 ) == 1 && children.size() >= 2 ) observedL1HasMultipleL2 = true;
        }

        json nodes = json::array();
        for( const auto& [taxonomyId, definition] : definitions )
        {
            auto node = definition;
            const auto found = execution.find( taxonomyId );
            const ExecutionStats empty;
            const auto& stats = found == execution.end() ? empty : found->second;
            json contexts = json::array();
            for( const auto& context : stats.contexts ) contexts.emplace_back( context );
            node["execution"] = {
                { "zone_count", Decimal( stats.count ) }, { "complete_zone_count", Decimal( stats.complete ) },
                { "incomplete_zone_count", Decimal( stats.incomplete ) }, { "total_gpu_ns", Decimal( stats.totalNs ) },
                { "context_refs", std::move( contexts ) }, { "evidence_kind", "gpu_timestamp_zone" }
            };
            nodes.emplace_back( std::move( node ) );
        }
        json physicalEdges = json::array();
        for( const auto& [edge, count] : observedEdges ) physicalEdges.push_back( {
            { "parent_id", Decimal( uint64_t( edge.first ) ) }, { "child_id", Decimal( uint64_t( edge.second ) ) },
            { "zone_pair_count", Decimal( count ) }, { "evidence_kind", "persisted_gpu_parent_ref" }
        } );

        auto result = base();
        result["scan_complete"] = !BudgetPartial();
        result["definition_count"] = taxonomy["definitions"].size();
        result["taxonomy_zone_count"] = Decimal( taxonomyZoneCount );
        result["explicit_pass_zone_count"] = Decimal( explicitPassZoneCount );
        result["explicit_pass_missing_zone_count"] = Decimal( explicitPassMissingZoneCount );
        result["unknown_taxonomy_zone_count"] = Decimal( unknownTaxonomyZoneCount );
        result["level_counts"] = taxonomy["level_counts"];
        result["hierarchy_gate"] = {
            { "static_l0_has_multiple_l1", staticL0HasMultipleL1 },
            { "static_l1_has_multiple_l2", staticL1HasMultipleL2 },
            { "observed_l0_has_multiple_l1", observedL0HasMultipleL1 },
            { "observed_l1_has_multiple_l2", observedL1HasMultipleL2 }
        };
        result["quality"] = taxonomy["quality"];
        result["quality"]["unknown_taxonomy_zone_count"] = Decimal( unknownTaxonomyZoneCount );
        result["quality"]["budget_partial"] = BudgetPartial();

        if( method == "gpu.taxonomy.tree" )
        {
            result["nodes"] = std::move( nodes );
            result["logical_edges"] = taxonomy["logical_edges"];
            result["observed_edges"] = std::move( physicalEdges );
            result["status_capabilities"] = taxonomy["status_capabilities"];
            return Success( id, std::move( result ), trace );
        }

        const auto producerCoverage = CaptureCoverageJson( info() );
        json fallbackProducer = nullptr;
        json nativePassProducer = nullptr;
        json managedPassProducer = nullptr;
        if( producerCoverage.value( "present", false ) )
            for( const auto& producer : producerCoverage["producers"] )
            {
                if( producer.value( "key", "" ) == "gpu.taxonomy.fallback" ) fallbackProducer = producer;
                else if( producer.value( "key", "" ) == "gpu.pass.explicit" ) nativePassProducer = producer;
                else if( producer.value( "key", "" ) == "gpu.pass.managed" ) managedPassProducer = producer;
            }
        const bool classifierAvailable = fallbackProducer.is_object() && fallbackProducer.value( "complete", false );
        const bool nativePassComplete = nativePassProducer.is_object() && nativePassProducer.value( "complete", false );
        const bool managedPassComplete = managedPassProducer.is_object() && managedPassProducer.value( "complete", false );
        const bool explicitProducerComplete = nativePassComplete || managedPassComplete;
        const bool explicitAvailable =
            ( nativePassComplete && nativePassProducer.value( "effective", false ) ) ||
            ( managedPassComplete && managedPassProducer.value( "effective", false ) );
        const bool authorityAvailable = explicitAvailable && !BudgetPartial() && totalGpuBusyNs != 0;
        const bool authorityPassed = authorityAvailable && explicitPassMissingZoneCount == 0 &&
            explicitGpuBusyRatio >= 0.95 && nonExplicitGpuBusyRatio <= 0.05;
        const std::string explicitUnavailableReason = !explicitProducerComplete ? "no complete GPU pass producer counter window" :
            "all complete GPU pass producers are ineffective";
        const auto producerCounter = [&]( const char* name ) -> json {
            if( !classifierAvailable || !fallbackProducer.contains( "counters" ) ||
                !fallbackProducer["counters"].contains( name ) ) return nullptr;
            return fallbackProducer["counters"][name];
        };
        const auto capability = [&]( const char* name ) -> json {
            if( taxonomy["status_capabilities"].is_object() && taxonomy["status_capabilities"].contains( name ) )
                return taxonomy["status_capabilities"][name];
            return { { "available", false }, { "reason", "status capability was not declared by the producer" } };
        };
        result["statuses"] = {
            { "executed", { { "available", true }, { "count", Decimal( taxonomyZoneCount ) },
                { "unit", "gpu_zones" }, { "evidence_kind", "gpu_timestamp_zone" } } },
            { "explicit", { { "available", explicitAvailable },
                { "count", explicitAvailable ? json( Decimal( explicitPassZoneCount ) ) : json( nullptr ) },
                { "missing_gpu_zone_count", Decimal( explicitPassMissingZoneCount ) }, { "unit", "gpu_pass_instances" },
                { "evidence_kind", "gfx_entity_link_plus_gpu_timestamp_zone" },
                { "reason", explicitAvailable ? json( nullptr ) : json( explicitUnavailableReason ) } } },
            { "fallback", { { "available", classifierAvailable }, { "count", classifierAvailable ? json( Decimal( fallbackZoneCount ) ) : json( nullptr ) },
                { "classified_marker_count", producerCounter( "emitted" ) }, { "unit", "gpu_zones" },
                { "evidence_kind", "unity_marker_classifier_plus_gpu_timestamp_zone" },
                { "reason", classifierAvailable ? json( nullptr ) : json( "no complete gpu.taxonomy.fallback counter window" ) } } },
            { "unclassified", { { "available", classifierAvailable }, { "count", producerCounter( "filtered" ) },
                { "unit", "markers" }, { "evidence_kind", "producer_filtered_counter" },
                { "reason", classifierAvailable ? json( nullptr ) : json( "no complete gpu.taxonomy.fallback counter window" ) } } },
            { "authority", { { "available", authorityAvailable }, { "passed", authorityPassed },
                { "total_gpu_busy_ns", Decimal( totalGpuBusyNs ) },
                { "explicit_gpu_busy_ns", Decimal( explicitGpuBusyNs ) },
                { "fallback_or_unclassified_gpu_busy_ns", Decimal( nonExplicitGpuBusyNs ) },
                { "explicit_gpu_busy_ratio", explicitGpuBusyRatio },
                { "fallback_or_unclassified_gpu_busy_ratio", nonExplicitGpuBusyRatio },
                { "required_explicit_ratio", 0.95 }, { "maximum_fallback_or_unclassified_ratio", 0.05 },
                { "missing_gpu_zone_count", Decimal( explicitPassMissingZoneCount ) },
                { "contexts", std::move( gpuBusyContexts ) },
                { "uncovered_interval_count", Decimal( uncoveredGpuIntervals.size() ) },
                { "top_uncovered_intervals", std::move( topUncoveredGpuIntervals ) },
                { "timeline_envelope", {
                    { "total_ns", Decimal( timelineEnvelopeNs ) },
                    { "explicit_ns", Decimal( explicitTimelineEnvelopeNs ) },
                    { "non_explicit_ns", Decimal( nonExplicitTimelineEnvelopeNs ) },
                    { "explicit_coverage_ratio", explicitTimelineEnvelopeRatio },
                    { "uncovered_interval_count", Decimal( timelineUncoveredGpuIntervals.size() ) },
                    { "top_uncovered_intervals", std::move( topTimelineUncoveredGpuIntervals ) },
                    { "evidence_kind", "parent_gpu_zone_timeline_envelope" },
                    { "used_for_gate", false }
                } },
                { "evidence_kind", "per_context_complete_leaf_gpu_interval_union" },
                { "denominator_semantics", "instrumented_gpu_work_excludes_parent_envelope_idle_gaps" },
                { "reason", authorityAvailable ? json( nullptr ) :
                    json( BudgetPartial() ? "query budget was exhausted" :
                        ( totalGpuBusyNs == 0 ? "no complete GPU timestamp intervals" :
                            "no effective complete explicit GPU pass producer" ) ) } } },
            { "culled", capability( "culled" ) }, { "disabled", capability( "disabled" ) }
        };
        result["classifier_quality"] = {
            { "producer", fallbackProducer }, { "dropped", producerCounter( "dropped" ) },
            { "overflow", producerCounter( "overflow" ) }, { "mismatch", producerCounter( "mismatch" ) },
            { "unresolved", producerCounter( "unresolved" ) }
        };
        result["explicit_quality"] = { { "producer", nativePassProducer.is_object() ? nativePassProducer : managedPassProducer },
            { "producers", json::array( { nativePassProducer, managedPassProducer } ) },
            { "missing_gpu_zone_count", Decimal( explicitPassMissingZoneCount ) } };
        return Success( id, std::move( result ), trace );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto events = source->ScanContextSwitchEvents( range );
            BudgetScanned( events.size(), chunk, allowed );
            for( const auto& event : events ) if( event.threadRef == threadRef && event.endNs ) running.emplace_back( *event.endNs - event.startNs );
            offset += events.size();
            if( events.size() < allowed ) break;
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
                const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                auto range = ScanRangeFrom( params, scanOffset, allowed );
                const auto values = source->ScanContextSwitchEvents( range );
                BudgetScanned( values.size(), chunk, allowed );
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
                if( values.size() < allowed ) break;
            }
            const size_t begin = std::min( page.offset, all.size() );
            const size_t end = std::min( begin + page.limit, all.size() );
            json migrations = json::array();
            for( size_t index = begin; index < end; index++ ) migrations.emplace_back( std::move( all[index] ) );
            const auto cursor = NextCursor( page, method, trace, end - begin, end < all.size() );
            return Success( id, { { "thread_ref", threadRef }, { "migration_count", Decimal( all.size() ) }, { "migrations", std::move( migrations ) } }, trace, PageJson( page, end - begin, cursor ) );
        }
        auto scanPage = ScanFiltered<analysis::ContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanContextSwitchEvents( range ); },
            [&]( const auto& event ) { return event.threadRef == threadRef; }, ContextSwitchJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "thread_ref", threadRef }, { "context_switches", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
        auto scanPage = ScanFiltered<analysis::CpuContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanCpuContextSwitchEvents( range ); },
            [&]( const auto& event ) { return !cpuFilter || event.cpu == *cpuFilter; }, CpuContextSwitchJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "segments", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
    }
    if( method == "context_switch.range" || method == "context_switch.thread" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string threadRef = params.value( "thread_ref", "" );
        if( method == "context_switch.thread" && threadRef.empty() ) throw QueryError( "INVALID_PARAMS", "thread_ref is required" );
        const auto cpuFilter = params.contains( "cpu" ) ? std::optional<unsigned>( params["cpu"].get<unsigned>() ) : std::nullopt;
        auto scanPage = ScanFiltered<analysis::ContextSwitchDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanContextSwitchEvents( range ); },
            [&]( const auto& event ) { return ( threadRef.empty() || event.threadRef == threadRef ) && ( !cpuFilter || event.cpu == *cpuFilter ); }, ContextSwitchJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "context_switches", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto events = source->ScanContextSwitchEvents( range );
            BudgetScanned( events.size(), chunk, allowed );
            for( const auto& event : events )
            {
                if( !byThread.contains( event.threadRef ) )
                {
                    if( !BudgetConsumeGroup() ) continue;
                    byThread.try_emplace( event.threadRef );
                }
                if( event.endNs ) byThread[event.threadRef].emplace_back( *event.endNs - event.startNs );
                if( event.wakeupNs && event.startNs >= *event.wakeupNs ) wakeLatency[event.threadRef].emplace_back( event.startNs - *event.wakeupNs );
            }
            offset += events.size();
            if( events.size() < allowed ) break;
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
                    const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                    analysis::ScanRange range; range.offset = offset; range.limit = allowed;
                    const auto frames = source->ScanFrames( range );
                    BudgetScanned( frames.size(), chunk, allowed );
                    const auto found = std::find_if( frames.begin(), frames.end(), [&]( const auto& frame ) { return frame.ref == requested; } );
                    if( found != frames.end() ) return Success( id, FrameJson( *found ), trace );
                    offset += frames.size(); if( frames.size() < allowed ) break;
                }
                if( BudgetPartial() ) return Success( id, { { "present", false }, { "reason", "query budget exhausted before the frame ref was resolved" } }, trace );
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
        auto scanPage = ScanFiltered<analysis::CpuZoneDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanCpuZones( range ); },
            [&]( const auto& value ) { return TextMatches( value.name, params ); }, CpuZoneJson );
        scanPage.values = ProjectFields( std::move( scanPage.values ), params );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "zones", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
        auto scanPage = ScanFiltered<analysis::GpuZoneDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanGpuZones( range ); },
            [&]( const auto& value ) { return TextMatches( value.name, params ); }, GpuZoneJson );
        scanPage.values = ProjectFields( std::move( scanPage.values ), params );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "zones", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            if( gpu )
            {
                const auto values = source->ScanGpuZones( range );
                BudgetScanned( values.size(), chunk, allowed );
                for( const auto& value : values ) if( value.gpuEndNs && TextMatches( value.name, params ) )
                {
                    if( !groups.contains( value.sourceLocationRef ) && !BudgetConsumeGroup() ) continue;
                    auto& group = groups[value.sourceLocationRef];
                    group.name = value.name; group.file = value.file; group.line = value.line;
                    group.inclusive.emplace_back( *value.gpuEndNs - value.gpuStartNs );
                    if( value.selfTimeNs ) group.self.emplace_back( *value.selfTimeNs );
                }
                offset += values.size();
                if( values.size() < allowed ) break;
            }
            else
            {
                const auto values = source->ScanCpuZones( range );
                BudgetScanned( values.size(), chunk, allowed );
                for( const auto& value : values ) if( value.endNs && TextMatches( value.name, params ) )
                {
                    if( !groups.contains( value.sourceLocationRef ) && !BudgetConsumeGroup() ) continue;
                    auto& group = groups[value.sourceLocationRef];
                    group.name = value.name; group.file = value.file; group.line = value.line;
                    group.inclusive.emplace_back( *value.endNs - value.startNs );
                    if( value.selfTimeNs ) group.self.emplace_back( *value.selfTimeNs );
                    if( value.runningTimeNs ) group.running.emplace_back( *value.runningTimeNs );
                }
                offset += values.size();
                if( values.size() < allowed ) break;
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
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
                    if( !groups.contains( path ) && !BudgetConsumeGroup() ) continue;
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
                    if( !groups.contains( path ) && !BudgetConsumeGroup() ) continue;
                    auto& stats = groups[path]; stats.path = path; stats.count++; stats.inclusive += *zone.endNs - zone.startNs;
                    if( zone.selfTimeNs ) stats.self += *zone.selfTimeNs;
                }
            }
            BudgetScanned( received, chunk, allowed );
            offset += received; if( received < allowed ) break;
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
        auto scanPage = ScanFiltered<analysis::MemoryEventDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMemoryEvents( range ); },
            [&]( const auto& value ) { return ( pool.empty() || value.poolRef == pool ) && TextMatches( value.address, params ); }, MemoryEventJson );
        scanPage.values = ProjectFields( std::move( scanPage.values ), params );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "events", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto events = source->ScanMemoryEvents( range );
            BudgetScanned( events.size(), chunk, allowed );
            const auto found = std::find_if( events.begin(), events.end(), [&]( const auto& event ) { return event.ref == requested; } );
            if( found != events.end() ) return Success( id, MemoryEventJson( *found ), trace );
            offset += events.size(); if( events.size() < allowed ) break;
        }
        if( BudgetPartial() ) return Success( id, { { "present", false }, { "reason", "query budget exhausted before the memory event ref was resolved" } }, trace );
        throw QueryError( "ENTITY_NOT_FOUND", "memory event ref was not found" );
    }
    if( method == "memory.active_at_time" )
    {
        if( !params.contains( "time_ns" ) ) throw QueryError( "INVALID_PARAMS", "time_ns is required" );
        const auto time = ScanRangeFrom( json { { "start_ns", params["time_ns"] }, { "end_ns", Decimal( std::numeric_limits<int64_t>::max() ) } }, 0, 1 ).startNs;
        const auto page = ParsePage( params, method, trace );
        const std::string pool = params.value( "pool_ref", "" );
        auto scanPage = ScanFiltered<analysis::MemoryEventDto>( *source, json::object(), page,
            []( const auto& item, const auto& range ) { return item.ScanMemoryEvents( range ); },
            [&]( const auto& event ) { return ( pool.empty() || event.poolRef == pool ) && event.allocationNs <= time && ( !event.freeNs || *event.freeNs > time ); }, MemoryEventJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "time_ns", Decimal( time ) }, { "events", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto events = source->ScanMemoryEvents( range );
            BudgetScanned( events.size(), chunk, allowed );
            for( const auto& event : events ) if( !event.freeNs ) active.emplace_back( event );
            offset += events.size(); if( events.size() < allowed ) break;
        }
        if( method == "memory.callstack_tree" )
        {
            struct Group { uint64_t bytes = 0, count = 0; };
            std::map<uint32_t, Group> groups; for( const auto& event : active )
            {
                if( !groups.contains( event.allocationCallstack ) && !BudgetConsumeGroup() ) continue;
                auto& group = groups[event.allocationCallstack]; group.bytes += event.size; group.count++;
            }
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
    if( method == "memory.gpu.allocations" || method == "memory.gpu.request_scopes" || method == "memory.gpu.pass_uses" || method == "memory.gpu.attribution" ||
        method == "memory.gpu.summary" || method == "memory.gpu.residency" || method == "memory.gpu.fragmentation" || method == "memory.gpu.churn" )
    {
        const bool n10Method = method == "memory.gpu.summary" || method == "memory.gpu.residency" ||
            method == "memory.gpu.fragmentation" || method == "memory.gpu.churn";
        if( n10Method )
        {
            const auto protocol2 = source->HasGpuMemoryProtocol2();
            if( protocol2 && !*protocol2 )
                return Success( id, { { "present", false }, { "reason", "trace contains no GTMEM2 allocation-origin/residency protocol" } }, trace );
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
                requestedPass = source->ParseEntityRef( params["pass_ref"].get<std::string>(), "gpu-memory-pass" );
                if( !requestedPass ) throw QueryError( "ENTITY_NOT_FOUND", "GPU memory pass ref was not found" );
            }
            const auto indexedPage = source->ScanGpuMemoryPasses( page.offset, page.limit, requestedPass, page.offset, page.limit );
            if( indexedPage )
            {
                if( requestedPass )
                {
                    if( indexedPage->passes.empty() ) throw QueryError( "ENTITY_NOT_FOUND", "GPU memory pass id was not found" );
                    auto pass = GpuPassJson( *source, indexedPage->passes.front(), true, 0, page.limit );
                    pass["uses_returned"] = indexedPage->passes.front().uses.size();
                    pass["uses_truncated"] = page.offset + indexedPage->passes.front().uses.size() < indexedPage->totalUses;
                    const auto cursor = NextCursor( page, method, trace, indexedPage->passes.front().uses.size(), page.offset + indexedPage->passes.front().uses.size() < indexedPage->totalUses );
                    return Success( id, { { "pass", std::move( pass ) }, { "complete", indexedPage->passes.front().complete } },
                        trace, PageJson( page, indexedPage->passes.front().uses.size(), cursor ) );
                }
                json passes = json::array();
                for( const auto& pass : indexedPage->passes ) passes.emplace_back( GpuPassJson( *source, pass, false ) );
                const auto cursor = NextCursor( page, method, trace, indexedPage->passes.size(), page.offset + indexedPage->passes.size() < indexedPage->totalPasses );
                return Success( id, { { "passes", std::move( passes ) }, { "complete", indexedPage->complete }, { "warnings", indexedPage->warnings } },
                    trace, PageJson( page, indexedPage->passes.size(), cursor ) );
            }
        }
        if( method == "memory.gpu.request_scopes" )
        {
            const auto page = ParsePage( params, method, trace );
            const auto indexedPage = source->ScanGpuMemoryRequestScopes( page.offset, page.limit );
            if( indexedPage )
            {
                json scopes = json::array();
                for( size_t index = 0; index < indexedPage->scopes.size(); index++ )
                {
                    const auto& scope = indexedPage->scopes[index];
                    scopes.push_back( {
                        { "ref", source->MakeEntityRef( "gpu-memory-scope", page.offset + index ) },
                        { "label_id", Decimal( scope.labelId ) }, { "frame", Decimal( scope.frame ) },
                        { "thread_id", Decimal( scope.thread ) }, { "start_ns", Decimal( scope.start ) }, { "end_ns", Decimal( scope.end ) },
                        { "name", scope.name }, { "cpu_zone_ref", source->GetCpuZoneRef( scope.cpuZoneIndex ).value_or( "" ) },
                        { "trust", "untrusted_trace_data" }
                    } );
                }
                const auto returned = scopes.size();
                const auto hasMore = page.offset + returned < indexedPage->totalScopes;
                const auto cursor = NextCursor( page, method, trace, returned, hasMore );
                return Success( id, { { "scopes", std::move( scopes ) }, { "total_scopes", Decimal( indexedPage->totalScopes ) } },
                    trace, PageJson( page, returned, cursor ) );
            }
        }
        if( method == "memory.gpu.allocations" || method == "memory.gpu.attribution" )
        {
            const auto page = ParsePage( params, method, trace );
            const std::string relationFilter = params.value( "relation_state", "" );
            if( !relationFilter.empty() && relationFilter != "request_and_uses" && relationFilter != "request_only" && relationFilter != "uses_only" && relationFilter != "unattributed" )
                throw QueryError( "INVALID_PARAMS", "relation_state must be request_and_uses, request_only, uses_only, or unattributed" );
            const std::string poolFilter = params.value( "pool_ref", "" );
            std::optional<uint64_t> allocationFilter;
            if( params.contains( "allocation_id" ) )
            {
                try { allocationFilter = params["allocation_id"].is_string() ? std::stoull( params["allocation_id"].get<std::string>() ) : params["allocation_id"].get<uint64_t>(); }
                catch( const std::exception& ) { throw QueryError( "INVALID_PARAMS", "allocation_id must be an unsigned decimal string" ); }
            }
            const auto indexedPage = source->ScanGpuMemoryAllocations( page.offset, page.limit, allocationFilter, poolFilter, relationFilter );
            if( indexedPage )
            {
                json allocations = json::array();
                for( const auto& item : indexedPage->allocations )
                {
                    const auto event = source->GetMemoryEvent( item.attribution.allocation.key );
                    if( !event ) continue;
                    const std::string relationState = item.attribution.requestLabelId && item.passRefCount != 0 ? "request_and_uses" :
                        item.attribution.requestLabelId ? "request_only" : item.passRefCount != 0 ? "uses_only" : "unattributed";
                    json passes = json::array();
                    for( const auto passId : item.passIds ) passes.emplace_back( source->MakeEntityRef( "gpu-memory-pass", passId ) );
                    json allocationJson = {
                        { "allocation", MemoryEventJson( *event ) }, { "allocation_id", Decimal( item.attribution.allocation.allocationId ) },
                        { "request_label_id", item.attribution.requestLabelId ? json( Decimal( *item.attribution.requestLabelId ) ) : json( nullptr ) },
                        { "pass_ref_count", Decimal( item.passRefCount ) }, { "pass_refs", std::move( passes ) },
                        { "pass_refs_truncated", item.passRefCount > item.passIds.size() }, { "relation_state", relationState },
                        { "logical_resource", nullptr }
                    };
                    if( item.origin )
                    {
                        const auto& value = *item.origin;
                        const std::string availability = value.replayed || value.preCapture ? "unavailable_pre_capture_or_replay" :
                            value.callstackRequested == 0 ? "disabled" : event->allocationCallstack != 0 ? "available" : "requested_but_unresolved";
                        allocationJson["origin"] = {
                            { "layer", std::string( 1, value.layer ) }, { "connection_id", Decimal( value.connectionId ) },
                            { "replayed", value.replayed }, { "pre_capture", value.preCapture },
                            { "callstack_requested", value.callstackRequested }, { "callstack_emitted", value.callstackEmitted },
                            { "callstack_availability", availability }, { "residency", std::string( 1, value.residency ) },
                            { "residency_managed", value.residencyManaged },
                            { "cpu_zone_ref", source->GetCpuZoneRef( value.cpuZoneIndex ).value_or( "" ) }
                        };
                    }
                    else allocationJson["origin"] = nullptr;
                    if( item.logicalResource )
                    {
                        const auto& resource = *item.logicalResource;
                        allocationJson["logical_resource"] = {
                            { "logical_resource_id", Decimal( resource.logicalResourceId ) }, { "physical_allocation_id", Decimal( resource.physicalAllocationId ) },
                            { "size_bytes", Decimal( resource.size ) }, { "physical_offset_bytes", Decimal( resource.physicalOffset ) },
                            { "primary_owner_id", Decimal( uint64_t( resource.primaryOwnerId ) ) }, { "physical_owner_id", Decimal( uint64_t( resource.physicalOwnerId ) ) },
                            { "kind", std::string( 1, resource.kind ) }, { "segment", std::string( 1, resource.segment ) },
                            { "flags", resource.flags }, { "name", resource.name }, { "trust", "untrusted_trace_data" }
                        };
                    }
                    allocations.emplace_back( std::move( allocationJson ) );
                }
                const auto returned = allocations.size();
                const auto cursor = NextCursor( page, method, trace, returned, indexedPage->hasMore );
                json data = { { "allocations", std::move( allocations ) }, { "protocol_present", indexedPage->protocolPresent },
                    { "complete", indexedPage->complete }, { "warnings", indexedPage->warnings } };
                if( method == "memory.gpu.attribution" )
                {
                    const auto summary = source->GetGpuMemorySummaryAttribution();
                    json passes = json::array();
                    const auto passPage = source->ScanGpuMemoryPasses( 0, DefaultTopN, std::nullopt, 0, 0 );
                    if( passPage ) for( const auto& pass : passPage->passes ) passes.emplace_back( GpuPassJson( *source, pass, false ) );
                    data["pass_count"] = Decimal( passPage ? passPage->totalPasses : summary.passes.size() );
                    data["pass_preview"] = std::move( passes );
                    json logicalResources = json::array();
                    for( size_t index = 0; index < std::min<size_t>( summary.logicalResources.size(), DefaultPageSize ); index++ )
                    {
                        const auto& resource = summary.logicalResources[index];
                        logicalResources.push_back( {
                            { "logical_resource_id", Decimal( resource.logicalResourceId ) }, { "physical_allocation_id", Decimal( resource.physicalAllocationId ) },
                            { "size_bytes", Decimal( resource.size ) }, { "physical_offset_bytes", Decimal( resource.physicalOffset ) },
                            { "primary_owner_id", Decimal( uint64_t( resource.primaryOwnerId ) ) }, { "physical_owner_id", Decimal( uint64_t( resource.physicalOwnerId ) ) },
                            { "kind", std::string( 1, resource.kind ) }, { "segment", std::string( 1, resource.segment ) },
                            { "flags", resource.flags }, { "name", resource.name }, { "trust", "untrusted_trace_data" }
                        } );
                    }
                    json ownerRollups = json::array();
                    for( const auto& rollup : summary.ownerRollups ) ownerRollups.push_back( {
                        { "taxonomy_id", Decimal( uint64_t( rollup.taxonomyId ) ) }, { "owned_physical_bytes", Decimal( rollup.physicalBytes ) },
                        { "physical_allocation_count", Decimal( rollup.physicalAllocationCount ) }, { "logical_resource_count", Decimal( rollup.logicalResourceCount ) },
                        { "owner_kind", rollup.taxonomyId == 0 ? "shared_or_unclassified" : "taxonomy" }
                    } );
                    data["logical_resource_count"] = Decimal( summary.logicalResources.size() ); data["logical_resources"] = std::move( logicalResources );
                    data["logical_resources_truncated"] = summary.logicalResources.size() > DefaultPageSize;
                    data["owner_rollups"] = std::move( ownerRollups ); data["working_sets"] = json::array();
                    data["working_sets_truncated"] = summary.aggregatedWorkingSetCount != 0;
                    data["working_set_count"] = Decimal( summary.aggregatedWorkingSetCount );
                    data["working_set_preview_availability"] = "use memory.gpu.pass_uses for exact paged evidence; the indexed summary stores aggregate cardinality";
                    data["rollup_semantics"] = {
                        { "owner", "each physical allocation is counted once under one primary owner" },
                        { "shared_heap", "taxonomy_id 0 is the explicit Shared/Unclassified owner; a heap is never assigned to its first placed resource" },
                        { "working_set", "deduplicated by physical allocation within each frame and taxonomy node" },
                        { "sibling_sum", "working sets of sibling taxonomy nodes may overlap and must not be summed as physical total" }
                    };
                    json unknownUses = json::array();
                    for( size_t index = 0; index < std::min<size_t>( summary.unknownUses.size(), DefaultPageSize ); index++ )
                    {
                        const auto& value = summary.unknownUses[index];
                        unknownUses.push_back( {
                            { "allocation_id", Decimal( value.allocationId ) },
                            { "physical_allocation_id", value.physicalAllocationId == 0 ? json( nullptr ) : json( Decimal( value.physicalAllocationId ) ) },
                            { "classification", value.classification }, { "occurrence_count", Decimal( value.occurrenceCount ) },
                            { "first_frame", Decimal( value.firstFrame ) }, { "last_frame", Decimal( value.lastFrame ) },
                            { "first_pass_ref", source->MakeEntityRef( "gpu-memory-pass", value.firstPassId ) },
                            { "last_pass_ref", source->MakeEntityRef( "gpu-memory-pass", value.lastPassId ) },
                            { "logical_metadata_present", value.logicalMetadataPresent }, { "logical_pool_event_present", value.logicalPoolEventPresent },
                            { "physical_pool_event_present", value.physicalPoolEventPresent }, { "trust", "untrusted_trace_data" }
                        } );
                    }
                    data["unknown_use_occurrences"] = Decimal( summary.unknownUseOccurrences );
                    data["unknown_unique_resource_classifications"] = Decimal( summary.unknownUses.size() );
                    data["unknown_uses"] = std::move( unknownUses );
                    data["unknown_uses_truncated"] = summary.unknownUses.size() > DefaultPageSize;
                }
                return Success( id, std::move( data ), trace, PageJson( page, returned, cursor ) );
            }
        }
        const auto attributionValue = CachedGpuAttribution( trace.id, source, n10Method );
        const auto& attribution = *attributionValue;
        if( !attribution.protocolPresent && method != "memory.gpu.allocations" ) throw QueryError( "CAPABILITY_UNAVAILABLE", "trace contains no GTMEM1 relation protocol" );
        if( n10Method && !attribution.protocol2Present )
            return Success( id, { { "present", false }, { "reason", "trace contains no GTMEM2 allocation-origin/residency protocol" } }, trace );

        if( method == "memory.gpu.churn" )
        {
            const auto& churn = attribution.churn;
            return Success( id, { { "present", true }, { "semantics", "live allocation churn excludes replayed/pre-capture baseline allocations" },
                { "peak_physical_bytes", Decimal( churn.peakPhysicalBytes ) }, { "active_physical_bytes", Decimal( churn.activePhysicalBytes ) },
                { "created_bytes", Decimal( churn.createdBytes ) }, { "created_count", Decimal( churn.createdCount ) },
                { "freed_bytes", Decimal( churn.freedBytes ) }, { "freed_count", Decimal( churn.freedCount ) },
                { "freed_from_baseline_bytes", Decimal( churn.freedFromBaselineBytes ) },
                { "freed_from_baseline_count", Decimal( churn.freedFromBaselineCount ) } }, trace );
        }
        if( method == "memory.gpu.fragmentation" )
        {
            const auto page = ParsePage( params, method, trace );
            const size_t begin = std::min( page.offset, attribution.fragmentation.size() );
            const size_t end = std::min( begin + page.limit, attribution.fragmentation.size() );
            json heaps = json::array();
            for( size_t index = begin; index < end; index++ )
            {
                const auto& heap = attribution.fragmentation[index];
                heaps.push_back( { { "heap_allocation_id", Decimal( heap.heapId ) },
                    { "capacity_bytes", Decimal( heap.capacityBytes ) }, { "requested_bytes", Decimal( heap.requestedBytes ) },
                    { "covered_bytes", Decimal( heap.coveredBytes ) }, { "aliased_bytes", Decimal( heap.aliasedBytes ) },
                    { "free_bytes", Decimal( heap.freeBytes ) }, { "largest_free_block_bytes", Decimal( heap.largestFreeBlockBytes ) },
                    { "logical_resource_count", Decimal( heap.logicalResourceCount ) },
                    { "external_fragmentation_ratio", heap.externalFragmentationRatio } } );
            }
            const auto cursor = NextCursor( page, method, trace, end - begin, end < attribution.fragmentation.size() );
            return Success( id, { { "present", true }, { "heaps", std::move( heaps ) },
                { "semantics", "active placed-resource intervals at capture end; alias overlap is reported separately and is not fragmentation" } },
                trace, PageJson( page, end - begin, cursor ) );
        }
        if( method == "memory.gpu.residency" )
        {
            const auto page = ParsePage( params, method, trace );
            const size_t begin = std::min( page.offset, attribution.residencyEvents.size() );
            const size_t end = std::min( begin + page.limit, attribution.residencyEvents.size() );
            json events = json::array();
            for( size_t index = begin; index < end; index++ )
            {
                const auto& event = attribution.residencyEvents[index];
                events.push_back( { { "allocation_id", Decimal( event.allocationId ) }, { "frame_id", Decimal( event.frame ) },
                    { "fence_value", Decimal( event.fence ) }, { "size_bytes", Decimal( event.size ) },
                    { "connection_id", Decimal( event.connectionId ) }, { "state", std::string( 1, event.state ) },
                    { "reason", event.reason }, { "replayed", event.replayed }, { "flags", event.flags },
                    { "time_ns", Decimal( event.timeNs ) },
                    { "cpu_zone_ref", source->GetCpuZoneRef( event.cpuZoneIndex ).value_or( "" ) } } );
            }
            const auto& residency = attribution.residency;
            const auto cursor = NextCursor( page, method, trace, end - begin, end < attribution.residencyEvents.size() );
            return Success( id, { { "present", true }, { "current", {
                    { "resident_bytes", Decimal( residency.residentBytes ) }, { "resident_count", Decimal( residency.residentCount ) },
                    { "evicted_bytes", Decimal( residency.evictedBytes ) }, { "evicted_count", Decimal( residency.evictedCount ) },
                    { "unknown_bytes", Decimal( residency.unknownBytes ) }, { "unknown_count", Decimal( residency.unknownCount ) } } },
                { "events", std::move( events ) }, { "sampling_interval_ms", 250 },
                { "semantics", "D3DX12 library residency state; OS physical page migration may not be observable as a state transition" } },
                trace, PageJson( page, end - begin, cursor ) );
        }
        if( method == "memory.gpu.summary" )
        {
            static const std::set<std::string> wantedPlots = {
                "GPU.DXGI.Local.UsageBytes", "GPU.DXGI.Local.BudgetBytes", "GPU.DXGI.Local.ReservationBytes",
                "GPU.DXGI.NonLocal.UsageBytes", "GPU.DXGI.NonLocal.BudgetBytes", "GPU.DXGI.NonLocal.ReservationBytes",
                "GPU.VRAM.EngineKnownPhysical.LocalBytes", "GPU.VRAM.EngineKnownPhysical.NonLocalBytes"
            };
            std::unordered_map<std::string, std::string> plotNameByRef;
            for( const auto& plot : source->GetPlotList() ) if( wantedPlots.count( plot.name ) != 0 ) plotNameByRef[plot.ref] = plot.name;
            struct LatestPlot { int64_t time = std::numeric_limits<int64_t>::min(); double value = 0; bool present = false; };
            std::unordered_map<std::string, LatestPlot> latest;
            size_t offset = 0;
            constexpr size_t chunk = 4096;
            while( true )
            {
                checkCancelled();
                const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                analysis::ScanRange range; range.offset = offset; range.limit = allowed;
                const auto points = source->ScanPlots( range );
                BudgetScanned( points.size(), chunk, allowed );
                for( const auto& point : points )
                {
                    const auto name = plotNameByRef.find( point.plotRef );
                    if( name == plotNameByRef.end() ) continue;
                    auto& value = latest[name->second];
                    if( !value.present || point.timeNs >= value.time ) value = { point.timeNs, point.value, true };
                }
                offset += points.size();
                if( points.size() < allowed ) break;
            }
            const bool budgetPartial = BudgetPartial();
            const auto bytes = [&]( const char* name ) -> uint64_t {
                const auto found = latest.find( name );
                return found == latest.end() || !found->second.present || found->second.value <= 0 ? 0 : uint64_t( found->second.value );
            };
            const auto segment = [&]( const char* prefix, const char* knownName ) {
                const std::string base = std::string( "GPU.DXGI." ) + prefix;
                const uint64_t usage = bytes( ( base + ".UsageBytes" ).c_str() );
                const uint64_t known = bytes( knownName );
                return json { { "usage_bytes", Decimal( usage ) }, { "budget_bytes", Decimal( bytes( ( base + ".BudgetBytes" ).c_str() ) ) },
                    { "reservation_bytes", Decimal( bytes( ( base + ".ReservationBytes" ).c_str() ) ) },
                    { "engine_known_physical_bytes", Decimal( known ) },
                    { "implicit_untracked_bytes", Decimal( usage > known ? usage - known : 0 ) },
                    { "overtracked_bytes", Decimal( known > usage ? known - usage : 0 ) } };
            };
            const auto& churn = attribution.churn;
            const auto& residency = attribution.residency;
            json registryQuality = {
                { "present", false }, { "complete", false }, { "producer_key", "memory.gpu.registry" },
                { "source_mode", nullptr }, { "state", "missing" }, { "counters", json::object() },
                { "incremental_contract", "authoritative_snapshot_then_incremental" }
            };
            const auto producerCoverage = CaptureCoverageJson( info() );
            if( producerCoverage.contains( "producers" ) && producerCoverage["producers"].is_array() )
            {
                const auto registry = std::find_if( producerCoverage["producers"].begin(), producerCoverage["producers"].end(),
                    []( const auto& producer ) { return producer.value( "key", "" ) == "memory.gpu.registry"; } );
                if( registry != producerCoverage["producers"].end() )
                {
                    const auto& counters = registry->at( "counters" );
                    const auto counterIsZero = [&]( const char* name ) {
                        if( !counters.contains( name ) ) return false;
                        const auto value = DecimalStringValue( counters[name] );
                        return value.has_value() && *value == 0;
                    };
                    const bool clean = registry->value( "complete", false ) &&
                        counterIsZero( "dropped" ) && counterIsZero( "overflow" ) &&
                        counterIsZero( "mismatch" ) && counterIsZero( "unresolved" ) &&
                        counterIsZero( "tail_truncated" );
                    registryQuality = {
                        { "present", true }, { "complete", clean }, { "producer_key", "memory.gpu.registry" },
                        { "source_mode", registry->value( "source_mode", "" ) },
                        { "state", registry->value( "state", "unknown" ) }, { "counters", counters },
                        { "incremental_contract", "authoritative_snapshot_then_incremental" }
                    };
                }
            }
            json summaryWarnings = attribution.warnings;
            if( budgetPartial )
                summaryWarnings.push_back( "query budget exhausted before the GPU memory summary scan completed; aggregate values are unavailable" );
            if( !registryQuality.value( "complete", false ) )
                summaryWarnings.push_back( "GPU memory registry producer is missing, incomplete, or contains dropped/unresolved data" );
            uint64_t incompleteReferencePasses = attribution.passQualityAggregated ? attribution.aggregatedIncompleteReferencePasses : 0;
            uint64_t structuredIncompleteReferencePasses = attribution.passQualityAggregated ? attribution.aggregatedStructuredIncompleteReferencePasses : 0;
            uint64_t legacyIncompleteReferencePasses = attribution.passQualityAggregated ? attribution.aggregatedLegacyIncompleteReferencePasses : 0;
            uint64_t truncatedReferencePasses = attribution.passQualityAggregated ? attribution.aggregatedTruncatedReferencePasses : 0;
            uint64_t failureFlagReferencePasses = attribution.passQualityAggregated ? attribution.aggregatedFailureFlagReferencePasses : 0;
            uint64_t commandListBoundaryPasses = attribution.passQualityAggregated ? attribution.aggregatedCommandListBoundaryPasses : 0;
            uint64_t droppedReferenceUses = attribution.passQualityAggregated ? attribution.aggregatedDroppedReferenceUses : 0;
            json incompleteReferencePreview = json::array();
            const auto appendIncompletePreview = [&]( const auto& pass )
            {
                if( incompleteReferencePreview.size() < 16 )
                {
                    incompleteReferencePreview.push_back( {
                        { "pass_id", Decimal( pass.passId ) }, { "frame", Decimal( pass.frame ) },
                        { "structured_binary", pass.structuredBinary }, { "flags", pass.flags },
                        { "truncated", pass.truncated }, { "dropped_uses", Decimal( uint64_t( pass.droppedUses ) ) },
                        { "emitted_use_count", Decimal( uint64_t( pass.emittedUseCount ) ) },
                        { "total_use_count", Decimal( uint64_t( pass.totalUseCount ) ) },
                        { "gpu_pairing", analysis::ToString( pass.gpuPairing ) }
                    } );
                }
            };
            if( attribution.passQualityAggregated )
            {
                for( const auto& pass : attribution.aggregatedIncompleteReferencePreview ) appendIncompletePreview( pass );
            }
            else for( const auto& pass : attribution.passes )
            {
                if( ( pass.flags & uint8_t( JnGpuReferenceFlags::CommandListBoundary ) ) != 0 ) commandListBoundaryPasses++;
                if( pass.complete || pass.gpuPairing == analysis::GpuZonePairing::CaptureBoundary ) continue;
                incompleteReferencePasses++;
                if( pass.structuredBinary ) structuredIncompleteReferencePasses++; else legacyIncompleteReferencePasses++;
                if( pass.truncated ) truncatedReferencePasses++;
                if( ( pass.flags & 0xA ) != 0 ) failureFlagReferencePasses++;
                droppedReferenceUses += pass.droppedUses;
                appendIncompletePreview( pass );
            }
            if( incompleteReferencePasses != 0 && summaryWarnings.empty() )
                summaryWarnings.push_back( std::to_string( incompleteReferencePasses ) +
                    " GPU reference pass(es) are incomplete outside the accepted capture boundary" );
            const bool summaryComplete = attribution.complete && registryQuality.value( "complete", false ) && !budgetPartial;
            const auto valueOrNull = [&]( uint64_t value ) -> json { return budgetPartial ? json( nullptr ) : json( Decimal( value ) ); };
            json dxgiReconciliation = {
                { "available", !budgetPartial },
                { "status", budgetPartial ? "unavailable_budget_partial" : "complete" },
                { "local", budgetPartial ? json( nullptr ) : segment( "Local", "GPU.VRAM.EngineKnownPhysical.LocalBytes" ) },
                { "non_local", budgetPartial ? json( nullptr ) : segment( "NonLocal", "GPU.VRAM.EngineKnownPhysical.NonLocalBytes" ) },
                { "semantics", "Implicit/Untracked is max(DXGI Usage - EngineKnownPhysical, 0) and is not proof of a leak" }
            };
            return Success( id, { { "present", true }, { "protocol", attribution.structuredReferencePresent ? "JN_GPU_REFERENCE_2" : "GTMEM2" },
                { "data_available", !budgetPartial },
                { "data_status", budgetPartial ? "unavailable_budget_partial" : summaryComplete ? "complete" : "available_incomplete" },
                { "dxgi_reconciliation", std::move( dxgiReconciliation ) },
                { "physical", { { "active_bytes", valueOrNull( churn.activePhysicalBytes ) }, { "peak_bytes", valueOrNull( churn.peakPhysicalBytes ) } } },
                { "logical_resource_count", valueOrNull( attribution.logicalResources.size() ) },
                { "owner_rollup_count", valueOrNull( attribution.ownerRollups.size() ) },
                { "working_set_count", valueOrNull( attribution.passQualityAggregated ? attribution.aggregatedWorkingSetCount : attribution.workingSets.size() ) },
                { "layers", {
                    { "cpu_allocation", { { "domain", "memory" }, { "included", false },
                        { "semantics", "Unity native CPU allocations are separate from GPU memory" } } },
                    { "gpu_physical", { { "domain", "memory.gpu" }, { "identity", "physical_allocation_id" },
                        { "semantics", "committed resources and heaps; bytes are counted once" } } },
                    { "gpu_logical", { { "domain", "memory.gpu" }, { "identity", "logical_resource_id" },
                        { "semantics", "textures, buffers, upload and readback resources mapped to physical allocations" } } },
                    { "primary_owner", { { "identity", "taxonomy_id" }, { "cardinality", "exactly_one_per_logical_resource" },
                        { "semantics", "owner bytes are non-duplicating rollups" } } },
                    { "pass_reference", { { "identity", "frame_taxonomy_physical_allocation" },
                        { "semantics", "per-node deduplicated working set; sibling nodes are not additive" } } }
                } },
                { "allocation_callstack_semantics", "captured at physical/logical create when enabled; references do not capture allocation stacks" },
                { "heap_fragmentation_count", valueOrNull( attribution.fragmentation.size() ) },
                { "residency", { { "resident_bytes", valueOrNull( residency.residentBytes ) }, { "evicted_bytes", valueOrNull( residency.evictedBytes ) },
                    { "unknown_bytes", valueOrNull( residency.unknownBytes ) } } },
                { "quality", { { "complete", summaryComplete }, { "warnings", std::move( summaryWarnings ) },
                    { "budget_partial", budgetPartial },
                    { "registry", std::move( registryQuality ) },
                    { "capture_boundary_passes", Decimal( attribution.captureBoundaryPasses ) },
                    { "submission_unobserved_passes", Decimal( attribution.submissionUnobservedPasses ) },
                    { "gpu_result_unavailable_passes", Decimal( attribution.gpuResultUnavailablePasses ) },
                    { "incomplete_reference_passes", Decimal( incompleteReferencePasses ) },
                    { "structured_incomplete_reference_passes", Decimal( structuredIncompleteReferencePasses ) },
                    { "legacy_incomplete_reference_passes", Decimal( legacyIncompleteReferencePasses ) },
                    { "truncated_reference_passes", Decimal( truncatedReferencePasses ) },
                    { "failure_flag_reference_passes", Decimal( failureFlagReferencePasses ) },
                    { "command_list_boundary_passes", Decimal( commandListBoundaryPasses ) },
                    { "dropped_reference_uses", Decimal( droppedReferenceUses ) },
                    { "incomplete_reference_preview", std::move( incompleteReferencePreview ) } } } }, trace );
        }
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
            const bool logicalPool = item.allocation.poolName.rfind( "GPU D3D12 Logical ", 0 ) == 0;
            const auto& originMap = logicalPool ? attribution.logicalOriginById : attribution.physicalOriginById;
            const auto origin = originMap.find( item.allocation.allocationId );
            if( origin != originMap.end() && origin->second < attribution.origins.size() )
            {
                const auto& value = attribution.origins[origin->second];
                const std::string availability = value.replayed || value.preCapture ? "unavailable_pre_capture_or_replay" :
                    value.callstackRequested == 0 ? "disabled" : event->allocationCallstack != 0 ? "available" : "requested_but_unresolved";
                allocationJson["origin"] = {
                    { "layer", std::string( 1, value.layer ) }, { "connection_id", Decimal( value.connectionId ) },
                    { "replayed", value.replayed }, { "pre_capture", value.preCapture },
                    { "callstack_requested", value.callstackRequested }, { "callstack_emitted", value.callstackEmitted },
                    { "callstack_availability", availability }, { "residency", std::string( 1, value.residency ) },
                    { "residency_managed", value.residencyManaged },
                    { "cpu_zone_ref", source->GetCpuZoneRef( value.cpuZoneIndex ).value_or( "" ) }
                };
            }
            else allocationJson["origin"] = nullptr;
            if( logicalPool )
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
                { "logical_resource_count", Decimal( rollup.logicalResourceCount ) },
                { "owner_kind", rollup.taxonomyId == 0 ? "shared_or_unclassified" : "taxonomy" }
            } );
            json workingSets = json::array();
            for( size_t index = 0; index < std::min<size_t>( attribution.workingSets.size(), MaximumPageSize ); index++ )
            {
                const auto& workingSet = attribution.workingSets[index];
                workingSets.push_back( {
                    { "frame", Decimal( workingSet.frame ) }, { "taxonomy_id", Decimal( uint64_t( workingSet.taxonomyId ) ) },
                    { "referenced_working_set_bytes", Decimal( workingSet.referencedPhysicalBytes ) },
                    { "physical_allocation_count", Decimal( workingSet.physicalAllocationCount ) },
                    { "logical_resource_count", Decimal( workingSet.logicalResourceCount ) },
                    { "inclusive_referenced_working_set_bytes", Decimal( workingSet.inclusiveReferencedPhysicalBytes ) },
                    { "inclusive_physical_allocation_count", Decimal( workingSet.inclusivePhysicalAllocationCount ) },
                    { "inclusive_logical_resource_count", Decimal( workingSet.inclusiveLogicalResourceCount ) },
                    { "provenance", workingSet.provenance }
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
                { "shared_heap", "taxonomy_id 0 is the explicit Shared/Unclassified owner; a heap is never assigned to its first placed resource" },
                { "working_set", "deduplicated by physical allocation within each frame and taxonomy node" },
                { "sibling_sum", "working sets of sibling taxonomy nodes may overlap and must not be summed as physical total" }
            };
            json unknownUses = json::array();
            for( size_t index = 0; index < std::min<size_t>( attribution.unknownUses.size(), DefaultPageSize ); index++ )
            {
                const auto& value = attribution.unknownUses[index];
                unknownUses.push_back( {
                    { "allocation_id", Decimal( value.allocationId ) },
                    { "physical_allocation_id", value.physicalAllocationId == 0 ? json( nullptr ) : json( Decimal( value.physicalAllocationId ) ) },
                    { "classification", value.classification }, { "occurrence_count", Decimal( value.occurrenceCount ) },
                    { "first_frame", Decimal( value.firstFrame ) }, { "last_frame", Decimal( value.lastFrame ) },
                    { "first_pass_ref", source->MakeEntityRef( "gpu-memory-pass", value.firstPassId ) },
                    { "last_pass_ref", source->MakeEntityRef( "gpu-memory-pass", value.lastPassId ) },
                    { "logical_metadata_present", value.logicalMetadataPresent }, { "logical_pool_event_present", value.logicalPoolEventPresent },
                    { "physical_pool_event_present", value.physicalPoolEventPresent }, { "trust", "untrusted_trace_data" }
                } );
            }
            data["unknown_use_occurrences"] = Decimal( attribution.unknownUseOccurrences );
            data["unknown_unique_resource_classifications"] = Decimal( attribution.unknownUses.size() );
            data["unknown_uses"] = std::move( unknownUses );
            data["unknown_uses_truncated"] = attribution.unknownUses.size() > DefaultPageSize;
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
        auto scanPage = ScanFiltered<analysis::LockEventDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanLockEvents( range ); },
            [&]( const auto& event ) { return lockRef.empty() || event.lockRef == lockRef; }, LockEventJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "events", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto events = source->ScanLockEvents( range );
            BudgetScanned( events.size(), chunk, allowed );
            for( const auto& event : events )
            {
                if( !groups.contains( event.lockRef ) && !BudgetConsumeGroup() ) continue;
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
            if( events.size() < allowed ) break;
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
        auto scanPage = ScanFiltered<analysis::PlotPointDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanPlots( range ); },
            [&]( const auto& value ) { return plot.empty() || value.plotRef == plot; },
            []( const auto& value ) { return json { { "ref", value.ref }, { "plot_ref", value.plotRef }, { "time_ns", Decimal( value.timeNs ) }, { "value", value.value } }; } );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "points", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto values = source->ScanPlots( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& point : values ) if( point.plotRef == plot ) points.emplace_back( point );
            offset += values.size();
            if( values.size() < allowed ) break;
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
        auto scanPage = ScanFiltered<analysis::MessageDto>( *source, params, page,
            []( const auto& source, const auto& range ) { return source.ScanMessages( range ); },
            [&]( const auto& value ) { return ( requestedRef.empty() || value.ref == requestedRef ) && ( threadRef.empty() || value.threadRef == threadRef ) &&
                ( !callstack || value.callstack == *callstack ) && TextMatches( value.text, params ); }, MessageJson );
        if( method == "message.get" )
        {
            if( scanPage.values.empty() )
            {
                if( BudgetPartial() ) return Success( id, { { "present", false }, { "reason", "query budget exhausted before the message ref was resolved" } }, trace );
                throw QueryError( "ENTITY_NOT_FOUND", "message ref was not found" );
            }
            return Success( id, std::move( scanPage.values.front() ), trace );
        }
        scanPage.values = ProjectFields( std::move( scanPage.values ), params );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "messages", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
    }
    if( method == "evidence.graph" || method == "frame.critical_path" || method == "frame.explain" )
    {
        const auto frameEvents = source->GetCorrelatedFrameEvents();
        if( frameEvents.empty() )
            return Success( id, { { "present", false }, { "complete", false },
                { "reason", "trace predates or does not contain JN FrameIdentity evidence" },
                { "evidence_kind", "unavailable" } }, trace );

        uint64_t frameId = 0;
        if( params.contains( "ref" ) )
        {
            if( !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref must be a FrameIdentity ref" );
            const auto parsed = source->ParseEntityRef( params["ref"].get<std::string>(), "frame-identity" );
            if( !parsed ) throw QueryError( "INVALID_PARAMS", "ref must be a FrameIdentity ref from this trace" );
            frameId = *parsed;
        }
        else if( params.contains( "frame_id" ) )
        {
            frameId = UnsignedParameter( params, "frame_id", 0, std::numeric_limits<uint64_t>::max() );
        }
        else throw QueryError( "INVALID_PARAMS", "ref or frame_id is required" );

        int64_t frameBegin = std::numeric_limits<int64_t>::max();
        int64_t frameEnd = std::numeric_limits<int64_t>::min();
        bool canonicalBegin = false;
        bool canonicalEnd = false;
        json frameEventRefs = json::array();
        for( const auto& event : frameEvents ) if( event.frameId == frameId )
        {
            frameBegin = std::min( frameBegin, event.timeNs );
            frameEnd = std::max( frameEnd, event.timeNs );
            canonicalBegin |= event.phase == 0 && ( event.flags & 1 ) != 0;
            canonicalEnd |= event.phase == 1 && ( event.flags & 1 ) != 0;
            frameEventRefs.emplace_back( event.ref );
        }
        if( frameBegin == std::numeric_limits<int64_t>::max() ) throw QueryError( "ENTITY_NOT_FOUND", "FrameIdentity was not found" );
        const bool frameComplete = canonicalBegin && canonicalEnd && frameEnd >= frameBegin;
        if( frameEnd < frameBegin ) frameEnd = frameBegin;
        const auto frameRef = source->MakeEntityRef( "frame-identity", frameId );

        const auto readBool = [&]( const char* name, bool fallback ) {
            if( !params.contains( name ) ) return fallback;
            if( !params[name].is_boolean() ) throw QueryError( "INVALID_PARAMS", std::string( name ) + " must be boolean" );
            return params[name].get<bool>();
        };
        const bool includeExact = readBool( "include_exact", true );
        const bool includeDerived = readBool( "include_derived", true );
        const bool includeHeuristic = readBool( "include_heuristic", false );
        const auto maxNodes = size_t( UnsignedParameter( params, "max_nodes", 10000, MaximumMaxNodes ) );
        const auto maxEdges = size_t( UnsignedParameter( params, "max_edges", DefaultMaxEdges, MaximumMaxEdges ) );
        std::set<std::string> selectedDomains;
        if( params.contains( "domains" ) )
        {
            if( !params["domains"].is_array() ) throw QueryError( "INVALID_PARAMS", "domains must be an array of strings" );
            for( const auto& domain : params["domains"] )
            {
                if( !domain.is_string() ) throw QueryError( "INVALID_PARAMS", "domains must contain only strings" );
                selectedDomains.emplace( domain.get<std::string>() );
            }
        }

        EvidenceGraphBuilder graph( *source, frameRef, frameBegin, frameEnd, maxNodes, maxEdges,
            includeExact, includeDerived, includeHeuristic, std::move( selectedDomains ) );
        graph.root = graph.AddNode( "frame:" + std::to_string( frameId ), frameRef, "frame", "frame",
            "Canonical Frame", frameBegin, frameEnd, {}, {}, std::nullopt, std::nullopt, frameComplete,
            true, { { "frame_id", Decimal( frameId ) }, { "connection_generation", uint16_t( frameId >> 48 ) },
                { "sequence", uint32_t( frameId ) }, { "event_refs", std::move( frameEventRefs ) } }, true );

        const auto jobs = source->GetEvidenceJobs( frameId );
        std::unordered_map<uint64_t, const analysis::JobDto*> jobsById;
        for( const auto& job : jobs ) jobsById[job.jobId] = &job;
        std::set<uint64_t> selectedJobIds;
        for( const auto& job : jobs ) if( job.originFrameId == frameId ) selectedJobIds.emplace( job.jobId );
        std::queue<uint64_t> dependencyFrontier;
        for( const auto jobId : selectedJobIds ) dependencyFrontier.push( jobId );
        std::set<uint64_t> expandedDependencies;
        while( !dependencyFrontier.empty() )
        {
            const auto jobId = dependencyFrontier.front();
            dependencyFrontier.pop();
            if( !expandedDependencies.emplace( jobId ).second ) continue;
            const auto found = jobsById.find( jobId );
            if( found == jobsById.end() ) continue;
            for( const auto& dependency : found->second->dependencies ) if( dependency.prerequisiteJobId != 0 )
            {
                selectedJobIds.emplace( dependency.prerequisiteJobId );
                const auto prerequisite = jobsById.find( dependency.prerequisiteJobId );
                if( prerequisite == jobsById.end() ) continue;
                const auto completed = prerequisite->second->completedNs;
                if( !completed || *completed >= frameBegin ) dependencyFrontier.push( dependency.prerequisiteJobId );
            }
        }

        std::unordered_map<uint64_t, size_t> jobNodes;
        std::unordered_map<uint64_t, size_t> jobReadyNodes;
        std::unordered_map<uint64_t, size_t> jobCompleteNodes;
        std::unordered_map<uint64_t, std::vector<size_t>> jobStageNodes;
        for( const auto jobId : selectedJobIds )
        {
            checkCancelled();
            const auto found = jobsById.find( jobId );
            if( found == jobsById.end() ) continue;
            const auto& job = *found->second;
            const auto jobNode = graph.AddNode( "job:" + std::to_string( job.jobId ), job.ref, "job", "job",
                job.name, job.scheduleNs, job.scheduleNs, job.scheduleThreadRef, {}, std::nullopt,
                job.scheduleCallstack == 0 ? std::nullopt : std::optional<std::string>( source->MakeEntityRef( "callstack", job.scheduleCallstack ) ),
                !job.incomplete && !job.truncated, true,
                { { "job_id", Decimal( job.jobId ) }, { "origin_frame_id", Decimal( job.originFrameId ) },
                    { "execution_ns", Decimal( job.executionNs ) }, { "wait_ns", Decimal( job.waitNs ) } } );
            jobNodes[job.jobId] = jobNode;
            if( job.originFrameId == frameId )
                graph.AddEdge( graph.root, jobNode, "schedules", "exact", "job_origin_frame_id_v1", 1.0, true,
                    { "JobDto.originFrameId", "JobDto.scheduleNs" } );

            if( job.readyNs )
            {
                const auto ready = graph.AddNode( "job-ready:" + std::to_string( job.jobId ), {}, "job_ready", "job",
                    job.name + " Ready", *job.readyNs, *job.readyNs, {}, {}, std::nullopt, std::nullopt,
                    !job.incomplete, true, { { "job_ref", job.ref }, { "lane", job.readyLane } } );
                jobReadyNodes[job.jobId] = ready;
                graph.AddEdge( jobNode, ready, "becomes_ready", "exact", "job_v2_ready_stage_v1", 1.0, true,
                    { "JobDto.readyNs" } );
            }
            if( job.completedNs )
            {
                const auto completed = graph.AddNode( "job-complete:" + std::to_string( job.jobId ), {}, "job_complete", "job",
                    job.name + " Complete", *job.completedNs, *job.completedNs, {}, {}, std::nullopt, std::nullopt,
                    !job.incomplete, true, { { "job_ref", job.ref } } );
                jobCompleteNodes[job.jobId] = completed;
                graph.AddEdge( jobNode, completed, "completes", "exact", "job_completed_stage_v1", 1.0, false,
                    { "JobDto.completedNs" } );
            }

            struct OpenSpan { analysis::JobStageDto stage; std::string family; std::string label; };
            std::map<std::string, OpenSpan> openSpans;
            auto stages = job.stages;
            std::sort( stages.begin(), stages.end(), []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );
            const auto beginFamily = []( uint8_t stage ) -> std::pair<const char*, const char*> {
                if( stage == uint8_t( JnJobStage::WorkerSliceBegin ) ) return { "slice", "Worker Slice" };
                if( stage == uint8_t( JnJobStage::WaitBegin ) ) return { "wait", "Wait" };
                if( stage == uint8_t( JnJobStage::WaitActiveHelpBegin ) ) return { "active-help", "Wait ActiveHelp" };
                if( stage == uint8_t( JnJobStage::WaitSpinYieldBegin ) ) return { "spin-yield", "Wait Spin/Yield" };
                if( stage == uint8_t( JnJobStage::WaitSleepBegin ) ) return { "sleep", "Wait Sleep" };
                return { nullptr, nullptr };
            };
            const auto endFamily = []( uint8_t stage ) -> const char* {
                if( stage == uint8_t( JnJobStage::WorkerSliceEnd ) ) return "slice";
                if( stage == uint8_t( JnJobStage::WaitEnd ) ) return "wait";
                if( stage == uint8_t( JnJobStage::WaitActiveHelpEnd ) ) return "active-help";
                if( stage == uint8_t( JnJobStage::WaitSpinYieldEnd ) ) return "spin-yield";
                if( stage == uint8_t( JnJobStage::WaitSleepEnd ) ) return "sleep";
                return nullptr;
            };
            for( const auto& stage : stages )
            {
                const auto begin = beginFamily( stage.stage );
                if( begin.first )
                {
                    const auto key = std::string( begin.first ) + '|' + std::to_string( stage.spanId ) + '|' + stage.threadRef;
                    openSpans[key] = { stage, begin.first, begin.second };
                    continue;
                }
                const auto family = endFamily( stage.stage );
                if( !family ) continue;
                const auto key = std::string( family ) + '|' + std::to_string( stage.spanId ) + '|' + stage.threadRef;
                const auto open = openSpans.find( key );
                if( open == openSpans.end() || stage.timeNs < open->second.stage.timeNs ) continue;
                const auto stageIndex = jobStageNodes[job.jobId].size();
                const auto kind = open->second.family == "slice" ? "job_slice" : "job_wait";
                const auto node = graph.AddNode( "job-stage:" + std::to_string( job.jobId ) + ':' +
                    std::to_string( stageIndex ), {}, kind, kind == "job_slice" ? "job" : "wait",
                    job.name + " " + open->second.label, open->second.stage.timeNs, stage.timeNs,
                    open->second.stage.threadRef, {}, std::nullopt, std::nullopt, true, true,
                    { { "job_ref", job.ref }, { "span_id", stage.spanId }, { "arg0", open->second.stage.arg0 },
                        { "arg1", open->second.stage.arg1 }, { "flags", open->second.stage.flags } } );
                if( node != EvidenceGraphBuilder::InvalidIndex )
                {
                    jobStageNodes[job.jobId].emplace_back( node );
                    const auto origin = jobReadyNodes.contains( job.jobId ) ? jobReadyNodes[job.jobId] : jobNode;
                    graph.AddEdge( origin, node, "executes_stage", "exact", "job_span_id_pair_v1", 1.0, true,
                        { "JobStageDto.spanId", "JobStageDto.stage", "JobStageDto.timeNs" } );
                    if( jobCompleteNodes.contains( job.jobId ) )
                        graph.AddEdge( node, jobCompleteNodes[job.jobId], "stage_precedes_completion", "exact",
                            "job_completed_stage_v1", 1.0, true, { "JobDto.completedNs" } );
                }
                openSpans.erase( open );
            }
        }

        for( const auto jobId : selectedJobIds )
        {
            const auto found = jobsById.find( jobId );
            if( found == jobsById.end() || !jobNodes.contains( jobId ) ) continue;
            for( const auto& dependency : found->second->dependencies )
            {
                if( !jobNodes.contains( dependency.prerequisiteJobId ) ) continue;
                const auto sourceNode = jobCompleteNodes.contains( dependency.prerequisiteJobId ) ?
                    jobCompleteNodes[dependency.prerequisiteJobId] : jobNodes[dependency.prerequisiteJobId];
                const auto targetNode = jobReadyNodes.contains( jobId ) ? jobReadyNodes[jobId] : jobNodes[jobId];
                graph.AddEdge( sourceNode, targetNode, "dependency_precedes", "exact", "job_dependency_id_v1", 1.0, true,
                    { "JobDependencyDto.prerequisiteJobId" } );
            }
        }

        uint64_t incompleteScriptZones = 0;
        uint64_t orphanScriptZoneEnds = 0;
        if( graph.DomainAllowed( "script" ) )
        {
            checkCancelled();
            const auto scriptFrames = source->GetScriptFrames();
            const auto scriptEvents = source->GetScriptStackEvents();
            std::unordered_map<uint32_t, const analysis::ScriptFrameDto*> framesById;
            framesById.reserve( scriptFrames.size() );
            for( const auto& scriptFrame : scriptFrames )
                if( scriptFrame.frameId != 0 && ScriptRuntimeValid( scriptFrame.runtime ) )
                    framesById.emplace( scriptFrame.frameId, &scriptFrame );

            struct ScriptStackEvidence
            {
                const analysis::ScriptStackEventDto* header = nullptr;
                std::vector<uint32_t> frameIds;
            };
            std::unordered_map<uint64_t, ScriptStackEvidence> stacksById;
            std::unordered_map<uint32_t, const analysis::ScriptStackEventDto*> markersById;
            for( const auto& event : scriptEvents )
            {
                if( event.kind == uint8_t( JnScriptRecordKind::StackHeader ) && event.primaryId != 0 &&
                    event.value != 0 && event.value <= 64 && ScriptRuntimeValid( event.runtime ) )
                {
                    auto& stack = stacksById[event.primaryId];
                    if( stack.header == nullptr )
                    {
                        stack.header = &event;
                        stack.frameIds.resize( event.value );
                    }
                }
                else if( event.kind == uint8_t( JnScriptRecordKind::Marker ) && event.primaryId != 0 &&
                    event.value != 0 && ScriptRuntimeValid( event.runtime ) )
                {
                    markersById.try_emplace( uint32_t( event.primaryId ), &event );
                }
            }
            for( const auto& event : scriptEvents )
            {
                if( event.kind != uint8_t( JnScriptRecordKind::StackFrame ) || event.primaryId == 0 ||
                    event.secondaryId == 0 || event.secondaryId > std::numeric_limits<uint32_t>::max() ) continue;
                const auto stack = stacksById.find( event.primaryId );
                if( stack == stacksById.end() || event.value >= stack->second.frameIds.size() ) continue;
                if( stack->second.frameIds[event.value] == 0 )
                    stack->second.frameIds[event.value] = uint32_t( event.secondaryId );
            }

            std::vector<const analysis::ScriptStackEventDto*> timeline;
            timeline.reserve( scriptEvents.size() );
            for( const auto& event : scriptEvents )
                if( event.kind == uint8_t( JnScriptRecordKind::ZoneBegin ) ||
                    event.kind == uint8_t( JnScriptRecordKind::ZoneEnd ) ) timeline.emplace_back( &event );
            std::sort( timeline.begin(), timeline.end(), []( const auto* lhs, const auto* rhs ) {
                if( lhs->timeNs != rhs->timeNs ) return lhs->timeNs < rhs->timeNs;
                return lhs->kind < rhs->kind;
            } );

            std::unordered_map<uint64_t, const analysis::ScriptStackEventDto*> openZones;
            const auto emitScriptZone = [&]( const analysis::ScriptStackEventDto& begin, int64_t endNs, bool zoneComplete ) {
                if( endNs < frameBegin || begin.timeNs > frameEnd ) return;
                const auto marker = markersById.find( begin.value );
                const analysis::ScriptStackEventDto* markerEvent = marker == markersById.end() ? nullptr : marker->second;
                const auto sourceFrameId = markerEvent == nullptr ? 0u : markerEvent->value;
                const auto sourceFrame = framesById.find( sourceFrameId );
                const analysis::ScriptFrameDto* source = sourceFrame == framesById.end() ? nullptr : sourceFrame->second;
                const auto runtime = ScriptRuntimeName( begin.runtime );
                const auto zoneName = markerEvent != nullptr && !markerEvent->text.empty() ? markerEvent->text :
                    std::string( runtime ) + " Script Zone";
                json details = {
                    { "zone_id", Decimal( begin.primaryId ) }, { "stack_id", Decimal( begin.secondaryId ) },
                    { "marker_id", begin.value }, { "runtime", runtime }, { "flags", begin.flags },
                    { "source_mode", "binary-script-schema-2" }
                };
                if( source != nullptr )
                {
                    details["source_function"] = source->function;
                    details["source_file"] = source->file;
                    details["source_line"] = source->line;
                }
                const auto zone = graph.AddNode( "script-zone:" + std::to_string( begin.primaryId ), begin.ref,
                    begin.runtime == 2 ? "lua_zone" : "managed_zone", "script", zoneName,
                    begin.timeNs, std::max( begin.timeNs, endNs ), begin.threadRef, {},
                    source == nullptr ? std::nullopt : std::optional<std::string>( source->ref ), std::nullopt,
                    zoneComplete, true, std::move( details ) );
                graph.AddEdge( graph.root, zone, "overlaps_frame", "derived", "script_zone_frame_overlap_v1", 0.85,
                    true, { "ScriptStackEventDto.timeNs", "FrameIdentity.begin/end" }, zoneComplete );

                const auto stack = stacksById.find( begin.secondaryId );
                if( stack == stacksById.end() || stack->second.header == nullptr ) return;
                const bool stackComplete = std::all_of( stack->second.frameIds.begin(), stack->second.frameIds.end(),
                    [&]( uint32_t frameId ) { return frameId != 0 && framesById.contains( frameId ); } );
                json frameIds = json::array();
                for( const auto frameId : stack->second.frameIds ) frameIds.emplace_back( frameId );
                const auto stackNode = graph.AddNode( "script-stack:" + std::to_string( begin.secondaryId ),
                    stack->second.header->ref, "source_stack", "script", zoneName + " Source Stack",
                    begin.timeNs, begin.timeNs, begin.threadRef, {}, std::nullopt, std::nullopt,
                    stackComplete, false, { { "stack_id", Decimal( begin.secondaryId ) }, { "runtime", runtime },
                        { "frame_ids", std::move( frameIds ) }, { "source_mode", "binary-script-schema-2" } } );
                graph.AddEdge( zone, stackNode, "captures_source_stack", "exact", "script_zone_stack_id_v1", 1.0,
                    false, { "ScriptStackEventDto.secondaryId" }, stackComplete );
                for( size_t depth = 0; depth < stack->second.frameIds.size(); depth++ )
                {
                    const auto frameId = stack->second.frameIds[depth];
                    const auto found = framesById.find( frameId );
                    if( found == framesById.end() ) continue;
                    const auto& frame = *found->second;
                    const auto frameNode = graph.AddNode( "script-source-frame:" + std::to_string( frameId ), frame.ref,
                        "source_frame", "script", frame.function, begin.timeNs, begin.timeNs, begin.threadRef, {},
                        frame.ref, std::nullopt, true, false,
                        { { "frame_id", frameId }, { "depth", depth }, { "runtime", ScriptRuntimeName( frame.runtime ) },
                            { "function", frame.function }, { "file", frame.file }, { "line", frame.line },
                            { "flags", frame.flags }, { "registered_at_ns", Decimal( frame.timeNs ) } } );
                    graph.AddEdge( stackNode, frameNode, "contains_source_frame", "exact", "script_stack_frame_index_v1",
                        1.0, false, { "ScriptStackEventDto.value", "ScriptStackEventDto.secondaryId" } );
                }
            };

            for( const auto* event : timeline )
            {
                if( event->kind == uint8_t( JnScriptRecordKind::ZoneBegin ) )
                {
                    if( event->primaryId != 0 && event->secondaryId != 0 && event->value != 0 )
                        openZones.try_emplace( event->primaryId, event );
                    continue;
                }
                const auto open = openZones.find( event->primaryId );
                if( open == openZones.end() )
                {
                    orphanScriptZoneEnds++;
                    continue;
                }
                if( event->timeNs >= open->second->timeNs ) emitScriptZone( *open->second, event->timeNs, true );
                openZones.erase( open );
            }
            for( const auto& [zoneId, begin] : openZones ) if( begin->timeNs <= frameEnd )
            {
                incompleteScriptZones++;
                emitScriptZone( *begin, frameEnd, false );
            }
        }

        const auto ioRequests = source->GetIoRequests();
        std::unordered_map<uint64_t, size_t> ioNodes;
        const auto frameSequence = uint32_t( frameId );
        for( const auto& request : ioRequests )
        {
            const auto requestBegin = request.orphan && request.startNs ? *request.startNs : request.queueNs;
            const auto requestEnd = request.endNs.value_or( frameEnd );
            const bool sequenceMatch = request.originFrameSequence != 0 && request.originFrameSequence == frameSequence;
            const bool overlaps = requestEnd >= frameBegin && requestBegin <= frameEnd;
            if( !sequenceMatch && !overlaps ) continue;
            const auto node = graph.AddNode( "io:" + std::to_string( request.requestId ), request.ref, "io_request", "io",
                IoOperationName( request.operation ), requestBegin, requestEnd, request.queueThreadRef, {}, std::nullopt,
                request.requestCallstack == 0 ? std::nullopt : std::optional<std::string>( source->MakeEntityRef( "callstack", request.requestCallstack ) ),
                request.endNs.has_value() && !request.truncated, true,
                { { "request_id", Decimal( request.requestId ) }, { "resource_id", Decimal( request.resourceId ) },
                    { "requested_bytes", Decimal( request.requestedBytes ) }, { "transferred_bytes", Decimal( request.transferredBytes ) },
                    { "status", IoStatusName( request.status ) } } );
            ioNodes[request.requestId] = node;
            graph.AddEdge( graph.root, node, sequenceMatch ? "originates_in_frame" : "overlaps_frame", "derived",
                sequenceMatch ? "origin_frame_sequence_v1" : "frame_window_overlap_v1", sequenceMatch ? 0.95 : 0.75, true,
                sequenceMatch ? json { "IoRequestDto.originFrameSequence", "FrameIdentity.sequence" } :
                    json { "IoRequestDto.queueNs", "IoRequestDto.endNs", "FrameIdentity.begin/end" } );
        }
        for( const auto& request : ioRequests ) if( ioNodes.contains( request.requestId ) )
        {
            const auto child = ioNodes[request.requestId];
            if( request.parentKind == uint8_t( JnIoParentKind::IoRequest ) && ioNodes.contains( request.parentId ) )
                graph.AddEdge( ioNodes[request.parentId], child, "io_parent", "exact", "io_parent_request_id_v1", 1.0, true,
                    { "IoRequestDto.parentKind", "IoRequestDto.parentId" } );
            else if( request.parentKind == uint8_t( JnIoParentKind::Job ) && jobNodes.contains( request.parentId ) )
                graph.AddEdge( jobNodes[request.parentId], child, "job_starts_io", "exact", "io_parent_job_id_v1", 1.0, true,
                    { "IoRequestDto.parentKind", "IoRequestDto.parentId" } );
            if( request.resourceId != 0 )
            {
                const auto resource = graph.AddNode( "resource:" + std::to_string( request.resourceId ),
                    source->MakeEntityRef( "io-resource", request.resourceId ), "resource", "resource",
                    "I/O Resource", request.queueNs, request.queueNs, {}, {}, std::nullopt, std::nullopt, true, false,
                    { { "resource_id", Decimal( request.resourceId ) }, { "identity_kind", "stable_resource_or_path_hash" } } );
                graph.AddEdge( child, resource, "accesses_resource", "exact", "io_resource_id_v1", 1.0, false,
                    { "IoRequestDto.resourceId" } );
            }
        }

        std::vector<uint64_t> gfxSeeds;
        gfxSeeds.reserve( selectedJobIds.size() );
        for( const auto jobId : selectedJobIds ) gfxSeeds.emplace_back( jobId );
        const auto gfxEvidence = source->GetEvidenceGfx( frameId, gfxSeeds );
        const auto& dispatches = gfxEvidence.dispatches;
        const auto& entities = gfxEvidence.entities;
        const auto& gfxLinks = gfxEvidence.links;
        std::set<uint64_t> reachableIds;
        std::unordered_map<uint64_t, size_t> rawNodes;
        for( const auto jobId : selectedJobIds ) if( jobNodes.contains( jobId ) )
        {
            reachableIds.emplace( jobId );
            rawNodes.emplace( jobId, jobNodes[jobId] );
        }
        for( const auto& dispatch : dispatches ) if( dispatch.frameIndex == frameId )
        {
            const auto node = graph.AddNode( "gfx-dispatch:" + std::to_string( dispatch.dispatchId ), dispatch.ref,
                "gfx_dispatch", "submission", "Gfx Dispatch", dispatch.timeNs, dispatch.timeNs, dispatch.threadRef, {},
                std::nullopt, std::nullopt, true, true,
                { { "dispatch_id", Decimal( dispatch.dispatchId ) }, { "expected_jobs", dispatch.expectedJobs },
                    { "threading_mode", dispatch.threadingMode } } );
            reachableIds.emplace( dispatch.dispatchId );
            rawNodes[dispatch.dispatchId] = node;
            graph.AddEdge( graph.root, node, "dispatches", "exact", "gfx_dispatch_frame_id_v1", 1.0, true,
                { "GfxDispatchDto.frameIndex" } );
        }

        auto taxonomy = GpuTaxonomyCatalogJson( info() );
        auto explicitPasses = BuildExplicitGpuPassSet( *source, taxonomy, &entities, &gfxLinks );
        std::vector<analysis::GpuZoneDto> frameGpuZones;
        {
            checkCancelled();
            const auto requested = std::max<size_t>( 1, maxNodes );
            const auto allowed = BudgetScanAllowance( requested );
            analysis::ScanRange range;
            range.startNs = frameBegin;
            range.endNs = frameEnd == frameBegin ? frameBegin + 1 : frameEnd;
            range.offset = 0;
            range.limit = allowed;
            const auto values = source->ScanGpuZones( range );
            BudgetScanned( values.size(), requested, allowed );
            for( const auto& zone : values )
            {
                MatchExplicitGpuPassZone( *source, explicitPasses, zone );
                frameGpuZones.emplace_back( zone );
            }
        }

        std::set<std::string> explicitGpuZoneRefs;
        std::unordered_map<uint64_t, size_t> explicitPassNodes;
        for( const auto& match : explicitPasses.matches ) if( match.frameId == frameId )
        {
            const auto start = match.zone ? match.zone->gpuStartNs : match.pass.timeNs;
            const auto end = match.zone && match.zone->gpuEndNs ? *match.zone->gpuEndNs : start;
            const auto passNode = graph.AddNode( "gpu-pass:" + std::to_string( match.pass.entityId ), match.pass.ref,
                "gpu_pass", "gpu", match.zone ? match.zone->name : "Explicit GPU Pass", start, end,
                match.pass.threadRef, match.zone ? match.zone->contextRef : std::string(),
                match.zone ? std::optional<std::string>( match.zone->sourceLocationRef ) : std::nullopt,
                match.zone ? match.zone->callstackRef : std::nullopt, match.zone && match.zone->complete, true,
                { { "pass_instance_id", Decimal( match.pass.entityId ) }, { "pass_source_id", match.pass.gpuQueryId },
                    { "taxonomy_id", Decimal( uint64_t( match.taxonomyId ) ) }, { "command_list_id", Decimal( match.commandListId ) },
                    { "reference_token", Decimal( match.referenceToken ) } } );
            if( passNode == EvidenceGraphBuilder::InvalidIndex ) continue;
            explicitPassNodes[match.pass.entityId] = passNode;
            rawNodes[match.pass.entityId] = passNode;
            reachableIds.emplace( match.pass.entityId );
            graph.AddEdge( graph.root, passNode, "executes_gpu_pass", "exact", "explicit_gpu_pass_frame_id_v1", 1.0, true,
                { "GfxLink.frameId", "GfxEntity.passInstanceId" } );
            if( match.zone )
            {
                explicitGpuZoneRefs.emplace( match.zone->ref );
                const auto zoneNode = graph.AddNode( "gpu-zone:" + match.zone->ref, match.zone->ref, "gpu_zone", "gpu",
                    match.zone->name, match.zone->gpuStartNs, match.zone->gpuEndNs.value_or( match.zone->gpuStartNs ),
                    match.zone->threadRef, match.zone->contextRef, match.zone->sourceLocationRef, match.zone->callstackRef,
                    match.zone->complete, false, { { "query_id", match.zone->queryId }, { "pairing", "context_query_id_exact" } } );
                graph.AddEdge( passNode, zoneNode, "binds_gpu_zone", "exact", "gpu_context_query_id_v1", 1.0, false,
                    { "GfxEntity.gpuContext", "GfxEntity.gpuQueryId", "GpuZoneDto.contextRef", "GpuZoneDto.queryId" } );
            }
        }

        bool reachabilityChanged = true;
        while( reachabilityChanged )
        {
            reachabilityChanged = false;
            for( const auto& entity : entities ) if( entity.parentId != 0 && reachableIds.contains( entity.parentId ) )
                reachabilityChanged |= reachableIds.emplace( entity.entityId ).second;
            for( const auto& link : gfxLinks ) if( reachableIds.contains( link.sourceId ) )
                reachabilityChanged |= reachableIds.emplace( link.targetId ).second;
        }
        for( const auto& entity : entities ) if( reachableIds.contains( entity.entityId ) && !rawNodes.contains( entity.entityId ) )
        {
            const auto domain = entity.kind == 3 || entity.kind == 4 ? "submission" : "gfx";
            const auto kind = entity.kind == 3 ? "submission" : "gfx_entity";
            const auto node = graph.AddNode( "gfx-entity:" + std::to_string( entity.entityId ), entity.ref, kind, domain,
                GfxEntityKindName( entity.kind ), entity.timeNs, entity.timeNs, entity.threadRef, {}, std::nullopt, std::nullopt,
                true, true, { { "entity_id", Decimal( entity.entityId ) }, { "entity_kind", entity.kind },
                    { "gpu_query_id", entity.gpuQueryId }, { "gpu_context", entity.gpuContext } } );
            rawNodes[entity.entityId] = node;
        }
        for( const auto& entity : entities ) if( rawNodes.contains( entity.entityId ) && rawNodes.contains( entity.parentId ) )
            graph.AddEdge( rawNodes[entity.parentId], rawNodes[entity.entityId], "parent", "exact", "gfx_entity_parent_id_v1", 1.0, true,
                { "GfxEntityDto.parentId" } );
        for( const auto& link : gfxLinks ) if( rawNodes.contains( link.sourceId ) && rawNodes.contains( link.targetId ) )
            graph.AddEdge( rawNodes[link.sourceId], rawNodes[link.targetId], GfxRelationName( link.relation ), "exact",
                "gfx_link_id_v1", 1.0, true, { "GfxLinkDto.sourceId", "GfxLinkDto.targetId", "GfxLinkDto.relation" } );

        std::vector<uint64_t> frameReferenceTokens;
        for( const auto& match : explicitPasses.matches )
            if( match.frameId == frameId && match.referenceToken != 0 ) frameReferenceTokens.emplace_back( match.referenceToken );
        std::sort( frameReferenceTokens.begin(), frameReferenceTokens.end() );
        frameReferenceTokens.erase( std::unique( frameReferenceTokens.begin(), frameReferenceTokens.end() ), frameReferenceTokens.end() );
        const auto gpuEvidence = source->GetGpuMemoryEvidence( frameReferenceTokens, maxNodes );
        std::unordered_map<uint64_t, const analysis::GpuMemoryPass*> evidencePasses;
        for( const auto& pass : gpuEvidence.passes ) evidencePasses.emplace( pass.passId, &pass );
        std::unordered_map<uint64_t, const analysis::GpuMemoryEvidenceResource*> evidenceResources;
        for( const auto& resource : gpuEvidence.resources ) evidenceResources.emplace( resource.resourceId, &resource );
        for( const auto& match : explicitPasses.matches ) if( match.frameId == frameId && match.referenceToken != 0 && explicitPassNodes.contains( match.pass.entityId ) )
        {
            const auto pass = evidencePasses.find( match.referenceToken );
            if( pass == evidencePasses.end() ) continue;
            for( const auto& use : pass->second->uses )
            {
                const auto metadata = evidenceResources.find( use.allocationId );
                const auto bytes = metadata == evidenceResources.end() ? 0 : metadata->second->size;
                const auto name = metadata == evidenceResources.end() || metadata->second->name.empty() ? "GPU Logical Resource" : metadata->second->name;
                const auto resource = graph.AddNode( "gpu-resource:" + std::to_string( use.allocationId ),
                    source->MakeEntityRef( "gpu-allocation", use.allocationId ), "gpu_logical_resource", "resource", name,
                    graph.nodes[explicitPassNodes[match.pass.entityId]].startNs,
                    graph.nodes[explicitPassNodes[match.pass.entityId]].startNs, {}, {}, std::nullopt, std::nullopt,
                    true, false, { { "allocation_id", Decimal( use.allocationId ) }, { "bytes", Decimal( bytes ) },
                        { "usage_mask", use.usageMask }, { "usage_kind", std::string( 1, use.kind ) },
                        { "resource_set_id", use.resourceSetId == 0 ? json( nullptr ) : json( Decimal( uint64_t( use.resourceSetId ) ) ) },
                        { "encoding", use.encoding == 2 ? "ResourceSetV2" : "PerUseV1" } } );
                graph.AddEdge( explicitPassNodes[match.pass.entityId], resource, "references_resource", "exact",
                    "gpu_reference_token_allocation_id_v1", 1.0, false,
                    { "ExplicitGpuPass.referenceToken", "GpuMemoryPassUse.allocationId" } );
                if( metadata != evidenceResources.end() )
                {
                    if( metadata->second->physicalAllocationId != 0 )
                    {
                        const auto physical = graph.AddNode( "gpu-physical:" + std::to_string( metadata->second->physicalAllocationId ),
                            source->MakeEntityRef( "gpu-physical-allocation", metadata->second->physicalAllocationId ),
                            "gpu_physical_allocation", "resource", "GPU Physical Allocation",
                            graph.nodes[explicitPassNodes[match.pass.entityId]].startNs,
                            graph.nodes[explicitPassNodes[match.pass.entityId]].startNs, {}, {}, std::nullopt, std::nullopt,
                            true, false, { { "physical_allocation_id", Decimal( metadata->second->physicalAllocationId ) } } );
                        graph.AddEdge( resource, physical, "backed_by", "exact", "gpu_logical_physical_id_v1", 1.0, false,
                            { "GpuMemoryLogicalResource.physicalAllocationId" } );
                    }
                    const auto owner = graph.AddNode( "gpu-owner:" + std::to_string( metadata->second->primaryOwnerId ),
                        source->MakeEntityRef( "gpu-owner", metadata->second->primaryOwnerId ), "gpu_primary_owner", "resource",
                        "GPU Primary Owner", graph.nodes[explicitPassNodes[match.pass.entityId]].startNs,
                        graph.nodes[explicitPassNodes[match.pass.entityId]].startNs, {}, {}, std::nullopt, std::nullopt,
                        true, false, { { "taxonomy_id", metadata->second->primaryOwnerId } } );
                    graph.AddEdge( resource, owner, "owned_by", "exact", "gpu_primary_owner_id_v1", 1.0, false,
                        { "GpuMemoryLogicalResource.primaryOwnerId" } );
                }
            }
        }

        std::vector<std::pair<size_t, std::optional<std::string>>> cpuParents;
        {
            checkCancelled();
            const auto requested = std::max<size_t>( 1, maxNodes );
            const auto allowed = BudgetScanAllowance( requested );
            analysis::ScanRange range;
            range.startNs = frameBegin;
            range.endNs = frameEnd == frameBegin ? frameBegin + 1 : frameEnd;
            range.offset = 0;
            range.limit = allowed;
            const auto zones = source->ScanCpuZones( range );
            BudgetScanned( zones.size(), requested, allowed );
            for( const auto& zone : zones ) if( zone.endNs )
            {
                const auto node = graph.AddNode( "cpu-zone:" + zone.ref, zone.ref, "cpu_zone", "cpu", zone.name,
                    zone.startNs, *zone.endNs, zone.threadRef, {}, zone.sourceLocationRef, zone.callstackRef,
                    zone.complete, true, { { "self_time_ns", zone.selfTimeNs ? json( Decimal( *zone.selfTimeNs ) ) : json( nullptr ) },
                        { "running_time_ns", zone.runningTimeNs ? json( Decimal( *zone.runningTimeNs ) ) : json( nullptr ) } } );
                cpuParents.emplace_back( node, zone.parentRef );
                graph.AddEdge( graph.root, node, "overlaps_frame", "derived", "frame_window_overlap_v1", 0.75, true,
                    { "CpuZoneDto.startNs/endNs", "FrameIdentity.begin/end" } );
            }
        }
        for( const auto& [node, parentRef] : cpuParents ) if( parentRef )
        {
            const auto parent = graph.FindSource( *parentRef );
            graph.AddEdge( parent, node, "zone_parent", "exact", "cpu_zone_parent_ref_v1", 1.0, false,
                { "CpuZoneDto.parentRef" } );
        }

        for( const auto& zone : frameGpuZones ) if( zone.gpuEndNs && !explicitGpuZoneRefs.contains( zone.ref ) )
        {
            const auto node = graph.AddNode( "gpu-zone:" + zone.ref, zone.ref, "gpu_zone", "gpu", zone.name,
                zone.gpuStartNs, *zone.gpuEndNs, zone.threadRef, zone.contextRef, zone.sourceLocationRef, zone.callstackRef,
                zone.complete, true, { { "query_id", zone.queryId }, { "pairing", "frame_window_only" } } );
            graph.AddEdge( graph.root, node, "overlaps_frame", "derived", "gpu_frame_window_overlap_v1", 0.70, true,
                { "GpuZoneDto.gpuStartNs/gpuEndNs", "FrameIdentity.begin/end" } );
            if( zone.parentRef ) graph.AddEdge( graph.FindSource( *zone.parentRef ), node, "zone_parent", "exact",
                "gpu_zone_parent_ref_v1", 1.0, false, { "GpuZoneDto.parentRef" } );
        }

        std::map<std::string, analysis::LockEventDto> lockWaits;
        {
            checkCancelled();
            const auto requested = std::max<size_t>( 1, maxNodes * 2 );
            const auto allowed = BudgetScanAllowance( requested );
            analysis::ScanRange range;
            range.startNs = frameBegin;
            range.endNs = frameEnd == frameBegin ? frameBegin + 1 : frameEnd;
            range.offset = 0;
            range.limit = allowed;
            const auto events = source->ScanLockEvents( range );
            BudgetScanned( events.size(), requested, allowed );
            for( const auto& event : events )
            {
                const auto key = event.lockRef + '|' + event.threadRef;
                if( event.type == "wait" || event.type == "wait_shared" ) lockWaits[key] = event;
                else if( ( event.type == "obtain" || event.type == "obtain_shared" ) && lockWaits.contains( key ) )
                {
                    const auto& wait = lockWaits[key];
                    const auto node = graph.AddNode( "lock-wait:" + wait.ref, wait.ref, "lock_wait", "lock",
                        "Lock Wait", wait.timeNs, event.timeNs, wait.threadRef, {}, wait.sourceLocationRef, std::nullopt,
                        true, true, { { "lock_ref", wait.lockRef }, { "obtain_event_ref", event.ref },
                            { "owner_thread_ref", wait.ownerThreadRef ? json( *wait.ownerThreadRef ) : json( nullptr ) } } );
                    graph.AddEdge( graph.root, node, "overlaps_frame", "derived", "lock_wait_obtain_pair_v1", 0.90, true,
                        { "LockEvent.wait.timeNs", "LockEvent.obtain.timeNs", "LockEvent.threadRef" } );
                    lockWaits.erase( key );
                }
            }
        }

        {
            checkCancelled();
            const auto requested = std::max<size_t>( 1, maxNodes );
            const auto allowed = BudgetScanAllowance( requested );
            analysis::ScanRange range;
            range.startNs = frameBegin;
            range.endNs = frameEnd == frameBegin ? frameBegin + 1 : frameEnd;
            range.offset = 0;
            range.limit = allowed;
            const auto events = source->ScanContextSwitchEvents( range );
            BudgetScanned( events.size(), requested, allowed );
            for( const auto& event : events )
            {
                if( event.endNs )
                {
                    const auto run = graph.AddNode( "context-run:" + event.ref, event.ref, "context_switch_run",
                        "context_switch", "Thread Running", event.startNs, *event.endNs, event.threadRef, {},
                        std::nullopt, std::nullopt, event.complete, true,
                        { { "cpu", event.cpu }, { "reason", event.reasonName }, { "state", event.stateName } } );
                    graph.AddEdge( graph.root, run, "overlaps_frame", "derived", "context_switch_running_interval_v1", 0.85, true,
                        { "ContextSwitchDto.startNs/endNs", "ContextSwitchDto.threadRef" } );
                }
                if( event.wakeupNs && *event.wakeupNs <= event.startNs )
                {
                    const auto wait = graph.AddNode( "context-wait:" + event.ref, {}, "context_switch_wait",
                        "context_switch", "Runnable Wait", *event.wakeupNs, event.startNs, event.threadRef, {},
                        std::nullopt, std::nullopt, event.complete, true,
                        { { "run_event_ref", event.ref }, { "reason", event.reasonName }, { "state", event.stateName } } );
                    graph.AddEdge( graph.root, wait, "overlaps_frame", "derived", "context_switch_wakeup_to_run_v1", 0.85, true,
                        { "ContextSwitchDto.wakeupNs", "ContextSwitchDto.startNs" } );
                }
            }
        }

        std::map<std::string, std::vector<size_t>> timelineByThread;
        for( size_t index = 0; index < graph.nodes.size(); index++ )
            if( index != graph.root && graph.nodes[index].criticalEligible && !graph.nodes[index].threadRef.empty() )
                timelineByThread[graph.nodes[index].threadRef].emplace_back( index );
        for( auto& [thread, timeline] : timelineByThread )
        {
            std::sort( timeline.begin(), timeline.end(), [&]( size_t lhs, size_t rhs ) {
                const auto& left = graph.nodes[lhs];
                const auto& right = graph.nodes[rhs];
                return left.startNs != right.startNs ? left.startNs < right.startNs : left.endNs < right.endNs;
            } );
            std::optional<size_t> previous;
            for( const auto current : timeline )
            {
                if( previous && graph.nodes[*previous].endNs <= graph.nodes[current].startNs )
                    graph.AddEdge( *previous, current, "timeline_precedes", "derived", "same_thread_adjacency_v1", 0.70, true,
                        { "node.thread_ref", "node.start_ns", "node.end_ns" } );
                if( !previous || graph.nodes[current].endNs >= graph.nodes[*previous].endNs ) previous = current;
            }
        }

        auto criticalPath = EvidenceCriticalPathJson( graph );
        json evidenceCounts = {
            { "exact", Decimal( graph.evidenceCounts["exact"] ) },
            { "derived", Decimal( graph.evidenceCounts["derived"] ) },
            { "heuristic", Decimal( graph.evidenceCounts["heuristic"] ) }
        };
        json domainCoverage = json::array();
        static constexpr std::pair<const char*, const char*> RequiredDomains[] = {
            { "cpu", "zone.cpu" }, { "job", "job" }, { "wait", "job" }, { "lock", "lock" },
            { "script", "runtime.script" },
            { "context_switch", "context_switch" }, { "io", "io" }, { "submission", "job.gfx" },
            { "gpu", "job.gfx" }, { "resource", "memory.gpu" }
        };
        const auto capabilities = source->GetCapabilities();
        for( const auto& [domain, capabilityDomain] : RequiredDomains )
        {
            const auto count = graph.domainCounts[domain];
            const auto capability = std::find_if( capabilities.begin(), capabilities.end(), [&]( const auto& value ) {
                return value.domain == capabilityDomain;
            } );
            const bool available = capability != capabilities.end() && capability->present;
            const char* status = count != 0 ? "present" : available ? "not_observed_in_selected_frame" : "unavailable";
            const std::string reason = count != 0 ? "" : available ?
                "capability is available, but no matching evidence was observed in the selected frame and query budget" :
                capability != capabilities.end() ? capability->reason : "required capability was not declared by the trace source";
            domainCoverage.push_back( {
                { "domain", domain }, { "present", count != 0 }, { "node_count", Decimal( count ) },
                { "status", status }, { "capability_domain", capabilityDomain }, { "capability_available", available },
                { "reason", reason }
            } );
        }
        const bool contributionValid = criticalPath.value( "valid_contribution", false );
        const bool complete = frameComplete && incompleteScriptZones == 0 && !graph.truncated && !BudgetPartial() &&
            contributionValid && !criticalPath.value( "has_cycle", true );
        json qualityFindings = json::array();
        if( !frameComplete ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "INCOMPLETE_FRAME" }, { "message", "canonical frame begin/end is incomplete" } } );
        if( graph.truncated || BudgetPartial() ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "EVIDENCE_BUDGET_PARTIAL" }, { "message", "node, edge, scan, or CPU budget truncated the evidence graph" } } );
        if( criticalPath.value( "has_cycle", false ) ) qualityFindings.push_back( { { "severity", "error" }, { "code", "CRITICAL_PATH_CYCLE" }, { "message", "critical-eligible evidence contains a cycle" } } );
        if( !contributionValid ) qualityFindings.push_back( { { "severity", "error" }, { "code", "INVALID_WALL_CLOCK_CONTRIBUTION" }, { "message", "critical path contribution exceeds frame wall time" } } );
        if( !includeHeuristic && graph.evidenceCounts["heuristic"] != 0 ) qualityFindings.push_back( { { "severity", "error" }, { "code", "HEURISTIC_LEAK" }, { "message", "heuristic evidence was emitted while disabled" } } );
        if( incompleteScriptZones != 0 ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "INCOMPLETE_SCRIPT_ZONES" }, { "message", "one or more C#/Lua source-stack zones overlap the frame without a matching end" }, { "count", Decimal( incompleteScriptZones ) } } );
        if( orphanScriptZoneEnds != 0 ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "ORPHAN_SCRIPT_ZONE_ENDS" }, { "message", "the script source-stack producer contains unmatched end records" }, { "count", Decimal( orphanScriptZoneEnds ) } } );
        if( !gpuEvidence.complete ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "GPU_REFERENCE_SOURCE_INCOMPLETE" }, { "message", "GPU resource-reference producer reported incomplete source data" } } );
        if( gpuEvidence.truncated ) qualityFindings.push_back( { { "severity", "warning" }, { "code", "GPU_REFERENCE_EVIDENCE_TRUNCATED" }, { "message", "GPU resource evidence exceeded the bounded per-frame use limit" }, { "omitted_uses", Decimal( gpuEvidence.omittedUses ) } } );

        const auto base = [&]() {
            return json {
                { "present", true }, { "schema_version", 1 }, { "complete", complete },
                { "reason", complete ? "" : "inspect quality_findings and missing_evidence" },
                { "frame", { { "ref", frameRef }, { "frame_id", Decimal( frameId ) },
                    { "begin_ns", Decimal( frameBegin ) }, { "end_ns", Decimal( frameEnd ) },
                    { "duration_ns", Decimal( std::max<int64_t>( 0, frameEnd - frameBegin ) ) }, { "complete", frameComplete } } },
                { "evidence_counts", evidenceCounts }, { "domain_coverage", domainCoverage },
                { "heuristic_enabled", includeHeuristic }, { "heuristic_used_by_default_path", false },
                { "truncated", graph.truncated || BudgetPartial() },
                { "omitted_nodes", Decimal( graph.omittedNodes ) }, { "omitted_edges", Decimal( graph.omittedEdges ) },
                { "quality_findings", qualityFindings }, { "trust", "untrusted_trace_data" }
            };
        };

        if( method == "evidence.graph" )
        {
            auto result = base();
            json nodes = json::array();
            for( size_t index = 0; index < graph.nodes.size(); index++ ) nodes.emplace_back( graph.NodeJson( index ) );
            json edges = json::array();
            for( size_t index = 0; index < graph.edges.size(); index++ ) edges.emplace_back( graph.EdgeJson( index ) );
            result["nodes"] = std::move( nodes );
            result["edges"] = std::move( edges );
            result["node_count"] = Decimal( graph.nodes.size() );
            result["edge_count"] = Decimal( graph.edges.size() );
            result["critical_path_summary"] = criticalPath;
            return Success( id, std::move( result ), trace );
        }
        if( method == "frame.critical_path" )
        {
            auto result = base();
            result["critical_path"] = std::move( criticalPath );
            result["algorithm"] = "causal_dag_incremental_wall_clock_v1";
            result["heuristic_baseline_same"] = true;
            return Success( id, std::move( result ), trace );
        }

        auto result = base();
        std::vector<size_t> contributors;
        for( size_t index = 0; index < graph.nodes.size(); index++ ) if( index != graph.root && graph.nodes[index].endNs > graph.nodes[index].startNs ) contributors.emplace_back( index );
        std::sort( contributors.begin(), contributors.end(), [&]( size_t lhs, size_t rhs ) {
            const auto left = graph.nodes[lhs].endNs - graph.nodes[lhs].startNs;
            const auto right = graph.nodes[rhs].endNs - graph.nodes[rhs].startNs;
            return left != right ? left > right : graph.nodes[lhs].ref < graph.nodes[rhs].ref;
        } );
        json top = json::array();
        for( size_t index = 0; index < std::min<size_t>( contributors.size(), 20 ); index++ ) top.emplace_back( graph.NodeJson( contributors[index] ) );
        json missing = json::array();
        for( const auto& value : domainCoverage ) if( !value.value( "present", false ) ) missing.emplace_back( value );
        const auto exactCount = graph.evidenceCounts["exact"];
        const auto derivedCount = graph.evidenceCounts["derived"];
        const auto confidence = !complete ? "partial" : exactCount != 0 && derivedCount == 0 ? "high" : exactCount != 0 ? "medium" : "low";
        result["critical_path"] = std::move( criticalPath );
        result["top_contributors"] = std::move( top );
        result["missing_evidence"] = std::move( missing );
        result["analysis_confidence"] = confidence;
        result["conclusion_contract"] = "all conclusions must cite returned node/edge refs; absent domains are not real zero";
        return Success( id, std::move( result ), trace );
    }
    if( method == "frame.identity" || method == "entity.related" || method == "correlation.chain" || method == "timeline.correlated_slice" )
    {
        auto frameEvents = source->GetCorrelatedFrameEvents();
        if( frameEvents.empty() )
            return Success( id, { { "present", false }, { "reason", "trace predates or does not contain JN frame correlation" }, { "identities", json::array() } }, trace );

        std::map<uint64_t, std::vector<analysis::CorrelatedFrameEventDto>> frames;
        for( auto& event : frameEvents ) frames[event.frameId].push_back( std::move( event ) );
        for( auto& [frameId, events] : frames )
            std::sort( events.begin(), events.end(), []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );

        const auto frameJson = [&]( uint64_t frameId ) {
            const auto found = frames.find( frameId );
            if( found == frames.end() ) throw QueryError( "ENTITY_NOT_FOUND", "FrameIdentity ref was not found" );
            int64_t begin = std::numeric_limits<int64_t>::max();
            int64_t end = std::numeric_limits<int64_t>::min();
            bool canonicalBegin = false;
            bool canonicalEnd = false;
            std::set<std::string> domains;
            json events = json::array();
            for( const auto& event : found->second )
            {
                begin = std::min( begin, event.timeNs );
                end = std::max( end, event.timeNs );
                domains.emplace( CorrelatedFrameDomainName( event.domain ) );
                canonicalBegin |= event.phase == 0 && ( event.flags & 1 ) != 0;
                canonicalEnd |= event.phase == 1 && ( event.flags & 1 ) != 0;
                events.push_back( CorrelatedFrameEventJson( *source, event ) );
            }
            return json {
                { "ref", source->MakeEntityRef( "frame-identity", frameId ) }, { "frame_id", Decimal( frameId ) },
                { "connection_generation", uint16_t( frameId >> 48 ) }, { "sequence", uint32_t( frameId ) },
                { "begin_ns", Decimal( begin ) }, { "end_ns", Decimal( end ) },
                { "duration_ns", Decimal( std::max<int64_t>( 0, end - begin ) ) },
                { "complete", canonicalBegin && canonicalEnd }, { "domains", domains },
                { "events", std::move( events ) }, { "evidence_kind", "exact" }
            };
        };

        const auto parseFrame = [&]() -> uint64_t {
            if( params.contains( "ref" ) )
            {
                if( !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref must be a FrameIdentity ref" );
                const auto parsed = source->ParseEntityRef( params["ref"].get<std::string>(), "frame-identity" );
                if( !parsed ) throw QueryError( "INVALID_PARAMS", "ref must be a FrameIdentity ref from this trace" );
                return *parsed;
            }
            if( params.contains( "frame_id" ) ) return UnsignedParameter( params, "frame_id", 0, std::numeric_limits<uint64_t>::max() );
            throw QueryError( "INVALID_PARAMS", "ref or frame_id is required" );
        };

        if( method == "frame.identity" )
        {
            if( params.contains( "ref" ) || params.contains( "frame_id" ) )
                return Success( id, { { "present", true }, { "identity", frameJson( parseFrame() ) } }, trace );
            const auto page = ParsePage( params, method, trace );
            json values = json::array();
            auto it = frames.begin();
            std::advance( it, std::min( page.offset, frames.size() ) );
            size_t count = 0;
            while( it != frames.end() && count < page.limit ) { values.push_back( frameJson( it->first ) ); ++it; ++count; }
            const bool hasMore = it != frames.end();
            const auto cursor = NextCursor( page, method, trace, count, hasMore );
            return Success( id, { { "present", true }, { "identities", std::move( values ) } }, trace, PageJson( page, count, cursor ) );
        }

        const auto jobs = source->GetJobs();
        const auto dispatches = source->GetGfxDispatches();
        const auto entities = source->GetGfxEntities();
        const auto gfxLinks = source->GetGfxLinks();
        std::unordered_map<uint64_t, std::string> jobRefs;
        std::unordered_map<uint64_t, std::string> dispatchRefs;
        std::unordered_map<uint64_t, std::string> entityRefs;
        for( const auto& value : jobs ) jobRefs[value.jobId] = value.ref;
        for( const auto& value : dispatches ) dispatchRefs[value.dispatchId] = value.ref;
        for( const auto& value : entities ) entityRefs[value.entityId] = value.ref;
        const auto refForId = [&]( uint64_t value ) -> std::string {
            if( const auto it = jobRefs.find( value ); it != jobRefs.end() ) return it->second;
            if( const auto it = dispatchRefs.find( value ); it != dispatchRefs.end() ) return it->second;
            if( const auto it = entityRefs.find( value ); it != entityRefs.end() ) return it->second;
            return source->MakeEntityRef( "gfx-external", value );
        };
        json relations = json::array();
        const auto addRelation = [&]( std::string sourceRef, std::string targetRef, const char* relation, uint64_t originFrameId ) {
            relations.push_back( {
                { "ref", source->MakeEntityRef( "correlation", relations.size() + 1 ) },
                { "source_ref", sourceRef }, { "target_ref", targetRef }, { "relation", relation },
                { "correlation_id", targetRef }, { "parent_span_id", sourceRef },
                { "origin_frame_id", originFrameId == 0 ? json( nullptr ) : json( Decimal( originFrameId ) ) },
                { "origin_frame_ref", originFrameId == 0 ? json( nullptr ) : json( source->MakeEntityRef( "frame-identity", originFrameId ) ) },
                { "evidence_kind", "exact" }
            } );
        };
        for( const auto& job : jobs )
        {
            if( job.originFrameId != 0 ) addRelation( source->MakeEntityRef( "frame-identity", job.originFrameId ), job.ref, "schedules", job.originFrameId );
            for( const auto& dependency : job.dependencies ) if( dependency.prerequisiteJobId != 0 )
                addRelation( source->MakeEntityRef( "job", dependency.prerequisiteJobId ), job.ref, "dependency_precedes", job.originFrameId );
        }
        for( const auto& dispatch : dispatches ) if( frames.contains( dispatch.frameIndex ) )
            addRelation( source->MakeEntityRef( "frame-identity", dispatch.frameIndex ), dispatch.ref, "dispatches", dispatch.frameIndex );
        for( const auto& entity : entities ) if( entity.parentId != 0 )
            addRelation( refForId( entity.parentId ), entity.ref, "parent", 0 );
        for( const auto& link : gfxLinks )
            addRelation( refForId( link.sourceId ), refForId( link.targetId ), GfxRelationName( link.relation ), 0 );

        if( !params.contains( "ref" ) || !params["ref"].is_string() ) throw QueryError( "INVALID_PARAMS", "ref is required" );
        const auto rootRef = params["ref"].get<std::string>();
        const auto parsedRootFrame = source->ParseEntityRef( rootRef, "frame-identity" );
        const auto parsedRootJob = source->ParseEntityRef( rootRef, "job" );

        // Expand lifecycle stages only for the requested Frame or Job. The
        // existing API contract and direct relations remain unchanged, while
        // correlation.chain gains exact navigation through wait continuation.
        for( const auto& job : jobs )
        {
            const bool expand = ( parsedRootFrame && job.originFrameId == *parsedRootFrame ) ||
                ( parsedRootJob && job.jobId == *parsedRootJob );
            if( !expand ) continue;

            std::map<std::pair<uint32_t, std::string>, std::string> waitEndsBySpan;
            std::map<std::pair<uint32_t, std::string>, std::string> continuationsBySpan;
            std::string completedRef;
            for( size_t index = 0; index < job.stages.size(); index++ )
            {
                const auto& stage = job.stages[index];
                if( stage.stage == uint8_t( JnJobStage::ScheduleCallstack ) ||
                    stage.stage == uint8_t( JnJobStage::WaitCallstack ) ) continue;
                const auto stageRef = JobStageRef( *source, job.jobId, index );
                addRelation( job.ref, stageRef, "has_stage", job.originFrameId );
                const auto key = std::make_pair( stage.spanId, stage.threadRef );
                if( stage.stage == uint8_t( JnJobStage::Completed ) ) completedRef = stageRef;
                else if( stage.stage == uint8_t( JnJobStage::WaitEnd ) ) waitEndsBySpan[key] = stageRef;
                else if( stage.stage == uint8_t( JnJobStage::Continuation ) ) continuationsBySpan[key] = stageRef;
            }
            for( const auto& [key, continuationRef] : continuationsBySpan )
            {
                const auto waitEnd = waitEndsBySpan.find( key );
                if( waitEnd != waitEndsBySpan.end() )
                    addRelation( waitEnd->second, continuationRef, "continues_on_waiter", job.originFrameId );
                if( !completedRef.empty() )
                    addRelation( completedRef, continuationRef, "completion_releases", job.originFrameId );
            }
        }

        std::unordered_map<std::string, std::vector<size_t>> relationIndicesByRef;
        relationIndicesByRef.reserve( relations.size() * 2 );
        for( size_t index = 0; index < relations.size(); index++ )
        {
            const auto sourceRef = relations[index].value( "source_ref", "" );
            const auto targetRef = relations[index].value( "target_ref", "" );
            if( !sourceRef.empty() ) relationIndicesByRef[sourceRef].push_back( index );
            if( !targetRef.empty() && targetRef != sourceRef ) relationIndicesByRef[targetRef].push_back( index );
        }
        const bool knownRoot = relationIndicesByRef.contains( rootRef ) || ( parsedRootFrame && frames.contains( *parsedRootFrame ) );
        if( !knownRoot ) throw QueryError( "ENTITY_NOT_FOUND", "correlated entity ref was not found" );

        if( method == "entity.related" )
        {
            json selected = json::array();
            if( const auto found = relationIndicesByRef.find( rootRef ); found != relationIndicesByRef.end() )
                for( const auto index : found->second ) selected.push_back( relations[index] );
            return Success( id, { { "present", true }, { "root_ref", rootRef }, { "relations", std::move( selected ) }, { "evidence_kind", "exact" } }, trace );
        }

        std::set<std::string> visited { rootRef };
        std::queue<std::string> frontier;
        frontier.push( rootRef );
        const auto maxNodes = size_t( UnsignedParameter( params, "max_nodes", 10000, 100000 ) );
        BudgetConsumeNode();
        bool truncated = false;
        while( !frontier.empty() )
        {
            checkCancelled();
            const auto current = frontier.front();
            frontier.pop();
            const auto adjacent = relationIndicesByRef.find( current );
            if( adjacent == relationIndicesByRef.end() ) continue;
            for( const auto index : adjacent->second )
            {
                const auto& edge = relations[index];
                std::string next;
                if( edge.value( "source_ref", "" ) == current ) next = edge.value( "target_ref", "" );
                else if( edge.value( "target_ref", "" ) == current ) next = edge.value( "source_ref", "" );
                if( next.empty() || visited.contains( next ) ) continue;
                if( !BudgetConsumeNode() || visited.size() >= maxNodes ) { truncated = true; break; }
                visited.emplace( next );
                frontier.push( next );
            }
            if( truncated ) break;
        }
        json selectedRelations = json::array();
        for( const auto& edge : relations ) if( visited.contains( edge.value( "source_ref", "" ) ) && visited.contains( edge.value( "target_ref", "" ) ) ) selectedRelations.push_back( edge );
        if( method == "correlation.chain" )
            return Success( id, { { "present", true }, { "root_ref", rootRef }, { "nodes", visited }, { "relations", std::move( selectedRelations ) }, { "truncated", truncated }, { "evidence_kind", "exact" } }, trace );

        const auto frameId = parseFrame();
        json relatedJobs = json::array();
        for( const auto& job : jobs ) if( job.originFrameId == frameId ) relatedJobs.push_back( JobJson( *source, job, false ) );
        json relatedDispatches = json::array();
        for( const auto& dispatch : dispatches ) if( dispatch.frameIndex == frameId ) relatedDispatches.push_back( GfxDispatchJson( dispatch ) );
        return Success( id, {
            { "present", true }, { "identity", frameJson( frameId ) }, { "jobs", std::move( relatedJobs ) },
            { "gfx_dispatches", std::move( relatedDispatches ) }, { "relations", std::move( selectedRelations ) },
            { "evidence_kind", "exact" }, { "window_inference_used", false }, { "truncated", truncated }
        }, trace );
    }
    if( method == "job.search" || method == "job.get" || method == "job.dependencies" || method == "job.critical_path" || method == "job.statistics" || method == "job.gfx.statistics" || method == "job.gfx_chain" )
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

        if( method == "job.statistics" )
        {
            std::vector<int64_t> scheduleToReady;
            std::vector<int64_t> readyToQueue;
            std::vector<int64_t> queueToFirstRun;
            std::vector<int64_t> dependencyReady;
            std::vector<int64_t> execution;
            std::vector<int64_t> wait;
            std::map<uint32_t, uint64_t> laneDispatches;
            std::map<uint32_t, uint64_t> laneStealsAsThief;
            std::map<uint32_t, uint64_t> laneStealsAsVictim;
            uint64_t completed = 0;
            uint64_t managed = 0;
            uint64_t burst = 0;
            uint64_t v2 = 0;
            uint64_t v3 = 0;
            uint64_t missingReady = 0;
            uint64_t missingQueue = 0;
            uint64_t duplicateReady = 0;
            uint64_t duplicateQueue = 0;
            uint64_t missingContinuation = 0;
            uint64_t continuationWithoutWait = 0;
            uint64_t continuationBeforeCompletion = 0;
            uint64_t waitEnds = 0;
            uint64_t continuations = 0;
            uint64_t jobsWithoutWaiter = 0;
            uint64_t invalidOrder = 0;
            uint64_t invalidScheduleToReady = 0;
            uint64_t invalidReadyToQueue = 0;
            uint64_t invalidQueueToFirstRun = 0;
            uint64_t schedulerSteals = 0;
            uint64_t rangeStealSlices = 0;
            uint64_t waitJobs = 0;
            uint64_t scheduleCallstacks = 0;
            uint64_t waitCallstacks = 0;
            uint64_t orphan = 0;
            uint64_t truncated = 0;
            json invalidOrderExamples = json::array();
            json missingReadyExamples = json::array();
            json missingQueueExamples = json::array();
            json duplicateStageExamples = json::array();
            const auto recordMissingStage = [&]( json& examples, const analysis::JobDto& job, const char* stage ) {
                if( examples.size() >= 16 ) return;
                bool dispatchTraceIdMissing = false;
                bool dispatchConnectionMissing = false;
                for( const auto& jobStage : job.stages )
                {
                    if( jobStage.stage != uint8_t( JnJobStage::Dispatch ) ) continue;
                    dispatchTraceIdMissing = dispatchTraceIdMissing || ( jobStage.flags & uint8_t( 1 << 3 ) ) != 0;
                    dispatchConnectionMissing = dispatchConnectionMissing || ( jobStage.flags & uint8_t( 1 << 4 ) ) != 0;
                }
                json sameHandleJobs = json::array();
                json sameSlotJobs = json::array();
                if( job.packedHandle != 0 )
                {
                    for( const auto& candidate : jobs )
                    {
                        if( candidate.jobId == job.jobId || candidate.packedHandle != job.packedHandle ) continue;
                        sameHandleJobs.push_back( {
                            { "job_ref", candidate.ref }, { "job_id", Decimal( candidate.jobId ) },
                            { "capture_boundary", candidate.captureBoundary }, { "orphan", candidate.orphan },
                            { "ready", candidate.readyNs.has_value() }, { "queue_enter", candidate.queueEnterNs.has_value() },
                            { "dispatch_count", candidate.dispatchCount }, { "stage_count", candidate.stages.size() }
                        } );
                        if( sameHandleJobs.size() >= 8 ) break;
                    }

                    std::vector<const analysis::JobDto*> slotCandidates;
                    const auto slotIndex = uint32_t( job.packedHandle );
                    for( const auto& candidate : jobs )
                    {
                        if( candidate.jobId == job.jobId || uint32_t( candidate.packedHandle ) != slotIndex ) continue;
                        slotCandidates.push_back( &candidate );
                    }
                    std::sort( slotCandidates.begin(), slotCandidates.end(), [&]( const auto* lhs, const auto* rhs ) {
                        const auto lhsDelta = lhs->jobId > job.jobId ? lhs->jobId - job.jobId : job.jobId - lhs->jobId;
                        const auto rhsDelta = rhs->jobId > job.jobId ? rhs->jobId - job.jobId : job.jobId - rhs->jobId;
                        return lhsDelta != rhsDelta ? lhsDelta < rhsDelta : lhs->jobId < rhs->jobId;
                    } );
                    for( const auto* candidate : slotCandidates )
                    {
                        uint32_t readyCount = 0;
                        uint32_t queueCount = 0;
                        for( const auto& candidateStage : candidate->stages )
                        {
                            readyCount += candidateStage.stage == uint8_t( JnJobStage::Ready );
                            queueCount += candidateStage.stage == uint8_t( JnJobStage::QueueEnter );
                        }
                        sameSlotJobs.push_back( {
                            { "job_ref", candidate->ref }, { "job_id", Decimal( candidate->jobId ) },
                            { "packed_handle", Decimal( candidate->packedHandle ) },
                            { "generation", uint32_t( candidate->packedHandle >> 32 ) },
                            { "schedule_ns", Decimal( candidate->scheduleNs ) },
                            { "capture_boundary", candidate->captureBoundary }, { "orphan", candidate->orphan },
                            { "ready_count", readyCount }, { "queue_enter_count", queueCount },
                            { "dispatch_count", candidate->dispatchCount }, { "stage_count", candidate->stages.size() }
                        } );
                        if( sameSlotJobs.size() >= 8 ) break;
                    }
                }
                examples.push_back( {
                    { "job_ref", job.ref }, { "job_id", Decimal( job.jobId ) }, { "name", job.name },
                    { "packed_handle", Decimal( job.packedHandle ) },
                    { "missing_stage", stage }, { "flags", job.flags }, { "kind", JobKindName( job.kind ) },
                    { "count", job.count }, { "grain_size", job.grainSize },
                    { "expected_dependency_count", job.expectedDependencyCount },
                    { "dispatch_count", job.dispatchCount }, { "completed", job.completedNs.has_value() },
                    { "dispatch_trace_id_missing", dispatchTraceIdMissing },
                    { "dispatch_connection_missing", dispatchConnectionMissing },
                    { "same_packed_handle_jobs", std::move( sameHandleJobs ) },
                    { "same_slot_jobs", std::move( sameSlotJobs ) }
                } );
            };
            const auto recordInvalidOrder = [&]( const analysis::JobDto& job, const char* relation, int64_t begin, int64_t end ) {
                if( invalidOrderExamples.size() >= 16 ) return;
                invalidOrderExamples.push_back( {
                    { "job_ref", job.ref }, { "job_id", Decimal( job.jobId ) }, { "name", job.name },
                    { "relation", relation }, { "begin_ns", Decimal( begin ) }, { "end_ns", Decimal( end ) },
                    { "delta_ns", Decimal( end - begin ) }
                } );
            };
            for( const auto& job : jobs )
            {
                checkCancelled();
                const bool captureBoundary = job.orphan || job.truncated;
                uint32_t readyStageCount = 0;
                uint32_t queueStageCount = 0;
                for( const auto& jobStage : job.stages )
                {
                    readyStageCount += jobStage.stage == uint8_t( JnJobStage::Ready );
                    queueStageCount += jobStage.stage == uint8_t( JnJobStage::QueueEnter ) &&
                        ( jobStage.flags & uint8_t( 1 << 6 ) ) == 0;
                }
                if( readyStageCount > 1 || queueStageCount > 1 )
                {
                    duplicateReady += readyStageCount > 1 ? readyStageCount - 1 : 0;
                    duplicateQueue += queueStageCount > 1 ? queueStageCount - 1 : 0;
                    if( duplicateStageExamples.size() < 16 ) duplicateStageExamples.push_back( {
                        { "job_ref", job.ref }, { "job_id", Decimal( job.jobId ) },
                        { "packed_handle", Decimal( job.packedHandle ) }, { "name", job.name },
                        { "ready_count", readyStageCount }, { "queue_enter_count", queueStageCount },
                        { "dispatch_count", job.dispatchCount }, { "stage_count", job.stages.size() }
                    } );
                }
                completed += job.completedNs.has_value();
                managed += job.kind == uint8_t( JnJobKind::Managed );
                burst += job.kind == uint8_t( JnJobKind::Burst );
                orphan += job.orphan;
                truncated += job.truncated;
                scheduleCallstacks += job.scheduleCallstack != 0;
                waitCallstacks += job.waitCallstacks.size();
                schedulerSteals += job.schedulerStealCount;
                rangeStealSlices += job.rangeStealSliceCount;
                waitEnds += job.waitEndCount;
                continuations += job.continuationCount;
                jobsWithoutWaiter += job.waitEndCount == 0 && job.continuationCount == 0;
                if( job.waitNs > 0 ) { waitJobs++; wait.push_back( job.waitNs ); }
                if( job.executionNs > 0 ) execution.push_back( job.executionNs );
                if( job.jobSchemaVersion >= 2 )
                {
                    v2++;
                    if( !captureBoundary && !job.readyNs )
                    {
                        missingReady++;
                        recordMissingStage( missingReadyExamples, job, "ready" );
                    }
                    if( !captureBoundary && job.dispatchCount != 0 && !job.queueEnterNs )
                    {
                        missingQueue++;
                        recordMissingStage( missingQueueExamples, job, "queue_enter" );
                    }
                }
                if( job.jobSchemaVersion >= 3 )
                {
                    v3++;
                    std::map<std::pair<uint32_t, std::string>, int64_t> waitEndBySpan;
                    std::map<std::pair<uint32_t, std::string>, int64_t> continuationBySpan;
                    for( const auto& stage : job.stages )
                    {
                        const auto key = std::make_pair( stage.spanId, stage.threadRef );
                        if( stage.stage == uint8_t( JnJobStage::WaitEnd ) ) waitEndBySpan[key] = stage.timeNs;
                        else if( stage.stage == uint8_t( JnJobStage::Continuation ) ) continuationBySpan[key] = stage.timeNs;
                    }
                    if( !captureBoundary && !job.incomplete )
                    {
                        for( const auto& [key, waitEnd] : waitEndBySpan )
                        {
                            const auto continuation = continuationBySpan.find( key );
                            if( continuation == continuationBySpan.end() || continuation->second < waitEnd ) missingContinuation++;
                        }
                        for( const auto& [key, continuation] : continuationBySpan )
                        {
                            const auto waitEnd = waitEndBySpan.find( key );
                            if( waitEnd == waitEndBySpan.end() || continuation < waitEnd->second ) continuationWithoutWait++;
                            if( job.completedNs && continuation < *job.completedNs ) continuationBeforeCompletion++;
                        }
                    }
                }
                if( !captureBoundary && job.readyNs )
                {
                    const auto value = *job.readyNs - job.scheduleNs;
                    if( value >= 0 ) scheduleToReady.push_back( value );
                    else { invalidOrder++; invalidScheduleToReady++; recordInvalidOrder( job, "schedule_to_ready", job.scheduleNs, *job.readyNs ); }
                }
                if( !captureBoundary && job.readyNs && job.queueEnterNs )
                {
                    const auto value = *job.queueEnterNs - *job.readyNs;
                    if( value >= 0 ) readyToQueue.push_back( value );
                    else { invalidOrder++; invalidReadyToQueue++; recordInvalidOrder( job, "ready_to_queue", *job.readyNs, *job.queueEnterNs ); }
                }
                if( !captureBoundary && job.queueEnterNs && job.firstRunNs )
                {
                    const auto value = *job.firstRunNs - *job.queueEnterNs;
                    if( value >= 0 ) queueToFirstRun.push_back( value );
                    else { invalidOrder++; invalidQueueToFirstRun++; recordInvalidOrder( job, "queue_to_first_run", *job.queueEnterNs, *job.firstRunNs ); }
                }
                if( !captureBoundary && job.dependencyReadyLatencyNs ) dependencyReady.push_back( *job.dependencyReadyLatencyNs );
                for( const auto& stage : job.stages )
                {
                    if( stage.stage == uint8_t( JnJobStage::Dispatch ) ) laneDispatches[stage.arg0]++;
                    else if( stage.stage == uint8_t( JnJobStage::Steal ) )
                    {
                        laneStealsAsThief[stage.arg0]++;
                        laneStealsAsVictim[stage.arg1]++;
                    }
                }
            }
            json lanes = json::array();
            std::set<uint32_t> laneIds;
            for( const auto& [lane, count] : laneDispatches ) laneIds.insert( lane );
            for( const auto& [lane, count] : laneStealsAsThief ) laneIds.insert( lane );
            for( const auto& [lane, count] : laneStealsAsVictim ) laneIds.insert( lane );
            for( const auto lane : laneIds ) lanes.push_back( {
                { "lane", lane }, { "dispatches", Decimal( laneDispatches[lane] ) },
                { "steals_as_thief", Decimal( laneStealsAsThief[lane] ) },
                { "steals_as_victim", Decimal( laneStealsAsVictim[lane] ) }
            } );
            return Success( id, {
                { "present", !jobs.empty() }, { "job_schema_version", v3 != 0 ? 3 : v2 != 0 ? 2 : 1 },
                { "source_mode", v3 != 0 ? "native-hooks-job-v3" : v2 != 0 ? "native-hooks-job-v2" : "native-hooks-job-v1" }, { "callstack_kind", "native" },
                { "counts", {
                    { "jobs", Decimal( jobs.size() ) }, { "completed", Decimal( completed ) },
                    { "managed", Decimal( managed ) }, { "burst", Decimal( burst ) },
                    { "v2", Decimal( v2 ) }, { "v2_or_newer", Decimal( v2 ) }, { "v3", Decimal( v3 ) },
                    { "scheduler_steals", Decimal( schedulerSteals ) },
                    { "range_steal_slices", Decimal( rangeStealSlices ) }, { "wait_jobs", Decimal( waitJobs ) },
                    { "schedule_callstacks", Decimal( scheduleCallstacks ) }, { "wait_callstacks", Decimal( waitCallstacks ) },
                    { "wait_ends", Decimal( waitEnds ) }, { "continuations", Decimal( continuations ) },
                    { "jobs_without_waiter", Decimal( jobsWithoutWaiter ) }
                } },
                { "latency", {
                    { "schedule_to_ready", StatisticsJson( analysis::ComputeStatistics( scheduleToReady ) ) },
                    { "ready_to_queue", StatisticsJson( analysis::ComputeStatistics( readyToQueue ) ) },
                    { "queue_to_first_run", StatisticsJson( analysis::ComputeStatistics( queueToFirstRun ) ) },
                    { "dependency_complete_to_ready", StatisticsJson( analysis::ComputeStatistics( dependencyReady ) ) },
                    { "execution", StatisticsJson( analysis::ComputeStatistics( execution ) ) },
                    { "wait", StatisticsJson( analysis::ComputeStatistics( wait ) ) }
                } },
                { "lanes", std::move( lanes ) },
                { "quality", {
                    { "complete", missingReady == 0 && missingQueue == 0 && invalidOrder == 0 &&
                        missingContinuation == 0 && continuationWithoutWait == 0 && continuationBeforeCompletion == 0 },
                    { "missing_ready", Decimal( missingReady ) }, { "missing_queue", Decimal( missingQueue ) },
                    { "missing_ready_examples", std::move( missingReadyExamples ) },
                    { "missing_queue_examples", std::move( missingQueueExamples ) },
                    { "duplicate_ready", Decimal( duplicateReady ) },
                    { "duplicate_queue", Decimal( duplicateQueue ) },
                    { "duplicate_stage_examples", std::move( duplicateStageExamples ) },
                    { "missing_continuation", Decimal( missingContinuation ) },
                    { "continuation_without_wait", Decimal( continuationWithoutWait ) },
                    { "continuation_before_completion", Decimal( continuationBeforeCompletion ) },
                    { "invalid_order", Decimal( invalidOrder ) },
                    { "invalid_schedule_to_ready", Decimal( invalidScheduleToReady ) },
                    { "invalid_ready_to_queue", Decimal( invalidReadyToQueue ) },
                    { "invalid_queue_to_first_run", Decimal( invalidQueueToFirstRun ) },
                    { "invalid_order_examples", std::move( invalidOrderExamples ) },
                    { "capture_boundary_orphan", Decimal( orphan ) },
                    { "capture_boundary_truncated", Decimal( truncated ) }, { "cancelled_supported", false },
                    { "cancelled_reason", "this Unity uJobs branch has no real cancellation API" }
                } }
            }, trace );
        }

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
        BudgetConsumeNode();
        bool truncated = false;
        while( !frontier.empty() )
        {
            const auto current = frontier.front();
            frontier.pop();
            for( const auto next : adjacency[current] )
            {
                if( visited.contains( next ) ) continue;
                if( !BudgetConsumeNode() || visited.size() >= maxNodes ) { truncated = true; break; }
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
        std::optional<uint32_t> callstack;
        if( params.contains( "callstack" ) ) callstack = parseCallstack( params["callstack"] );
        auto scanPage = ScanFiltered<analysis::SampleDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanSampleEvents( range ); },
            [&]( const auto& sample ) {
                return ( threadRef.empty() || sample.threadRef == threadRef ) &&
                    ( kind.empty() || sample.kind == kind ) && ( !callstack || sample.callstack == *callstack );
            }, SampleJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "samples", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
    }
    if( method == "sample.ghost_zones" )
    {
        const auto page = ParsePage( params, method, trace );
        const std::string threadRef = params.value( "thread_ref", "" );
        auto scanPage = ScanFiltered<analysis::GhostZoneDto>( *source, params, page,
            []( const auto& item, const auto& range ) { return item.ScanGhostZones( range ); },
            [&]( const auto& zone ) { return ( threadRef.empty() || zone.threadRef == threadRef ) && TextMatches( zone.name + " " + zone.file, params ); }, GhostZoneJson );
        const auto returned = scanPage.values.size();
        const auto cursor = NextCursorAt( page, method, trace, scanPage.nextOffset, scanPage.nextRawOffset, scanPage.hasMore );
        return Success( id, { { "ready", true }, { "ghost_zones", std::move( scanPage.values ) } }, trace, PageJson( page, returned, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto samples = source->ScanSampleEvents( range );
            BudgetScanned( samples.size(), chunk, allowed );
            for( const auto& sample : samples ) if( sample.callstack != 0 && ( threadRef.empty() || sample.threadRef == threadRef ) && ( kind == "all" || sample.kind == kind ) )
            {
                if( !counts.contains( sample.callstack ) && !BudgetConsumeGroup() ) continue;
                counts[sample.callstack]++;
            }
            offset += samples.size(); if( samples.size() < allowed ) break;
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
    if( method == "source.callsite" || method == "source.callsite.search" )
    {
        auto callsites = source->GetCallsites();
        if( method == "source.callsite" )
        {
            const auto requested = uint32_t( UnsignedParameter( params, "callsite_id", 0, std::numeric_limits<uint32_t>::max() ) );
            const auto it = std::find_if( callsites.begin(), callsites.end(), [&]( const auto& value ) { return value.callsiteId == requested; } );
            if( it == callsites.end() ) throw QueryError( "ENTITY_NOT_FOUND", "callsite_id was not found" );
            return Success( id, CallsiteJson( *it ), trace );
        }

        std::optional<uint32_t> requestedId;
        if( params.contains( "callsite_id" ) ) requestedId = uint32_t( UnsignedParameter( params, "callsite_id", 0, std::numeric_limits<uint32_t>::max() ) );
        const auto requestedProvenance = params.value( "provenance", std::string() );
        const auto requestedDomain = params.value( "domain", std::string() );
        callsites.erase( std::remove_if( callsites.begin(), callsites.end(), [&]( const auto& value ) {
            if( requestedId && value.callsiteId != *requestedId ) return true;
            if( !requestedProvenance.empty() && value.provenance != requestedProvenance ) return true;
            if( !requestedDomain.empty() && std::to_string( value.domain ) != requestedDomain ) return true;
            const auto searchable = std::to_string( value.callsiteId ) + " " + value.threadRef + " " + value.sourceLocationRef + " " + value.provenance + " " + value.unavailableReason.value_or( "" );
            return !TextMatches( searchable, params );
        } ), callsites.end() );
        const auto page = ParsePage( params, method, trace );
        const size_t begin = std::min( page.offset, callsites.size() );
        const size_t end = std::min( begin + page.limit, callsites.size() );
        json values = json::array();
        for( size_t index = begin; index < end; index++ ) values.emplace_back( CallsiteJson( callsites[index] ) );
        const auto cursor = NextCursor( page, method, trace, values.size(), end < callsites.size() );
        return Success( id, { { "callsites", std::move( values ) } }, trace, PageJson( page, end - begin, cursor ) );
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
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto zones = source->ScanCpuZones( range );
            BudgetScanned( zones.size(), chunk, allowed );
            for( const auto& zone : zones ) if( zone.endNs )
            {
                if( !groups.contains( zone.sourceLocationRef ) && !BudgetConsumeGroup() ) continue;
                auto& stats = groups[zone.sourceLocationRef]; stats.cpuCount++; stats.cpuInclusive += *zone.endNs - zone.startNs;
                if( zone.selfTimeNs ) stats.cpuSelf += *zone.selfTimeNs; if( zone.runningTimeNs ) stats.cpuRunning += *zone.runningTimeNs;
            }
            offset += zones.size(); if( zones.size() < allowed ) break;
        }
        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            auto range = ScanRangeFrom( params, offset, allowed );
            const auto zones = source->ScanGpuZones( range );
            BudgetScanned( zones.size(), chunk, allowed );
            for( const auto& zone : zones ) if( zone.gpuEndNs )
            {
                if( !groups.contains( zone.sourceLocationRef ) && !BudgetConsumeGroup() ) continue;
                auto& stats = groups[zone.sourceLocationRef]; stats.gpuCount++; stats.gpuInclusive += *zone.gpuEndNs - zone.gpuStartNs;
                if( zone.selfTimeNs ) stats.gpuSelf += *zone.selfTimeNs;
            }
            offset += zones.size(); if( zones.size() < allowed ) break;
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
    if( method == "compare.compatibility" || method == "compare.normalized" || method == "compare.zones" || method == "compare.frames" || method == "compare.source" )
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

        const auto comparisonMode = params.value( "comparison_mode", "performance" );
        if( comparisonMode != "performance" && comparisonMode != "contract" ) throw QueryError( "INVALID_PARAMS", "comparison_mode must be performance or contract" );
        const auto baselineInfo = baseline->GetTraceInfo();
        const auto candidateInfo = source->GetTraceInfo();
        const auto baselineIdentity = CaptureIdentityJson( baselineInfo );
        const auto candidateIdentity = CaptureIdentityJson( candidateInfo );
        const auto baselineContext = CaptureContextJson( baselineInfo );
        const auto candidateContext = CaptureContextJson( candidateInfo );
        const auto baselineCoverage = CaptureCoverageJson( baselineInfo );
        const auto candidateCoverage = CaptureCoverageJson( candidateInfo );
        const auto comparisonWindow = SelectComparisonFrameWindow( *baseline, *source, params, baselineContext, candidateContext );

        json compatibilityChecks = json::array();
        size_t hardFailures = 0;
        size_t warningFailures = 0;
        size_t informationDifferences = 0;
        const auto addCheck = [&]( const std::string& checkId, const char* severity, const std::string& path,
            json left, json right, bool matched, const std::string& reason ) {
            compatibilityChecks.push_back( {
                { "id", checkId }, { "severity", severity }, { "path", path },
                { "baseline", std::move( left ) }, { "candidate", std::move( right ) },
                { "matched", matched }, { "reason", reason }
            } );
            if( matched ) return;
            if( std::string_view( severity ) == "hard" ) hardFailures++;
            else if( std::string_view( severity ) == "warning" ) warningFailures++;
            else informationDifferences++;
        };
        const auto comparePath = [&]( const std::string& checkId, const char* severity, const std::string& path,
            const json& leftDocument, const json& rightDocument, const std::string& reason ) {
            const auto* left = JsonPointerValue( leftDocument, path.c_str() );
            const auto* right = JsonPointerValue( rightDocument, path.c_str() );
            const bool required = std::string_view( severity ) == "hard";
            addCheck( checkId, severity, path, left ? *left : json( nullptr ), right ? *right : json( nullptr ),
                ( left == nullptr && right == nullptr && !required ) || ( left != nullptr && right != nullptr && *left == *right ), reason );
        };

        addCheck( "identity.complete", "hard", "/capture_identity/complete",
            baselineIdentity.value( "complete", false ), candidateIdentity.value( "complete", false ),
            baselineIdentity.value( "complete", false ) && candidateIdentity.value( "complete", false ),
            "both traces require complete, conflict-free Capture Identity" );
        addCheck( "context.complete", "hard", "/capture_context/complete",
            baselineContext.value( "complete", false ), candidateContext.value( "complete", false ),
            baselineContext.value( "complete", false ) && candidateContext.value( "complete", false ),
            "both traces require complete runtime, workload, and capture configuration context" );
        comparePath( "protocol.jn_abi", "hard", "/identity/protocol/jn_abi_version", baselineIdentity, candidateIdentity, "JN C ABI must match" );
        comparePath( "protocol.jn_config", "hard", "/identity/protocol/jn_config_hash", baselineIdentity, candidateIdentity, "JN compile-time configuration must match" );
        comparePath( "protocol.tracy", "hard", "/identity/protocol/tracy_protocol_version", baselineIdentity, candidateIdentity, "Tracy wire protocol must match" );
        comparePath( "runtime.architecture", "hard", "/identity/runtime/architecture", baselineIdentity, candidateIdentity, "CPU architecture must match" );
        comparePath( "runtime.graphics_api", "hard", "/identity/runtime/graphics_api", baselineIdentity, candidateIdentity, "graphics API must match" );
        comparePath( "runtime.gfx_jobs_requested", "hard", "/context/runtime/graphics_jobs_requested", baselineContext, candidateContext, "requested Graphics Jobs mode must match" );
        comparePath( "runtime.gfx_jobs_effective", "hard", "/context/runtime/graphics_jobs_effective", baselineContext, candidateContext, "effective Graphics Jobs mode must match" );

        const auto targetSeverity = comparisonMode == "performance" ? "hard" : "info";
        comparePath( "runtime.target_kind", targetSeverity, "/identity/runtime/target_kind", baselineIdentity, candidateIdentity,
            comparisonMode == "performance" ? "performance comparisons require the same Editor or Player target" : "contract mode permits Editor/Player target differences" );
        comparePath( "workload.scene", targetSeverity, "/context/workload/scene", baselineContext, candidateContext,
            comparisonMode == "performance" ? "performance comparisons require the same scene" : "contract mode records scene differences without treating them as failure" );
        comparePath( "workload.scenario", targetSeverity, "/context/workload/scenario", baselineContext, candidateContext,
            comparisonMode == "performance" ? "performance comparisons require the same scenario" : "contract mode records scenario differences without treating them as failure" );

        for( const auto& [idSuffix, pointer] : std::vector<std::pair<std::string, std::string>> {
            { "engine", "/identity/build/repositories/engine/revision" },
            { "package", "/identity/build/repositories/package/revision" },
            { "tracy", "/identity/build/repositories/tracy/revision" },
            { "unity", "/identity/build/artifacts/unity/sha256" },
            { "jn_client", "/identity/build/artifacts/jn_client/sha256" },
            { "query", "/identity/build/artifacts/query/sha256" } } )
            comparePath( "build." + idSuffix, "warning", pointer, baselineIdentity, candidateIdentity, "build identity differs; expected changes must be reviewed" );

        for( const auto& [idSuffix, pointer] : std::vector<std::pair<std::string, std::string>> {
            { "resolution_width", "/context/runtime/resolution_width" },
            { "resolution_height", "/context/runtime/resolution_height" },
            { "quality_level", "/context/runtime/quality_level" },
            { "vsync_count", "/context/runtime/vsync_count" },
            { "target_frame_rate", "/context/runtime/target_frame_rate" },
            { "dynamic_resolution_width", "/context/runtime/dynamic_resolution_width_scale" },
            { "dynamic_resolution_height", "/context/runtime/dynamic_resolution_height_scale" },
            { "capture_profile_legacy", "/context/capture_config/profile" },
            { "capture_profile_requested", "/context/capture_config/profile_requested" },
            { "capture_profile_effective", "/context/capture_config/profile_effective" },
            { "capture_profile_fallback", "/context/capture_config/profile_fallback" },
            { "managed_profile", "/context/capture_config/n11_profile" },
            { "managed_stack", "/context/capture_config/managed_stack_explicit" },
            { "lua_stack", "/context/capture_config/lua_stack_explicit" },
            { "job_mode", "/context/capture_config/n12_job_mode" },
            { "job_callstack_depth", "/context/capture_config/job_callstack_depth_requested" } } )
            comparePath( "configuration." + idSuffix, "warning", pointer, baselineContext, candidateContext, "runtime or capture configuration differs" );

        const auto producerSchemas = []( const json& coverage ) {
            json schemas = json::object();
            if( coverage.contains( "producers" ) && coverage["producers"].is_array() )
                for( const auto& producer : coverage["producers"] )
                    schemas[producer.value( "key", "" )] = producer.value( "producer_schema", 0 );
            return schemas;
        };
        const auto leftSchemas = producerSchemas( baselineCoverage );
        const auto rightSchemas = producerSchemas( candidateCoverage );
        addCheck( "producer.schemas", "warning", "/capture_coverage/producers", leftSchemas, rightSchemas,
            leftSchemas == rightSchemas, "producer keys and schemas should match for normalized domain comparison" );
        addCheck( "producer.coverage_complete", "warning", "/capture_coverage/complete",
            baselineCoverage.value( "complete", false ), candidateCoverage.value( "complete", false ),
            baselineCoverage.value( "complete", false ) && candidateCoverage.value( "complete", false ),
            "producer counter windows must be closed and internally consistent" );
        addCheck( "trace.complete", "warning", "/trace/complete", baselineTrace.complete, trace.complete,
            baselineTrace.complete && trace.complete, "truncated or open stream tails reduce confidence" );
        addCheck( "frame_window", comparisonMode == "performance" ? "hard" : "info", "/frame_window",
            comparisonWindow.valid ? json( comparisonWindow.frameCount ) : json( nullptr ),
            comparisonWindow.valid ? json( comparisonWindow.frameCount ) : json( nullptr ), comparisonWindow.valid,
            comparisonWindow.valid ? "complete frames are aligned by ordinal after warmup" : comparisonWindow.reason );

        const auto compatibilityVerdict = hardFailures != 0 ? "incompatible" : warningFailures != 0 ? "compatible_with_warnings" : "compatible";
        const json compatibility = {
            { "traces", tracePair }, { "comparison_mode", comparisonMode }, { "verdict", compatibilityVerdict },
            { "performance_comparable", comparisonMode == "performance" && hardFailures == 0 },
            { "contract_comparable", hardFailures == 0 },
            { "hard_failure_count", hardFailures }, { "warning_count", warningFailures },
            { "information_difference_count", informationDifferences },
            { "frame_window", ComparisonWindowJson( comparisonWindow ) }, { "checks", compatibilityChecks },
            { "policy", { { "warnings_require_explicit_override_for_normalized_compare", true },
                { "stable_matching_required", true }, { "legacy_fallback_reported", true } } }
        };
        if( method == "compare.compatibility" ) return Success( id, compatibility, trace );

        if( method == "compare.normalized" )
        {
            const bool allowWarnings = params.value( "allow_warnings", false );
            if( comparisonMode != "performance" )
                return Success( id, { { "performed", false }, { "reason", "compare.normalized is a performance comparison; use compare.compatibility with contract mode for Editor/Player data-contract checks" },
                    { "compatibility", compatibility } }, trace );
            if( hardFailures != 0 )
                return Success( id, { { "performed", false }, { "reason", "capture pair is incompatible" }, { "compatibility", compatibility } }, trace );
            if( warningFailures != 0 && !allowWarnings )
                return Success( id, { { "performed", false }, { "reason", "capture pair has compatibility warnings; review them and set allow_warnings=true to proceed" },
                    { "compatibility", compatibility } }, trace );

            struct MetricGroup
            {
                std::string key;
                std::string name;
                std::string matchKind;
                std::vector<int64_t> inclusive;
                std::vector<int64_t> self;
                std::vector<int64_t> running;
                std::vector<int64_t> latency;
                std::vector<int64_t> wait;
            };
            using MetricMap = std::map<std::string, MetricGroup>;
            const auto buildCatalogMaps = []( const json& catalog ) {
                std::map<std::string, std::string> sourceKeys;
                std::map<std::string, std::string> jobKeys;
                std::set<std::string> ambiguousSources;
                if( !catalog.value( "present", false ) ) return std::pair { sourceKeys, jobKeys };
                for( const auto& definition : catalog["definitions"] )
                {
                    const auto kind = definition.value( "kind", "" );
                    const auto name = definition.value( "canonical_name", "" );
                    const auto stable = definition.value( "definition_key", "" );
                    if( kind == "job" && !name.empty() && !stable.empty() ) jobKeys.emplace( name, stable );
                    if( kind != "source" || name.empty() || stable.empty() ) continue;
                    const auto function = definition["source"].value( "function", "" );
                    const auto line = definition["source"].value( "line", 0u );
                    const auto key = name + '\n' + function + '\n' + std::to_string( line );
                    if( sourceKeys.contains( key ) ) ambiguousSources.emplace( key );
                    else sourceKeys.emplace( key, stable );
                }
                for( const auto& key : ambiguousSources ) sourceKeys.erase( key );
                return std::pair { sourceKeys, jobKeys };
            };
            const auto baselineCatalog = CatalogJson( baselineInfo );
            const auto candidateCatalog = CatalogJson( candidateInfo );
            const auto [baselineSourceKeys, baselineJobKeys] = buildCatalogMaps( baselineCatalog );
            const auto [candidateSourceKeys, candidateJobKeys] = buildCatalogMaps( candidateCatalog );

            const auto collectCpu = [&]( const analysis::TraceSource& item, analysis::ScanRange selectedRange,
                const std::map<std::string, std::string>& sourceKeys ) {
                MetricMap groups;
                size_t offset = 0;
                constexpr size_t chunk = 4096;
                while( true )
                {
                    checkCancelled();
                    const auto allowed = BudgetScanAllowance( chunk );
                    if( allowed == 0 ) break;
                    selectedRange.offset = offset;
                    selectedRange.limit = allowed;
                    const auto values = item.ScanCpuZones( selectedRange );
                    BudgetScanned( values.size(), chunk, allowed );
                    for( const auto& zone : values )
                    {
                        if( !zone.endNs || *zone.endNs < zone.startNs ) continue;
                        const auto displayName = zone.name.empty() ? zone.function : zone.name;
                        const auto sourceMatch = displayName + '\n' + zone.function + '\n' + std::to_string( zone.line );
                        const auto stable = sourceKeys.find( sourceMatch );
                        const auto fallback = NormalizeSourceKey( zone.file ) + ':' + std::to_string( zone.line ) + '|' + displayName + '|' + zone.function;
                        const auto key = stable == sourceKeys.end() ? "fallback:cpu:" + fallback : "stable:" + stable->second;
                        auto& group = groups[key];
                        group.key = key;
                        group.name = displayName;
                        group.matchKind = stable == sourceKeys.end() ? "legacy_source_fallback" : "catalog_definition_key";
                        group.inclusive.emplace_back( *zone.endNs - zone.startNs );
                        if( zone.selfTimeNs ) group.self.emplace_back( *zone.selfTimeNs );
                        if( zone.runningTimeNs ) group.running.emplace_back( *zone.runningTimeNs );
                    }
                    offset += values.size();
                    if( values.size() < allowed ) break;
                }
                return groups;
            };

            const auto collectGpu = [&]( const analysis::TraceSource& item, const analysis::TraceInfoDto& itemInfo,
                analysis::ScanRange selectedRange ) {
                MetricMap groups;
                const auto taxonomy = GpuTaxonomyCatalogJson( itemInfo );
                std::map<uint32_t, json> definitions;
                if( taxonomy.value( "present", false ) )
                    for( const auto& definition : taxonomy["definitions"] )
                    {
                        const auto id = DecimalStringValue( definition["taxonomy_id"] );
                        if( id && *id <= std::numeric_limits<uint32_t>::max() ) definitions.emplace( uint32_t( *id ), definition );
                    }
                auto explicitPasses = BuildExplicitGpuPassSet( item, taxonomy );
                std::vector<analysis::GpuZoneDto> zones;
                size_t offset = 0;
                constexpr size_t chunk = 4096;
                while( true )
                {
                    checkCancelled();
                    const auto allowed = BudgetScanAllowance( chunk );
                    if( allowed == 0 ) break;
                    selectedRange.offset = offset;
                    selectedRange.limit = allowed;
                    auto values = item.ScanGpuZones( selectedRange );
                    BudgetScanned( values.size(), chunk, allowed );
                    for( const auto& value : values )
                    {
                        MatchExplicitGpuPassZone( item, explicitPasses, value );
                        zones.emplace_back( value );
                    }
                    offset += values.size();
                    if( values.size() < allowed ) break;
                }
                std::set<std::string> explicitlyMatchedZones;
                for( const auto& pass : explicitPasses.matches )
                {
                    if( !pass.zone || !pass.zone->gpuEndNs || *pass.zone->gpuEndNs < pass.zone->gpuStartNs ) continue;
                    if( pass.zone->gpuStartNs < selectedRange.startNs || pass.zone->gpuStartNs >= selectedRange.endNs ) continue;
                    explicitlyMatchedZones.emplace( pass.zone->ref );
                    const auto definition = definitions.find( pass.taxonomyId );
                    const bool stable = pass.taxonomyStableId && definition != definitions.end();
                    const auto displayName = stable ? definition->second.value( "canonical_name", pass.zone->name ) :
                        ( pass.zone->name.empty() ? pass.zone->function : pass.zone->name );
                    const auto fallback = NormalizeSourceKey( pass.zone->file ) + ':' + std::to_string( pass.zone->line ) + '|' + displayName;
                    const auto key = stable ? "stable:gpu-taxonomy:" + std::to_string( pass.taxonomyId ) : "fallback:gpu:" + fallback;
                    auto& group = groups[key];
                    group.key = key;
                    group.name = displayName;
                    group.matchKind = stable ? "gpu_taxonomy_stable_id" : "legacy_source_fallback";
                    group.inclusive.emplace_back( *pass.zone->gpuEndNs - pass.zone->gpuStartNs );
                    if( pass.zone->selfTimeNs ) group.self.emplace_back( *pass.zone->selfTimeNs );
                }
                for( const auto& zone : zones )
                {
                    if( explicitlyMatchedZones.contains( zone.ref ) || !zone.gpuEndNs || *zone.gpuEndNs < zone.gpuStartNs ) continue;
                    const auto displayName = zone.name.empty() ? zone.function : zone.name;
                    const auto fallback = NormalizeSourceKey( zone.file ) + ':' + std::to_string( zone.line ) + '|' + displayName;
                    const auto key = "fallback:gpu:" + fallback;
                    auto& group = groups[key];
                    group.key = key;
                    group.name = displayName;
                    group.matchKind = "legacy_source_fallback";
                    group.inclusive.emplace_back( *zone.gpuEndNs - zone.gpuStartNs );
                    if( zone.selfTimeNs ) group.self.emplace_back( *zone.selfTimeNs );
                }
                return groups;
            };

            const auto collectJobs = []( const analysis::TraceSource& item, const analysis::ScanRange& range,
                const std::map<std::string, std::string>& jobKeys ) {
                MetricMap groups;
                for( const auto& job : item.GetJobs() )
                {
                    if( job.scheduleNs < range.startNs || job.scheduleNs >= range.endNs ) continue;
                    const auto stable = jobKeys.find( job.name );
                    const auto key = stable == jobKeys.end() ? "fallback:job:" + job.name : "stable:" + stable->second;
                    auto& group = groups[key];
                    group.key = key;
                    group.name = job.name;
                    group.matchKind = stable == jobKeys.end() ? "job_name_fallback" : "catalog_definition_key";
                    group.inclusive.emplace_back( job.executionNs );
                    group.wait.emplace_back( job.waitNs );
                    if( job.firstRunNs ) group.latency.emplace_back( *job.firstRunNs - job.scheduleNs );
                }
                return groups;
            };

            const auto compareMetricMaps = [&]( const MetricMap& left, const MetricMap& right, size_t frameCount ) {
                std::set<std::string> keys;
                for( const auto& [key, value] : left ) keys.emplace( key );
                for( const auto& [key, value] : right ) keys.emplace( key );
                struct RankedGroup { double magnitude = 0; std::string key; json value; };
                std::vector<RankedGroup> ranked;
                size_t stableMatches = 0, fallbackMatches = 0, unmatched = 0;
                for( const auto& key : keys )
                {
                    const auto before = left.find( key );
                    const auto after = right.find( key );
                    const bool both = before != left.end() && after != right.end();
                    const auto& metadata = after != right.end() ? after->second : before->second;
                    if( !both ) unmatched++;
                    else if( key.rfind( "stable:", 0 ) == 0 ) stableMatches++;
                    else fallbackMatches++;
                    const analysis::Statistics empty;
                    const auto beforeInclusive = before == left.end() ? empty : analysis::ComputeStatistics( before->second.inclusive );
                    const auto afterInclusive = after == right.end() ? empty : analysis::ComputeStatistics( after->second.inclusive );
                    const auto beforeSelf = before == left.end() ? empty : analysis::ComputeStatistics( before->second.self );
                    const auto afterSelf = after == right.end() ? empty : analysis::ComputeStatistics( after->second.self );
                    const auto beforeRunning = before == left.end() ? empty : analysis::ComputeStatistics( before->second.running );
                    const auto afterRunning = after == right.end() ? empty : analysis::ComputeStatistics( after->second.running );
                    const auto beforeLatency = before == left.end() ? empty : analysis::ComputeStatistics( before->second.latency );
                    const auto afterLatency = after == right.end() ? empty : analysis::ComputeStatistics( after->second.latency );
                    const auto beforeWait = before == left.end() ? empty : analysis::ComputeStatistics( before->second.wait );
                    const auto afterWait = after == right.end() ? empty : analysis::ComputeStatistics( after->second.wait );
                    const double deltaPerFrame = frameCount == 0 ? 0 : double( afterInclusive.total - beforeInclusive.total ) / double( frameCount );
                    const double deltaP95 = afterInclusive.p95 - beforeInclusive.p95;
                    ranked.push_back( { std::max( std::abs( deltaPerFrame ), std::abs( deltaP95 ) ), key, {
                        { "match_key", key }, { "match_kind", metadata.matchKind }, { "name", metadata.name },
                        { "presence", !both ? ( before == left.end() ? "candidate_only" : "baseline_only" ) : "both" },
                        { "baseline", { { "event", StatisticsJson( beforeInclusive ) }, { "self", StatisticsJson( beforeSelf ) },
                            { "running", StatisticsJson( beforeRunning ) }, { "latency", StatisticsJson( beforeLatency ) },
                            { "wait", StatisticsJson( beforeWait ) }, { "total_per_frame_ns", frameCount == 0 ? 0 : double( beforeInclusive.total ) / double( frameCount ) } } },
                        { "candidate", { { "event", StatisticsJson( afterInclusive ) }, { "self", StatisticsJson( afterSelf ) },
                            { "running", StatisticsJson( afterRunning ) }, { "latency", StatisticsJson( afterLatency ) },
                            { "wait", StatisticsJson( afterWait ) }, { "total_per_frame_ns", frameCount == 0 ? 0 : double( afterInclusive.total ) / double( frameCount ) } } },
                        { "delta", { { "total_per_frame_ns", deltaPerFrame }, { "p95_ns", deltaP95 },
                            { "mean_ratio", beforeInclusive.mean == 0 ? json( nullptr ) : json( afterInclusive.mean / beforeInclusive.mean ) } } }
                    } } );
                }
                std::sort( ranked.begin(), ranked.end(), []( const auto& leftValue, const auto& rightValue ) {
                    return leftValue.magnitude != rightValue.magnitude ? leftValue.magnitude > rightValue.magnitude : leftValue.key < rightValue.key;
                } );
                const auto total = ranked.size();
                if( ranked.size() > limit ) ranked.resize( limit );
                json groups = json::array();
                for( auto& value : ranked ) groups.emplace_back( std::move( value.value ) );
                return json { { "group_count", total }, { "stable_match_count", stableMatches },
                    { "fallback_match_count", fallbackMatches }, { "unmatched_count", unmatched }, { "groups", std::move( groups ) } };
            };

            const auto baselineCpu = collectCpu( *baseline, comparisonWindow.baselineRange, baselineSourceKeys );
            const auto candidateCpu = collectCpu( *source, comparisonWindow.candidateRange, candidateSourceKeys );
            const auto baselineGpu = collectGpu( *baseline, baselineInfo, comparisonWindow.baselineRange );
            const auto candidateGpu = collectGpu( *source, candidateInfo, comparisonWindow.candidateRange );
            const auto baselineJobs = collectJobs( *baseline, comparisonWindow.baselineRange, baselineJobKeys );
            const auto candidateJobs = collectJobs( *source, comparisonWindow.candidateRange, candidateJobKeys );

            std::vector<int64_t> baselineFrameDurations, candidateFrameDurations;
            for( const auto& frame : comparisonWindow.baselineFrames ) baselineFrameDurations.emplace_back( *frame.endNs - frame.beginNs );
            for( const auto& frame : comparisonWindow.candidateFrames ) candidateFrameDurations.emplace_back( *frame.endNs - frame.beginNs );
            const auto beforeFrames = analysis::ComputeStatistics( baselineFrameDurations );
            const auto afterFrames = analysis::ComputeStatistics( candidateFrameDurations );

            const auto producerMap = []( const json& coverage ) {
                std::map<std::string, json> output;
                if( coverage.contains( "producers" ) && coverage["producers"].is_array() )
                    for( const auto& producer : coverage["producers"] )
                    {
                        const auto key = producer.value( "key", "" ) + ":schema-" + std::to_string( producer.value( "producer_schema", 0 ) );
                        output.emplace( key, producer );
                    }
                return output;
            };
            const auto beforeProducers = producerMap( baselineCoverage );
            const auto afterProducers = producerMap( candidateCoverage );
            std::set<std::string> producerKeys;
            for( const auto& [key, value] : beforeProducers ) producerKeys.emplace( key );
            for( const auto& [key, value] : afterProducers ) producerKeys.emplace( key );
            json producerComparisons = json::array();
            for( const auto& key : producerKeys )
            {
                const auto before = beforeProducers.find( key );
                const auto after = afterProducers.find( key );
                producerComparisons.push_back( {
                    { "match_key", key }, { "presence", before == beforeProducers.end() ? "candidate_only" : after == afterProducers.end() ? "baseline_only" : "both" },
                    { "baseline", before == beforeProducers.end() ? json( nullptr ) : before->second },
                    { "candidate", after == afterProducers.end() ? json( nullptr ) : after->second }
                } );
            }

            return Success( id, {
                { "performed", true }, { "compatibility", compatibility }, { "frame_window", ComparisonWindowJson( comparisonWindow ) },
                { "normalization", { { "unit", "nanoseconds" }, { "per_frame_denominator", comparisonWindow.frameCount },
                    { "frame_alignment", "complete-frame ordinal" }, { "stable_id_precedence", { "catalog_definition_key", "gpu_taxonomy_stable_id", "legacy_fallback" } } } },
                { "frames", { { "baseline", StatisticsJson( beforeFrames ) }, { "candidate", StatisticsJson( afterFrames ) },
                    { "delta", { { "mean_ns", afterFrames.mean - beforeFrames.mean }, { "p95_ns", afterFrames.p95 - beforeFrames.p95 },
                        { "p99_ns", afterFrames.p99 - beforeFrames.p99 }, { "mean_ratio", beforeFrames.mean == 0 ? json( nullptr ) : json( afterFrames.mean / beforeFrames.mean ) } } } } },
                { "cpu", compareMetricMaps( baselineCpu, candidateCpu, comparisonWindow.frameCount ) },
                { "gpu", compareMetricMaps( baselineGpu, candidateGpu, comparisonWindow.frameCount ) },
                { "jobs", compareMetricMaps( baselineJobs, candidateJobs, comparisonWindow.frameCount ) },
                { "producers", { { "match_key", "producer key + schema" }, { "values", std::move( producerComparisons ) } } },
                { "partial", BudgetPartial() }, { "legacy_fallback_is_performance_evidence", false }
            }, trace );
        }

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
            std::set<std::string> budgetedGroupKeys;
            const auto collect = [&]( const std::shared_ptr<analysis::TraceSource>& item ) {
                ZoneMap groups;
                size_t offset = 0;
                constexpr size_t chunk = 4096;
                while( true )
                {
                    checkCancelled();
                    const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                    auto range = ScanRangeFrom( params, offset, allowed );
                    size_t count = 0;
                    if( domain == "gpu" )
                    {
                        const auto zones = item->ScanGpuZones( range );
                        count = zones.size();
                        for( const auto& zone : zones ) if( zone.gpuEndNs && TextMatches( zone.name + " " + zone.function + " " + zone.file, params ) )
                        {
                            const auto key = NormalizeSourceKey( zone.file ) + ':' + std::to_string( zone.line ) + '|' + zone.name + '|' + zone.function;
                            if( !budgetedGroupKeys.contains( key ) )
                            {
                                if( !BudgetConsumeGroup() ) continue;
                                budgetedGroupKeys.emplace( key );
                            }
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
                            if( !budgetedGroupKeys.contains( key ) )
                            {
                                if( !BudgetConsumeGroup() ) continue;
                                budgetedGroupKeys.emplace( key );
                            }
                            auto& group = groups[key]; group.name = zone.name; group.function = zone.function; group.file = zone.file; group.line = zone.line;
                            group.inclusive.emplace_back( *zone.endNs - zone.startNs );
                            if( zone.selfTimeNs ) group.self.emplace_back( *zone.selfTimeNs );
                            if( zone.runningTimeNs ) group.running.emplace_back( *zone.runningTimeNs );
                        }
                    }
                    BudgetScanned( count, chunk, allowed );
                    offset += count;
                    if( count < allowed ) break;
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
        if( trace.sourceKind == analysis::TraceSourceKind::Segment && !trace.complete )
        {
            addFinding( "warning", "TRUNCATED_STREAM_TAIL", "the selected committed stream revision has an incomplete or truncated tail; results are bounded to the last readable records", 1 );
        }
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
        const auto validEntityRef = [&]( const std::string& ref, std::string_view kind, uint64_t count ) {
            const auto parsed = source->ParseEntityRef( ref, kind );
            return parsed && *parsed < count;
        };
        std::set<uint32_t> referencedCallstacks;
        size_t offset = 0;
        constexpr size_t chunk = 4096;
        const auto indexedZoneValidation = source->ValidateZoneIndex( [&]( size_t requested ) {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( requested );
            if( allowed != 0 ) BudgetScanned( allowed, requested, allowed );
            return allowed;
        } );
        if( indexedZoneValidation )
        {
            for( const auto& value : indexedZoneValidation->findings )
            {
                json refs = json::array();
                for( const auto& ref : value.refs ) refs.emplace_back( ref );
                addFinding( value.severity.c_str(), value.code.c_str(), value.message, value.count, std::move( refs ) );
            }
            referencedCallstacks.insert( indexedZoneValidation->referencedCallstacks.begin(), indexedZoneValidation->referencedCallstacks.end() );
        }
        else
        {
            struct FrameRootInterval
            {
                std::string ref;
                std::string threadRef;
                int64_t startNs = 0;
                int64_t endNs = 0;
            };
            size_t incompleteCpu = 0, invalidCpu = 0, unresolvedCpuNames = 0;
            size_t incompleteGpu = 0, invalidGpu = 0;
            json incompleteCpuRefs = json::array(), invalidCpuRefs = json::array(), unresolvedCpuNameRefs = json::array(), incompleteGpuRefs = json::array(), invalidGpuRefs = json::array();
            std::vector<FrameRootInterval> mainPlayerLoopRoots;
            uint64_t duplicateUnityPlayerLoopRoots = 0;
            json duplicateUnityPlayerLoopRefs = json::array(), nestedMainPlayerLoopRefs = json::array();
            uint64_t nestedMainPlayerLoopRoots = 0;
            while( true )
            {
                checkCancelled();
                const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                analysis::ScanRange range; range.offset = offset; range.limit = allowed;
                const auto values = source->ScanCpuZones( range );
                BudgetScanned( values.size(), chunk, allowed );
                for( const auto& value : values )
                {
                    if( value.parentRef && !validEntityRef( *value.parentRef, "cpu-zone", metadata.counts.cpuZones ) ) noteReference( "CPU_ZONE_PARENT", value.ref );
                    if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                    if( !value.sourceLocationRef.empty() && sourceRefs.find( value.sourceLocationRef ) == sourceRefs.end() ) noteReference( "SOURCE_LOCATION", value.ref );
                    if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
                    if( !value.nameResolved ) { unresolvedCpuNames++; addRef( unresolvedCpuNameRefs, value.ref ); }
                    if( !value.complete ) { incompleteCpu++; addRef( incompleteCpuRefs, value.ref ); }
                    if( value.endNs && *value.endNs < value.startNs ) { invalidCpu++; addRef( invalidCpuRefs, value.ref ); }
                    if( value.name == "PlayerLoop" )
                    {
                        duplicateUnityPlayerLoopRoots++;
                        addRef( duplicateUnityPlayerLoopRefs, value.ref );
                    }
                    if( value.name == "Main.PlayerLoop" )
                    {
                        if( value.endNs )
                            mainPlayerLoopRoots.push_back( { value.ref, value.threadRef, value.startNs, *value.endNs } );
                        auto parentRef = value.parentRef;
                        size_t parentDepth = 0;
                        while( parentRef && parentDepth++ < 4096 )
                        {
                            const auto parent = source->GetCpuZone( *parentRef );
                            if( !parent ) break;
                            if( parent->name == "Main.PlayerLoop" )
                            {
                                nestedMainPlayerLoopRoots++;
                                addRef( nestedMainPlayerLoopRefs, value.ref );
                                break;
                            }
                            parentRef = parent->parentRef;
                        }
                    }
                }
                offset += values.size();
                if( values.size() < allowed ) break;
            }
            if( !mainPlayerLoopRoots.empty() && duplicateUnityPlayerLoopRoots != 0 )
                addFinding( "error", "DUPLICATE_AUTHORITATIVE_PLAYERLOOP_ROOT",
                    "Unity's cross-frame PlayerLoop marker was mirrored beside the authoritative Main.PlayerLoop root",
                    duplicateUnityPlayerLoopRoots, std::move( duplicateUnityPlayerLoopRefs ) );
            if( nestedMainPlayerLoopRoots != 0 )
                addFinding( "error", "CPU_FRAME_ROOT_NESTED_SAME_SOURCE",
                    "Main.PlayerLoop is nested below another Main.PlayerLoop root",
                    nestedMainPlayerLoopRoots, std::move( nestedMainPlayerLoopRefs ) );
            std::unordered_map<std::string, std::vector<FrameRootInterval>> rootsByThread;
            for( auto& root : mainPlayerLoopRoots ) rootsByThread[root.threadRef].emplace_back( std::move( root ) );
            uint64_t crossingRoots = 0;
            json crossingRootRefs = json::array();
            for( auto& [threadRef, roots] : rootsByThread )
            {
                std::sort( roots.begin(), roots.end(), []( const auto& lhs, const auto& rhs ) {
                    return lhs.startNs != rhs.startNs ? lhs.startNs < rhs.startNs : lhs.ref < rhs.ref;
                } );
                if( roots.empty() ) continue;
                auto longestOpen = roots.front();
                for( size_t rootIndex = 1; rootIndex < roots.size(); rootIndex++ )
                {
                    const auto& current = roots[rootIndex];
                    if( current.startNs < longestOpen.endNs )
                    {
                        crossingRoots++;
                        addRef( crossingRootRefs, current.ref );
                    }
                    if( current.endNs > longestOpen.endNs ) longestOpen = current;
                }
            }
            if( crossingRoots != 0 )
                addFinding( "error", "CPU_FRAME_ROOT_CROSSING",
                    "Main.PlayerLoop overlaps the next authoritative frame root",
                    crossingRoots, std::move( crossingRootRefs ) );
            offset = 0;
            while( true )
            {
                checkCancelled();
                const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
                analysis::ScanRange range; range.offset = offset; range.limit = allowed;
                const auto values = source->ScanGpuZones( range );
                BudgetScanned( values.size(), chunk, allowed );
                for( const auto& value : values )
                {
                    if( value.parentRef && !validEntityRef( *value.parentRef, "gpu-zone", metadata.counts.gpuZones ) ) noteReference( "GPU_ZONE_PARENT", value.ref );
                    if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                    if( !value.contextRef.empty() && gpuContextRefs.find( value.contextRef ) == gpuContextRefs.end() ) noteReference( "GPU_CONTEXT", value.ref );
                    if( !value.sourceLocationRef.empty() && sourceRefs.find( value.sourceLocationRef ) == sourceRefs.end() ) noteReference( "SOURCE_LOCATION", value.ref );
                    if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
                    if( !value.complete ) { incompleteGpu++; addRef( incompleteGpuRefs, value.ref ); }
                    if( ( value.gpuEndNs && *value.gpuEndNs < value.gpuStartNs ) || ( value.cpuEndNs && *value.cpuEndNs < value.cpuStartNs ) ) { invalidGpu++; addRef( invalidGpuRefs, value.ref ); }
                }
                offset += values.size();
                if( values.size() < allowed ) break;
            }
            if( incompleteCpu ) addFinding( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", incompleteCpu, std::move( incompleteCpuRefs ) );
            if( invalidCpu ) addFinding( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", invalidCpu, std::move( invalidCpuRefs ) );
            if( unresolvedCpuNames ) addFinding( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", unresolvedCpuNames, std::move( unresolvedCpuNameRefs ) );
            if( incompleteGpu ) addFinding( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", incompleteGpu, std::move( incompleteGpuRefs ) );
            if( invalidGpu ) addFinding( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", invalidGpu, std::move( invalidGpuRefs ) );
        }
        size_t incompleteFrames = 0, invalidFrames = 0;
        json incompleteFrameRefs = json::array(), invalidFrameRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanFrames( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.frameSetRef.empty() && frameSetRefs.find( value.frameSetRef ) == frameSetRefs.end() ) noteReference( "FRAME_SET", value.ref );
                if( value.imageRef && frameImageRefs.find( *value.imageRef ) == frameImageRefs.end() ) noteReference( "FRAME_IMAGE", value.ref );
                if( !value.complete ) { incompleteFrames++; addRef( incompleteFrameRefs, value.ref ); }
                if( value.endNs && *value.endNs < value.beginNs ) { invalidFrames++; addRef( invalidFrameRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < allowed ) break;
        }
        if( incompleteFrames ) addFinding( "info", "INCOMPLETE_FRAMES", "frame sets contain an open final frame", incompleteFrames, std::move( incompleteFrameRefs ) );
        if( invalidFrames ) addFinding( "error", "INVALID_FRAME_TIMING", "frames end before they begin", invalidFrames, std::move( invalidFrameRefs ) );

        size_t invalidMemory = 0, unresolvedMemoryCallstacks = 0;
        json invalidMemoryRefs = json::array(), memoryCallstackRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanMemoryEvents( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.poolRef.empty() && memoryPoolRefs.find( value.poolRef ) == memoryPoolRefs.end() ) noteReference( "MEMORY_POOL", value.ref );
                if( !value.allocationThreadRef.empty() && threadRefs.find( value.allocationThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.freeThreadRef && threadRefs.find( *value.freeThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.allocationZoneRef && !validEntityRef( *value.allocationZoneRef, "cpu-zone", metadata.counts.cpuZones ) ) noteReference( "CPU_ZONE", value.ref );
                if( value.freeZoneRef && !validEntityRef( *value.freeZoneRef, "cpu-zone", metadata.counts.cpuZones ) ) noteReference( "CPU_ZONE", value.ref );
                if( value.allocationCallstack != 0 ) referencedCallstacks.emplace( value.allocationCallstack );
                if( value.freeCallstack != 0 ) referencedCallstacks.emplace( value.freeCallstack );
                if( value.freeNs && *value.freeNs < value.allocationNs ) { invalidMemory++; addRef( invalidMemoryRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < allowed ) break;
        }
        if( invalidMemory ) addFinding( "error", "INVALID_MEMORY_LIFETIME", "memory events are freed before allocation", invalidMemory, std::move( invalidMemoryRefs ) );
        size_t invalidContextSwitches = 0, invalidWakeups = 0;
        json invalidContextRefs = json::array(), invalidWakeupRefs = json::array();
        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanContextSwitchEvents( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.endNs && *value.endNs < value.startNs ) { invalidContextSwitches++; addRef( invalidContextRefs, value.ref ); }
                if( value.wakeupNs && *value.wakeupNs > value.startNs ) { invalidWakeups++; addRef( invalidWakeupRefs, value.ref ); }
            }
            offset += values.size(); if( values.size() < allowed ) break;
        }
        if( invalidContextSwitches ) addFinding( "error", "INVALID_CONTEXT_SWITCH_TIMING", "context-switch running intervals are reversed", invalidContextSwitches, std::move( invalidContextRefs ) );
        if( invalidWakeups ) addFinding( "warning", "INVALID_WAKEUP_ORDER", "thread wakeup occurs after its running interval begins", invalidWakeups, std::move( invalidWakeupRefs ) );

        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanSampleEvents( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
            }
            offset += values.size(); if( values.size() < allowed ) break;
        }
        if( metadata.samplesInconsistent ) addFinding( "warning", "INCONSISTENT_SAMPLES", "sampling data was marked inconsistent by Worker", 1 );

        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanMessages( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.callstack != 0 ) referencedCallstacks.emplace( value.callstack );
            }
            offset += values.size(); if( values.size() < allowed ) break;
        }
        offset = 0;
        while( true )
        {
            checkCancelled();
            const auto allowed = BudgetScanAllowance( chunk ); if( allowed == 0 ) break;
            analysis::ScanRange range; range.offset = offset; range.limit = allowed;
            const auto values = source->ScanLockEvents( range );
            BudgetScanned( values.size(), chunk, allowed );
            for( const auto& value : values )
            {
                if( !value.lockRef.empty() && lockRefs.find( value.lockRef ) == lockRefs.end() ) noteReference( "LOCK", value.ref );
                if( !value.threadRef.empty() && threadRefs.find( value.threadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                if( value.ownerThreadRef && threadRefs.find( *value.ownerThreadRef ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
                for( const auto& waiter : value.waiterThreadRefs ) if( threadRefs.find( waiter ) == threadRefs.end() ) noteReference( "THREAD", value.ref );
            }
            offset += values.size(); if( values.size() < allowed ) break;
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
            if( indexedZoneValidation )
            {
                addFinding( "info", "INDEXED_GTMEM_DEEP_VALIDATION_SEPARATE", "indexed validation avoids materializing the complete GPU-memory attribution graph; use memory.gpu.attribution and producer quality for the dedicated deep check", 0 );
            }
            else
            {
                const auto attributionValue = CachedGpuAttribution( trace.id, source );
                const auto& attribution = *attributionValue;
                if( attribution.protocolPresent )
                {
                    size_t incompletePasses = 0, missingGpu = 0, ambiguousGpu = 0;
                    json incompleteRefs = json::array(), missingRefs = json::array(), ambiguousRefs = json::array();
                    for( const auto& pass : attribution.passes )
                    {
                        const auto ref = source->MakeEntityRef( "gpu-memory-pass", pass.passId );
                        if( !pass.complete && pass.gpuPairing != analysis::GpuZonePairing::CaptureBoundary )
                        {
                            incompletePasses++;
                            addRef( incompleteRefs, ref );
                        }
                        if( pass.gpuPairing == analysis::GpuZonePairing::Missing ) { missingGpu++; addRef( missingRefs, ref ); }
                        if( pass.gpuPairing == analysis::GpuZonePairing::Ambiguous ) { ambiguousGpu++; addRef( ambiguousRefs, ref ); }
                    }
                    if( incompletePasses ) addFinding( "warning", "INCOMPLETE_GTMEM_PASS", "GTMEM1 pass payload chunks or use counts are incomplete", incompletePasses, std::move( incompleteRefs ) );
                    if( missingGpu ) addFinding( "warning", "MISSING_GTMEM_GPU_ZONE", "GTMEM1 passes have no matching GPU zone", missingGpu, std::move( missingRefs ) );
                    if( ambiguousGpu ) addFinding( "warning", "AMBIGUOUS_GTMEM_GPU_ZONE", "GTMEM1 passes match more than one GPU zone", ambiguousGpu, std::move( ambiguousRefs ) );
                    if( attribution.unknownUseOccurrences )
                    {
                        json refs = json::array();
                        for( const auto& value : attribution.unknownUses )
                        {
                            addRef( refs, source->MakeEntityRef( "gpu-memory-pass", value.firstPassId ) );
                            if( refs.size() >= DefaultPageSize ) break;
                        }
                        addFinding( "warning", "UNKNOWN_GTMEM_ALLOCATION",
                            "GPU pass resource uses are not fully resolvable through a live logical registration and physical backing; inspect memory.gpu.attribution unknown_uses for reason-coded unique resources",
                            attribution.unknownUseOccurrences, std::move( refs ) );
                    }
                }
            }
        }

        for( const auto& capability : source->GetCapabilities() ) if( !capability.present )
        {
            findings.push_back( { { "severity", "info" }, { "code", "CAPABILITY_ABSENT" }, { "message", capability.reason.empty() ? capability.domain + " is absent" : capability.reason },
                { "count", "0" }, { "refs", json::array() }, { "domain", capability.domain } } );
        }
        if( BudgetPartial() ) addFinding( "info", "VALIDATION_PARTIAL", "validation stopped at the caller-supplied query budget; rerun with a larger budget for a complete verdict", 1 );
        const auto errors = std::count_if( findings.begin(), findings.end(), []( const auto& finding ) { return finding.value( "severity", "" ) == "error"; } );
        return Success( id, {
            { "valid", errors == 0 }, { "complete", !BudgetPartial() && trace.complete }, { "error_count", errors }, { "finding_count", findings.size() }, { "findings", std::move( findings ) },
            { "checks", { "worker_load", "stream_completeness", "cpu_zone_timing", "cpu_frame_root_crossing", "cpu_frame_root_nesting", "duplicate_authoritative_root", "gpu_zone_timing", "zone_parent_references", "frame_boundaries", "frame_image_references", "memory_lifetimes", "entity_references", "callstack_references", "symbol_references", "context_switch_timing", "sample_consistency", "gtmem1_protocol", "gpu_pass_pairing", "capability_presence" } }
        }, trace );
    }

    throw QueryError( "METHOD_NOT_FOUND", "unknown method: " + method );
}

std::string DumpProtocolJson( const json& value )
{
    return value.dump( -1, ' ', false, json::error_handler_t::replace );
}

}
