#include "TracyQueryGpuReferenceStore.hpp"
#include "TracyQueryCatalogStore.hpp"
#include "TracyOfflineCatalog.hpp"
#include "TracyQueue.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyHash.hpp"
#include "TracyPagedRecordStore.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

class Input final : public tracy::query::QueryGpuReferenceInput
{
public:
    uint64_t PassCount() const override { return passes.size(); }
    uint64_t UseCount() const override { return uses.size(); }
    uint64_t EndCount() const override { return ends.size(); }
    tracy::JnGpuReferencePassData Pass( uint64_t ordinal ) const override { return passes.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceUseData Use( uint64_t ordinal ) const override { return uses.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceEndData End( uint64_t ordinal ) const override { return ends.at( size_t( ordinal ) ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    {
        if( pointerToken != 100 ) return 0;
        return time <= 20 ? 200 : 201;
    }

    std::vector<tracy::JnGpuReferencePassData> passes;
    std::vector<tracy::JnGpuReferenceUseData> uses;
    std::vector<tracy::JnGpuReferenceEndData> ends;
};

class GeneratedInput final : public tracy::query::QueryGpuReferenceInput
{
public:
    GeneratedInput( uint64_t useCount, uint64_t resourceCount,
        const tracy::query::QueryCatalogStoreReader* catalog = nullptr )
        : m_uses( useCount ), m_resources( resourceCount ), m_catalog( catalog ) {}

    uint64_t PassCount() const override { return ( m_uses + 31 ) / 32; }
    uint64_t UseCount() const override { return m_uses; }
    uint64_t EndCount() const override { return PassCount(); }
    tracy::JnGpuReferencePassData Pass( uint64_t ordinal ) const override
    { return { int64_t( ordinal * 100 ), ordinal + 1, ordinal, 1, 1, 0, 0 }; }
    tracy::JnGpuReferenceUseData Use( uint64_t ordinal ) const override
    {
        const auto passId = ordinal % PassCount() + 1;
        return { int64_t( ( passId - 1 ) * 100 + 1 ), passId,
            ordinal % m_resources + 1, 1, 1, 1, 0, 1 };
    }
    tracy::JnGpuReferenceEndData End( uint64_t ordinal ) const override
    { return { int64_t( ordinal * 100 + 99 ), ordinal + 1, 0, 1, 32, 0, 0 }; }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    { return m_catalog ? m_catalog->ResolveResourcePointer( pointerToken, time ) : pointerToken; }

private:
    uint64_t m_uses;
    uint64_t m_resources;
    const tracy::query::QueryCatalogStoreReader* m_catalog;
};

int CapacityProbe( uint64_t useCount, uint64_t resourceCount, uint64_t maximumBytes,
    bool inventory = false, bool realCatalog = false, bool shuffledCatalog = false,
    bool shuffledAllocation = false, bool multiDomain = false, bool validate = false )
{
    namespace fs = std::filesystem;
    using namespace tracy;
    using namespace tracy::analysis;
    using namespace tracy::query;
    if( useCount < 100000 || useCount > 34000000 || resourceCount == 0 ||
        resourceCount > useCount || maximumBytes == 0 || maximumBytes > ( 20ull << 30 ) ) return 2;
    const auto root = fs::temp_directory_path() /
        ( "query-gpu-reference-capacity-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
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
        uint64_t nextSample = 16ull << 20;
        if( inventory ) options.disk->checkSpace = [&] {
            const auto current = usage->Current();
            if( current < nextSample ) return;
            nextSample = current + ( 16ull << 20 );
            const auto scratch = root / "reverse-scratch";
            uint64_t source = 0, sorted = 0, runs = 0, other = 0;
            std::string active;
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
                    if( bytes != 0 && ( name.ends_with( "-source.work" ) ||
                        name.ends_with( "-sorted.work" ) ) ) active += name + ',';
                }
            std::ostringstream line;
            line << "gpu_reference_inventory elapsed_ms=" <<
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started ).count()
                << " ledger=" << current << " source=" << source
                << " sorted=" << sorted << " runs=" << runs << " other=" << other
                << " active=" << active;
            inventoryRows.emplace_back( line.str() );
        };
        bool sawUsePassSource = false;
        bool sawResourceSource = false;
        bool sawCatalogResourceSource = false;
        uint64_t diskGrowthChecks = 0;
        const auto inventoryCheck = options.disk->checkSpace;
        options.disk->checkSpace = [&] {
            if( inventoryCheck ) inventoryCheck();
            if( ( ++diskGrowthChecks & 4095 ) == 0 &&
                fs::exists( root / "reverse-scratch" / "use-pass-source.work" ) )
                sawUsePassSource = true;
            if( ( diskGrowthChecks & 4095 ) == 0 &&
                fs::exists( root / "reverse-scratch" / "resource-source.work" ) )
                sawResourceSource = true;
            if( ( diskGrowthChecks & 4095 ) == 0 &&
                fs::exists( root / "reverse-scratch" / "catalog-resource-source.work" ) )
                sawCatalogResourceSource = true;
        };
        const std::string identity( 64, 'g' );
        std::unique_ptr<QueryCatalogStoreReader> catalog;
        uint64_t catalogBuildMs = 0;
        uint64_t catalogIndexMs = 0;
        QueryCatalogResourceIndexReadStatistics catalogReads;
        if( realCatalog )
        {
            const auto catalogStarted = std::chrono::steady_clock::now();
            const auto rawResourceCount = multiDomain ? useCount : resourceCount;
            const auto rawAllocationCount = multiDomain ? rawResourceCount * 359 / 1000 : resourceCount;
            const auto viewCount = multiDomain ? rawResourceCount * 224 / 1000 : 0;
            const auto relationCount = multiDomain ? rawResourceCount * 1111 / 1000 : 0;
            const auto rangeCount = multiDomain ? rawResourceCount * 928 / 1000 : 0;
            {
                QueryCatalogStoreWriter writer( root / "catalog", identity, options );
                writer.SetMetadata( { 1, 1, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion } );
                writer.Begin( OfflineCatalogDomain::CatalogResource, rawResourceCount,
                    sizeof( JnGpuCatalogResourceRecordV1 ) );
                AnalysisWorkspaceReservation resourceRowsMemory( options.workspace,
                    4096 * sizeof( JnGpuCatalogResourceRecordV1 ) );
                std::vector<JnGpuCatalogResourceRecordV1> rows( 4096 );
                uint64_t shuffleMultiplier = resourceCount / 2 + 1;
                while( std::gcd( shuffleMultiplier, resourceCount ) != 1 ) shuffleMultiplier++;
                for( uint64_t offset = 0; offset < rawResourceCount; )
                {
                    const auto take = std::min<uint64_t>( rows.size(), rawResourceCount - offset );
                    for( uint64_t index = 0; index < take; ++index )
                    {
                        auto& row = rows[size_t( index )];
                        row = {};
                        row.resourceId = multiDomain ? ( offset + index ) % resourceCount + 1 :
                            shuffledCatalog ?
                            ( ( offset + index ) * shuffleMultiplier ) % resourceCount + 1 :
                            offset + index + 1;
                        row.pointerToken = row.resourceId;
                        row.allocationId = ( shuffledAllocation || multiDomain ) ? row.resourceId : 0;
                        row.time = multiDomain ? int64_t( offset + index ) : 0;
                        row.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
                        row.capacityBytes = 64;
                    }
                    writer.Append( OfflineCatalogDomain::CatalogResource, offset, rows.data(), take,
                        sizeof( rows[0] ) );
                    offset += take;
                }
                writer.End( OfflineCatalogDomain::CatalogResource );
                if( shuffledAllocation || multiDomain )
                {
                    writer.Begin( OfflineCatalogDomain::CatalogAllocation, rawAllocationCount,
                        sizeof( JnGpuCatalogAllocationRecordV1 ) );
                    AnalysisWorkspaceReservation allocationRowsMemory( options.workspace,
                        4096 * sizeof( JnGpuCatalogAllocationRecordV1 ) );
                    std::vector<JnGpuCatalogAllocationRecordV1> allocations( 4096 );
                    for( uint64_t offset = 0; offset < rawAllocationCount; )
                    {
                        const auto take = std::min<uint64_t>( allocations.size(), rawAllocationCount - offset );
                        for( uint64_t index = 0; index < take; index++ )
                        {
                            auto& row = allocations[size_t( index )];
                            row = {};
                            row.allocationId = ( ( offset + index ) * shuffleMultiplier ) % resourceCount + 1;
                            row.pointerToken = row.allocationId;
                            row.time = multiDomain ? int64_t( offset + index ) : 0;
                            row.sizeBytes = 64;
                            row.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
                        }
                        writer.Append( OfflineCatalogDomain::CatalogAllocation, offset,
                            allocations.data(), take, sizeof( allocations[0] ) );
                        offset += take;
                    }
                    writer.End( OfflineCatalogDomain::CatalogAllocation );
                }
                if( multiDomain )
                {
                    writer.Begin( OfflineCatalogDomain::CatalogView, viewCount,
                        sizeof( JnGpuCatalogViewRecordV1 ) );
                    AnalysisWorkspaceReservation viewMemory( options.workspace,
                        4096 * sizeof( JnGpuCatalogViewRecordV1 ) );
                    std::vector<JnGpuCatalogViewRecordV1> views( 4096 );
                    for( uint64_t offset = 0; offset < viewCount; )
                    {
                        const auto take = std::min<uint64_t>( views.size(), viewCount - offset );
                        for( uint64_t index = 0; index < take; index++ )
                        {
                            auto& row = views[size_t( index )];
                            row = {};
                            row.time = int64_t( offset + index );
                            row.viewId = offset + index + 1;
                            row.resourceId = ( offset + index ) % resourceCount + 1;
                            row.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
                        }
                        writer.Append( OfflineCatalogDomain::CatalogView, offset,
                            views.data(), take, sizeof( views[0] ) );
                        offset += take;
                    }
                    writer.End( OfflineCatalogDomain::CatalogView );

                    writer.Begin( OfflineCatalogDomain::CatalogRelation, relationCount,
                        sizeof( JnGpuCatalogRelationRecordV1 ) );
                    AnalysisWorkspaceReservation relationMemory( options.workspace,
                        4096 * sizeof( JnGpuCatalogRelationRecordV1 ) );
                    std::vector<JnGpuCatalogRelationRecordV1> relations( 4096 );
                    for( uint64_t offset = 0; offset < relationCount; )
                    {
                        const auto take = std::min<uint64_t>( relations.size(), relationCount - offset );
                        for( uint64_t index = 0; index < take; index++ )
                        {
                            auto& row = relations[size_t( index )];
                            row = {};
                            row.time = int64_t( offset + index );
                            row.sourceId = ( offset + index ) % resourceCount + 1;
                            row.targetId = ( offset + index + 1 ) % resourceCount + 1;
                            row.operation = uint8_t( JnGpuCatalogRecordOperation::Bind );
                            row.relation = uint8_t( JnGpuCatalogRelationKind::RtasUses );
                        }
                        writer.Append( OfflineCatalogDomain::CatalogRelation, offset,
                            relations.data(), take, sizeof( relations[0] ) );
                        offset += take;
                    }
                    writer.End( OfflineCatalogDomain::CatalogRelation );

                    writer.Begin( OfflineCatalogDomain::CatalogRangeSet, rangeCount,
                        sizeof( JnGpuRangeSetRecordV1 ) );
                    AnalysisWorkspaceReservation rangeMemory( options.workspace,
                        4096 * sizeof( JnGpuRangeSetRecordV1 ) );
                    std::vector<JnGpuRangeSetRecordV1> ranges( 4096 );
                    for( uint64_t offset = 0; offset < rangeCount; )
                    {
                        const auto take = std::min<uint64_t>( ranges.size(), rangeCount - offset );
                        for( uint64_t index = 0; index < take; index++ )
                        {
                            auto& row = ranges[size_t( index )];
                            row = {};
                            row.passInstanceId = ( offset + index ) / 32 + 1;
                            row.resourceId = ( offset + index ) % resourceCount + 1;
                            row.pointerToken = row.resourceId;
                            row.lengthBytes = 64;
                        }
                        writer.Append( OfflineCatalogDomain::CatalogRangeSet, offset,
                            ranges.data(), take, sizeof( ranges[0] ) );
                        offset += take;
                    }
                    writer.End( OfflineCatalogDomain::CatalogRangeSet );
                }
                std::array<JnGpuCatalogBatchData, 5> batches {};
                batches[0].generation = 1;
                batches[0].kind = uint8_t( JnGpuCatalogBatchKind::Resource );
                batches[0].recordCount = rawResourceCount;
                if( shuffledAllocation || multiDomain )
                {
                    batches[1].generation = 1;
                    batches[1].kind = uint8_t( JnGpuCatalogBatchKind::Allocation );
                    batches[1].recordCount = rawAllocationCount;
                }
                if( multiDomain )
                {
                    const std::array<std::pair<JnGpuCatalogBatchKind, uint64_t>, 3> otherBatches {{
                        { JnGpuCatalogBatchKind::View, viewCount },
                        { JnGpuCatalogBatchKind::Relation, relationCount },
                        { JnGpuCatalogBatchKind::RangeSet, rangeCount }
                    }};
                    for( size_t index = 0; index < otherBatches.size(); index++ )
                    {
                        batches[index + 2].generation = 1;
                        batches[index + 2].kind = uint8_t( otherBatches[index].first );
                        batches[index + 2].recordCount = otherBatches[index].second;
                    }
                }
                const auto batchCount = multiDomain ? 5 : shuffledAllocation ? 2 : 1;
                writer.Begin( OfflineCatalogDomain::CatalogBatch, batchCount, sizeof( batches[0] ) );
                writer.Append( OfflineCatalogDomain::CatalogBatch, 0, batches.data(), batchCount,
                    sizeof( batches[0] ) );
                writer.End( OfflineCatalogDomain::CatalogBatch );
                writer.Finish();
            }
            catalog = std::make_unique<QueryCatalogStoreReader>( root / "catalog", identity, options );
            const auto indexStarted = std::chrono::steady_clock::now();
            auto phaseStarted = indexStarted;
            std::string currentPhase;
            const auto endPhase = [&]( std::chrono::steady_clock::time_point now ) {
                if( currentPhase.empty() ) return;
                std::cout << "catalog_index_phase=" << currentPhase << " elapsed_ms=" <<
                    std::chrono::duration_cast<std::chrono::milliseconds>( now - phaseStarted ).count()
                    << " disk_current_bytes=" << usage->Current() << std::endl;
            };
            catalog->BuildResourceIndex( root / "catalog" / "resource-index-scratch",
                [&]( std::string_view phase ) {
                    const auto now = std::chrono::steady_clock::now();
                    endPhase( now );
                    currentPhase = phase;
                    phaseStarted = now;
                }, &catalogReads );
            const auto indexEnded = std::chrono::steady_clock::now();
            endPhase( indexEnded );
            catalogIndexMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                indexEnded - indexStarted ).count();
            if( shuffledCatalog && resourceCount <= 2528691 )
            {
                const auto resourceBlocks =
                    ( resourceCount * sizeof( JnGpuCatalogResourceRecordV1 ) + options.blockBytes - 1 ) /
                    options.blockBytes;
                const auto generationBlocks =
                    ( resourceCount * sizeof( uint64_t ) + options.blockBytes - 1 ) /
                    options.blockBytes;
                Require( catalogReads.resourceBlockLoads <= resourceBlocks + 1 &&
                    catalogReads.generationBlockLoads <= generationBlocks + 1,
                    "shuffled Catalog resource index reread compressed pages by key" );
            }
            if( shuffledAllocation && resourceCount <= 1000000 )
            {
                const auto allocationBlocks =
                    ( resourceCount * sizeof( JnGpuCatalogAllocationRecordV1 ) + options.blockBytes - 1 ) /
                    options.blockBytes;
                const auto generationBlocks =
                    ( resourceCount * sizeof( uint64_t ) + options.blockBytes - 1 ) /
                    options.blockBytes;
                Require( catalogReads.allocationBlockLoads <= allocationBlocks + 1 &&
                    catalogReads.allocationGenerationBlockLoads <= generationBlocks + 1,
                    "shuffled Catalog allocation index reread compressed pages by key" );
                Require( catalog->AllocationSummaryCount() == resourceCount,
                    "shuffled Catalog allocation summaries lost a key" );
                for( const auto id : { uint64_t( 1 ), resourceCount } )
                {
                    const auto state = catalog->AllocationState( id );
                    const auto page = catalog->ResourceAllocations( id, 0, 1 );
                    Require( state && state->record.allocationId == id &&
                        state->record.sizeBytes == 64 && page.total == 1 &&
                        page.values.size() == 1 && page.values.front().record.allocationId == id,
                        "shuffled Catalog allocation lookup changed identity or association" );
                }
                const auto memory = catalog->GpuMemorySummary();
                Require( memory.livePhysicalBytes == resourceCount * 64 &&
                    memory.peakPhysicalBytes == resourceCount * 64 &&
                    memory.allocationCreateCount == resourceCount,
                    "shuffled Catalog allocation memory summary changed" );
            }
            if( multiDomain )
            {
                Require( catalog->ResourceIndexCount() == resourceCount &&
                    catalog->AllocationSummaryCount() == resourceCount,
                    "multi-domain Catalog index lost Resource or Allocation keys" );
                const auto observed = usage->Snapshot();
                // This distribution exceeded 900 bytes per Resource row when all sort
                // intermediates survived until the final cleanup.
                Require( observed.peakBytes <= useCount * 500,
                    "multi-domain Catalog index retained completed sort intermediates" );
                std::cout << "catalog_multidomain_resource_rows=" << rawResourceCount
                    << " resource_keys=" << resourceCount
                    << " allocation_rows=" << rawAllocationCount
                    << " view_rows=" << viewCount
                    << " relation_rows=" << relationCount
                    << " range_rows=" << rangeCount
                    << " catalog_index_ms=" << catalogIndexMs
                    << " resource_cache_bytes=" << catalogReads.resourceCacheBytes
                    << " resource_block_loads=" << catalogReads.resourceBlockLoads
                    << " allocation_cache_bytes=" << catalogReads.allocationCacheBytes
                    << " allocation_block_loads=" << catalogReads.allocationBlockLoads
                    << " peak_bytes=" << observed.peakBytes
                    << " final_bytes=" << observed.currentBytes << '\n';
                catalog.reset();
                std::error_code cleanup;
                fs::remove_all( root, cleanup );
                if( cleanup ) throw std::runtime_error( "Catalog multi-domain cleanup: " + cleanup.message() );
                return 0;
            }
            if( !catalog->PreparePointerResolutionFastPath() )
                throw std::runtime_error( "GPU reference capacity Catalog pointer fast path unavailable" );
            catalogBuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - catalogStarted ).count();
        }
        const auto sourceBytes = usage->Snapshot().currentBytes;
        GeneratedInput input( useCount, resourceCount, catalog.get() );
        const auto buildStarted = std::chrono::steady_clock::now();
        QueryGpuReferenceBuildStatistics stages;
        QueryGpuReferenceStore::Build( root, identity, input, options, &stages );
        const auto buildObserved = usage->Snapshot();
        Require( !sawUsePassSource,
            "GPU reference use/pass sorting still writes a complete source copy" );
        Require( !sawResourceSource,
            "GPU reference Resource and Catalog sorting still replay a complete source copy" );
        Require( !sawCatalogResourceSource,
            "GPU reference Catalog sorting still writes a complete source copy" );
        const auto buildMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted ).count();
        const auto measuredStages = stages.passEventSortMs + stages.passTimePublishMs +
            stages.usePassSortMs + stages.resolveUsesMs + stages.resourceSortMs +
            stages.resourcePublishMs + stages.catalogReplayMs + stages.catalogSortMs +
            stages.catalogPublishMs;
        Require( measuredStages != 0 && measuredStages <= uint64_t( buildMilliseconds ),
            "GPU reference capacity phase times do not reconcile with Build wall time" );
        Require( stages.passEventSort.inputRecords == 2 * input.PassCount() &&
            stages.usePassSort.inputRecords == useCount &&
            stages.resourceSort.inputRecords == useCount &&
            stages.catalogSort.inputRecords == useCount &&
            stages.usePassSort.initialRunWriteBytes == useCount * sizeof( AnalysisUInt64Pair ) &&
            stages.catalogSort.initialRunWriteBytes == useCount *
                sizeof( tracy::query::QueryGpuReferenceLookupRecord ),
            "GPU reference sort telemetry lost or double-counted logical facts" );
        QueryGpuReferenceValidationStats validationStats;
        tracy::analysis::AnalysisDiskUsageSnapshot validationObserved {};
        bool validationComplete = false;
        {
            QueryGpuReferenceStoreReader reader( root, identity, options );
            Require( reader.UseCount() == useCount, "GPU reference capacity use count changed" );
            const auto first = reader.UsesByPass( 1, 0, 1 );
            const auto last = reader.UsesByPass( input.PassCount(), 0, 1 );
            Require( first.total != 0 && last.total != 0 &&
                first.values.front().passId == 1 && last.values.front().passId == input.PassCount(),
                "GPU reference capacity pass identity changed" );
            if( validate || useCount <= 1000000 )
            {
                const auto validationScratch = root / "capacity-validation-scratch";
                auto validationUsage = std::make_shared<AnalysisDiskUsage>(
                    std::vector<fs::path> { validationScratch } );
                auto validationOptions = options;
                validationOptions.disk = std::make_shared<AnalysisDiskBudget>(
                    AnalysisDiskBudget { validationUsage, maximumBytes, {} } );
                QueryGpuReferenceStoreReader validationReader( root, identity, validationOptions );
                validationStats = validationReader.ValidateAll( [&]( uint64_t token, int64_t time ) {
                    return input.ResolveCatalogResource( token, time );
                }, validationScratch );
                validationObserved = validationUsage->Snapshot();
                validationComplete = true;
                Require( validationObserved.currentBytes == 0 && !fs::exists( validationScratch ),
                    "GPU reference capacity validation retained scratch" );
            }
        }
        const auto observed = usage->Snapshot();
        const auto validationCombinedPeak = validationObserved.peakBytes > UINT64_MAX - observed.currentBytes ?
            UINT64_MAX : observed.currentBytes + validationObserved.peakBytes;
        const auto buildPlusValidationPeak = validationComplete ?
            std::max( buildObserved.peakBytes, validationCombinedPeak ) : buildObserved.peakBytes;
        std::cout << "gpu_reference_uses=" << useCount << " passes=" << input.PassCount()
            << " resource_keys=" << resourceCount << " real_catalog=" << realCatalog
            << " shuffled_catalog=" << shuffledCatalog
            << " shuffled_allocation=" << shuffledAllocation
            << " multi_domain=" << multiDomain
            << " catalog_resource_cache_bytes=" << catalogReads.resourceCacheBytes
            << " catalog_build_ms=" << catalogBuildMs << " catalog_index_ms=" << catalogIndexMs
            << " catalog_resource_block_loads=" << catalogReads.resourceBlockLoads
            << " catalog_resource_decoded_bytes=" << catalogReads.resourceDecodedBytes
            << " catalog_generation_block_loads=" << catalogReads.generationBlockLoads
            << " catalog_generation_decoded_bytes=" << catalogReads.generationDecodedBytes
            << " catalog_allocation_cache_bytes=" << catalogReads.allocationCacheBytes
            << " catalog_allocation_block_loads=" << catalogReads.allocationBlockLoads
            << " catalog_allocation_decoded_bytes=" << catalogReads.allocationDecodedBytes
            << " catalog_allocation_generation_block_loads=" <<
                catalogReads.allocationGenerationBlockLoads
            << " catalog_allocation_generation_decoded_bytes=" <<
                catalogReads.allocationGenerationDecodedBytes
            << " source_bytes=" << sourceBytes
            << " peak_bytes=" << observed.peakBytes
            << " build_peak_bytes=" << buildObserved.peakBytes
            << " build_plus_validation_peak_bytes=" << buildPlusValidationPeak
            << " final_bytes=" << observed.currentBytes
            << " build_elapsed_ms=" << buildMilliseconds
            << " validation_requested=" << validate
            << " validation_complete=" << validationComplete
            << " validation_elapsed_ms=" << validationStats.totalMs
            << " validation_scratch_peak_bytes=" << validationObserved.peakBytes
            << " validation_pass_use_ms=" << validationStats.passAndUseValidationMs
            << " validation_resource_source_ms=" << validationStats.resourceSourceMs
            << " validation_resource_sort_ms=" << validationStats.resourceSortMs
            << " validation_resource_compare_ms=" << validationStats.resourceCompareMs
            << " validation_catalog_source_ms=" << validationStats.catalogSourceMs
            << " validation_catalog_sort_ms=" << validationStats.catalogSortMs
            << " validation_catalog_compare_ms=" << validationStats.catalogCompareMs
            << " validation_cleanup_ms=" << validationStats.cleanupMs
            << " validation_resource_sort_runs=" << validationStats.resourceSort.initialRuns
            << " validation_resource_sort_merge_passes=" << validationStats.resourceSort.mergePasses
            << " validation_resource_sort_source_read_bytes=" << validationStats.resourceSort.sourceReadBytes
            << " validation_resource_sort_run_write_bytes=" << validationStats.resourceSort.initialRunWriteBytes
            << " validation_resource_sort_merge_read_bytes=" << validationStats.resourceSort.mergeReadBytes
            << " validation_resource_sort_merge_write_bytes=" << validationStats.resourceSort.mergeWriteBytes
            << " validation_catalog_sort_runs=" << validationStats.catalogSort.initialRuns
            << " validation_catalog_sort_merge_passes=" << validationStats.catalogSort.mergePasses
            << " validation_catalog_sort_source_read_bytes=" << validationStats.catalogSort.sourceReadBytes
            << " validation_catalog_sort_run_write_bytes=" << validationStats.catalogSort.initialRunWriteBytes
            << " validation_catalog_sort_merge_read_bytes=" << validationStats.catalogSort.mergeReadBytes
            << " validation_catalog_sort_merge_write_bytes=" << validationStats.catalogSort.mergeWriteBytes
            << " pass_sort_ms=" << stages.passEventSortMs
            << " pass_publish_ms=" << stages.passTimePublishMs
            << " use_sort_ms=" << stages.usePassSortMs
            << " resolve_ms=" << stages.resolveUsesMs
            << " resource_sort_ms=" << stages.resourceSortMs
            << " resource_publish_ms=" << stages.resourcePublishMs
            << " catalog_replay_ms=" << stages.catalogReplayMs
            << " catalog_sort_ms=" << stages.catalogSortMs
            << " catalog_publish_ms=" << stages.catalogPublishMs
            << " use_sort_runs=" << stages.usePassSort.initialRuns
            << " use_sort_merge_passes=" << stages.usePassSort.mergePasses
            << " use_sort_source_read_bytes=" << stages.usePassSort.sourceReadBytes
            << " use_sort_run_write_bytes=" << stages.usePassSort.initialRunWriteBytes
            << " use_sort_merge_read_bytes=" << stages.usePassSort.mergeReadBytes
            << " use_sort_merge_write_bytes=" << stages.usePassSort.mergeWriteBytes
            << " resource_sort_runs=" << stages.resourceSort.initialRuns
            << " resource_sort_merge_passes=" << stages.resourceSort.mergePasses
            << " resource_sort_source_read_bytes=" << stages.resourceSort.sourceReadBytes
            << " resource_sort_run_write_bytes=" << stages.resourceSort.initialRunWriteBytes
            << " resource_sort_merge_read_bytes=" << stages.resourceSort.mergeReadBytes
            << " resource_sort_merge_write_bytes=" << stages.resourceSort.mergeWriteBytes
            << " catalog_sort_runs=" << stages.catalogSort.initialRuns
            << " catalog_sort_merge_passes=" << stages.catalogSort.mergePasses
            << " catalog_sort_source_read_bytes=" << stages.catalogSort.sourceReadBytes
            << " catalog_sort_run_write_bytes=" << stages.catalogSort.initialRunWriteBytes
            << " catalog_sort_merge_read_bytes=" << stages.catalogSort.mergeReadBytes
            << " catalog_sort_merge_write_bytes=" << stages.catalogSort.mergeWriteBytes << '\n';
        if( inventory )
            for( const auto& line : inventoryRows ) std::cout << line << '\n';
        catalog.reset();
        std::error_code cleanup;
        fs::remove_all( root, cleanup );
        if( cleanup ) throw std::runtime_error( "GPU reference capacity cleanup: " + cleanup.message() );
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all( root, ignored );
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
            false, true, false, false, false, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-shuffled-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, true, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-allocation-shuffled-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, true, false, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-multidomain-capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ),
            false, true, false, false, true );
    if( argc == 5 && std::string_view( argv[1] ) == "--catalog-inventory" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), std::stoull( argv[4] ), true, true );
    try
    {
        namespace fs = std::filesystem;
        if( argc == 6 && std::string_view( argv[1] ) == "--validate-existing" )
        {
            const auto referenceRoot = fs::path( argv[2] );
            const auto catalogRoot = fs::path( argv[3] );
            const std::string source = argv[4];
            const auto scratch = fs::path( argv[5] );
            if( source.size() != 64 || fs::exists( scratch ) )
                throw std::runtime_error( "GPU reference diagnostic identity or scratch path" );
            tracy::analysis::PagedRecordOptions options;
            options.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
            options.disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
            options.disk->usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path> { scratch } );
            options.disk->maximumBytes = 120ull << 30;
            tracy::query::QueryCatalogStoreReader catalog( catalogRoot, source, options );
            if( !catalog.PreparePointerResolutionFastPath() )
                throw std::runtime_error( "GPU reference diagnostic pointer fast path was unavailable" );
            tracy::query::QueryGpuReferenceStoreReader reader( referenceRoot, source, options );
            const auto start = std::chrono::steady_clock::now();
            const auto stats = reader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
                return catalog.ResolveResourcePointer( pointerToken, time );
            }, scratch );
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start ).count();
            std::cout << "gpu_reference_real_validation elapsed_ms=" << milliseconds
                << " telemetry_total_ms=" << stats.totalMs
                << " pass_use_ms=" << stats.passAndUseValidationMs
                << " resource_source_ms=" << stats.resourceSourceMs
                << " resource_sort_ms=" << stats.resourceSortMs
                << " resource_compare_ms=" << stats.resourceCompareMs
                << " catalog_source_ms=" << stats.catalogSourceMs
                << " catalog_sort_ms=" << stats.catalogSortMs
                << " catalog_compare_ms=" << stats.catalogCompareMs
                << " cleanup_ms=" << stats.cleanupMs
                << " use_block_loads=" << stats.useBlockLoads
                << " use_decoded_bytes=" << stats.useDecodedBytes
                << " sorted_references=" << stats.sortedReferenceCount
                << " resource_sort_runs=" << stats.resourceSort.initialRuns
                << " resource_sort_merge_passes=" << stats.resourceSort.mergePasses
                << " resource_sort_source_read_bytes=" << stats.resourceSort.sourceReadBytes
                << " resource_sort_run_write_bytes=" << stats.resourceSort.initialRunWriteBytes
                << " resource_sort_merge_read_bytes=" << stats.resourceSort.mergeReadBytes
                << " resource_sort_merge_write_bytes=" << stats.resourceSort.mergeWriteBytes
                << " catalog_sort_runs=" << stats.catalogSort.initialRuns
                << " catalog_sort_merge_passes=" << stats.catalogSort.mergePasses
                << " catalog_sort_source_read_bytes=" << stats.catalogSort.sourceReadBytes
                << " catalog_sort_run_write_bytes=" << stats.catalogSort.initialRunWriteBytes
                << " catalog_sort_merge_read_bytes=" << stats.catalogSort.mergeReadBytes
                << " catalog_sort_merge_write_bytes=" << stats.catalogSort.mergeWriteBytes
                << " scratch_peak_bytes=" << options.disk->usage->Snapshot().peakBytes
                << " scratch_remaining_bytes=" << options.disk->usage->Snapshot().currentBytes << '\n';
            return 0;
        }
        if( argc == 5 && std::string_view( argv[1] ) == "--validation-probe" )
        {
            const auto root = fs::path( argv[2] );
            const std::string source = argv[3];
            const auto count = std::stoull( argv[4] );
            if( source.size() != 64 || count == 0 || count > 4096 )
                throw std::runtime_error( "GPU reference validation probe arguments" );
            const auto identity = [&]( std::string_view kind ) {
                tracy::analysis::Sha256Builder hash;
                hash.Update( source.data(), source.size() );
                hash.Update( kind.data(), kind.size() );
                constexpr std::string_view version = "query-gpu-reference-v1";
                hash.Update( version.data(), version.size() );
                return hash.FinalHex();
            };
            for( const uint64_t cacheBytes : { uint64_t( 0 ), uint64_t( 64 ) << 20 } )
            {
                tracy::analysis::PagedRecordOptions options;
                options.readerCacheBytes = cacheBytes;
                options.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
                tracy::analysis::PagedRecordReader lookup( root / "by-resource", identity( "by-resource" ),
                    sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), options );
                tracy::analysis::PagedRecordReader uses( root / "resolved-uses", identity( "resolved-uses" ),
                    sizeof( tracy::query::QueryGpuReferenceResolvedUseRecord ), options );
                const auto start = std::chrono::steady_clock::now();
                uint64_t hash = 14695981039346656037ull;
                uint64_t minimum = UINT64_MAX, maximum = 0;
                for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
                {
                    tracy::analysis::AnalysisKeyMetadataRecord row {};
                    lookup.Read( ordinal, &row, 1 );
                    if( row.metadata >= uses.Count() ) throw std::runtime_error( "GPU reference probe ordinal" );
                    tracy::query::QueryGpuReferenceResolvedUseRecord use {};
                    uses.Read( row.metadata, &use, 1 );
                    if( use.resourceId != row.keyHigh ) throw std::runtime_error( "GPU reference probe identity" );
                    minimum = std::min( minimum, row.metadata );
                    maximum = std::max( maximum, row.metadata );
                    hash ^= use.resourceId ^ use.passId ^ row.metadata;
                    hash *= 1099511628211ull;
                }
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start ).count();
                const auto stats = uses.Statistics();
                std::cout << "gpu_reference_validation_probe cache_bytes=" << cacheBytes
                    << " count=" << count << " elapsed_ms=" << milliseconds
                    << " minimum_use_ordinal=" << minimum << " maximum_use_ordinal=" << maximum
                    << " use_block_loads=" << stats.blockLoads << " use_decoded_bytes=" << stats.decodedBytes
                    << " use_decompressions=" << stats.decompressions << " hash=" << hash << '\n';
            }
            return 0;
        }
        const auto root = fs::temp_directory_path() / "tracy-query-gpu-reference-store";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::create_directories( root );
        {
            using tracy::analysis::AnalysisExternalSortStatistics;
            using tracy::analysis::AnalysisKeyMetadataRecord;
            const auto sortRoot = root / "sort-telemetry";
            fs::create_directories( sortRoot );
            constexpr uint64_t recordCount = 65;
            constexpr uint64_t bytes = recordCount * sizeof( AnalysisKeyMetadataRecord );
            const auto makeRow = []( uint64_t ordinal ) {
                AnalysisKeyMetadataRecord row {};
                row.keyHigh = recordCount - 1 - ordinal;
                row.metadata = ordinal;
                return row;
            };
            const auto checkOutput = [&]( const fs::path& path ) {
                std::ifstream file( path, std::ios::binary );
                for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ )
                {
                    AnalysisKeyMetadataRecord row {};
                    file.read( reinterpret_cast<char*>( &row ), sizeof( row ) );
                    Require( bool( file ) && row.keyHigh == ordinal && row.metadata == recordCount - 1 - ordinal,
                        "external sort changed key or source ordinal" );
                }
                Require( file.peek() == std::char_traits<char>::eof(), "external sort appended a record" );
            };
            const auto checkStats = [&]( const AnalysisExternalSortStatistics& stats, uint64_t sourceReadBytes ) {
                Require( stats.inputRecords == recordCount && stats.initialRuns == recordCount &&
                    stats.mergePasses == 2 && stats.mergedGroups == 3 &&
                    stats.sourceReadBytes == sourceReadBytes && stats.initialRunWriteBytes == bytes &&
                    stats.mergeReadBytes == 2 * bytes && stats.mergeWriteBytes == 2 * bytes,
                    "external sort run and logical I/O statistics do not match its 65-record merge" );
            };
            std::string sortError;
            AnalysisExternalSortStatistics generatedStats;
            const auto generatedOutput = sortRoot / "generated.work";
            Require( tracy::analysis::SortAnalysisKeyMetadataRecordsGenerated( generatedOutput, sortRoot,
                "generated-telemetry", 1,
                [&]( const auto& emit ) {
                    for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ ) emit( makeRow( ordinal ) );
                }, sortError, {}, {}, {}, &generatedStats ),
                "generated external sort failed" );
            checkOutput( generatedOutput );
            checkStats( generatedStats, 0 );
            {
                const auto failureRoot = root / "generated-metadata-open-failure";
                fs::create_directories( failureRoot );
                auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                    std::vector<fs::path> { failureRoot } );
                auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                    tracy::analysis::AnalysisDiskBudget { usage,
                        2 * sizeof( AnalysisKeyMetadataRecord ), {} } );
                const auto blockedRun = failureRoot / "generated-metadata-open-failure-run-0-0.work";
                bool refused = false;
                try
                {
                    tracy::analysis::SortAnalysisKeyMetadataRecordsGenerated(
                        failureRoot / "output.work", failureRoot,
                        "generated-metadata-open-failure", 1,
                        [&]( const auto& emit ) {
                            fs::create_directory( blockedRun );
                            emit( makeRow( 0 ) );
                        }, sortError, disk );
                }
                catch( const std::runtime_error& error )
                { refused = std::string_view( error.what() ) == "analysis_metadata_sort_run_write_open_failed"; }
                Require( refused && usage->Snapshot().currentBytes ==
                    tracy::analysis::AnalysisDiskUsage::Bytes( failureRoot ),
                    "generated metadata sorter kept unwritten run bytes charged after open failure" );
                if( fs::exists( blockedRun ) ) fs::remove( blockedRun );
            }
            AnalysisExternalSortStatistics simultaneousStats;
            tracy::analysis::AnalysisKeyMetadataRunCollector simultaneous(
                sortRoot / "simultaneous.work", sortRoot, "simultaneous", 1,
                {}, {}, {}, &simultaneousStats );
            for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ )
                simultaneous.Emit( makeRow( ordinal ) );
            Require( simultaneous.Finish( sortError ), "incremental metadata run collector failed" );
            checkOutput( sortRoot / "simultaneous.work" );
            checkStats( simultaneousStats, 0 );
            {
                auto shared = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>(
                    sizeof( AnalysisKeyMetadataRecord ), sizeof( AnalysisKeyMetadataRecord ) );
                tracy::analysis::AnalysisKeyMetadataRunCollector first(
                    sortRoot / "shared-first.work", sortRoot, "shared-first", 1, {}, shared );
                bool secondRejected = false;
                try
                {
                    tracy::analysis::AnalysisKeyMetadataRunCollector second(
                        sortRoot / "shared-second.work", sortRoot, "shared-second", 1, {}, shared );
                }
                catch( const std::runtime_error& error )
                { secondRejected = std::string_view( error.what() ) == "analysis_workspace_budget"; }
                Require( secondRejected && shared->Snapshot().currentBytes ==
                    sizeof( AnalysisKeyMetadataRecord ),
                    "simultaneous sort collectors obtained separate workspace budgets" );
            }
            {
                const auto refusalRoot = root / "collector-open-failure";
                fs::create_directories( refusalRoot );
                auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                    std::vector<fs::path> { refusalRoot } );
                auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                    tracy::analysis::AnalysisDiskBudget { usage, 2 * sizeof( AnalysisKeyMetadataRecord ), {} } );
                tracy::analysis::AnalysisKeyMetadataRunCollector collector(
                    refusalRoot / "output.work", refusalRoot, "collector-open-failure", 1, disk );
                const auto blockedRun = refusalRoot / "collector-open-failure-run-0-0.work";
                fs::create_directory( blockedRun );
                bool refused = false;
                try { collector.Emit( makeRow( 0 ) ); }
                catch( const std::runtime_error& error )
                { refused = std::string_view( error.what() ) == "analysis_metadata_sort_run_write_open_failed"; }
                Require( refused && usage->Snapshot().currentBytes ==
                    tracy::analysis::AnalysisDiskUsage::Bytes( refusalRoot ),
                    "metadata collector kept unwritten run bytes charged after open failure" );
                fs::remove( blockedRun );
            }
            const auto pairOutput = sortRoot / "generated-pairs.work";
            AnalysisExternalSortStatistics pairStats;
            Require( tracy::analysis::SortAnalysisUInt64PairsGenerated( pairOutput, sortRoot,
                "generated-pairs", 1,
                [&]( const auto& emit ) {
                    for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ )
                        emit( tracy::analysis::AnalysisUInt64Pair {
                            ( recordCount - 1 - ordinal ) % 5, ordinal } );
                }, sortError, {}, {}, {}, &pairStats ),
                "generated pair multi-run sort failed" );
            {
                std::ifstream input( pairOutput, std::ios::binary );
                tracy::analysis::AnalysisUInt64Pair previous {};
                for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ )
                {
                    tracy::analysis::AnalysisUInt64Pair current {};
                    input.read( reinterpret_cast<char*>( &current ), sizeof( current ) );
                    Require( bool( input ) && current.key < 5 && current.value < recordCount &&
                        ( ordinal == 0 || previous.key < current.key ||
                            ( previous.key == current.key && previous.value < current.value ) ),
                        "generated pair sort lost duplicate-key ordinal order" );
                    previous = current;
                }
                Require( input.peek() == std::char_traits<char>::eof(),
                    "generated pair sort appended a record" );
            }
            const auto pairBytes = recordCount * sizeof( tracy::analysis::AnalysisUInt64Pair );
            Require( pairStats.inputRecords == recordCount && pairStats.initialRuns == recordCount &&
                pairStats.mergePasses == 2 && pairStats.mergedGroups == 3 &&
                pairStats.sourceReadBytes == 0 && pairStats.initialRunWriteBytes == pairBytes &&
                pairStats.mergeReadBytes == 2 * pairBytes &&
                pairStats.mergeWriteBytes == 2 * pairBytes,
                "generated pair sort logical I/O accounting is wrong" );
            uint64_t sourcePairCancelChecks = 0;
            Require( !tracy::analysis::SortAnalysisUInt64Pairs( pairOutput,
                    sortRoot / "cancelled-source-pairs.work", sortRoot,
                    "cancelled-source-pairs", recordCount, 1, sortError, {}, {},
                    [&] { return ++sourcePairCancelChecks > recordCount; } ) &&
                    sortError == "analysis_pair_sort_cancelled" &&
                    !fs::exists( sortRoot / "cancelled-source-pairs.work" ),
                "source pair sorter ignored cancellation before merging runs" );
            bool cancelPairs = false, pairsCancelled = false;
            try
            {
                tracy::analysis::SortAnalysisUInt64PairsGenerated(
                    sortRoot / "cancelled-pairs.work", sortRoot, "cancelled-pairs", 2,
                    [&]( const auto& emit ) {
                        emit( { 2, 0 } ); emit( { 1, 1 } );
                        cancelPairs = true; emit( { 1, 2 } );
                    }, sortError, {}, {}, [&] { return cancelPairs; } );
            }
            catch( const std::runtime_error& error )
            { pairsCancelled = std::string_view( error.what() ).find( "cancelled" ) != std::string_view::npos; }
            Require( pairsCancelled && !fs::exists( sortRoot / "cancelled-pairs.work" ),
                "generated pair sorter published after cancellation" );
            Require( tracy::analysis::CleanupAnalysisExternalSortFiles( sortRoot,
                "cancelled-pairs-run-", sortError ),
                "generated pair cancellation left run files" );
            const auto pairRefusalRoot = root / "pair-refusal";
            fs::create_directories( pairRefusalRoot );
            auto pairUsage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path> { pairRefusalRoot } );
            auto pairDisk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                tracy::analysis::AnalysisDiskBudget { pairUsage, sizeof( tracy::analysis::AnalysisUInt64Pair ), {} } );
            bool pairRejected = false;
            try
            {
                tracy::analysis::SortAnalysisUInt64PairsGenerated(
                    pairRefusalRoot / "sorted.work", pairRefusalRoot, "pair-refusal", 1,
                    [&]( const auto& emit ) { emit( { 2, 0 } ); emit( { 1, 1 } ); },
                    sortError, pairDisk );
            }
            catch( const std::runtime_error& error )
            { pairRejected = std::string_view( error.what() ).find( "disk_budget" ) != std::string_view::npos; }
            Require( pairRejected && !fs::exists( pairRefusalRoot / "sorted.work" ) &&
                pairUsage->Snapshot().currentBytes ==
                    tracy::analysis::AnalysisDiskUsage::Bytes( pairRefusalRoot ),
                "generated pair sorter exceeded write-before-growth budget" );
            Require( tracy::analysis::CleanupAnalysisExternalSortFiles( pairRefusalRoot,
                "pair-refusal-run-", sortError, pairDisk ) && pairUsage->Snapshot().currentBytes == 0,
                "generated pair refusal did not release owned run bytes" );
            {
                const auto failureRoot = root / "generated-pair-open-failure";
                fs::create_directories( failureRoot );
                auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                    std::vector<fs::path> { failureRoot } );
                auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                    tracy::analysis::AnalysisDiskBudget { usage,
                        2 * sizeof( tracy::analysis::AnalysisUInt64Pair ), {} } );
                const auto blockedRun = failureRoot / "generated-pair-open-failure-run-0-0.work";
                bool refused = false;
                try
                {
                    tracy::analysis::SortAnalysisUInt64PairsGenerated(
                        failureRoot / "output.work", failureRoot,
                        "generated-pair-open-failure", 1,
                        [&]( const auto& emit ) {
                            fs::create_directory( blockedRun );
                            emit( { 1, 2 } );
                        }, sortError, disk );
                }
                catch( const std::runtime_error& error )
                { refused = std::string_view( error.what() ) == "analysis_pair_sort_run_write_open_failed"; }
                Require( refused && usage->Snapshot().currentBytes ==
                    tracy::analysis::AnalysisDiskUsage::Bytes( failureRoot ),
                    "generated pair sorter kept unwritten run bytes charged after open failure" );
                if( fs::exists( blockedRun ) ) fs::remove( blockedRun );
            }
            {
                const auto failureRoot = root / "source-pair-open-failure";
                fs::create_directories( failureRoot );
                const auto blockedRun = failureRoot / "source-pair-open-failure-run-0-0.work";
                auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                    std::vector<fs::path> { failureRoot } );
                auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
                disk->usage = usage;
                disk->maximumBytes = 2 * pairBytes;
                disk->checkSpace = [&] {
                    if( !fs::exists( blockedRun ) ) fs::create_directory( blockedRun );
                };
                Require( !tracy::analysis::SortAnalysisUInt64Pairs( pairOutput,
                        failureRoot / "output.work", failureRoot,
                        "source-pair-open-failure", recordCount, 1, sortError, disk ) &&
                        sortError == "analysis_pair_sort_run_write_open_failed" &&
                        usage->Snapshot().currentBytes ==
                            tracy::analysis::AnalysisDiskUsage::Bytes( failureRoot ),
                    "source pair sorter kept unwritten run bytes charged after open failure" );
                fs::remove( blockedRun );
            }
            const auto source = sortRoot / "source.work";
            {
                std::ofstream file( source, std::ios::binary );
                for( uint64_t ordinal = 0; ordinal < recordCount; ordinal++ )
                {
                    const auto row = makeRow( ordinal );
                    file.write( reinterpret_cast<const char*>( &row ), sizeof( row ) );
                }
            }
            {
                const auto failureRoot = root / "source-metadata-open-failure";
                fs::create_directories( failureRoot );
                const auto blockedRun = failureRoot / "source-metadata-open-failure-run-0-0.work";
                auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                    std::vector<fs::path> { failureRoot } );
                auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
                disk->usage = usage;
                disk->maximumBytes = 2 * bytes;
                disk->checkSpace = [&] {
                    if( !fs::exists( blockedRun ) ) fs::create_directory( blockedRun );
                };
                Require( !tracy::analysis::SortAnalysisKeyMetadataRecords( source,
                        failureRoot / "output.work", failureRoot,
                        "source-metadata-open-failure", recordCount, 1, sortError, disk ) &&
                        sortError == "analysis_metadata_sort_run_write_open_failed" &&
                        usage->Snapshot().currentBytes ==
                            tracy::analysis::AnalysisDiskUsage::Bytes( failureRoot ),
                    "source metadata sorter kept unwritten run bytes charged after open failure" );
                fs::remove( blockedRun );
            }
            AnalysisExternalSortStatistics sourceStats;
            const auto sourceOutput = sortRoot / "source-sorted.work";
            Require( tracy::analysis::SortAnalysisKeyMetadataRecords( source, sourceOutput, sortRoot,
                "source-telemetry", recordCount, 1, sortError, {}, {}, {}, &sourceStats ),
                "source external sort failed" );
            checkOutput( sourceOutput );
            checkStats( sourceStats, bytes );
            auto generous = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
            const auto wideRun = tracy::analysis::ChooseAnalysisExternalSortBufferRecords(
                34000000, sizeof( AnalysisKeyMetadataRecord ), generous );
            Require( wideRun >= 531250 && wideRun <= ( 512ull << 20 ) / sizeof( AnalysisKeyMetadataRecord ),
                "sort run selection did not avoid the second 64-way merge" );
            auto tight = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 64ull << 20, 64ull << 20 );
            tracy::analysis::AnalysisWorkspaceReservation occupied( tight, 56ull << 20 );
            const auto narrowRun = tracy::analysis::ChooseAnalysisExternalSortBufferRecords(
                34000000, sizeof( AnalysisKeyMetadataRecord ), tight );
            Require( narrowRun > 0 && narrowRun <= ( 4ull << 20 ) / sizeof( AnalysisKeyMetadataRecord ),
                "sort run selection ignored other shared workspace users" );
        }
        {
            const auto sortRoot = root / "sort-batched-growth";
            fs::create_directories( sortRoot );
            auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path>{ sortRoot } );
            auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                tracy::analysis::AnalysisDiskBudget{ usage, 2ull << 20, {} } );
            uint64_t growthChecks = 0;
            disk->checkSpace = [&] { growthChecks++; };
            auto workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>(
                1ull << 20, 1ull << 20 );
            std::string error;
            tracy::analysis::AnalysisExternalSortStatistics stats;
            const auto output = sortRoot / "sorted.work";
            Require( tracy::analysis::SortAnalysisKeyMetadataRecordsGenerated( output, sortRoot,
                "batched-growth", 512,
                [&]( const auto& emit ) {
                    for( uint64_t index = 0; index < 10000; index++ )
                    {
                        tracy::analysis::AnalysisKeyMetadataRecord row {};
                        row.keyHigh = 10000 - index;
                        row.metadata = index;
                        emit( row );
                    }
                }, error, disk, workspace, {}, &stats ), "batched-growth external sort failed" );
            Require( stats.initialRuns == 20 && stats.mergePasses == 1 &&
                fs::file_size( output ) == 10000 * sizeof( tracy::analysis::AnalysisKeyMetadataRecord ),
                "batched-growth sort count or output changed" );
            Require( usage->Snapshot().currentBytes == fs::file_size( output ),
                "batched-growth sort disk ledger changed" );
            Require( growthChecks < 100,
                "external merge still requests disk growth for every output record" );
        }
        {
            const auto sortRoot = root / "sort-batched-refusal";
            fs::create_directories( sortRoot );
            auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path>{ sortRoot } );
            auto disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>(
                tracy::analysis::AnalysisDiskBudget{ usage, 2ull << 20, {} } );
            uint64_t growthChecks = 0;
            disk->checkSpace = [&] {
                if( ++growthChecks == 22 ) throw std::runtime_error( "injected merge growth refusal" );
            };
            auto workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>(
                1ull << 20, 1ull << 20 );
            std::string error;
            bool rejected = false;
            try
            {
                tracy::analysis::SortAnalysisKeyMetadataRecordsGenerated( sortRoot / "sorted.work", sortRoot,
                    "batched-refusal", 512,
                    [&]( const auto& emit ) {
                        for( uint64_t index = 0; index < 10000; index++ )
                        {
                            tracy::analysis::AnalysisKeyMetadataRecord row {};
                            row.keyHigh = 10000 - index;
                            emit( row );
                        }
                    }, error, disk, workspace );
            }
            catch( const std::runtime_error& exception )
            { rejected = std::string_view( exception.what() ) == "injected merge growth refusal"; }
            Require( rejected && usage->Snapshot().currentBytes ==
                tracy::analysis::AnalysisDiskUsage::Bytes( sortRoot ),
                "external merge refusal left unaccounted or overcharged scratch bytes" );
        }
        Input input;
        input.passes = { { 10, 1, 0, 1, 0, 0, 0 }, { 30, 2, 0, 1, 0, 0, 0 } };
        input.ends = { { 20, 1, 0, 1, 0, 0, 0 } };
        input.uses = {
            { 30, 2, 100, 1, 5, 8, 0, 1 },
            { 10, 1, 100, 1, 3, 7, 0, 2 },
            { 10, 1, 0, 1, 1, 0, 0, 1 },
            { 40, 999, 100, 1, 1, 0, 0, 1 }
        };
        const std::string identity( 64, 'a' );
        tracy::query::QueryGpuReferenceStore::Build( root, identity, input );
        Require( !fs::exists( root / "reverse-scratch" ), "GPU reference scratch was retained" );
        tracy::analysis::Sha256Builder compactLookupHash;
        compactLookupHash.Update( identity.data(), identity.size() );
        constexpr std::string_view compactLookupKind = "by-resource";
        compactLookupHash.Update( compactLookupKind.data(), compactLookupKind.size() );
        constexpr std::string_view compactLookupVersion = "query-gpu-reference-v1";
        compactLookupHash.Update( compactLookupVersion.data(), compactLookupVersion.size() );
        tracy::analysis::PagedRecordReader compactLookup( root / "by-resource",
            compactLookupHash.FinalHex(), sizeof( tracy::analysis::AnalysisKeyMetadataRecord ) );
        Require( compactLookup.Count() == 2,
            "GPU reference compact Resource lookup count changed" );
        tracy::query::QueryGpuReferenceStoreReader reader( root, identity );
        reader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
            return input.ResolveCatalogResource( pointerToken, time );
        } );
        const auto first = reader.UsesByResource( 100, 0, 1 );
        const auto second = reader.UsesByResource( 100, 1, 1 );
        Require( first.total == 2 && first.values.size() == 1 && first.values[0].passId == 2 &&
            first.values[0].passStart == 30 && first.values[0].passEnd == 30 &&
            first.values[0].usageMask == 5 && first.values[0].resourceSetId == 8,
            "GPU reference first resource page changed" );
        Require( second.total == 2 && second.values.size() == 1 && second.values[0].passId == 1 &&
            second.values[0].passStart == 10 && second.values[0].passEnd == 20 &&
            second.values[0].encoding == 2,
            "GPU reference continuation or open pass boundary changed" );
        Require( reader.UsesByResource( 999, 0, 10 ).total == 0,
            "GPU reference missing resource behavior changed" );
        const auto catalog200 = reader.UsesByCatalogResource( 200, 0, 10 );
        const auto catalog201 = reader.UsesByCatalogResource( 201, 0, 10 );
        Require( catalog200.total == 1 && catalog200.values.size() == 1 &&
            catalog200.values[0].passId == 1 && catalog200.values[0].resourceId == 100 &&
            catalog201.total == 1 && catalog201.values.size() == 1 &&
            catalog201.values[0].passId == 2 && catalog201.values[0].resourceId == 100,
            "GPU reference Catalog resource lifetime resolution changed" );
        const auto pass1 = reader.UsesByPass( 1, 0, 10 );
        const auto pass2 = reader.UsesByPass( 2, 0, 10 );
        Require( pass1.total == 1 && pass1.values.size() == 1 && pass1.values[0].passId == 1 &&
            pass2.total == 1 && pass2.values.size() == 1 && pass2.values[0].passId == 2 &&
            reader.UsesByPass( 999, 0, 10 ).total == 0,
            "GPU reference pass paging changed" );
        const auto scaleRoot = root.parent_path() / "tracy-query-gpu-reference-validation-scale";
        const auto scaleScratch = root.parent_path() / "tracy-query-gpu-reference-validation-scale-scratch";
        fs::remove_all( scaleRoot, ignored );
        fs::remove_all( scaleScratch, ignored );
        tracy::analysis::PagedRecordOptions scaleOptions;
        scaleOptions.blockBytes = 4096;
        scaleOptions.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>( 64ull << 20, 32ull << 20 );
        Input scaleInput;
        scaleInput.passes = { { 10, 1, 0, 1, 0, 0, 0 } };
        scaleInput.ends = { { 20, 1, 0, 1, 0, 0, 0 } };
        for( unsigned i = 0; i < 4096; i++ )
            scaleInput.uses.push_back( { 10, 1, 100 + i % 8, 1, 1, 0, 0, 1 } );
        tracy::query::QueryGpuReferenceStore::Build( scaleRoot, identity, scaleInput, scaleOptions );
        tracy::query::QueryGpuReferenceStoreReader scaleReader( scaleRoot, identity, scaleOptions );
        const auto validation = scaleReader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
            return scaleInput.ResolveCatalogResource( pointerToken, time );
        }, scaleScratch );
        Require( validation.sortedReferenceCount == scaleInput.uses.size() + scaleInput.uses.size() / 8 &&
            validation.resourceSort.inputRecords == scaleInput.uses.size() &&
            validation.resourceSort.sourceReadBytes == 0 &&
            validation.resourceSort.initialRunWriteBytes == scaleInput.uses.size() *
                sizeof( tracy::analysis::AnalysisUInt64Pair ) &&
            validation.catalogSort.inputRecords == scaleInput.uses.size() / 8 &&
            validation.catalogSort.sourceReadBytes == 0 &&
            validation.catalogSort.initialRunWriteBytes == scaleInput.uses.size() / 8 *
                sizeof( tracy::analysis::AnalysisUInt64Pair ) &&
            validation.totalMs >= validation.passAndUseValidationMs +
                validation.resourceSourceMs + validation.resourceSortMs + validation.resourceCompareMs +
                validation.catalogSourceMs + validation.catalogSortMs + validation.catalogCompareMs +
                validation.cleanupMs &&
            validation.useDecodedBytes <= scaleInput.uses.size() *
                sizeof( tracy::query::QueryGpuReferenceResolvedUseRecord ) * 5 &&
            !fs::exists( scaleScratch ),
            "GPU reference deep validation telemetry, reads, or scratch cleanup changed" );
        const auto deniedScratch = root.parent_path() / "tracy-query-gpu-reference-validation-denied";
        fs::remove_all( deniedScratch, ignored );
        auto deniedOptions = scaleOptions;
        auto diskUsage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
            std::vector<fs::path> { deniedScratch } );
        deniedOptions.disk = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
        deniedOptions.disk->usage = diskUsage;
        deniedOptions.disk->maximumBytes = 1;
        bool diskRejected = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader denied( scaleRoot, identity, deniedOptions );
            denied.ValidateAll( {}, deniedScratch );
        }
        catch( const std::exception& error )
        { diskRejected = std::string_view( error.what() ).find( "analysis_scan_cache_disk_budget" ) != std::string_view::npos; }
        Require( diskRejected && !fs::exists( deniedScratch ),
            "GPU reference validation did not reject disk growth and clean its owned scratch" );
        const auto cancelledScratch = root.parent_path() / "tracy-query-gpu-reference-validation-cancelled";
        fs::remove_all( cancelledScratch, ignored );
        auto cancelledOptions = scaleOptions;
        uint64_t validationCancellationChecks = 0;
        cancelledOptions.cancelled = [&] { return ++validationCancellationChecks >= 2; };
        bool cancelledDuringSort = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader cancelled( scaleRoot, identity, cancelledOptions );
            cancelled.ValidateAll( {}, cancelledScratch );
        }
        catch( const std::exception& error )
        { cancelledDuringSort = std::string_view( error.what() ).find( "cancelled" ) != std::string_view::npos; }
        Require( cancelledDuringSort && !fs::exists( cancelledScratch ),
            "GPU reference validation did not cancel external sorting and clean scratch" );
        const auto corruptedRoot = root.parent_path() / "tracy-query-gpu-reference-validation-corrupt";
        const auto corruptedScratch = root.parent_path() / "tracy-query-gpu-reference-validation-corrupt-scratch";
        fs::remove_all( corruptedRoot, ignored );
        fs::remove_all( corruptedScratch, ignored );
        tracy::query::QueryGpuReferenceStore::Build( corruptedRoot, identity, scaleInput, scaleOptions );
        tracy::analysis::Sha256Builder lookupHash;
        lookupHash.Update( identity.data(), identity.size() );
        constexpr std::string_view lookupKind = "by-resource";
        lookupHash.Update( lookupKind.data(), lookupKind.size() );
        constexpr std::string_view lookupVersion = "query-gpu-reference-v1";
        lookupHash.Update( lookupVersion.data(), lookupVersion.size() );
        const auto lookupIdentity = lookupHash.FinalHex();
        const auto replacement = corruptedRoot / "by-resource-replacement";
        {
            tracy::analysis::PagedRecordReader original( corruptedRoot / "by-resource", lookupIdentity,
                sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), scaleOptions );
            tracy::analysis::PagedRecordWriter changed( replacement, lookupIdentity,
                sizeof( tracy::analysis::AnalysisKeyMetadataRecord ), scaleOptions );
            for( uint64_t ordinal = 0; ordinal < original.Count(); ordinal++ )
            {
                tracy::analysis::AnalysisKeyMetadataRecord row {};
                original.Read( ordinal, &row, 1 );
                if( ordinal == 0 ) row.metadata = 1;
                changed.Append( &row, 1 );
            }
            changed.Commit();
        }
        fs::remove_all( corruptedRoot / "by-resource" );
        fs::rename( replacement, corruptedRoot / "by-resource" );
        bool rejectedCorruption = false;
        try
        {
            tracy::query::QueryGpuReferenceStoreReader corrupted( corruptedRoot, identity, scaleOptions );
            corrupted.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
                return scaleInput.ResolveCatalogResource( pointerToken, time );
            }, corruptedScratch );
        }
        catch( const std::exception& error )
        { rejectedCorruption = std::string_view( error.what() ).find( "resource lookup ordinal mismatch" ) != std::string_view::npos; }
        Require( rejectedCorruption && !fs::exists( corruptedScratch ),
            "GPU reference validation accepted a valid-checksum but missing Use ordinal" );
        fs::remove_all( corruptedRoot, ignored );
        fs::remove_all( scaleRoot, ignored );
        fs::remove_all( root, ignored );
        std::cout << "query GPU reference store passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
