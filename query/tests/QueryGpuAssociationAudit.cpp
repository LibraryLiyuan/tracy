#include "TracyQueryGpuPassResourceStore.hpp"
#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyTraceFormat.hpp"
#include "TracyQueue.hpp"

#include <filesystem>
#include <array>
#include <fstream>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace
{

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

std::string StoreIdentity( std::string_view fingerprint, std::string_view kind )
{
    tracy::analysis::Sha256Builder hash;
    hash.Update( fingerprint.data(), fingerprint.size() );
    constexpr std::string_view version = "query-gpu-pass-resource-v1";
    hash.Update( kind.data(), kind.size() );
    hash.Update( version.data(), version.size() );
    return hash.FinalHex();
}

std::string SectionIdentity( std::string_view source, uint32_t kind )
{
    const auto value = std::string( source ) + ":" + tracy::ExtendedTraceDecoderIdentity +
        ":jnidx:" + std::to_string( tracy::query::QueryIndexSchemaVersion ) + ":" +
        std::to_string( kind );
    tracy::analysis::Sha256Builder hash;
    hash.Update( value.data(), value.size() );
    return hash.FinalHex();
}

class MappingInput final : public tracy::query::QueryGpuPassResourceInput
{
public:
    MappingInput( const std::filesystem::path& referencePassRoot,
        const std::filesystem::path& gfxLinkRoot, const std::filesystem::path& catalogRoot,
        const std::string& fingerprint, tracy::analysis::PagedRecordOptions options )
        : m_passes( referencePassRoot, SectionIdentity( fingerprint, 8 ),
            sizeof( tracy::JnGpuReferencePassData ), options )
        , m_links( gfxLinkRoot, SectionIdentity( fingerprint, 6 ),
            sizeof( tracy::JnGfxLinkData ), options )
        , m_catalog( catalogRoot, fingerprint, std::move( options ) )
    {}
    uint64_t MappingCount() const override { return m_passes.Count() + m_links.Count(); }
    tracy::query::QueryGpuPassMapping Mapping( uint64_t ordinal ) const override
    {
        if( ordinal < m_passes.Count() )
        {
            tracy::JnGpuReferencePassData row {};
            m_passes.Read( ordinal, &row, 1 );
            return { row.passId, row.passId };
        }
        tracy::JnGfxLinkData row {};
        m_links.Read( ordinal - m_passes.Count(), &row, 1 );
        return row.relation == uint8_t( tracy::JnGfxRelation::ReferencesResources ) ?
            tracy::query::QueryGpuPassMapping { row.targetId, row.sourceId } :
            tracy::query::QueryGpuPassMapping {};
    }
    uint64_t RangeCount() const override { return m_catalog.RangeCount(); }
    tracy::JnGpuRangeSetRecordV1 Range( uint64_t ordinal ) const override
    { return m_catalog.RangeAt( ordinal ).record; }
    uint64_t UseCount() const override
    { throw std::logic_error( "mapping audit does not provide Uses" ); }
    tracy::query::QueryGpuReferenceResolvedUseRecord Use( uint64_t ) const override
    { throw std::logic_error( "mapping audit does not provide Uses" ); }
    uint64_t ResolveCatalogResource( uint64_t, int64_t ) const override
    { throw std::logic_error( "mapping audit does not resolve Resources" ); }
    uint64_t ResourceAllocation( uint64_t ) const override
    { throw std::logic_error( "mapping audit does not provide Allocations" ); }
    uint64_t AllocationBytesAtEnd( uint64_t ) const override
    { throw std::logic_error( "mapping audit does not provide Allocations" ); }
private:
    mutable tracy::analysis::PagedRecordReader m_passes, m_links;
    tracy::query::QueryCatalogStoreReader m_catalog;
};

}

int main( int argc, char** argv )
{
    try
    {
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::query;
        using nlohmann::json;
        if( argc == 6 && std::string_view( argv[1] ) == "sample" )
        {
            const std::string fingerprint = argv[3];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "sample source fingerprint is invalid" );
            const auto seed = std::stoull( argv[4] );
            const auto requested = std::stoull( argv[5] );
            if( requested == 0 || requested > 1000 )
                throw std::runtime_error( "sample count must be between 1 and 1000" );
            tracy::analysis::PagedRecordReader summaries( fs::path( argv[2] ) /
                "pass-resource-summary", StoreIdentity( fingerprint, "pass-resource-summary" ),
                sizeof( QueryGpuPassResourceSummary ) );
            const auto population = summaries.Count();
            const auto selected = std::min<uint64_t>( requested, population );
            std::set<uint64_t> ordinals;
            const auto threshold = population == 0 ? 0 : uint64_t( 0 - population ) % population;
            for( uint64_t counter = 0; ordinals.size() < selected; counter++ )
            {
                const auto value = Mix( seed + counter );
                if( value >= threshold ) ordinals.emplace( value % population );
            }
            json rows = json::array();
            for( const auto ordinal : ordinals )
            {
                QueryGpuPassResourceSummary row {};
                summaries.Read( ordinal, &row, 1 );
                if( row.passId == 0 ) throw std::runtime_error( "sampled Pass ID is zero" );
                rows.push_back( { { "ordinal", std::to_string( ordinal ) },
                    { "pass_id", std::to_string( row.passId ) },
                    { "range_count", std::to_string( row.rangeCount ) },
                    { "use_count", std::to_string( row.useCount ) },
                    { "resource_count", std::to_string( row.resourceCount ) } } );
            }
            std::cout << json( { { "algorithm", "splitmix64-rejection-unique-sorted-v1" },
                { "seed", std::to_string( seed ) }, { "requested", std::to_string( requested ) },
                { "population", std::to_string( population ) }, { "passes", std::move( rows ) } } ).dump() << '\n';
            return 0;
        }
        if( argc == 6 && std::string_view( argv[1] ) == "counts" )
        {
            const std::string fingerprint = argv[5];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "count source fingerprint is invalid" );
            const auto lookupCount = [&]( std::string_view kind, uint32_t recordBytes ) {
                analysis::PagedRecordReader reader( fs::path( argv[2] ) / kind,
                    StoreIdentity( fingerprint, kind ), recordBytes );
                return std::to_string( reader.Count() );
            };
            QueryCatalogStoreReader catalog( argv[3], fingerprint );
            QueryGpuReferenceStoreReader references( argv[4], fingerprint );
            std::cout << json( { { "summaries", lookupCount( "pass-resource-summary",
                    sizeof( QueryGpuPassResourceSummary ) ) },
                { "mappings", lookupCount( "pass-resource-evidence-ids",
                    sizeof( analysis::AnalysisKeyMetadataRecord ) ) },
                { "ranges", lookupCount( "pass-resource-ranges",
                    sizeof( analysis::AnalysisKeyMetadataRecord ) ) },
                { "uses", lookupCount( "pass-resource-uses",
                    sizeof( analysis::AnalysisKeyMetadataRecord ) ) },
                { "raw_ranges", std::to_string( catalog.RangeCount() ) },
                { "raw_uses", std::to_string( references.UseCount() ) } } ).dump() << '\n';
            return 0;
        }
        if( argc == 8 && std::string_view( argv[1] ) == "validate" )
        {
            const std::string fingerprint = argv[5];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "validation source fingerprint is invalid" );
            const fs::path scratch = argv[6];
            const auto scratchLimit = std::stoull( argv[7] );
            if( scratchLimit == 0 ) throw std::runtime_error( "validation scratch limit is zero" );
            auto usage = std::make_shared<tracy::analysis::AnalysisDiskUsage>(
                std::vector<fs::path> { scratch } );
            auto budget = std::make_shared<tracy::analysis::AnalysisDiskBudget>();
            budget->usage = usage;
            budget->maximumBytes = scratchLimit;
            tracy::analysis::PagedRecordOptions options;
            options.disk = budget;
            options.workspace = std::make_shared<tracy::analysis::AnalysisWorkspaceBudget>(
                4ull << 30, 2ull << 30 );
            options.readerCacheBytes = 64ull << 20;
            QueryGpuPassResourceStoreReader reader( argv[2], argv[3], argv[4], fingerprint,
                options );
            const auto stats = reader.ValidateRelationsExact( scratch, [&]( std::string_view phase ) {
                std::cerr << "association_validation_phase=" << phase << '\n';
            } );
            std::cout << json( { { "status", "passed" }, { "algorithm", "pass-resource-exact-ordinal-v1" },
                { "mapping_rows", std::to_string( stats.mappingRows ) },
                { "range_rows", std::to_string( stats.rangeRows ) },
                { "use_rows", std::to_string( stats.useRows ) },
                { "mapping_sort_ms", stats.mappingSortMs },
                { "range_sort_ms", stats.rangeSortMs },
                { "range_compare_ms", stats.rangeCompareMs },
                { "use_sort_ms", stats.useSortMs },
                { "use_compare_ms", stats.useCompareMs },
                { "total_ms", stats.totalMs },
                { "scratch_peak_bytes", std::to_string( usage->Snapshot().peakBytes ) },
                { "scratch_remaining_bytes", std::to_string( usage->Snapshot().currentBytes ) },
                { "workspace_peak_bytes", std::to_string( options.workspace->Snapshot().peakBytes ) } } ).dump() << '\n';
            return 0;
        }
        if( argc == 7 && std::string_view( argv[1] ) == "oracle" )
        {
            const std::string fingerprint = argv[5];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "oracle source fingerprint is invalid" );
            std::ifstream selectedFile( argv[6] );
            if( !selectedFile ) throw std::runtime_error( "oracle Pass selection cannot be opened" );
            const auto selected = json::parse( selectedFile );
            if( !selected.contains( "passes" ) || !selected["passes"].is_array() ||
                selected["passes"].size() > 1000 )
                throw std::runtime_error( "oracle Pass selection is invalid" );
            analysis::PagedRecordOptions options;
            options.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                4ull << 30, 2ull << 30 );
            options.readerCacheBytes = 64ull << 20;
            QueryGpuPassResourceStoreReader reader( argv[2], argv[3], argv[4], fingerprint,
                options );
            QueryCatalogStoreReader catalog( argv[3], fingerprint, options );
            catalog.PreparePointerResolutionFastPath();
            const auto exactness = []( uint8_t value ) {
                switch( JnGpuCatalogExactness( value ) )
                {
                case JnGpuCatalogExactness::Exact: return "exact";
                case JnGpuCatalogExactness::OpenBoundary: return "open_boundary";
                case JnGpuCatalogExactness::Partial: return "partial";
                case JnGpuCatalogExactness::Unknown: return "unknown";
                case JnGpuCatalogExactness::Invalid: return "invalid";
                }
                return "unknown";
            };
            const auto decimal = []( uint64_t value ) -> json { return std::to_string( value ); };
            const auto nullable = [&]( uint64_t value ) -> json {
                return value == 0 ? json( nullptr ) : decimal( value );
            };
            size_t completed = 0;
            for( const auto& item : selected["passes"] )
            {
                const auto passId = std::stoull( item.at( "pass_id" ).get<std::string>() );
                const auto summary = reader.Summary( passId );
                if( !summary ) throw std::runtime_error( "oracle selected Pass summary is absent" );
                tracy::analysis::Sha256Builder rangeHash, useHash, evidenceHash;
                const auto add = []( tracy::analysis::Sha256Builder& hash, const json& tuple ) {
                    const auto line = tuple.dump() + '\n';
                    hash.Update( line.data(), line.size() );
                };
                uint64_t ranges = 0, uses = 0, evidence = 0;
                for( uint64_t offset = 0; offset < summary->rangeCount; offset += 1000 )
                {
                    const auto page = reader.Ranges( passId, offset, 1000 );
                    if( page.total != summary->rangeCount || page.values.empty() )
                        throw std::runtime_error( "oracle Range pagination is incomplete" );
                    for( const auto& value : page.values )
                    {
                        const auto& row = value.record;
                        add( rangeHash, json::array( { decimal( row.resourceId ),
                            nullable( row.viewDefinitionId ), decimal( row.rangeKind ),
                            decimal( row.offsetBytes ), decimal( row.lengthBytes ),
                            decimal( row.firstSubresource ), decimal( row.subresourceCount ),
                            decimal( row.usageMask ), exactness( row.exactness ) } ) );
                        ranges++;
                    }
                }
                for( uint64_t offset = 0; offset < summary->useCount; offset += 1000 )
                {
                    const auto page = reader.Uses( passId, offset, 1000 );
                    if( page.total != summary->useCount || page.values.empty() )
                        throw std::runtime_error( "oracle Use pagination is incomplete" );
                    for( const auto& row : page.values )
                    {
                        const auto resourceId = catalog.ResolveResourcePointer( row.resourceId, row.passEnd );
                        const auto state = catalog.ResourceState( resourceId );
                        const auto allocationId = state ? state->latest.allocationId : 0;
                        add( useHash, json::array( { nullable( allocationId ), decimal( row.resourceId ),
                            nullable( resourceId ), nullable( allocationId ),
                            nullable( row.resourceSetId ), row.encoding == 2 ? "ResourceSetV2" : "PerUseV1",
                            decimal( row.usageMask ), resourceId == 0 ? "0" : "1",
                            resourceId == 0 ? "unresolved_logical_resource" : "pointer_lifetime_exact" } ) );
                        uses++;
                    }
                }
                for( uint64_t offset = 0;; offset += 1000 )
                {
                    const auto page = reader.EvidencePassIds( passId, offset, 1000 );
                    if( page.values.empty() )
                    {
                        if( offset < page.total )
                            throw std::runtime_error( "oracle Evidence pagination is incomplete" );
                        break;
                    }
                    for( const auto value : page.values )
                    { add( evidenceHash, json::array( { decimal( value ) } ) ); evidence++; }
                }
                std::cout << json( { { "pass_id", std::to_string( passId ) },
                    { "range_count", std::to_string( ranges ) },
                    { "use_count", std::to_string( uses ) },
                    { "evidence_count", std::to_string( evidence ) },
                    { "range_sha256", rangeHash.FinalHex() },
                    { "use_sha256", useHash.FinalHex() },
                    { "evidence_sha256", evidenceHash.FinalHex() },
                    { "resource_count", std::to_string( summary->resourceCount ) },
                    { "allocation_count", std::to_string( summary->allocationCount ) },
                    { "direct_range_bytes", std::to_string( summary->directRangeBytes ) },
                    { "referenced_physical_bytes", std::to_string( summary->referencedPhysicalBytes ) } } ).dump() << '\n';
                if( ++completed % 100 == 0 )
                    std::cerr << "association_oracle_passes=" << completed << '\n';
            }
            return 0;
        }
        if( argc == 9 && std::string_view( argv[1] ) == "probe" )
        {
            const std::string fingerprint = argv[5];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "probe source fingerprint is invalid" );
            const fs::path scratch = argv[6];
            const auto limit = std::stoull( argv[7] );
            const auto maximumScratch = std::stoull( argv[8] );
            if( limit == 0 || limit > 1000000 || maximumScratch == 0 || fs::exists( scratch ) )
                throw std::runtime_error( "probe limits or scratch are invalid" );
            auto usage = std::make_shared<analysis::AnalysisDiskUsage>( std::vector<fs::path> { scratch } );
            auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
            disk->usage = usage; disk->maximumBytes = maximumScratch;
            analysis::PagedRecordOptions options;
            options.disk = disk;
            options.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>( 4ull << 30, 2ull << 30 );
            options.readerCacheBytes = 64ull << 20;
            analysis::PagedRecordReader lookup( fs::path( argv[2] ) / "pass-resource-uses",
                [&] {
                    analysis::Sha256Builder hash;
                    hash.Update( fingerprint.data(), fingerprint.size() );
                    constexpr std::string_view kind = "pass-resource-uses";
                    constexpr std::string_view version = "query-gpu-pass-resource-v1";
                    hash.Update( kind.data(), kind.size() );
                    hash.Update( version.data(), version.size() );
                    return hash.FinalHex();
                }(), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            const auto count = std::min<uint64_t>( limit, lookup.Count() );
            fs::create_directories( scratch );
            const auto started = std::chrono::steady_clock::now();
            std::string sortError;
            if( !analysis::SortAnalysisUInt64PairsGenerated( scratch / "probe-sorted.work", scratch,
                    "association-probe", analysis::ChooseAnalysisExternalSortBufferRecords(
                        count, sizeof( analysis::AnalysisUInt64Pair ), options.workspace ),
                    [&]( const auto& emit ) {
                        std::array<analysis::AnalysisKeyMetadataRecord, 4096> rows {};
                        for( uint64_t ordinal = 0; ordinal < count; )
                        {
                            const auto take = std::min<uint64_t>( rows.size(), count - ordinal );
                            lookup.Read( ordinal, rows.data(), take );
                            for( uint64_t index = 0; index < take; index++ )
                                emit( { rows[index].metadata, rows[index].keyHigh } );
                            ordinal += take;
                        }
                    }, sortError, disk, options.workspace ) )
                throw std::runtime_error( "association probe sort failed: " + sortError );
            const auto sortedAt = std::chrono::steady_clock::now();
            QueryCatalogStoreReader catalog( argv[3], fingerprint, options );
            QueryGpuReferenceStoreReader references( argv[4], fingerprint, options );
            catalog.PreparePointerResolutionFastPath();
            uint64_t resolved = 0;
            for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
            {
                const auto use = references.UseAt( ordinal );
                if( catalog.ResolveResourcePointer( use.resourceId, use.passEnd ) != 0 ) resolved++;
            }
            const auto scannedAt = std::chrono::steady_clock::now();
            std::error_code removalError;
            analysis::AnalysisDiskRemove( scratch, disk, removalError, true );
            if( removalError ) throw std::runtime_error( "association probe cleanup failed: " +
                removalError.message() );
            const auto ms = []( auto first, auto last ) {
                return std::chrono::duration_cast<std::chrono::milliseconds>( last - first ).count();
            };
            std::cout << json( { { "status", "passed" }, { "rows", std::to_string( count ) },
                { "resolved_uses", std::to_string( resolved ) },
                { "sort_ms", ms( started, sortedAt ) },
                { "source_scan_ms", ms( sortedAt, scannedAt ) },
                { "scratch_peak_bytes", std::to_string( usage->Snapshot().peakBytes ) },
                { "scratch_remaining_bytes", std::to_string( usage->Snapshot().currentBytes ) },
                { "workspace_peak_bytes", std::to_string( options.workspace->Snapshot().peakBytes ) } } ).dump() << '\n';
            return 0;
        }
        if( argc == 9 && std::string_view( argv[1] ) == "mapping-validate" )
        {
            const std::string fingerprint = argv[6];
            if( fingerprint.size() != 64 ) throw std::runtime_error( "mapping source fingerprint is invalid" );
            const fs::path scratch = argv[7];
            const auto scratchLimit = std::stoull( argv[8] );
            if( scratchLimit == 0 ) throw std::runtime_error( "mapping scratch limit is zero" );
            auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
                std::vector<fs::path> { scratch } );
            auto budget = std::make_shared<analysis::AnalysisDiskBudget>();
            budget->usage = usage; budget->maximumBytes = scratchLimit;
            analysis::PagedRecordOptions options;
            options.disk = budget;
            options.workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
                4ull << 30, 2ull << 30 );
            options.readerCacheBytes = 64ull << 20;
            MappingInput input( argv[4], argv[5], argv[3], fingerprint, options );
            QueryGpuPassResourceStoreReader reader( argv[2], argv[3], argv[2],
                fingerprint, options );
            const auto stats = reader.ValidateMappingSources( input, scratch,
                [&]( std::string_view phase ) {
                    std::cerr << "association_mapping_phase=" << phase << '\n';
                } );
            std::cout << json( { { "status", "passed" },
                { "algorithm", "pass-resource-source-mapping-v1" },
                { "source_rows", std::to_string( stats.sourceRows ) },
                { "unique_mappings", std::to_string( stats.uniqueMappings ) },
                { "sort_ms", stats.sortMs }, { "compare_ms", stats.compareMs },
                { "total_ms", stats.totalMs },
                { "scratch_peak_bytes", std::to_string( usage->Snapshot().peakBytes ) },
                { "scratch_remaining_bytes", std::to_string( usage->Snapshot().currentBytes ) },
                { "workspace_peak_bytes", std::to_string( options.workspace->Snapshot().peakBytes ) } } ).dump() << '\n';
            return 0;
        }
        std::cerr << "usage: sample <pass-root> <source-sha> <seed> <count> | "
            "validate <pass-root> <catalog-root> <reference-root> <source-sha> <scratch> <max-scratch-bytes> | "
            "oracle <pass-root> <catalog-root> <reference-root> <source-sha> <selection-json> | "
            "probe <pass-root> <catalog-root> <reference-root> <source-sha> <scratch> <limit-rows> <max-scratch-bytes> | "
            "mapping-validate <pass-root> <catalog-root> <reference-pass-root> <gfx-link-root> <source-sha> <scratch> <max-scratch-bytes>\n";
        return 2;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
