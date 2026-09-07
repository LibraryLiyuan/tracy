#include "TracyBoundedScanCursor.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>

namespace tracy::analysis
{
namespace
{

constexpr uint64_t FnvOffset = 1469598103934665603ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

void HashBytes( uint64_t& hash, const void* data, size_t size )
{
    const auto* bytes = static_cast<const uint8_t*>( data );
    for( size_t i = 0; i < size; ++i )
    {
        hash ^= bytes[i];
        hash *= FnvPrime;
    }
}

template<typename T>
void HashValue( uint64_t& hash, const T& value )
{
    HashBytes( hash, &value, sizeof( value ) );
}

std::string CursorToken( const BoundedScanCursor& cursor )
{
    uint64_t hash = FnvOffset;
    constexpr std::string_view prefix = "JNTracyBoundedScanCursorV1";
    HashBytes( hash, prefix.data(), prefix.size() );
    HashValue( hash, cursor.schemaVersion );
    HashValue( hash, cursor.domain );
    HashValue( hash, cursor.sourceKind );
    HashValue( hash, cursor.sourceRevision );
    HashValue( hash, cursor.startNs );
    HashValue( hash, cursor.endNs );
    HashValue( hash, cursor.ordinal );
    HashValue( hash, cursor.lastTimeNs );
    HashBytes( hash, cursor.lastStableEntity.data(), cursor.lastStableEntity.size() );
    HashValue( hash, cursor.complete );
    std::ostringstream out;
    out << std::hex << std::setfill( '0' ) << std::setw( 16 ) << hash;
    return out.str();
}

template<typename T>
size_t SizeOf( const T& values ) { return values.size(); }
size_t SizeOf( const std::monostate& ) { return 0; }

template<typename T>
std::string StableRef( const T& value ) { return value.ref; }
std::string StableRef( const GpuAnalysisResourceSummary& value )
{ return "gpu-resource:" + std::to_string( value.resourceId ); }
std::string StableRef( const GpuAllocationAnalysisRecord& value )
{ return "gpu-allocation:" + std::to_string( value.allocationId ); }
std::string StableRef( const GpuPassWorkingSet& value )
{ return "gpu-pass:" + std::to_string( value.passId ); }
std::string StableRef( const GpuAnalysisRangeStoreEntry& value )
{ return "gpu-range:" + std::to_string( value.record.passInstanceId ) + ':' + std::to_string( value.resourceId ); }

int64_t EventTime( const FrameDto& value ) { return value.beginNs; }
int64_t EventTime( const CpuZoneDto& value ) { return value.startNs; }
int64_t EventTime( const GpuZoneDto& value ) { return value.gpuStartNs; }
int64_t EventTime( const JobDto& value ) { return value.scheduleNs; }
int64_t EventTime( const RelationDto& value ) { return value.timeNs; }
int64_t EventTime( const MemoryEventDto& value ) { return value.allocationNs; }
int64_t EventTime( const SampleDto& value ) { return value.timeNs; }
int64_t EventTime( const ContextSwitchDto& value ) { return value.startNs; }
int64_t EventTime( const RuntimeDomainStateDto& value ) { return value.timeNs; }
int64_t EventTime( const ScriptFrameDto& value ) { return value.timeNs; }
int64_t EventTime( const ScriptStackEventDto& value ) { return value.timeNs; }
int64_t EventTime( const IoRequestDto& value ) { return value.queueNs; }
int64_t EventTime( const GfxDispatchDto& value ) { return value.timeNs; }
int64_t EventTime( const GfxEntityDto& value ) { return value.timeNs; }
int64_t EventTime( const GfxLinkDto& value ) { return value.timeNs; }
int64_t EventTime( const MessageDto& value ) { return value.timeNs; }
int64_t EventTime( const PlotPointDto& value ) { return value.timeNs; }
int64_t EventTime( const LockEventDto& value ) { return value.timeNs; }
int64_t EventTime( const CpuUsagePointDto& value ) { return value.timeNs; }
int64_t EventTime( const CallsiteDto& ) { return 0; }
int64_t EventTime( const GpuAnalysisResourceSummary& value ) { return int64_t( value.createTime ); }
int64_t EventTime( const GpuAllocationAnalysisRecord& value ) { return int64_t( value.createTime ); }
int64_t EventTime( const GpuPassWorkingSet& value ) { return value.startNs; }
int64_t EventTime( const GpuAnalysisRangeStoreEntry& ) { return 0; }

template<typename T>
void UpdateTail( const std::vector<T>& values, BoundedScanCursor& cursor )
{
    if( values.empty() ) return;
    cursor.lastTimeNs = EventTime( values.back() );
    cursor.lastStableEntity = StableRef( values.back() );
}
void UpdateTail( const std::monostate&, BoundedScanCursor& ) {}

ScanRange RangeFor( const BoundedScanCursor& cursor, size_t limit )
{
    return { cursor.startNs, cursor.endNs, size_t( cursor.ordinal ), limit };
}

}

size_t BoundedScanBatch::Count() const
{
    return std::visit( []( const auto& values ) { return SizeOf( values ); }, records );
}

BoundedTraceScanner::BoundedTraceScanner( const TraceSource& source )
    : m_source( source )
    , m_bounded( [&]() -> const NativeBoundedTraceSource& {
        const auto* bounded = dynamic_cast<const NativeBoundedTraceSource*>( &source );
        if( !bounded || bounded->NativeBoundedScanVersion() != NativeBoundedScanSchemaVersion )
            throw BoundedScanError( "trace_source_has_no_native_bounded_scan_contract" );
        return *bounded;
    }() )
    , m_gpu( dynamic_cast<const GpuCatalogBoundedScanSource*>( &source ) )
{}

BoundedScanBatch BoundedTraceScanner::Read( const BoundedScanRequest& request ) const
{
    if( request.limit == 0 || request.limit > NativeBoundedScanMaximumBatch )
        throw BoundedScanError( "bounded_scan_limit_out_of_range" );
    if( request.startNs >= request.endNs )
        throw BoundedScanError( "bounded_scan_time_range_invalid" );

    const auto view = m_source.AcquireReadView();
    if( view.state != TraceSourceState::Ready )
        throw BoundedScanError( "trace_source_not_ready" );
    if( !view.complete )
        throw BoundedScanError( "trace_revision_not_complete" );

    BoundedScanCursor cursor;
    if( request.cursor )
    {
        cursor = *request.cursor;
        if( cursor.schemaVersion != NativeBoundedScanSchemaVersion ||
            cursor.domain != request.domain || cursor.sourceKind != view.sourceKind ||
            cursor.sourceRevision != view.revision || cursor.startNs != request.startNs ||
            cursor.endNs != request.endNs || cursor.resumeToken != CursorToken( cursor ) )
            throw BoundedScanError( "bounded_scan_cursor_mismatch" );
        if( cursor.complete ) return { std::move( cursor ), std::monostate {} };
    }
    else
    {
        cursor.domain = request.domain;
        cursor.sourceKind = view.sourceKind;
        cursor.sourceRevision = view.revision;
        cursor.startNs = request.startNs;
        cursor.endNs = request.endNs;
    }

    const auto offset = size_t( cursor.ordinal );
    BoundedScanRecords records;
    switch( request.domain )
    {
    case BoundedScanDomain::Frame: records = m_source.ScanFrames( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::CpuZone: records = m_source.ScanCpuZones( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::GpuZone: records = m_source.ScanGpuZones( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::Job: records = m_source.ScanJobs( offset, request.limit ); break;
    case BoundedScanDomain::Relation: records = m_source.ScanRelations( offset, request.limit ); break;
    case BoundedScanDomain::Memory: records = m_source.ScanMemoryEvents( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::Sample: records = m_source.ScanSampleEvents( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::ContextSwitch: records = m_source.ScanContextSwitchEvents( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::RuntimeDomain: records = m_source.ScanRuntimeDomainStates( offset, request.limit ); break;
    case BoundedScanDomain::ScriptFrame: records = m_source.ScanScriptFrames( offset, request.limit ); break;
    case BoundedScanDomain::ScriptStack: records = m_source.ScanScriptStackEvents( offset, request.limit ); break;
    case BoundedScanDomain::IoRequest: records = m_source.ScanIoRequests( offset, request.limit ); break;
    case BoundedScanDomain::GfxDispatch: records = m_source.ScanGfxDispatches( offset, request.limit ); break;
    case BoundedScanDomain::GfxEntity: records = m_source.ScanGfxEntities( offset, request.limit ); break;
    case BoundedScanDomain::GfxLink: records = m_source.ScanGfxLinks( offset, request.limit ); break;
    case BoundedScanDomain::Message: records = m_source.ScanMessages( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::Plot: records = m_source.ScanPlots( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::LockEvent: records = m_source.ScanLockEvents( RangeFor( cursor, request.limit ) ); break;
    case BoundedScanDomain::CpuUsage: records = m_source.ScanCpuUsage( offset, request.limit ); break;
    case BoundedScanDomain::Callsite: records = m_source.ScanCallsites( offset, request.limit ); break;
    case BoundedScanDomain::GpuResource:
        if( !m_gpu ) throw BoundedScanError( "gpu_catalog_bounded_scan_unavailable" );
        records = m_gpu->ScanGpuCatalogResourcesBounded( offset, request.limit ); break;
    case BoundedScanDomain::GpuAllocation:
        if( !m_gpu ) throw BoundedScanError( "gpu_catalog_bounded_scan_unavailable" );
        records = m_gpu->ScanGpuCatalogAllocationsBounded( offset, request.limit ); break;
    case BoundedScanDomain::GpuPass:
        if( !m_gpu ) throw BoundedScanError( "gpu_catalog_bounded_scan_unavailable" );
        records = m_gpu->ScanGpuCatalogPassesBounded( offset, request.limit ); break;
    case BoundedScanDomain::GpuRange:
        if( !m_gpu ) throw BoundedScanError( "gpu_catalog_bounded_scan_unavailable" );
        records = m_gpu->ScanGpuCatalogRangesBounded( offset, request.limit ); break;
    }

    const auto count = std::visit( []( const auto& values ) { return SizeOf( values ); }, records );
    if( count > request.limit ) throw BoundedScanError( "bounded_scan_source_exceeded_batch" );
    std::visit( [&]( const auto& values ) { UpdateTail( values, cursor ); }, records );
    cursor.ordinal += count;
    cursor.complete = count < request.limit;
    cursor.resumeToken = CursorToken( cursor );
    return { std::move( cursor ), std::move( records ) };
}

const char* BoundedScanDomainName( BoundedScanDomain domain )
{
    switch( domain )
    {
    case BoundedScanDomain::Frame: return "frame";
    case BoundedScanDomain::CpuZone: return "cpu_zone";
    case BoundedScanDomain::GpuZone: return "gpu_zone";
    case BoundedScanDomain::Job: return "job";
    case BoundedScanDomain::Relation: return "relation";
    case BoundedScanDomain::Memory: return "memory";
    case BoundedScanDomain::Sample: return "sample";
    case BoundedScanDomain::ContextSwitch: return "context_switch";
    case BoundedScanDomain::RuntimeDomain: return "runtime_domain";
    case BoundedScanDomain::ScriptFrame: return "script_frame";
    case BoundedScanDomain::ScriptStack: return "script_stack";
    case BoundedScanDomain::IoRequest: return "io_request";
    case BoundedScanDomain::GfxDispatch: return "gfx_dispatch";
    case BoundedScanDomain::GfxEntity: return "gfx_entity";
    case BoundedScanDomain::GfxLink: return "gfx_link";
    case BoundedScanDomain::Message: return "message";
    case BoundedScanDomain::Plot: return "plot";
    case BoundedScanDomain::LockEvent: return "lock_event";
    case BoundedScanDomain::CpuUsage: return "cpu_usage";
    case BoundedScanDomain::Callsite: return "callsite";
    case BoundedScanDomain::GpuResource: return "gpu_resource";
    case BoundedScanDomain::GpuAllocation: return "gpu_allocation";
    case BoundedScanDomain::GpuPass: return "gpu_pass";
    case BoundedScanDomain::GpuRange: return "gpu_range";
    }
    return "unknown";
}

}
