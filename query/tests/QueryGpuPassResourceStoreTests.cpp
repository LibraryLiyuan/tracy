#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryGpuPassResourceStore.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyHash.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{

using namespace tracy;
using namespace tracy::query;

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

uint64_t MixGenerated( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

class ReferenceInput final : public QueryGpuReferenceInput
{
public:
    uint64_t PassCount() const override { return passes.size(); }
    uint64_t UseCount() const override { return uses.size(); }
    uint64_t EndCount() const override { return ends.size(); }
    JnGpuReferencePassData Pass( uint64_t ordinal ) const override { return passes.at( size_t( ordinal ) ); }
    JnGpuReferenceUseData Use( uint64_t ordinal ) const override { return uses.at( size_t( ordinal ) ); }
    JnGpuReferenceEndData End( uint64_t ordinal ) const override { return ends.at( size_t( ordinal ) ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t ) const override
    {
        return pointerToken == 900 ? 100 : pointerToken == 901 ? 102 : 0;
    }
    std::vector<JnGpuReferencePassData> passes;
    std::vector<JnGpuReferenceUseData> uses;
    std::vector<JnGpuReferenceEndData> ends;
};

class PassInput final : public QueryGpuPassResourceInput
{
public:
    PassInput( const QueryCatalogStoreReader& catalog, const QueryGpuReferenceStoreReader& references )
        : m_catalog( catalog ), m_references( references )
    {
        mappings = { { 10, 10 }, { 20, 20 }, { 10, 1000 }, { 20, 1000 }, { 10, 1000 } };
    }
    uint64_t MappingCount() const override { return mappings.size(); }
    QueryGpuPassMapping Mapping( uint64_t ordinal ) const override { return mappings.at( size_t( ordinal ) ); }
    uint64_t RangeCount() const override { return m_catalog.RangeCount(); }
    JnGpuRangeSetRecordV1 Range( uint64_t ordinal ) const override
    { rangeCalls++; return m_catalog.RangeAt( ordinal ).record; }
    uint64_t UseCount() const override { return m_references.UseCount(); }
    QueryGpuReferenceResolvedUseRecord Use( uint64_t ordinal ) const override
    {
        if( !scratchRoot.empty() )
            Require( !std::filesystem::exists( scratchRoot / "fact-source.work" ),
                "GPU pass input still writes a complete fact source before sorting" );
        return m_references.UseAt( ordinal );
    }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    { return m_catalog.ResolveResourcePointer( pointerToken, time ); }
    uint64_t ResourceOrdinalCount() const override { return m_catalog.ResourceIndexCount(); }
    std::optional<uint32_t> FindResourceOrdinal( uint64_t resourceId ) const override
    {
        uint64_t first = 0, length = m_catalog.ResourceIndexCount();
        while( length != 0 )
        {
            const auto half = length / 2;
            const auto middle = first + half;
            if( m_catalog.ResourceStateAt( middle ).latest.resourceId < resourceId )
            { first = middle + 1; length -= half + 1; }
            else length = half;
        }
        if( first >= m_catalog.ResourceIndexCount() ||
            m_catalog.ResourceStateAt( first ).latest.resourceId != resourceId ) return std::nullopt;
        return uint32_t( first );
    }
    std::optional<uint32_t> ResolveCatalogResourceOrdinal( uint64_t token, int64_t time ) const override
    { return FindResourceOrdinal( ResolveCatalogResource( token, time ) ); }
    uint64_t ResourceIdAtOrdinal( uint32_t ordinal ) const override
    { return m_catalog.ResourceStateAt( ordinal ).latest.resourceId; }
    uint64_t ResourceAllocationAtOrdinal( uint32_t ordinal ) const override
    { return m_catalog.ResourceStateAt( ordinal ).latest.allocationId; }
    uint64_t ResourceAllocation( uint64_t resourceId ) const override
    {
        if( !scratchRoot.empty() )
        {
            for( const auto* obsolete : { "mapping-source.work", "mapping-sorted.work",
                    "fact-source.work", "fact-sorted.work", "evidence-source.work",
                    "expanded-source.work", "range-source.work", "use-source.work",
                    "allocation-source.work" } )
                Require( !std::filesystem::exists( scratchRoot / obsolete ),
                    "GPU pass build retained consumed scratch during summary" );
        }
        resourceAllocationCalls++;
        const auto state = m_catalog.ResourceState( resourceId );
        return state ? state->latest.allocationId : 0;
    }
    uint64_t AllocationBytesAtEnd( uint64_t allocationId ) const override
    {
        allocationBytesCalls++;
        const auto state = m_catalog.AllocationState( allocationId );
        return state && state->record.operation != uint8_t( JnGpuCatalogRecordOperation::Destroy ) ?
            state->record.sizeBytes : 0;
    }
    std::vector<QueryGpuPassMapping> mappings;
    mutable uint64_t resourceAllocationCalls = 0;
    mutable uint64_t allocationBytesCalls = 0;
    mutable uint64_t rangeCalls = 0;
    std::filesystem::path scratchRoot;
private:
    const QueryCatalogStoreReader& m_catalog;
    const QueryGpuReferenceStoreReader& m_references;
};

class GeneratedPassInput final : public QueryGpuPassResourceInput
{
public:
    GeneratedPassInput( uint64_t uses, uint64_t resources,
        const QueryCatalogStoreReader* catalog = nullptr,
        std::shared_ptr<tracy::analysis::AnalysisWorkspaceBudget> workspace = {},
        bool mixedHighFanout = false )
        : m_uses( uses ), m_resources( resources ), m_catalog( catalog )
        , m_mixedHighFanout( mixedHighFanout ), m_resourceMemory( std::move( workspace ) )
    {
        if( !m_catalog ) return;
        const auto count = m_catalog->ResourceIndexCount();
        if( count > SIZE_MAX / sizeof( std::pair<uint64_t, uint64_t> ) )
            throw std::runtime_error( "GPU pass probe resource fast path exceeds address space" );
        m_resourceMemory.Resize( count * sizeof( std::pair<uint64_t, uint64_t> ) );
        m_resourceAllocations.reserve( size_t( count ) );
        uint64_t previousId = 0;
        for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
        {
            const auto state = m_catalog->ResourceStateAt( ordinal );
            if( state.latest.resourceId == 0 || ( ordinal != 0 && state.latest.resourceId <= previousId ) )
                throw std::runtime_error( "GPU pass probe resource fast path order changed" );
            m_resourceAllocations.emplace_back( state.latest.resourceId, state.latest.allocationId );
            previousId = state.latest.resourceId;
        }
        m_resourceMemory.Resize( m_resourceAllocations.capacity() * sizeof( m_resourceAllocations[0] ) );
    }
    uint64_t MappingCount() const override
    {
        const auto base = BasePassCount();
        return base + ( m_mixedHighFanout ? ( base + 15 ) / 16 : 0 );
    }
    QueryGpuPassMapping Mapping( uint64_t ordinal ) const override
    {
        const auto base = BasePassCount();
        if( ordinal < base ) return { ordinal + 1, ordinal + 1 };
        const auto fanoutOrdinal = ordinal - base;
        return { fanoutOrdinal * 16 + 1, base + fanoutOrdinal + 1 };
    }
    uint64_t RangeCount() const override { return m_mixedHighFanout ? m_uses / 10 : 0; }
    JnGpuRangeSetRecordV1 Range( uint64_t ordinal ) const override
    {
        if( ordinal >= RangeCount() ) throw std::runtime_error( "unexpected range" );
        JnGpuRangeSetRecordV1 row {};
        row.passInstanceId = ordinal % BasePassCount() + 1;
        row.resourceId = MixGenerated( ordinal ^ 0x8877665544332211ull ) % m_resources + 1;
        row.lengthBytes = 64;
        return row;
    }
    uint64_t UseCount() const override { return m_uses; }
    QueryGpuReferenceResolvedUseRecord Use( uint64_t ordinal ) const override
    {
        QueryGpuReferenceResolvedUseRecord row {};
        row.passId = ordinal % BasePassCount() + 1;
        row.resourceId = m_mixedHighFanout ?
            MixGenerated( ordinal ^ 0x1020304050607080ull ) % m_resources + 1 :
            ordinal % m_resources + 1;
        row.passStart = int64_t( row.passId * 100 );
        row.passEnd = row.passStart + 99;
        row.usageMask = 1;
        return row;
    }
    uint64_t ResolveCatalogResource( uint64_t token, int64_t time ) const override
    { return m_catalog ? m_catalog->ResolveResourcePointer( token, time ) : token; }
    uint64_t ResourceAllocation( uint64_t resourceId ) const override
    {
        resourceAllocationCalls++;
        if( !m_catalog ) return resourceId;
        const auto found = std::lower_bound( m_resourceAllocations.begin(), m_resourceAllocations.end(),
            resourceId, []( const auto& row, uint64_t key ) { return row.first < key; } );
        return found != m_resourceAllocations.end() && found->first == resourceId ? found->second : 0;
    }
    uint64_t AllocationBytesAtEnd( uint64_t ) const override
    { allocationEndCalls++; return 64; }
    uint64_t ResourceOrdinalCount() const override { return m_resources; }
    std::optional<uint32_t> FindResourceOrdinal( uint64_t resourceId ) const override
    {
        return resourceId != 0 && resourceId <= m_resources ?
            std::optional<uint32_t>( uint32_t( resourceId - 1 ) ) : std::nullopt;
    }
    std::optional<uint32_t> ResolveCatalogResourceOrdinal( uint64_t token, int64_t time ) const override
    {
        const auto resourceId = ResolveCatalogResource( token, time );
        return resourceId != 0 && resourceId <= m_resources ?
            std::optional<uint32_t>( uint32_t( resourceId - 1 ) ) : std::nullopt;
    }
    uint64_t ResourceIdAtOrdinal( uint32_t ordinal ) const override
    { return ordinal < m_resources ? uint64_t( ordinal ) + 1 : 0; }
    uint64_t ResourceAllocationAtOrdinal( uint32_t ordinal ) const override
    { return ResourceIdAtOrdinal( ordinal ); }
    mutable uint64_t resourceAllocationCalls = 0;
    mutable uint64_t allocationEndCalls = 0;
private:
    uint64_t BasePassCount() const { return ( m_uses + 31 ) / 32; }
    uint64_t m_uses;
    uint64_t m_resources;
    const QueryCatalogStoreReader* m_catalog;
    bool m_mixedHighFanout = false;
    tracy::analysis::AnalysisWorkspaceReservation m_resourceMemory;
    std::vector<std::pair<uint64_t, uint64_t>> m_resourceAllocations;
};

class GeneratedReferenceInput final : public QueryGpuReferenceInput
{
public:
    GeneratedReferenceInput( uint64_t uses, uint64_t resources,
        const QueryCatalogStoreReader* catalog )
        : m_uses( uses ), m_resources( resources ), m_catalog( catalog ) {}

    uint64_t PassCount() const override { return ( m_uses + 31 ) / 32; }
    uint64_t UseCount() const override { return m_uses; }
    uint64_t EndCount() const override { return PassCount(); }
    JnGpuReferencePassData Pass( uint64_t ordinal ) const override
    { return { int64_t( ( ordinal + 1 ) * 100 ), ordinal + 1, ordinal, 1, 1, 0, 0 }; }
    JnGpuReferenceUseData Use( uint64_t ordinal ) const override
    {
        const auto passId = ordinal % PassCount() + 1;
        return { int64_t( passId * 100 + 1 ), passId,
            ordinal % m_resources + 1, 1, 1, 1, 0, 1 };
    }
    JnGpuReferenceEndData End( uint64_t ordinal ) const override
    { return { int64_t( ( ordinal + 1 ) * 100 + 99 ), ordinal + 1, 0, 1, 32, 0, 0 }; }
    uint64_t ResolveCatalogResource( uint64_t token, int64_t time ) const override
    { return m_catalog ? m_catalog->ResolveResourcePointer( token, time ) : token; }

private:
    uint64_t m_uses;
    uint64_t m_resources;
    const QueryCatalogStoreReader* m_catalog;
};

int CapacityProbe( uint64_t useCount, uint64_t resourceCount, uint64_t maximumBytes,
    bool inventory = false, bool realCatalog = false, bool validate = false,
    bool fused = false, bool mixedHighFanout = false )
{
    namespace fs = std::filesystem;
    using namespace tracy::analysis;
    if( useCount < 100000 || useCount > 10000000 || resourceCount == 0 ||
        resourceCount > useCount || maximumBytes == 0 || maximumBytes > ( 20ull << 30 ) ) return 2;
    const auto root = fs::temp_directory_path() /
        ( "query-gpu-pass-capacity-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    auto referenceRoot = root; referenceRoot += "-references";
    try
    {
        fs::create_directories( root );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
        PagedRecordOptions options;
        options.disk = std::make_shared<AnalysisDiskBudget>(
            AnalysisDiskBudget{ usage, maximumBytes, {} } );
        options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
        const auto started = std::chrono::steady_clock::now();
        options.cancelled = [started] {
            return std::chrono::steady_clock::now() - started > std::chrono::minutes( 10 );
        };
        std::vector<std::string> inventoryRows;
        uint64_t nextSample = 32ull << 20;
        if( inventory ) options.disk->checkSpace = [&] {
            const auto current = usage->Current();
            if( current < nextSample ) return;
            nextSample = current + ( 32ull << 20 );
            const auto scratch = root / "pass-resource-scratch";
            uint64_t source = 0, sorted = 0, runs = 0, other = 0;
            std::ostringstream details;
            if( fs::exists( scratch ) )
                for( const auto& item : fs::directory_iterator( scratch ) )
                {
                    if( !item.is_regular_file() ) continue;
                    const auto bytes = item.file_size();
                    const auto name = item.path().filename().string();
                    if( name.ends_with( "-source.work" ) ) source += bytes;
                    else if( name.ends_with( "-sorted.work" ) ) sorted += bytes;
                    else if( name.find( "-run-" ) != std::string::npos ) runs += bytes;
                    else other += bytes;
                    if( current >= 100000000 && bytes != 0 ) details << ' ' << name << '=' << bytes;
                }
            std::ostringstream line;
            line << "gpu_pass_inventory ledger=" << current << " source=" << source
                << " sorted=" << sorted << " runs=" << runs << " other=" << other
                << details.str();
            inventoryRows.emplace_back( line.str() );
        };
        const std::string identity( 64, 'a' );
        std::unique_ptr<QueryCatalogStoreReader> catalog;
        uint64_t catalogBuildMs = 0;
        if( realCatalog )
        {
            const auto catalogStarted = std::chrono::steady_clock::now();
            {
                QueryCatalogStoreWriter writer( root / "catalog", identity, options );
                writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
                writer.Begin( OfflineCatalogDomain::CatalogResource, resourceCount,
                    sizeof( JnGpuCatalogResourceRecordV1 ) );
                AnalysisWorkspaceReservation resourceRowsMemory( options.workspace,
                    4096 * sizeof( JnGpuCatalogResourceRecordV1 ) );
                std::vector<JnGpuCatalogResourceRecordV1> rows( 4096 );
                for( uint64_t offset = 0; offset < resourceCount; )
                {
                    const auto take = std::min<uint64_t>( rows.size(), resourceCount - offset );
                    for( uint64_t index = 0; index < take; index++ )
                    {
                        auto& row = rows[size_t( index )];
                        row = {};
                        row.resourceId = offset + index + 1;
                        row.pointerToken = row.resourceId;
                        row.allocationId = row.resourceId;
                        row.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
                        row.capacityBytes = 64;
                    }
                    writer.Append( OfflineCatalogDomain::CatalogResource, offset, rows.data(), take,
                        sizeof( rows[0] ) );
                    offset += take;
                }
                writer.End( OfflineCatalogDomain::CatalogResource );
                JnGpuCatalogBatchData batch {};
                batch.generation = 1;
                batch.kind = uint8_t( JnGpuCatalogBatchKind::Resource );
                batch.recordCount = resourceCount;
                writer.Begin( OfflineCatalogDomain::CatalogBatch, 1, sizeof( batch ) );
                writer.Append( OfflineCatalogDomain::CatalogBatch, 0, &batch, 1, sizeof( batch ) );
                writer.End( OfflineCatalogDomain::CatalogBatch );
                writer.Finish();
            }
            catalog = std::make_unique<QueryCatalogStoreReader>( root / "catalog", identity, options );
            catalog->BuildResourceIndex( root / "catalog" / "resource-index-scratch" );
            if( !catalog->PreparePointerResolutionFastPath() )
                throw std::runtime_error( "GPU pass capacity Catalog pointer fast path unavailable" );
            catalogBuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - catalogStarted ).count();
        }
        const auto sourceBytes = usage->Snapshot().currentBytes;
        uint64_t referenceFixtureMs = 0;
        if( validate )
        {
            const auto referenceStarted = std::chrono::steady_clock::now();
            auto referenceOptions = options;
            auto referenceUsage = std::make_shared<AnalysisDiskUsage>(
                std::vector<fs::path>{ referenceRoot } );
            referenceOptions.disk = std::make_shared<AnalysisDiskBudget>(
                AnalysisDiskBudget{ referenceUsage, maximumBytes, {} } );
            GeneratedReferenceInput referenceInput( useCount, resourceCount, catalog.get() );
            QueryGpuReferenceStore::Build( referenceRoot, identity, referenceInput, referenceOptions );
            referenceFixtureMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - referenceStarted ).count();
        }
        const auto fastPathStarted = std::chrono::steady_clock::now();
        GeneratedPassInput input( useCount, resourceCount, catalog.get(), options.workspace,
            mixedHighFanout );
        const auto fastPathBuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fastPathStarted ).count();
        const auto buildStarted = std::chrono::steady_clock::now();
        QueryGpuPassResourceBuildStatistics stages;
        Require( input.ResourceOrdinalCount() == resourceCount &&
            input.ResolveCatalogResourceOrdinal( 1, 0 ) == 0 &&
            input.ResourceIdAtOrdinal( 0 ) == 1 && input.ResourceAllocationAtOrdinal( 0 ) == 1,
            "GPU Pass fused Resource ordinal contract changed" );
        QueryGpuPassResourceStore::Build( root, identity, input, options, &stages,
            fused ? QueryGpuPassResourceStore::BuildMode::Fused :
                QueryGpuPassResourceStore::BuildMode::Legacy );
        const auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted ).count();
        if( fused )
            Require( stages.factSort.inputRecords == useCount + input.RangeCount() &&
                stages.inputMs + stages.mappingSortMs + stages.factSortMs +
                    stages.evidenceSortMs + stages.expansionMs + stages.summaryMs <= uint64_t( buildMs ),
                "GPU Pass fused build telemetry lost facts or exceeded wall time" );
        else
            Require( stages.factSort.inputRecords == useCount &&
                stages.expandedSort.inputRecords == useCount &&
                stages.useSort.inputRecords == useCount &&
                stages.allocationSort.inputRecords != 0 &&
                stages.inputMs + stages.factSortMs + stages.expansionMs +
                    stages.expandedSortMs + stages.allocationSortMs <= uint64_t( buildMs ),
                "GPU Pass/Resource build telemetry lost facts or exceeded wall time" );
        Require( !fs::exists( root / "pass-resource-scratch" ),
            "GPU pass capacity probe retained scratch" );
        QueryGpuPassResourceValidationStats validationStats;
        uint64_t validationMs = 0;
        if( validate )
        {
            QueryGpuPassResourceStoreReader validationReader( root, root / "catalog", referenceRoot,
                identity, options );
            const auto validationStarted = std::chrono::steady_clock::now();
            validationStats = validationReader.ValidateAll();
            validationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - validationStarted ).count();
        }
        const auto observed = usage->Snapshot();
        const auto observedWorkspace = options.workspace->Snapshot();
        std::cout << "gpu_pass_uses=" << useCount << " passes=" << input.MappingCount()
            << " resource_keys=" << resourceCount << " real_catalog=" << realCatalog
            << " fused=" << fused << " mixed_high_fanout=" << mixedHighFanout
            << " catalog_build_ms=" << catalogBuildMs << " fast_path_ms=" << fastPathBuildMs
            << " reference_fixture_ms=" << referenceFixtureMs
            << " source_bytes=" << sourceBytes
            << " peak_bytes=" << observed.peakBytes
            << " workspace_peak_bytes=" << observedWorkspace.peakBytes
            << " final_bytes=" << observed.currentBytes << " build_elapsed_ms=" << buildMs
            << " input_ms=" << stages.inputMs
            << " mapping_sort_ms=" << stages.mappingSortMs
            << " fact_sort_ms=" << stages.factSortMs
            << " expansion_ms=" << stages.expansionMs
            << " expanded_sort_ms=" << stages.expandedSortMs
            << " use_sort_ms=" << stages.useSortMs
            << " summary_ms=" << stages.summaryMs
            << " allocation_sort_ms=" << stages.allocationSortMs
            << " allocation_aggregate_ms=" << stages.allocationAggregateMs
            << " validation_requested=" << validate
            << " validation_elapsed_ms=" << validationMs
            << " validation_telemetry_total_ms=" << validationStats.totalMs
            << " validation_paged_records_ms=" << validationStats.pagedRecordsMs
            << " validation_semantic_compare_ms=" << validationStats.semanticCompareMs
            << " validation_summary_block_loads=" << validationStats.summaryBlockLoads
            << " validation_range_block_loads=" << validationStats.rangeBlockLoads
            << " validation_use_block_loads=" << validationStats.useBlockLoads
            << " validation_evidence_block_loads=" << validationStats.evidenceBlockLoads
            << " validation_summary_decoded_bytes=" << validationStats.summaryDecodedBytes
            << " validation_range_decoded_bytes=" << validationStats.rangeDecodedBytes
            << " validation_use_decoded_bytes=" << validationStats.useDecodedBytes
            << " validation_evidence_decoded_bytes=" << validationStats.evidenceDecodedBytes
            << " resource_allocation_callback_calls=" << input.resourceAllocationCalls
            << " allocation_end_callback_calls=" << input.allocationEndCalls
            << " fact_runs=" << stages.factSort.initialRuns
            << " fact_merges=" << stages.factSort.mergePasses
            << " expanded_runs=" << stages.expandedSort.initialRuns
            << " expanded_merges=" << stages.expandedSort.mergePasses
            << " allocation_runs=" << stages.allocationSort.initialRuns
            << " allocation_merges=" << stages.allocationSort.mergePasses << '\n';
        if( inventory )
            for( const auto& line : inventoryRows ) std::cout << line << '\n';
        catalog.reset();
        std::error_code cleanup;
        fs::remove_all( root, cleanup );
        if( cleanup ) throw std::runtime_error( "GPU pass capacity cleanup: " + cleanup.message() );
        cleanup.clear();
        fs::remove_all( referenceRoot, cleanup );
        if( cleanup ) throw std::runtime_error( "GPU pass reference fixture cleanup: " + cleanup.message() );
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::remove_all( referenceRoot, ignored );
        return 1;
    }
}

}

int main( int argc, char** argv )
{
    if( argc == 5 && std::string_view( argv[1] ) == "--capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ) );
    if( argc == 5 && std::string_view( argv[1] ) == "--capacity-inventory" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ), true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ), false, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-capacity-validated" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, true, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--fused-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, false, false, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--fused-capacity-high-fanout" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, false, false, true, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--fused-catalog-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, true, false, true );
    if( argc != 1 ) return 2;
    try
    {
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / "tracy-query-gpu-pass-resource-store";
        const auto catalogRoot = root / "catalog";
        const auto referenceRoot = root / "references";
        const auto passRoot = root / "passes";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::create_directories( root );
        const std::string identity( 64, 'p' );
        {
            QueryCatalogStoreWriter writer( catalogRoot, identity );
            writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
            JnGpuCatalogResourceRecordV1 resources[2] {};
            resources[0].time = 1; resources[0].resourceId = 100; resources[0].pointerToken = 900;
            resources[0].allocationId = 500;
            resources[1].time = 2; resources[1].resourceId = 102; resources[1].pointerToken = 901;
            resources[1].allocationId = 600;
            writer.Begin( OfflineCatalogDomain::CatalogResource, 2, sizeof( resources[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogResource, 0, resources, 2, sizeof( resources[0] ) );
            writer.End( OfflineCatalogDomain::CatalogResource );
            JnGpuCatalogAllocationRecordV1 allocations[3] {};
            allocations[0].time = 1; allocations[0].allocationId = 500; allocations[0].sizeBytes = 1000;
            allocations[1].time = 2; allocations[1].allocationId = 600; allocations[1].sizeBytes = 2000;
            allocations[2].time = 3; allocations[2].allocationId = 500;
            allocations[2].operation = uint8_t( JnGpuCatalogRecordOperation::Destroy );
            allocations[2].sizeBytes = 1000;
            writer.Begin( OfflineCatalogDomain::CatalogAllocation, 3, sizeof( allocations[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogAllocation, 0, allocations, 3, sizeof( allocations[0] ) );
            writer.End( OfflineCatalogDomain::CatalogAllocation );
            JnGpuRangeSetRecordV1 ranges[3] {};
            ranges[0].passInstanceId = 10; ranges[0].resourceId = 100; ranges[0].lengthBytes = 64;
            ranges[1].passInstanceId = 20; ranges[1].resourceId = 102; ranges[1].lengthBytes = 128;
            ranges[2].passInstanceId = 10; ranges[2].resourceId = 999; ranges[2].lengthBytes = 32;
            writer.Begin( OfflineCatalogDomain::CatalogRangeSet, 3, sizeof( ranges[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogRangeSet, 0, ranges, 3, sizeof( ranges[0] ) );
            writer.End( OfflineCatalogDomain::CatalogRangeSet );
            JnGpuCatalogBatchData batches[3] {};
            batches[0].generation = 1; batches[0].recordCount = 2;
            batches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
            batches[1].generation = 2; batches[1].recordCount = 3;
            batches[1].kind = uint8_t( JnGpuCatalogBatchKind::Allocation );
            batches[2].generation = 3; batches[2].recordCount = 3;
            batches[2].kind = uint8_t( JnGpuCatalogBatchKind::RangeSet );
            writer.Begin( OfflineCatalogDomain::CatalogBatch, 3, sizeof( batches[0] ) );
            writer.Append( OfflineCatalogDomain::CatalogBatch, 0, batches, 3, sizeof( batches[0] ) );
            writer.End( OfflineCatalogDomain::CatalogBatch );
            writer.Finish();
        }
        QueryCatalogStoreReader catalog( catalogRoot, identity );
        catalog.BuildResourceIndex( catalogRoot / "resource-index-scratch" );
        uint64_t visitedAllocations = 0;
        catalog.VisitRawAllocations( [&]( uint64_t ordinal, const JnGpuCatalogAllocationRecordV1& ) {
            Require( ordinal == visitedAllocations, "Catalog allocation stream ordinal changed" );
            visitedAllocations++;
        } );
        Require( visitedAllocations == 3, "Catalog allocation stream missed records" );
        ReferenceInput referenceInput;
        referenceInput.passes = { { 10, 10, 0, 1, 0, 0, 0 }, { 20, 20, 0, 1, 0, 0, 0 } };
        referenceInput.ends = { { 15, 10, 0, 1, 0, 0, 0 }, { 25, 20, 0, 1, 0, 0, 0 } };
        referenceInput.uses = {
            { 11, 10, 900, 1, 3, 7, 0, 2 },
            { 21, 20, 901, 1, 5, 8, 0, 2 }
        };
        QueryGpuReferenceStore::Build( referenceRoot, identity, referenceInput );
        QueryGpuReferenceStoreReader references( referenceRoot, identity );
        PassInput input( catalog, references );
        input.scratchRoot = passRoot / "pass-resource-scratch";
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            256ull * 1024 * 1024, 128ull * 1024 * 1024 );
        analysis::PagedRecordOptions budgeted;
        budgeted.workspace = workspace;
        QueryGpuPassResourceStore::Build( passRoot, identity, input, budgeted, nullptr,
            QueryGpuPassResourceStore::BuildMode::Legacy );
        Require( workspace->Snapshot().currentBytes == 0 && workspace->Snapshot().peakBytes != 0,
            "GPU pass build leaked or skipped shared workspace accounting" );
        Require( input.resourceAllocationCalls == 3 && input.allocationBytesCalls == 2 &&
            input.rangeCalls == input.RangeCount(),
            "GPU pass resource build repeated identical Catalog state lookups" );
        Require( !fs::exists( passRoot / "pass-resource-scratch" ),
            "GPU pass resource scratch was retained" );
        QueryGpuPassResourceStoreReader reader( passRoot, catalogRoot, referenceRoot, identity );
        reader.ValidateAll();
        reader.ValidateRelationsExact( passRoot / "exact-validation-scratch" );
        reader.ValidateMappingSources( input, passRoot / "mapping-validation-scratch" );
        {
            PassInput missingMapping( catalog, references );
            missingMapping.mappings.erase( std::remove_if( missingMapping.mappings.begin(),
                missingMapping.mappings.end(), []( const auto& row ) {
                    return row.evidencePassId == 20 && row.queryPassId == 1000;
                } ), missingMapping.mappings.end() );
            const auto scratch = passRoot / "missing-mapping-validation-scratch";
            bool rejected = false;
            try { reader.ValidateMappingSources( missingMapping, scratch ); }
            catch( const std::exception& error )
            { rejected = std::string_view( error.what() ).find( "mapping source" ) != std::string_view::npos; }
            Require( rejected && !fs::exists( scratch ),
                "GPU pass source mapping omission was accepted" );
        }
        {
            analysis::PagedRecordOptions cancelledOptions;
            bool cancel = false;
            cancelledOptions.cancelled = [&] { return cancel; };
            QueryGpuPassResourceStoreReader cancelled( passRoot, catalogRoot, referenceRoot,
                identity, cancelledOptions );
            const auto scratch = passRoot / "exact-cancelled-scratch";
            bool stopped = false;
            cancel = true;
            try { cancelled.ValidateRelationsExact( scratch ); }
            catch( const std::exception& ) { stopped = true; }
            Require( stopped && !fs::exists( scratch ),
                "GPU pass exact validation cancellation kept scratch or succeeded" );
        }
        {
            const auto scratch = passRoot / "exact-disk-refused-scratch";
            auto usage = std::make_shared<analysis::AnalysisDiskUsage>( std::vector<fs::path>{ scratch } );
            auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
            disk->usage = usage; disk->maximumBytes = 1;
            analysis::PagedRecordOptions refusedOptions;
            refusedOptions.disk = disk;
            QueryGpuPassResourceStoreReader refused( passRoot, catalogRoot, referenceRoot,
                identity, refusedOptions );
            bool stopped = false;
            try { refused.ValidateRelationsExact( scratch ); }
            catch( const std::exception& ) { stopped = true; }
            Require( stopped && !fs::exists( scratch ) && usage->Snapshot().currentBytes == 0,
                "GPU pass exact validation disk rejection kept scratch or succeeded" );
        }
        const auto passIdentity = [&]( std::string_view kind ) {
            analysis::Sha256Builder hash;
            hash.Update( identity.data(), identity.size() );
            hash.Update( kind.data(), kind.size() );
            constexpr std::string_view version = "query-gpu-pass-resource-v1";
            hash.Update( version.data(), version.size() );
            return hash.FinalHex();
        };
        const auto corruptDetail = [&]( std::string_view kind, uint64_t rowOrdinal,
            uint64_t replacementMetadata, std::string_view expectedError ) {
            const auto corruptRoot = root / ( "corrupt-" + std::string( kind ) + "-" +
                std::to_string( rowOrdinal ) );
            fs::copy( passRoot, corruptRoot, fs::copy_options::recursive );
            const auto detail = corruptRoot / std::string( kind );
            const auto replacement = corruptRoot / "replacement";
            const auto storeIdentity = passIdentity( kind );
            {
                analysis::PagedRecordReader original( detail, storeIdentity,
                    sizeof( analysis::AnalysisKeyMetadataRecord ) );
                analysis::PagedRecordWriter changed( replacement, storeIdentity,
                    sizeof( analysis::AnalysisKeyMetadataRecord ) );
                Require( rowOrdinal < original.Count(), "GPU pass corruption fixture ordinal is absent" );
                for( uint64_t index = 0; index < original.Count(); index++ )
                {
                    analysis::AnalysisKeyMetadataRecord row {};
                    original.Read( index, &row, 1 );
                    if( index == rowOrdinal ) row.metadata = row.keyLow = replacementMetadata;
                    changed.Append( &row, 1 );
                }
                changed.Commit();
            }
            fs::remove_all( detail );
            fs::rename( replacement, detail );
            bool rejected = false;
            try
            {
                QueryGpuPassResourceStoreReader corrupted( corruptRoot, catalogRoot,
                    referenceRoot, identity );
                corrupted.ValidateRelationsExact( corruptRoot / "exact-validation-scratch" );
            }
            catch( const std::exception& error )
            { rejected = std::string_view( error.what() ).find( expectedError ) != std::string_view::npos; }
            Require( rejected, "GPU pass valid-checksum semantic corruption was accepted" );
            fs::remove_all( corruptRoot );
        };
        corruptDetail( "pass-resource-ranges", 0, 2, "Range exact" );
        corruptDetail( "pass-resource-uses", 0, 1, "Use exact" );
        {
            const auto corruptRoot = root / "corrupt-evidence-direct-range";
            fs::copy( passRoot, corruptRoot, fs::copy_options::recursive );
            const auto detail = corruptRoot / "pass-resource-evidence-ids";
            const auto replacement = corruptRoot / "replacement";
            const auto storeIdentity = passIdentity( "pass-resource-evidence-ids" );
            {
                analysis::PagedRecordReader original( detail, storeIdentity,
                    sizeof( analysis::AnalysisKeyMetadataRecord ) );
                analysis::PagedRecordWriter changed( replacement, storeIdentity,
                    sizeof( analysis::AnalysisKeyMetadataRecord ) );
                for( uint64_t index = 0; index < original.Count(); index++ )
                {
                    analysis::AnalysisKeyMetadataRecord row {};
                    original.Read( index, &row, 1 );
                    if( index == 0 ) row.keyLow = 20;
                    changed.Append( &row, 1 );
                }
                changed.Commit();
            }
            fs::remove_all( detail );
            fs::rename( replacement, detail );
            bool rejected = false;
            try
            {
                QueryGpuPassResourceStoreReader corrupted( corruptRoot, catalogRoot,
                    referenceRoot, identity );
                corrupted.ValidateRelationsExact( corruptRoot / "exact-validation-scratch" );
            }
            catch( const std::exception& error )
            { rejected = std::string_view( error.what() ).find( "direct mapping missing" ) != std::string_view::npos; }
            Require( rejected, "GPU pass direct Range mapping corruption was accepted" );
            fs::remove_all( corruptRoot );
        }
        {
            const auto unresolvedReferenceRoot = root / "unresolved-references";
            const auto unresolvedPassRoot = root / "unresolved-passes";
            ReferenceInput unresolved = referenceInput;
            unresolved.passes.push_back( { 30, 30, 0, 1, 0, 0, 0 } );
            unresolved.ends.push_back( { 35, 30, 0, 1, 0, 0, 0 } );
            unresolved.uses.push_back( { 31, 30, 999, 1, 7, 9, 0, 2 } );
            QueryGpuReferenceStore::Build( unresolvedReferenceRoot, identity, unresolved );
            QueryGpuReferenceStoreReader unresolvedReferences( unresolvedReferenceRoot, identity );
            PassInput unresolvedInput( catalog, unresolvedReferences );
            QueryGpuPassResourceStore::Build( unresolvedPassRoot, identity, unresolvedInput,
                budgeted, nullptr, QueryGpuPassResourceStore::BuildMode::Fused );
            QueryGpuPassResourceStoreReader unresolvedReader( unresolvedPassRoot, catalogRoot,
                unresolvedReferenceRoot, identity, budgeted );
            unresolvedReader.ValidateRelationsExact( unresolvedPassRoot / "exact-validation-scratch" );
            Require( !unresolvedReader.Summary( 30 ),
                "GPU pass unresolved Catalog pointer was silently materialized" );
        }
        Require( reader.SummaryCount() == 3 && reader.SummaryAt( 0 ).passId == 10 &&
            reader.SummaryAt( 1 ).passId == 20 && reader.SummaryAt( 2 ).passId == 1000,
            "GPU pass summary ordinal scan changed count or order" );
        const auto summary = reader.Summary( 1000 );
        Require( summary && summary->resourceCount == 3 && summary->allocationCount == 2 &&
            summary->directRangeBytes == 224 && summary->referencedPhysicalBytes == 2000 &&
            summary->rangedResourceCount == 3 && summary->rangeCount == 3 && summary->useCount == 2,
            "GPU pass aggregate summary changed" );
        const auto ranges = reader.Ranges( 1000, 0, 10 );
        const auto uses = reader.Uses( 1000, 0, 10 );
        const auto evidence = reader.EvidencePassIds( 1000, 0, 10 );
        Require( ranges.total == 3 && ranges.values.size() == 3 &&
            ranges.values[0].record.passInstanceId == 10 && ranges.values[1].record.passInstanceId == 20 &&
            ranges.values[2].record.resourceId == 999 &&
            uses.total == 2 && uses.values.size() == 2 && uses.values[0].passId == 10 &&
            uses.values[1].passId == 20 && evidence.total == 2 && evidence.values.size() == 2 &&
            evidence.values[0] == 10 && evidence.values[1] == 20 &&
            reader.Summary( 999 ) == std::nullopt,
            "GPU pass range/use expansion or missing-key semantics changed" );
        const auto direct = reader.Summary( 10 );
        Require( direct && direct->resourceCount == 2 && direct->directRangeBytes == 96 &&
            direct->referencedPhysicalBytes == 0,
            "GPU reference pass identity mapping changed" );
        const auto fusedRoot = root / "passes-fused";
        QueryGpuPassResourceStore::Build( fusedRoot, identity, input, budgeted, nullptr,
            QueryGpuPassResourceStore::BuildMode::Fused );
        QueryGpuPassResourceStoreReader fusedReader( fusedRoot, catalogRoot, referenceRoot, identity );
        fusedReader.ValidateAll();
        Require( fusedReader.SummaryCount() == reader.SummaryCount(),
            "GPU pass fused summary count changed" );
        for( uint64_t ordinal = 0; ordinal < reader.SummaryCount(); ordinal++ )
        {
            const auto expected = reader.SummaryAt( ordinal );
            const auto actual = fusedReader.SummaryAt( ordinal );
            Require( expected.passId == actual.passId &&
                expected.resourceCount == actual.resourceCount &&
                expected.allocationCount == actual.allocationCount &&
                expected.directRangeBytes == actual.directRangeBytes &&
                expected.referencedPhysicalBytes == actual.referencedPhysicalBytes &&
                expected.rangedResourceCount == actual.rangedResourceCount &&
                expected.rangeCount == actual.rangeCount && expected.useCount == actual.useCount,
                "GPU pass fused summary semantics changed" );
            const auto expectedRanges = reader.Ranges( expected.passId, 0, 1024 );
            const auto actualRanges = fusedReader.Ranges( expected.passId, 0, 1024 );
            const auto expectedUses = reader.Uses( expected.passId, 0, 1024 );
            const auto actualUses = fusedReader.Uses( expected.passId, 0, 1024 );
            const auto expectedEvidence = reader.EvidencePassIds( expected.passId, 0, 1024 );
            const auto actualEvidence = fusedReader.EvidencePassIds( expected.passId, 0, 1024 );
            Require( expectedRanges.total == actualRanges.total &&
                expectedUses.total == actualUses.total &&
                expectedEvidence.values == actualEvidence.values,
                "GPU pass fused detail count or evidence semantics changed" );
            for( size_t index = 0; index < expectedRanges.values.size(); index++ )
                Require( expectedRanges.values[index].record.passInstanceId ==
                    actualRanges.values[index].record.passInstanceId &&
                    expectedRanges.values[index].record.resourceId ==
                    actualRanges.values[index].record.resourceId,
                    "GPU pass fused range order changed" );
            for( size_t index = 0; index < expectedUses.values.size(); index++ )
                Require( expectedUses.values[index].passId == actualUses.values[index].passId &&
                    expectedUses.values[index].resourceId == actualUses.values[index].resourceId,
                    "GPU pass fused use order changed" );
        }
        PassInput manyPasses( catalog, references );
        manyPasses.mappings.clear();
        for( uint64_t index = 0; index < 512; index++ )
            manyPasses.mappings.push_back( { 10, 10000 + index } );
        analysis::PagedRecordOptions smallPages;
        smallPages.blockBytes = 4096;
        const auto manyPassRoot = root / "many-passes";
        auto diskUsage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<fs::path> { manyPassRoot } );
        auto diskBudget = std::make_shared<analysis::AnalysisDiskBudget>();
        diskBudget->usage = diskUsage;
        diskBudget->maximumBytes = 150000;
        smallPages.disk = diskBudget;
        QueryGpuPassResourceStore::Build( manyPassRoot, identity, manyPasses, smallPages );
        Require( diskUsage->Snapshot().peakBytes <= diskBudget->maximumBytes &&
            diskUsage->Snapshot().rejectedGrowths == 0 &&
            diskUsage->Snapshot().currentBytes == analysis::AnalysisDiskUsage::Bytes( manyPassRoot ),
            "GPU pass build retained consumed scratch beyond the shared disk budget" );
        QueryGpuPassResourceStoreReader manyPassReader( manyPassRoot, catalogRoot, referenceRoot,
            identity, smallPages );
        Require( manyPassReader.SummaryCount() == 514, "GPU pass fixture did not span pages" );
        const auto readBefore = manyPassReader.ReadStatistics();
        const auto validationStats = manyPassReader.ValidateAll();
        const auto readAfter = manyPassReader.ReadStatistics();
        Require( validationStats.totalMs >= validationStats.pagedRecordsMs +
                validationStats.semanticCompareMs &&
            validationStats.rangeBlockLoads == readAfter.rangeBlockLoads - readBefore.rangeBlockLoads &&
            validationStats.useBlockLoads == readAfter.useBlockLoads - readBefore.useBlockLoads &&
            validationStats.rangeDecodedBytes != 0 && validationStats.useDecodedBytes != 0 &&
            readAfter.rangeBlockLoads - readBefore.rangeBlockLoads < 100 &&
            readAfter.useBlockLoads - readBefore.useBlockLoads < 100,
            "GPU pass deep validation telemetry or bounded lookup reads changed" );
        {
            const auto fusedCancelRoot = root / "fused-cancelled";
            GeneratedPassInput cancelInput( 100000, 10000 );
            analysis::PagedRecordOptions cancelOptions;
            cancelOptions.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                256ull << 20, 128ull << 20 );
            uint64_t cancellationPolls = 0;
            cancelOptions.cancelled = [&] { return ++cancellationPolls > 12; };
            bool cancelled = false;
            try
            {
                QueryGpuPassResourceStore::Build( fusedCancelRoot, identity, cancelInput,
                    cancelOptions, nullptr, QueryGpuPassResourceStore::BuildMode::Fused );
            }
            catch( const std::exception& error )
            { cancelled = std::string_view( error.what() ).find( "cancelled" ) != std::string_view::npos; }
            Require( cancelled && !fs::exists( fusedCancelRoot / "pass-resource-scratch" ) &&
                !fs::exists( fusedCancelRoot / "pass-resource-summary" ),
                "GPU pass fused cancellation retained scratch or published output" );
            fs::remove_all( fusedCancelRoot, ignored );
        }
        {
            const auto fusedWorkspaceRoot = root / "fused-workspace-refusal";
            GeneratedPassInput refusalInput( 100000, 100000 );
            analysis::PagedRecordOptions refusalOptions;
            refusalOptions.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                64ull << 10, 32ull << 10 );
            bool refused = false;
            try
            {
                QueryGpuPassResourceStore::Build( fusedWorkspaceRoot, identity, refusalInput,
                    refusalOptions, nullptr, QueryGpuPassResourceStore::BuildMode::Fused );
            }
            catch( const std::exception& ) { refused = true; }
            Require( refused && refusalOptions.workspace->Snapshot().rejectedReservations != 0 &&
                refusalOptions.workspace->Snapshot().currentBytes == 0 &&
                !fs::exists( fusedWorkspaceRoot / "pass-resource-scratch" ) &&
                !fs::exists( fusedWorkspaceRoot / "pass-resource-summary" ),
                "GPU pass fused workspace refusal was late or leaked state" );
            fs::remove_all( fusedWorkspaceRoot, ignored );
        }
        {
            const auto fusedDiskRoot = root / "fused-disk-refusal";
            GeneratedPassInput refusalInput( 100000, 10000 );
            auto refusalUsage = std::make_shared<analysis::AnalysisDiskUsage>(
                std::vector<fs::path>{ fusedDiskRoot } );
            analysis::PagedRecordOptions refusalOptions;
            refusalOptions.disk = std::make_shared<analysis::AnalysisDiskBudget>(
                analysis::AnalysisDiskBudget { refusalUsage, 4096, {} } );
            refusalOptions.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                256ull << 20, 128ull << 20 );
            bool refused = false;
            try
            {
                QueryGpuPassResourceStore::Build( fusedDiskRoot, identity, refusalInput,
                    refusalOptions, nullptr, QueryGpuPassResourceStore::BuildMode::Fused );
            }
            catch( const std::exception& ) { refused = true; }
            Require( refused && refusalUsage->Snapshot().rejectedGrowths != 0 &&
                refusalUsage->Snapshot().currentBytes == 0 &&
                !fs::exists( fusedDiskRoot / "pass-resource-scratch" ) &&
                !fs::exists( fusedDiskRoot / "pass-resource-summary" ),
                "GPU pass fused disk refusal was late or leaked state" );
            fs::remove_all( fusedDiskRoot, ignored );
        }
        {
            const auto cancelRoot = root / "cancelled-summary";
            GeneratedPassInput cancelInput( 100000, 10000 );
            analysis::PagedRecordOptions cancelOptions;
            cancelOptions.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                256ull << 20, 128ull << 20 );
            cancelOptions.cancelled = [&] { return cancelInput.resourceAllocationCalls >= 10; };
            bool cancelled = false;
            try
            {
                QueryGpuPassResourceStore::Build( cancelRoot, identity, cancelInput, cancelOptions,
                    nullptr, QueryGpuPassResourceStore::BuildMode::Legacy );
            }
            catch( const std::exception& error )
            { cancelled = std::string_view( error.what() ).find( "cancelled" ) != std::string_view::npos; }
            Require( cancelled && cancelInput.resourceAllocationCalls < 5000 &&
                !fs::exists( cancelRoot / "pass-resource-scratch" ),
                "GPU pass summary ignored cancellation until after a full fact-table scan" );
        }
        fs::remove_all( root, ignored );
        std::cout << "query GPU pass resource store passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
