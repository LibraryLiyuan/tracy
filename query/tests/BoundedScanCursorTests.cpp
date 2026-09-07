#include "TracyBoundedScanCursor.hpp"
#include "FakeTraceSource.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>

using namespace tracy::analysis;

namespace
{

template<typename T>
std::vector<T> Slice( std::vector<T> values, size_t offset, size_t limit )
{
    const auto begin = std::min( offset, values.size() );
    const auto end = begin + std::min( limit, values.size() - begin );
    return { values.begin() + begin, values.begin() + end };
}

class StrictBoundedSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource, public GpuCatalogBoundedScanSource
{
public:
    explicit StrictBoundedSource( TraceSourceKind kind, bool complete = true )
        : m_kind( kind ), m_complete( complete )
    {}

    TraceReadView AcquireReadView() const override
    {
        return { m_kind, state, revision, 100, m_complete };
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }

    std::vector<JobDto> GetJobs() const override
    {
        ++fullMaterializationCalls;
        return FakeTraceSource::GetJobs();
    }
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        Observe( limit );
        return Slice( FakeTraceSource::GetJobs(), offset, limit );
    }

    std::vector<IoRequestDto> GetIoRequests() const override
    {
        ++fullMaterializationCalls;
        return FakeTraceSource::GetIoRequests();
    }
    std::vector<IoRequestDto> ScanIoRequests( size_t offset, size_t limit ) const override
    {
        Observe( limit );
        return Slice( FakeTraceSource::GetIoRequests(), offset, limit );
    }

    std::vector<GfxDispatchDto> GetGfxDispatches() const override
    {
        ++fullMaterializationCalls;
        return FakeTraceSource::GetGfxDispatches();
    }
    std::vector<GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const override
    {
        Observe( limit );
        return Slice( FakeTraceSource::GetGfxDispatches(), offset, limit );
    }

    std::vector<RelationDto> GetRelations() const override
    {
        ++fullMaterializationCalls;
        return FakeTraceSource::GetRelations();
    }
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        Observe( limit );
        return Slice( FakeTraceSource::GetRelations(), offset, limit );
    }

    uint64_t GetGpuCatalogResourceCountBounded() const override { return 3; }
    uint64_t GetGpuCatalogAllocationCountBounded() const override { return 1; }
    uint64_t GetGpuCatalogPassCountBounded() const override { return 1; }
    uint64_t GetGpuCatalogRangeCountBounded() const override { return 1; }

    std::vector<GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded(
        size_t offset, size_t limit ) const override
    {
        Observe( limit );
        std::vector<GpuAnalysisResourceSummary> values( 3 );
        for( size_t i = 0; i < values.size(); ++i )
        {
            values[i].resourceId = 100 + i;
            values[i].createTime = 10 + i;
            values[i].name = "Resource" + std::to_string( i );
        }
        return Slice( std::move( values ), offset, limit );
    }
    std::vector<GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded(
        size_t offset, size_t limit ) const override
    {
        Observe( limit );
        GpuAllocationAnalysisRecord value;
        value.allocationId = 200;
        value.createTime = 20;
        return Slice( std::vector<GpuAllocationAnalysisRecord> { value }, offset, limit );
    }
    std::vector<GpuPassWorkingSet> ScanGpuCatalogPassesBounded(
        size_t offset, size_t limit ) const override
    {
        Observe( limit );
        GpuPassWorkingSet value;
        value.passId = 300;
        value.startNs = 30;
        value.endNs = 40;
        return Slice( std::vector<GpuPassWorkingSet> { value }, offset, limit );
    }
    std::vector<GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded(
        size_t offset, size_t limit ) const override
    {
        Observe( limit );
        GpuAnalysisRangeStoreEntry value;
        value.resourceId = 100;
        value.record.passInstanceId = 300;
        return Slice( std::vector<GpuAnalysisRangeStoreEntry> { value }, offset, limit );
    }

    void Observe( size_t limit ) const
    {
        ++boundedCalls;
        maxRequestedBatch = std::max( maxRequestedBatch, limit );
    }

    mutable size_t fullMaterializationCalls = 0;
    mutable size_t boundedCalls = 0;
    mutable size_t maxRequestedBatch = 0;
    mutable uint64_t revision = 73;
    mutable TraceSourceState state = TraceSourceState::Ready;

private:
    TraceSourceKind m_kind;
    bool m_complete = true;
};

void VerifySource( TraceSourceKind kind )
{
    StrictBoundedSource source( kind );
    BoundedTraceScanner scanner( source );

    BoundedScanRequest request;
    request.domain = BoundedScanDomain::GpuResource;
    request.limit = 2;
    auto first = scanner.Read( request );
    assert( first.Count() == 2 );
    assert( !first.cursor.complete );
    assert( first.cursor.ordinal == 2 );
    assert( first.cursor.sourceRevision == 73 );
    assert( first.cursor.lastStableEntity == "gpu-resource:101" );
    assert( !first.cursor.resumeToken.empty() );

    request.cursor = first.cursor;
    auto second = scanner.Read( request );
    assert( second.Count() == 1 );
    assert( second.cursor.complete );
    assert( second.cursor.ordinal == 3 );
    const auto& resources = std::get<std::vector<GpuAnalysisResourceSummary>>( second.records );
    assert( resources.front().resourceId == 102 );

    request.domain = BoundedScanDomain::Job;
    request.cursor.reset();
    request.limit = 1;
    auto jobs0 = scanner.Read( request );
    assert( jobs0.Count() == 1 && !jobs0.cursor.complete );
    request.cursor = jobs0.cursor;
    auto jobs1 = scanner.Read( request );
    assert( jobs1.Count() == 1 && !jobs1.cursor.complete );
    request.cursor = jobs1.cursor;
    auto jobsEnd = scanner.Read( request );
    assert( jobsEnd.Count() == 0 && jobsEnd.cursor.complete );

    assert( source.fullMaterializationCalls == 0 );
    assert( source.boundedCalls >= 5 );
    assert( source.maxRequestedBatch <= 2 );

    auto tampered = first.cursor;
    tampered.ordinal = 1;
    request.domain = BoundedScanDomain::GpuResource;
    request.limit = 2;
    request.cursor = tampered;
    bool rejected = false;
    try { (void)scanner.Read( request ); }
    catch( const BoundedScanError& ) { rejected = true; }
    assert( rejected );

    request.cursor = first.cursor;
    source.revision++;
    rejected = false;
    try { (void)scanner.Read( request ); }
    catch( const BoundedScanError& ) { rejected = true; }
    assert( rejected );

}

}

int main()
{
    static_assert( NativeBoundedScanSchemaVersion == 1 );
    VerifySource( TraceSourceKind::Snapshot );
    VerifySource( TraceSourceKind::Segment );

    tracy::query::test::FakeTraceSource unsupported;
    bool rejected = false;
    try { BoundedTraceScanner scanner( unsupported ); }
    catch( const BoundedScanError& ) { rejected = true; }
    assert( rejected );

    StrictBoundedSource buildingSession( TraceSourceKind::Segment, false );
    BoundedTraceScanner buildingScanner( buildingSession );
    rejected = false;
    try { (void)buildingScanner.Read( {} ); }
    catch( const BoundedScanError& ) { rejected = true; }
    assert( rejected );

    StrictBoundedSource loadingSnapshot( TraceSourceKind::Snapshot );
    loadingSnapshot.state = TraceSourceState::Loading;
    BoundedTraceScanner loadingScanner( loadingSnapshot );
    rejected = false;
    try { (void)loadingScanner.Read( {} ); }
    catch( const BoundedScanError& ) { rejected = true; }
    assert( rejected );
    return 0;
}
