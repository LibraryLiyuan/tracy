#include "TracyQueryExplicitPassBuilder.hpp"
#include "TracyQueryExplicitPassStore.hpp"
#include "TracyQueue.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace tracy;

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

template<typename T>
std::vector<T> Page( const std::vector<T>& values, uint64_t offset, size_t limit )
{
    if( offset >= values.size() ) return {};
    const auto end = size_t( offset ) + std::min<size_t>( limit, values.size() - size_t( offset ) );
    return { values.begin() + size_t( offset ), values.begin() + end };
}

}

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() /
            ( "tracy-explicit-pass-builder-test-" +
                std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directories( root );
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all( path, error ); }
        } cleanup { root };

        const std::vector<query::QueryExplicitPassEntityFact> entities {
            { 100, 0, 10, 0, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 0 },
            { 200, 1, 21, 2, uint8_t( JnGfxEntityKind::GpuSegment ), 0 },
            { 201, 2, 0, 0, uint8_t( JnGfxEntityKind::CommandList ), 0 },
            { 101, 3, 11, 1, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 2 },
            { 201, 4, 22, 3, uint8_t( JnGfxEntityKind::GpuSegment ), 0 },
            { 202, 5, 23, 4, uint8_t( JnGfxEntityKind::GpuSegment ), 0 },
            { 100, 6, 10, 0, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 0 }
        };
        const std::vector<query::QueryExplicitPassLinkFact> links {
            { 100, 200, 0, uint8_t( JnGfxRelation::RunsOnGpu ) },
            { 100, 999, 1, uint8_t( JnGfxRelation::RunsOnGpu ) },
            { 100, 300, 2, uint8_t( JnGfxRelation::RecordedOnCommandList ) },
            { 100, 301, 3, uint8_t( JnGfxRelation::RecordedOnCommandList ) },
            { 100, 400, 4, uint8_t( JnGfxRelation::BelongsToFrame ) },
            { 100, 123, 5, uint8_t( JnGfxRelation::ClassifiesAsTaxonomy ) },
            { 101, 201, 6, uint8_t( JnGfxRelation::RunsOnGpu ) },
            { 101, 202, 7, uint8_t( JnGfxRelation::RunsOnGpu ) },
            { 101, 500, 8, uint8_t( JnGfxRelation::ReferencesResources ) },
            { 9999, 1, 9, uint8_t( JnGfxRelation::BelongsToFrame ) }
        };
        const std::vector<query::QueryExplicitPassGpuZoneFact> zones {
            { 0, 2, 21, 100 }, { 1, 2, 21, 90 }, { 2, 4, 23, 200 }
        };
        query::QueryExplicitPassBuildInput input;
        input.entityCount = entities.size();
        input.linkCount = links.size();
        input.gpuZoneCount = zones.size();
        input.taxonomyIds = { 7, 8 };
        input.scanEntities = [&]( uint64_t offset, size_t limit ) { return Page( entities, offset, limit ); };
        input.scanLinks = [&]( uint64_t offset, size_t limit ) { return Page( links, offset, limit ); };
        input.scanGpuZones = [&]( uint64_t offset, size_t limit ) { return Page( zones, offset, limit ); };

        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 64 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            8 * 1024 * 1024, 8 * 1024 * 1024 );
        analysis::PagedRecordOptions options;
        options.disk = disk;
        options.workspace = workspace;
        options.blockBytes = 4096;
        const std::string fingerprint( 64, 'e' );
        const auto store = root / "store";
        const auto scratch = root / "scratch";
        const auto stats = query::BuildQueryExplicitPassStore(
            store, scratch, fingerprint, input, options );
        Require( stats.passes == 3 && stats.relevantLinks == 10 &&
            stats.segmentRequests == 4 && stats.resolvedSegments == 2 &&
            stats.gpuZoneKeys == 3, "explicit Pass builder statistics changed" );
        Require( !std::filesystem::exists( scratch ), "explicit Pass builder left scratch files" );
        {
            query::QueryExplicitPassStoreReader reader(
                store, fingerprint, entities.size(), zones.size(), options );
            reader.ValidateAll();
            Require( reader.MissingGpuZoneCount() == 1 &&
                reader.UnresolvedTaxonomyCount() == 1,
                "explicit Pass aggregate quality counts changed" );
            const auto values = reader.ReadPasses( 0, 10 );
            Require( values.size() == 3 && values[0].entityOrdinal == 0 &&
                values[1].entityOrdinal == 3 && values[2].entityOrdinal == 6,
                "explicit Pass builder changed original entity order" );
            Require( values[0].HasSegment() && values[0].SegmentOrdinal() == 1 &&
                values[0].commandListId == 301 && values[0].frameId == 400 &&
                values[0].taxonomyId == 123 && values[0].TaxonomyStable(),
                "explicit Pass builder changed first-Pass link semantics" );
            Require( values[1].HasSegment() && values[1].SegmentOrdinal() == 5 &&
                values[1].referenceToken == 500 && !values[1].TaxonomyStable(),
                "explicit Pass builder changed valid-segment fallback" );
            Require( !values[2].HasSegment() && values[2].commandListId == 0,
                "duplicate Pass ID unexpectedly received first-Pass links" );
            Require( !reader.IsManaged( 0 ) && reader.IsManaged( 1 ) && !reader.IsManaged( 2 ),
                "explicit Pass builder changed source mode" );
            Require( reader.FindNearestGpuZone( ( uint64_t( 2 ) << 32 ) | 21, 95 ) == 0,
                "explicit Pass builder changed equal-distance Zone tie" );
            size_t stable = 0, legacy = 0, source = 0;
            for( const auto& candidate : reader.ReadFilterCandidates( 123, 0, 64 ).values )
                stable += candidate.tag == query::QueryExplicitPassFilterTag::Taxonomy &&
                    candidate.projectionOrdinal == 0;
            for( const auto& candidate : reader.ReadFilterCandidates( 8, 0, 64 ).values )
                legacy += candidate.tag == query::QueryExplicitPassFilterTag::Taxonomy &&
                    candidate.projectionOrdinal == 1;
            for( const auto& candidate : reader.ReadFilterCandidates( 10, 0, 64 ).values )
                source += candidate.tag == query::QueryExplicitPassFilterTag::PassSource;
            Require( stable == 1 && legacy == 1 && source == 2,
                "explicit Pass builder changed filter keys" );
        }
        Require( workspace->Snapshot().currentBytes == 0,
            "explicit Pass builder leaked workspace" );
        std::cout << "Explicit Pass partition joins and semantic projection passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
