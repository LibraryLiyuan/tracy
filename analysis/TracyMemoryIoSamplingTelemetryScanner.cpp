#include "TracyMemoryIoSamplingTelemetryScanner.hpp"

#include "TracyQueue.hpp"
#include "TracyAnalysisDictionary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace tracy::analysis
{
namespace
{

using json = nlohmann::json;
constexpr size_t RepresentativeLimit = 8;

void CheckCancelled( const std::function<bool()>& cancelled )
{
    if( cancelled && cancelled() ) throw BoundedScanError( "memory_io_sampling_scan_cancelled" );
}

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char c ) {
        return char( std::tolower( c ) );
    } );
    return value;
}

bool Contains( const std::string& value, std::string_view needle )
{
    return value.find( needle ) != std::string::npos;
}

std::string ThreadRole( const std::string& name )
{
    const auto lower = Lower( name );
    if( Contains( lower, "main" ) ) return "Main";
    if( Contains( lower, "render" ) || Contains( lower, "gfxdevice" ) ) return "Render";
    if( Contains( lower, "job.worker" ) || Contains( lower, "job worker" ) ) return "JobWorker";
    if( Contains( lower, "submission" ) ) return "Submission";
    if( Contains( lower, "loading" ) || Contains( lower, "async read" ) ) return "Loading";
    if( Contains( lower, "audio" ) ) return "Audio";
    return name.empty() ? "Unknown" : name;
}

const Capability* FindCapability( const std::vector<Capability>& capabilities, std::string_view domain )
{
    const auto found = std::find_if( capabilities.begin(), capabilities.end(),
        [&]( const auto& value ) { return value.domain == domain; } );
    return found == capabilities.end() ? nullptr : &*found;
}

uint64_t TextBytes( const std::optional<std::string>& text ) { return text ? text->size() : 0; }
uint64_t RecordBytes( const MemoryEventDto& value )
{
    return 2048 + 4 * ( value.ref.size() + value.poolRef.size() + value.address.size() +
        value.allocationThreadRef.size() + TextBytes( value.freeThreadRef ) +
        TextBytes( value.allocationCallstackRef ) + TextBytes( value.freeCallstackRef ) +
        TextBytes( value.allocationZoneRef ) + TextBytes( value.freeZoneRef ) );
}
uint64_t RecordBytes( const GpuAllocationAnalysisRecord& value )
{ return 1024 + 32 * ( value.history.size() + value.resources.size() ); }
uint64_t RecordBytes( const GpuAnalysisResourceSummary& value )
{ return 1024 + 4 * value.name.size(); }
uint64_t RecordBytes( const GpuPassWorkingSet& value )
{
    return 1024 + 4 * value.name.size() + 32 * ( value.directResources.size() + value.inclusiveResources.size() ) +
        4 * sizeof( GpuDetailedEvidenceAnalysisRecord ) * value.detailedEvidence.size();
}
uint64_t RecordBytes( const GpuAnalysisRangeStoreEntry& ) { return 1024; }
uint64_t RecordBytes( const IoRequestDto& value )
{
    uint64_t bytes = 1024 + 4 * ( value.ref.size() + value.queueThreadRef.size() );
    for( const auto& stage : value.stages ) bytes += 256 + 4 * stage.threadRef.size();
    return bytes;
}
uint64_t RecordBytes( const SampleDto& value )
{ return 1024 + 4 * ( value.ref.size() + value.threadRef.size() + value.kind.size() + TextBytes( value.callstackRef ) ); }
uint64_t RecordBytes( const ContextSwitchDto& value )
{
    return 2048 + 4 * ( value.ref.size() + value.threadRef.size() + value.reasonName.size() +
        value.stateName.size() + TextBytes( value.relatedThreadRef ) + value.wakeupCpuAvailability.reason.size() );
}
uint64_t RecordBytes( const CallstackFrameDto& value )
{
    return 1024 + 4 * ( value.ref.size() + value.name.size() + value.file.size() + value.address.size() +
        value.symbolAddress.size() + TextBytes( value.imageName ) );
}

GpuCatalogState CatalogState( const Capability* capability )
{
    if( capability == nullptr ) return GpuCatalogState::Absent;
    const auto reason = Lower( capability->reason );
    if( Contains( reason, "disabled" ) ) return GpuCatalogState::Disabled;
    if( capability->present && ( !capability->queryable || Contains( reason, "invalid" ) ) )
        return GpuCatalogState::Invalid;
    if( capability->present && capability->queryable ) return GpuCatalogState::Complete;
    return GpuCatalogState::Absent;
}

template<typename T, typename Visitor>
void VisitAll( const BoundedTraceScanner& scanner, BoundedScanDomain domain,
    size_t batchSize, size_t& maximumBatch, const std::function<bool()>& cancelled,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace, Visitor&& visitor )
{
    BoundedScanRequest request;
    request.domain = domain;
    request.limit = batchSize;
    for( ;; )
    {
        CheckCancelled( cancelled );
        // Source DTO sizes are only known after NativeBoundedScan returns.
        // Account for the complete live page before analysis copies/retention;
        // source-side allocation is separately covered by the process sampler,
        // not an absolute allocator cap.
        AnalysisWorkspaceReservation pageWorkspace( workspace );
        const auto batch = scanner.Read( request );
        CheckCancelled( cancelled );
        maximumBatch = std::max( maximumBatch, batch.Count() );
        const auto& values = std::get<std::vector<T>>( batch.records );
        for( const auto& value : values ) { CheckCancelled( cancelled ); pageWorkspace.Add( RecordBytes( value ) ); }
        for( const auto& value : values )
        {
            CheckCancelled( cancelled );
            visitor( value );
        }
        CheckCancelled( cancelled );
        if( batch.cursor.complete ) break;
        request.cursor = batch.cursor;
    }
}

void AddRepresentative( MemoryIoSamplingQualityFinding& finding, std::string_view ref )
{
    if( finding.representativeRefs.size() < RepresentativeLimit )
        finding.representativeRefs.emplace_back( ref );
}

uint64_t SaturatingAdd( uint64_t lhs, uint64_t rhs )
{
    return rhs > std::numeric_limits<uint64_t>::max() - lhs ?
        std::numeric_limits<uint64_t>::max() : lhs + rhs;
}

std::optional<uint64_t> JsonUnsigned( const json& value )
{
    if( value.is_number_unsigned() ) return value.get<uint64_t>();
    if( value.is_number_integer() )
    {
        const auto parsed = value.get<int64_t>();
        return parsed < 0 ? std::nullopt : std::optional<uint64_t>( uint64_t( parsed ) );
    }
    if( value.is_string() )
    {
        const auto text = value.get<std::string>();
        uint64_t parsed = 0;
        const auto result = std::from_chars( text.data(), text.data() + text.size(), parsed );
        if( result.ec == std::errc() && result.ptr == text.data() + text.size() ) return parsed;
    }
    return std::nullopt;
}

uint64_t Counter( const json& producer, const char* name )
{
    if( !producer.contains( "counters" ) || !producer["counters"].is_object() ||
        !producer["counters"].contains( name ) ) return 0;
    return JsonUnsigned( producer["counters"][name] ).value_or( 0 );
}

struct ProducerSnapshot
{
    uint64_t sequence = 0;
    json producer;
};

void AddEvidenceClass( std::vector<TelemetryEvidenceClass>& values, TelemetryEvidenceClass value )
{
    if( std::find( values.begin(), values.end(), value ) == values.end() ) values.emplace_back( value );
}

IoLifecycleState IoState( const IoRequestDto& request, const std::function<bool()>& cancelled )
{
    const auto hasStage = [&]( JnIoStage stage ) {
        return std::any_of( request.stages.begin(), request.stages.end(), [&]( const auto& value ) {
            CheckCancelled( cancelled );
            return value.stage == uint8_t( stage );
        } );
    };
    if( request.orphan ) return IoLifecycleState::Orphan;
    if( request.status == uint8_t( JnIoStatus::Cancelled ) || hasStage( JnIoStage::Cancel ) )
        return IoLifecycleState::Cancelled;
    if( request.status == uint8_t( JnIoStatus::Failure ) ||
        request.status == uint8_t( JnIoStatus::Truncated ) || hasStage( JnIoStage::Error ) )
        return IoLifecycleState::Error;
    if( request.status == uint8_t( JnIoStatus::Success ) && request.endNs && request.terminalCount == 1 )
        return IoLifecycleState::Complete;
    return IoLifecycleState::Incomplete;
}

}

MemoryIoSamplingTelemetryScanner::MemoryIoSamplingTelemetryScanner( const TraceSource& source,
    size_t batchSize, int64_t transientLifetimeNs, int64_t leakCandidateAgeNs )
    : m_source( source )
    , m_batchSize( batchSize )
    , m_transientLifetimeNs( transientLifetimeNs )
    , m_leakCandidateAgeNs( leakCandidateAgeNs )
{
    if( batchSize == 0 || batchSize > NativeBoundedScanMaximumBatch )
        throw BoundedScanError( "memory_io_sampling_scan_batch_out_of_range" );
    if( transientLifetimeNs < 0 || leakCandidateAgeNs < transientLifetimeNs )
        throw BoundedScanError( "memory_io_sampling_scan_threshold_invalid" );
}

MemoryIoSamplingTelemetryScanResult MemoryIoSamplingTelemetryScanner::Scan( const std::function<bool()>& cancelled ) const
{
    return ScanImpl( cancelled, true, {} );
}

MemoryIoSamplingTelemetryScanResult MemoryIoSamplingTelemetryScanner::ScanSummary(
    const std::function<bool()>& cancelled, std::shared_ptr<AnalysisWorkspaceBudget> workspace ) const
{
    return ScanImpl( cancelled, false, std::move( workspace ) );
}

MemoryIoSamplingTelemetryScanResult MemoryIoSamplingTelemetryScanner::ScanImpl(
    const std::function<bool()>& cancelled, bool retainDetails, std::shared_ptr<AnalysisWorkspaceBudget> workspace ) const
{
    const auto checkCancelled = [&] { CheckCancelled( cancelled ); };
    checkCancelled();
    MemoryIoSamplingTelemetryScanResult result;
    result.outputWorkspace = AnalysisWorkspaceReservation( workspace, 8192 );
    AnalysisWorkspaceReservation metadataWorkspace( workspace );
    BoundedTraceScanner scanner( m_source );
    const auto info = m_source.GetTraceInfo();
    checkCancelled();
    metadataWorkspace.Add( 2048 + 4 * ( info.fingerprint.size() + info.captureName.size() +
        info.captureProgram.size() + info.hostInfo.size() + info.cpuManufacturer.size() +
        info.cpuArchitecture.size() + info.legacyQueueDelayAvailability.reason.size() ) );
    for( const auto& text : info.appInfo ) metadataWorkspace.Add( 256 + 4 * text.size() );
    const auto capabilities = m_source.GetCapabilities();
    checkCancelled();
    for( const auto& capability : capabilities )
    {
        metadataWorkspace.Add( 1024 + 8 * ( capability.domain.size() + capability.reason.size() ) );
        for( const auto& method : capability.methods ) metadataWorkspace.Add( 128 + 4 * method.size() );
    }
    const auto pools = m_source.GetMemoryPools();
    checkCancelled();
    for( const auto& pool : pools ) metadataWorkspace.Add( 2048 + 8 * ( pool.ref.size() + pool.name.size() + pool.storedName.size() ) );
    const auto threads = m_source.GetThreads();
    checkCancelled();

    std::unordered_map<std::string, std::string> roleByThread;
    for( const auto& thread : threads )
    {
        checkCancelled();
        metadataWorkspace.Add( 2048 + 8 * ( thread.ref.size() + thread.name.size() + TextBytes( thread.externalProcessName ) +
            TextBytes( thread.externalThreadName ) + TextBytes( thread.localName ) + thread.groupHintAvailability.reason.size() ) );
        roleByThread.emplace( thread.ref, ThreadRole( thread.name ) );
    }
    const auto role = [&]( const std::string& ref ) {
        const auto found = roleByThread.find( ref );
        return found == roleByThread.end() ? std::string( "Unknown" ) : found->second;
    };

    // CPU allocation facts. D3D12 named pools are handled independently below
    // and are never mixed into CPU live-byte or leak classifications.
    std::unordered_map<std::string, size_t> cpuPoolIndex;
    for( const auto& pool : pools )
    {
        checkCancelled();
        if( pool.gpuD3D12 ) continue;
        result.outputWorkspace.Add( 2048 + 4 * ( pool.ref.size() + pool.name.size() ) );
        cpuPoolIndex.emplace( pool.ref, result.cpuMemoryPools.size() );
        CpuMemoryPoolFact fact;
        fact.poolRef = pool.ref; fact.name = pool.name; fact.hasCapacity = true;
        fact.classifications.emplace_back( CpuMemoryClassification::Capacity );
        result.cpuMemoryPools.emplace_back( std::move( fact ) );
    }
    {
    AnalysisWorkspaceReservation memoryWorkspace( workspace );
    AnalysisDictionary memoryDictionary( workspace );
    struct MemoryLife
    {
        AnalysisDictionary::StringId ref, address;
        uint64_t size;
        int64_t allocationNs;
        std::optional<int64_t> freeNs;
    };
    std::vector<std::vector<MemoryLife>> memoryByPool( result.cpuMemoryPools.size() );
    VisitAll<MemoryEventDto>( scanner, BoundedScanDomain::Memory,
        m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const MemoryEventDto& event ) {
        checkCancelled();
        result.inputMemoryEventCount++;
        const auto found = cpuPoolIndex.find( event.poolRef );
        if( found != cpuPoolIndex.end() )
        {
            // Keep only exact lifetime state; callstacks, zone refs and thread
            // payloads have no role in the capacity/quality formulas below.
            memoryWorkspace.Add( 256 ); // vector capacity and old/new growth overlap
            const auto ref = memoryDictionary.Intern( event.ref );
            const auto address = memoryDictionary.Intern( event.address );
            memoryByPool[found->second].push_back( { ref, address, event.size, event.allocationNs, event.freeNs } );
        }
    } );
    for( size_t poolIndex = 0; poolIndex < result.cpuMemoryPools.size(); ++poolIndex )
    {
        checkCancelled();
        auto& fact = result.cpuMemoryPools[poolIndex];
        auto& events = memoryByPool[poolIndex];
        std::sort( events.begin(), events.end(), [&]( const auto& lhs, const auto& rhs ) {
            checkCancelled();
            return lhs.allocationNs != rhs.allocationNs ? lhs.allocationNs < rhs.allocationNs :
                memoryDictionary.Text( lhs.ref ) < memoryDictionary.Text( rhs.ref );
        } );
        struct AddressLife { std::optional<int64_t> freeNs; };
        AnalysisWorkspaceReservation poolWorkspace( workspace );
        std::unordered_map<AnalysisDictionary::StringId, AddressLife> addressLife;
        std::unordered_set<AnalysisDictionary::StringId> ambiguousAddresses;
        struct SweepPoint { int64_t time; uint64_t size; bool allocation; AnalysisDictionary::StringId ref; };
        std::vector<SweepPoint> sweep;
        poolWorkspace.Add( 1024 * events.size() );
        sweep.reserve( events.size() * 2 );
        for( const auto& value : events )
        {
            checkCancelled();
            const auto* event = &value;
            fact.eventCount++;
            fact.totalAllocatedBytes = SaturatingAdd( fact.totalAllocatedBytes, event->size );
            if( event->allocationNs <= info.firstTimeNs ) fact.openBoundaryCount++;
            const auto previous = addressLife.find( event->address );
            if( previous != addressLife.end() )
            {
                if( previous->second.freeNs && *previous->second.freeNs <= event->allocationNs ) fact.addressReuseCount++;
                else
                {
                    fact.accountingGapCount++;
                    ambiguousAddresses.emplace( event->address );
                }
            }
            addressLife[event->address] = { event->freeNs };
            sweep.push_back( { event->allocationNs, event->size, true, event->ref } );
            if( event->freeNs )
            {
                fact.freeCount++;
                fact.totalFreedBytes = SaturatingAdd( fact.totalFreedBytes, event->size );
                if( *event->freeNs < event->allocationNs )
                {
                    fact.accountingGapCount++;
                    ambiguousAddresses.emplace( event->address );
                }
                else
                {
                    if( *event->freeNs - event->allocationNs <= m_transientLifetimeNs ) fact.transientCount++;
                    sweep.push_back( { *event->freeNs, event->size, false, event->ref } );
                }
            }
            else
            {
                fact.aliveAtEndCount++;
            }
        }
        // Address ambiguity may only become visible at a later allocation.
        // Classify leak candidates after the complete pool lifetime pass so the
        // answer is independent from input/event order.
        for( const auto& value : events )
        {
            checkCancelled();
            const auto* event = &value;
            if( event->freeNs ) continue;
            const bool observedCreate = event->allocationNs > info.firstTimeNs;
            const bool oldEnough = info.lastTimeNs >= event->allocationNs &&
                info.lastTimeNs - event->allocationNs >= m_leakCandidateAgeNs;
            if( observedCreate && oldEnough && ambiguousAddresses.find( event->address ) == ambiguousAddresses.end() )
                fact.leakCandidateCount++;
        }
        std::sort( sweep.begin(), sweep.end(), [&]( const auto& lhs, const auto& rhs ) {
            checkCancelled();
            if( lhs.time != rhs.time ) return lhs.time < rhs.time;
            if( lhs.allocation != rhs.allocation ) return !lhs.allocation;
            return memoryDictionary.Text( lhs.ref ) < memoryDictionary.Text( rhs.ref );
        } );
        uint64_t live = 0;
        for( const auto& point : sweep )
        {
            checkCancelled();
            if( point.allocation ) live = SaturatingAdd( live, point.size );
            else if( live >= point.size ) live -= point.size;
            else
            {
                live = 0;
                fact.accountingGapCount++;
            }
            fact.peakLiveBytes = std::max( fact.peakLiveBytes, live );
        }
        fact.endLiveBytes = live;
        fact.hasGrowth = fact.endLiveBytes != 0;
        fact.hasChurn = fact.freeCount != 0 || fact.addressReuseCount != 0;
        fact.hasLeakCandidate = fact.leakCandidateCount != 0;
        fact.hasTransient = fact.transientCount != 0;
        fact.hasAccountingGap = fact.accountingGapCount != 0;
        if( fact.hasGrowth ) fact.classifications.emplace_back( CpuMemoryClassification::Growth );
        if( fact.hasChurn ) fact.classifications.emplace_back( CpuMemoryClassification::Churn );
        if( fact.hasLeakCandidate ) fact.classifications.emplace_back( CpuMemoryClassification::LeakCandidate );
        if( fact.hasTransient ) fact.classifications.emplace_back( CpuMemoryClassification::Transient );
        if( fact.hasAccountingGap )
        {
            fact.classifications.emplace_back( CpuMemoryClassification::AccountingGap );
            result.qualityComplete = false;
            result.outputWorkspace.Add( 2048 );
            MemoryIoSamplingQualityFinding finding { "cpu_memory_accounting_gap",
                "Overlapping address lifetimes or invalid allocation/free ordering prevent exact accounting.",
                fact.accountingGapCount, {} };
            for( const auto& value : events )
            {
                checkCancelled();
                const auto* event = &value;
                if( ambiguousAddresses.find( event->address ) != ambiguousAddresses.end() && finding.representativeRefs.size() < RepresentativeLimit )
                {
                    const auto ref = memoryDictionary.Text( event->ref );
                    result.outputWorkspace.Add( 128 + 4 * ref.size() );
                    AddRepresentative( finding, ref );
                }
            }
            result.qualityFindings.emplace_back( std::move( finding ) );
        }
    }
    }

    // GPU totals use unique allocation identities. Logical capacity, Pass
    // working sets and Range evidence stay in separate fields by construction.
    const auto* catalogCapability = FindCapability( capabilities, "gpu.catalog" );
    result.gpuMemory.catalogState = CatalogState( catalogCapability );
    result.outputWorkspace.Add( 1024 + ( catalogCapability ? 4 * catalogCapability->reason.size() : 0 ) );
    result.gpuMemory.catalogReason = catalogCapability ? catalogCapability->reason : "gpu_catalog_capability_absent";
    for( const auto& pool : pools )
    {
        checkCancelled();
        const auto lower = Lower( pool.name );
        if( pool.gpuD3D12 && Contains( lower, "dxgi" ) && Contains( lower, "local" ) )
            result.gpuMemory.dxgiUsageBytes = std::max( result.gpuMemory.dxgiUsageBytes.value_or( 0 ), pool.persistedUsageBytes );
    }
    if( dynamic_cast<const GpuCatalogBoundedScanSource*>( &m_source ) )
    {
        AnalysisWorkspaceReservation allocationWorkspace( workspace );
        std::unordered_set<uint64_t> allocationIds;
        struct PhysicalPoint { uint64_t time; uint64_t bytes; bool create; };
        std::vector<PhysicalPoint> physicalPoints;
        VisitAll<GpuAllocationAnalysisRecord>( scanner, BoundedScanDomain::GpuAllocation,
            m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const auto& allocation ) {
            checkCancelled();
            result.inputGpuAllocationCount++;
            if( allocation.invalid || allocation.allocationId == 0 ||
                ( allocation.destroyTime != 0 && allocation.destroyTime < allocation.createTime ) )
            {
                result.gpuMemory.invalidAllocationCount++;
                result.gpuMemory.physicalPeakExact = false;
                return;
            }
            if( allocationIds.contains( allocation.allocationId ) ) return;
            allocationWorkspace.Add( 512 ); // identity, buckets and two sweep points including growth peak
            allocationIds.emplace( allocation.allocationId );
            result.gpuMemory.allocationCount++;
            // Child placed ranges are already accounted for by their heap.
            if( allocation.parentAllocationId == 0 )
            {
                if( allocation.lastUpdateTime > allocation.createTime &&
                    allocation.lastUpdateTime != allocation.destroyTime )
                    result.gpuMemory.physicalPeakExact = false;
                // A final mutable size cannot reconstruct historical size changes.
                if( allocation.destroyTime == 0 || allocation.destroyTime > allocation.createTime )
                {
                    physicalPoints.push_back( { allocation.createTime, allocation.sizeBytes, true } );
                    if( allocation.destroyTime != 0 )
                        physicalPoints.push_back( { allocation.destroyTime, allocation.sizeBytes, false } );
                }
                if( allocation.destroyTime == 0 )
                    result.gpuMemory.residentBytes = SaturatingAdd( result.gpuMemory.residentBytes, allocation.residentBytes );
            }
            if( allocation.resources.size() > 1 ) result.gpuMemory.sharedAllocationCount++;
        } );
        std::sort( physicalPoints.begin(), physicalPoints.end(), [&]( const auto& a, const auto& b ) {
            checkCancelled();
            if( a.time != b.time ) return a.time < b.time;
            return a.create < b.create; // Half-open lifetimes: free before create at equal time.
        } );
        uint64_t livePhysicalBytes = 0;
        for( const auto& point : physicalPoints )
        {
            checkCancelled();
            if( point.create )
            {
                if( UINT64_MAX - livePhysicalBytes < point.bytes ) result.gpuMemory.physicalPeakExact = false;
                livePhysicalBytes = SaturatingAdd( livePhysicalBytes, point.bytes );
            }
            else if( point.bytes > livePhysicalBytes ) result.gpuMemory.physicalPeakExact = false;
            else livePhysicalBytes -= point.bytes;
            if( livePhysicalBytes > result.gpuMemory.physicalBytes )
            {
                result.gpuMemory.physicalBytes = livePhysicalBytes;
                result.gpuMemory.physicalPeakTimeNs = point.time;
            }
        }
        result.gpuMemory.ownedPhysicalBytes = result.gpuMemory.physicalBytes;
        result.gpuMemory.physicalFactsAvailable = true;

        if( result.gpuMemory.catalogState == GpuCatalogState::Complete )
        {
            AnalysisWorkspaceReservation resourceWorkspace( workspace );
            std::unordered_set<uint64_t> resourceIds;
            VisitAll<GpuAnalysisResourceSummary>( scanner, BoundedScanDomain::GpuResource,
                m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const auto& resource ) {
                checkCancelled();
                result.inputGpuResourceCount++;
                if( resource.resourceId == 0 )
                {
                    result.gpuMemory.invalidResourceCount++;
                    return;
                }
                if( resourceIds.contains( resource.resourceId ) ) return;
                resourceWorkspace.Add( 128 );
                resourceIds.emplace( resource.resourceId );
                result.gpuMemory.resourceCount++;
                result.gpuMemory.logicalCapacityBytes = SaturatingAdd( result.gpuMemory.logicalCapacityBytes, resource.capacityBytes );
                if( resource.hasAliasGroup ) result.gpuMemory.aliasResourceCount++;
            } );
            VisitAll<GpuPassWorkingSet>( scanner, BoundedScanDomain::GpuPass,
                m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const auto& pass ) {
                checkCancelled();
                result.inputGpuPassCount++;
                result.gpuMemory.passCount++;
                result.gpuMemory.maximumDirectWorkingSetBytes = std::max(
                    result.gpuMemory.maximumDirectWorkingSetBytes, pass.directPhysicalBytes );
                result.gpuMemory.maximumInclusiveWorkingSetBytes = std::max(
                    result.gpuMemory.maximumInclusiveWorkingSetBytes, pass.inclusivePhysicalBytes );
            } );
            VisitAll<GpuAnalysisRangeStoreEntry>( scanner, BoundedScanDomain::GpuRange,
                m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const auto& range ) {
                checkCancelled();
                result.inputGpuRangeCount++;
                result.gpuMemory.rangeCount++;
                result.gpuMemory.rangeEvidenceBytes = SaturatingAdd(
                    result.gpuMemory.rangeEvidenceBytes, range.record.lengthBytes );
            } );
            result.gpuMemory.resourceFactsAvailable = true;
        }
    }
    // DXGI is a sample, whereas physicalBytes is an event-level lifetime peak.
    // Without a common timestamp their difference is not untracked memory.
    // Keep the optional unavailable instead of manufacturing an exact value.

    VisitAll<IoRequestDto>( scanner, BoundedScanDomain::IoRequest,
        m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const auto& request ) {
        checkCancelled();
        result.inputIoRequestCount++;
        IoRequestFact fact;
        if( retainDetails )
        {
            result.outputWorkspace.Add( 1024 + 4 * request.ref.size() );
            fact.ref = request.ref;
        }
        fact.requestId = request.requestId; fact.state = IoState( request, cancelled );
        fact.requestedBytes = request.requestedBytes; fact.transferredBytes = request.transferredBytes;
        fact.stageCount = uint32_t( request.stages.size() );
        if( request.startNs && *request.startNs >= request.queueNs ) fact.queueLatencyNs = *request.startNs - request.queueNs;
        else if( request.startNs ) fact.exact = false;
        if( request.startNs && request.endNs && *request.endNs >= *request.startNs ) fact.executionNs = *request.endNs - *request.startNs;
        else if( request.endNs ) fact.exact = false;
        if( request.endNs && *request.endNs >= request.queueNs ) fact.totalNs = *request.endNs - request.queueNs;
        else if( request.endNs ) fact.exact = false;
        if( request.terminalCount > 1 || request.truncated ) fact.exact = false;
        switch( fact.state )
        {
        case IoLifecycleState::Complete: result.ioSummary.completeCount++; break;
        case IoLifecycleState::Cancelled: result.ioSummary.cancelledCount++; break;
        case IoLifecycleState::Error: result.ioSummary.errorCount++; break;
        case IoLifecycleState::Orphan: result.ioSummary.orphanCount++; break;
        case IoLifecycleState::Incomplete: result.ioSummary.incompleteCount++; break;
        }
        result.ioSummary.requestedBytes = SaturatingAdd( result.ioSummary.requestedBytes, request.requestedBytes );
        result.ioSummary.transferredBytes = SaturatingAdd( result.ioSummary.transferredBytes, request.transferredBytes );
        if( retainDetails ) result.ioRequests.emplace_back( std::move( fact ) );
    } );

    const auto* sampleCapability = FindCapability( capabilities, "sample" );
    result.sampling.available = sampleCapability && sampleCapability->present && sampleCapability->queryable;
    result.outputWorkspace.Add( 1024 + ( sampleCapability ? 4 * sampleCapability->reason.size() : 0 ) );
    if( !result.sampling.available ) result.sampling.unavailableReason = sampleCapability ?
        sampleCapability->reason : "sampling_capability_absent";
    if( result.sampling.available )
    {
        AnalysisWorkspaceReservation samplingWorkspace( workspace );
        struct Key
        {
            std::string role;
            std::string kind;
            uint32_t callstack = 0;
            bool operator<( const Key& other ) const
            { return std::tie( role, kind, callstack ) < std::tie( other.role, other.kind, other.callstack ); }
        };
        std::map<uint32_t, uint64_t> callstacks;
        std::map<Key, SamplingLeafFact> aggregate;
        // Sample captures can contain tens or hundreds of millions of events.
        // Aggregate each bounded page immediately; retaining every SampleDto
        // made Query memory grow linearly with capture duration even though the
        // final result only needs one row per role/kind/callstack key.
        VisitAll<SampleDto>( scanner, BoundedScanDomain::Sample, m_batchSize,
            result.maximumBatchObserved, cancelled, workspace, [&]( const SampleDto& sample ) {
                result.inputSampleCount++;
                result.sampling.totalSamples++;
                if( sample.callstack == 0 ) result.sampling.unresolvedSamples++;
                else
                {
                    if( !callstacks.contains( sample.callstack ) ) samplingWorkspace.Add( 160 );
                    callstacks[sample.callstack]++;
                }
                if( !retainDetails ) return;
                AnalysisWorkspaceReservation currentKey( workspace, 1024 + 8 * ( sample.threadRef.size() + sample.kind.size() ) );
                const Key key { role( sample.threadRef ), sample.kind, sample.callstack };
                if( !aggregate.contains( key ) ) samplingWorkspace.Add( 2048 + 8 * ( key.role.size() + key.kind.size() ) );
                auto& fact = aggregate[key];
                fact.threadRole = key.role;
                fact.sampleKind = key.kind;
                fact.callstack = key.callstack;
                fact.sampleCount++;
            } );
        std::unordered_map<uint32_t, CallstackFrameDto> leaves;
        for( auto next = callstacks.begin(); next != callstacks.end(); )
        {
            checkCancelled();
            AnalysisWorkspaceReservation pageWorkspace( workspace, 1024 + 32 * std::min( m_batchSize, callstacks.size() ) );
            std::vector<uint32_t> page;
            page.reserve( std::min( m_batchSize, callstacks.size() ) );
            for( ; next != callstacks.end() && page.size() < m_batchSize; ++next ) page.push_back( next->first );
            const auto frames = m_source.ResolveCallstacks( page, 1 );
            checkCancelled();
            for( const auto& frame : frames ) pageWorkspace.Add( RecordBytes( frame ) );
            // Resolution may legitimately return no rows. Its working step
            // still needs a fresh post-read check under concurrent pressure.
            pageWorkspace.Add( 1024 );
            std::unordered_set<uint32_t> resolved;
            for( const auto& frame : frames )
            {
                checkCancelled();
                resolved.emplace( frame.callstack );
                if( !retainDetails ) continue;
                const auto found = leaves.find( frame.callstack );
                if( found == leaves.end() || frame.depth < found->second.depth )
                {
                    samplingWorkspace.Add( RecordBytes( frame ) );
                    leaves[frame.callstack] = frame;
                }
            }
            for( auto id : page ) if( !resolved.contains( id ) ) result.sampling.unresolvedSamples =
                SaturatingAdd( result.sampling.unresolvedSamples, callstacks.at( id ) );
        }
        for( auto& [key, fact] : aggregate )
        {
            checkCancelled();
            const auto leaf = leaves.find( key.callstack );
            fact.resolved = key.callstack != 0 && leaf != leaves.end();
            result.outputWorkspace.Add( 2048 + 8 * ( fact.threadRole.size() + fact.sampleKind.size() ) +
                ( fact.resolved ? 8 * ( leaf->second.name.size() + leaf->second.file.size() ) : 0 ) );
            if( fact.resolved )
            {
                fact.leafFunction = leaf->second.name; fact.leafFile = leaf->second.file;
                fact.leafLine = leaf->second.line;
            }
            else fact.leafFunction = "<unresolved>";
            result.sampling.leaves.emplace_back( std::move( fact ) );
        }
    }

    const auto* contextCapability = FindCapability( capabilities, "context_switch" );
    result.scheduling.available = contextCapability && contextCapability->present && contextCapability->queryable;
    result.outputWorkspace.Add( 1024 + ( contextCapability ? 4 * contextCapability->reason.size() : 0 ) );
    if( !result.scheduling.available ) result.scheduling.unavailableReason = contextCapability ?
        contextCapability->reason : "context_switch_capability_absent";
    if( result.scheduling.available )
    {
        AnalysisWorkspaceReservation schedulingWorkspace( workspace );
        std::map<std::string, SchedulingRoleFact> aggregate;
        VisitAll<ContextSwitchDto>( scanner, BoundedScanDomain::ContextSwitch,
            m_batchSize, result.maximumBatchObserved, cancelled, workspace, [&]( const ContextSwitchDto& value ) {
                result.inputContextSwitchCount++;
                if( !retainDetails ) return;
                AnalysisWorkspaceReservation currentRole( workspace, 1024 + 8 * value.threadRef.size() );
                const auto threadRole = role( value.threadRef );
                if( !aggregate.contains( threadRole ) ) schedulingWorkspace.Add( 1024 + 8 * threadRole.size() );
                auto& fact = aggregate[threadRole];
                fact.threadRole = threadRole; fact.intervalCount++;
                if( value.complete && value.endNs && *value.endNs >= value.startNs )
                    fact.runningNs += *value.endNs - value.startNs;
                else fact.incompleteCount++;
                if( value.wakeupNs && *value.wakeupNs <= value.startNs )
                    fact.readyWaitNs += value.startNs - *value.wakeupNs;
                const auto state = Lower( value.stateName );
                const auto reasonName = Lower( value.reasonName );
                if( Contains( state, "wait" ) || Contains( reasonName, "wait" ) ||
                    Contains( reasonName, "mutex" ) || Contains( reasonName, "sleep" ) ) fact.waitCount++;
                if( Contains( state, "ready" ) || Contains( reasonName, "preempt" ) ||
                    Contains( reasonName, "quantum" ) ) fact.preemptCount++;
            } );
        for( auto& [key, fact] : aggregate )
        {
            checkCancelled();
            result.outputWorkspace.Add( 1024 + 4 * key.size() );
            result.scheduling.roles.emplace_back( std::move( fact ) );
        }
    }

    // Producer self-cost is the only cost measurable from one trace. Overall
    // capture overhead is a counterfactual and therefore always requires A/B.
    AnalysisWorkspaceReservation telemetryWorkspace( workspace );
    std::map<std::pair<uint64_t, std::string>, std::vector<ProducerSnapshot>> producerSnapshots;
    bool invalidProducerRecord = false;
    for( const auto& record : info.appInfo )
    {
        checkCancelled();
        if( !record.starts_with( "JNQ1|" ) ) continue;
        result.inputTelemetryRecordCount++;
        // Outside the parse catch: budget rejection must abort, never become
        // a quality warning that permits publication of an incomplete scan.
        telemetryWorkspace.Add( 8192 + 64 * record.size() );
        try
        {
            const auto document = json::parse( record.begin() + 5, record.end() );
            if( !document.contains( "producer" ) || !document["producer"].is_object() ) continue;
            const auto& producer = document["producer"];
            const auto producerId = producer.contains( "id" ) ? JsonUnsigned( producer["id"] ).value_or( 0 ) : 0;
            const auto key = producer.value( "key", std::string() );
            if( key.empty() ) continue;
            const auto sequence = document.contains( "snapshot_sequence" ) ?
                JsonUnsigned( document["snapshot_sequence"] ).value_or( 0 ) : 0;
            producerSnapshots[{ producerId, key }].push_back( { sequence, producer } );
        }
        catch( const std::exception& )
        {
            invalidProducerRecord = true;
            result.qualityComplete = false;
        }
    }
    result.telemetry.producerQualityPresent = !producerSnapshots.empty();
    result.telemetry.producerQualityComplete = result.telemetry.producerQualityPresent && !invalidProducerRecord;
    const auto delta = []( uint64_t first, uint64_t last, bool& complete ) {
        if( last >= first ) return last - first;
        complete = false;
        return uint64_t( 0 );
    };
    for( auto& [identity, snapshots] : producerSnapshots )
    {
        checkCancelled();
        std::sort( snapshots.begin(), snapshots.end(), [&]( const auto& lhs, const auto& rhs ) {
            checkCancelled();
            return lhs.sequence < rhs.sequence;
        } );
        result.outputWorkspace.Add( 4096 + 8 * identity.second.size() +
            ( snapshots.back().producer.contains( "source_mode" ) && snapshots.back().producer["source_mode"].is_string() ?
                8 * snapshots.back().producer["source_mode"].get_ref<const std::string&>().size() : 0 ) );
        TelemetryProducerFact fact;
        fact.producerId = identity.first; fact.key = identity.second;
        fact.sourceMode = snapshots.back().producer.value( "source_mode", std::string() );
        const auto& first = snapshots.front().producer;
        const auto& last = snapshots.back().producer;
        const bool requested = last.value( "requested", false );
        const bool supported = last.value( "supported", false );
        const bool effective = last.value( "effective", false );
        const bool permissionDenied = last.value( "permission_denied", false );
        const bool deferred = last.value( "deferred", false );
        fact.state = permissionDenied ? "permission_denied" : deferred ? "deferred" :
            !requested ? "not_requested" : !supported ? "unsupported" : effective ? "effective" : "disabled";
        fact.observed = delta( Counter( first, "observed" ), Counter( last, "observed" ), fact.complete );
        fact.eventCount = delta( Counter( first, "emitted" ), Counter( last, "emitted" ), fact.complete );
        fact.eventBytes = delta( Counter( first, "event_bytes" ), Counter( last, "event_bytes" ), fact.complete );
        fact.cpuTimeNs = delta( Counter( first, "cpu_time_ns" ), Counter( last, "cpu_time_ns" ), fact.complete );
        fact.dropped = delta( Counter( first, "dropped" ), Counter( last, "dropped" ), fact.complete );
        fact.filtered = delta( Counter( first, "filtered" ), Counter( last, "filtered" ), fact.complete );
        fact.sampledOut = delta( Counter( first, "sampled_out" ), Counter( last, "sampled_out" ), fact.complete );
        fact.overflow = delta( Counter( first, "overflow" ), Counter( last, "overflow" ), fact.complete );
        fact.mismatch = delta( Counter( first, "mismatch" ), Counter( last, "mismatch" ), fact.complete );
        fact.unresolved = delta( Counter( first, "unresolved" ), Counter( last, "unresolved" ), fact.complete );
        fact.tailTruncated = delta( Counter( first, "tail_truncated" ), Counter( last, "tail_truncated" ), fact.complete );
        fact.degrade = delta( Counter( first, "degrade" ), Counter( last, "degrade" ), fact.complete );
        if( snapshots.size() < 2 ) fact.complete = false;
        if( fact.dropped != 0 || fact.overflow != 0 || fact.mismatch != 0 ||
            fact.unresolved != 0 || fact.tailTruncated != 0 || fact.degrade != 0 )
            fact.complete = false;
        result.telemetry.producerQualityComplete &= fact.complete;
        result.telemetry.totalCpuTimeNs = SaturatingAdd( result.telemetry.totalCpuTimeNs, fact.cpuTimeNs );
        result.telemetry.totalEventCount = SaturatingAdd( result.telemetry.totalEventCount, fact.eventCount );
        result.telemetry.totalEventBytes = SaturatingAdd( result.telemetry.totalEventBytes, fact.eventBytes );
        result.telemetry.totalDropped = SaturatingAdd( result.telemetry.totalDropped, fact.dropped );
        result.telemetry.totalOverflow = SaturatingAdd( result.telemetry.totalOverflow, fact.overflow );
        if( fact.cpuTimeNs != 0 ) AddEvidenceClass( result.telemetry.evidenceClasses, TelemetryEvidenceClass::MeasuredSelfCost );
        if( fact.dropped != 0 || fact.overflow != 0 || fact.degrade != 0 )
            AddEvidenceClass( result.telemetry.evidenceClasses, TelemetryEvidenceClass::ObservedSystemPressure );
        if( fact.eventBytes != 0 || fact.eventCount != 0 )
            AddEvidenceClass( result.telemetry.evidenceClasses, TelemetryEvidenceClass::EstimatedAttribution );
        result.telemetry.producers.emplace_back( std::move( fact ) );
    }
    if( !result.telemetry.producerQualityComplete && result.telemetry.producerQualityPresent )
    {
        result.qualityComplete = false;
        result.outputWorkspace.Add( 2048 );
        MemoryIoSamplingQualityFinding finding;
        finding.code = "telemetry_producer_quality_incomplete";
        finding.message = "Producer counter windows contain loss, overflow, degradation, truncation, or invalid records.";
        for( const auto& producer : result.telemetry.producers )
        {
            checkCancelled();
            if( producer.complete ) continue;
            finding.count++;
            if( finding.representativeRefs.size() < RepresentativeLimit ) result.outputWorkspace.Add( 128 + 4 * producer.key.size() );
            AddRepresentative( finding, producer.key );
        }
        if( invalidProducerRecord ) finding.count++;
        result.qualityFindings.emplace_back( std::move( finding ) );
    }
    if( result.gpuMemory.catalogState == GpuCatalogState::Absent )
    {
        const auto producer = std::find_if( result.telemetry.producers.begin(), result.telemetry.producers.end(),
            [&]( const auto& value ) { checkCancelled(); return value.key == "gpu.catalog"; } );
        if( producer != result.telemetry.producers.end() && producer->state != "effective" )
        {
            result.gpuMemory.catalogState = GpuCatalogState::Disabled;
            result.gpuMemory.catalogReason = "gpu.catalog producer state: " + producer->state;
        }
    }
    result.telemetry.frameImageCount = info.counts.frameImages;
    if( result.telemetry.frameImageCount != 0 )
        AddEvidenceClass( result.telemetry.evidenceClasses, TelemetryEvidenceClass::EstimatedAttribution );
    AddEvidenceClass( result.telemetry.evidenceClasses, TelemetryEvidenceClass::RequiresABValidation );
    checkCancelled();
    return result;
}

const char* CpuMemoryClassificationName( CpuMemoryClassification value )
{
    switch( value )
    {
    case CpuMemoryClassification::Capacity: return "Capacity";
    case CpuMemoryClassification::Growth: return "Growth";
    case CpuMemoryClassification::Churn: return "Churn";
    case CpuMemoryClassification::LeakCandidate: return "LeakCandidate";
    case CpuMemoryClassification::Transient: return "Transient";
    case CpuMemoryClassification::AccountingGap: return "AccountingGap";
    }
    return "Unknown";
}

const char* GpuCatalogStateName( GpuCatalogState value )
{
    switch( value )
    {
    case GpuCatalogState::Complete: return "Complete";
    case GpuCatalogState::Invalid: return "Invalid";
    case GpuCatalogState::Disabled: return "Disabled";
    case GpuCatalogState::Absent: return "Absent";
    }
    return "Absent";
}

const char* IoLifecycleStateName( IoLifecycleState value )
{
    switch( value )
    {
    case IoLifecycleState::Complete: return "Complete";
    case IoLifecycleState::Cancelled: return "Cancelled";
    case IoLifecycleState::Error: return "Error";
    case IoLifecycleState::Orphan: return "Orphan";
    case IoLifecycleState::Incomplete: return "Incomplete";
    }
    return "Incomplete";
}

const char* TelemetryEvidenceClassName( TelemetryEvidenceClass value )
{
    switch( value )
    {
    case TelemetryEvidenceClass::MeasuredSelfCost: return "MeasuredSelfCost";
    case TelemetryEvidenceClass::ObservedSystemPressure: return "ObservedSystemPressure";
    case TelemetryEvidenceClass::EstimatedAttribution: return "EstimatedAttribution";
    case TelemetryEvidenceClass::RequiresABValidation: return "RequiresABValidation";
    }
    return "RequiresABValidation";
}

}
