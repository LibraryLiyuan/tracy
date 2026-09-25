#include "TracyQueryExplicitPassBuilder.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyQueryExplicitPassStore.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace tracy::query
{
namespace
{

constexpr uint32_t PartitionCount = 16;
constexpr size_t PageSize = 4096;
constexpr uint64_t LinkOrdinalMask = ( uint64_t( 1 ) << 56 ) - 1;

#pragma pack( push, 1 )
struct IdentityRecord { uint64_t entityId; uint32_t ordinal; uint8_t kind; };
struct PassRecord
{
    uint64_t entityId;
    uint32_t ordinal;
    uint32_t passSourceId;
    uint8_t legacyTaxonomy;
    uint8_t managed;
};
struct LinkRecord { uint64_t sourceId; uint64_t targetId; uint64_t encodedRelationOrdinal; };
struct PartialRecord
{
    QueryExplicitPassRecord projection;
    uint32_t passSourceId;
    uint8_t legacyTaxonomy;
    uint8_t managed;
};
struct SegmentRequest { uint64_t targetId; uint64_t linkOrdinal; uint32_t passOrdinal; };
struct ResolvedSegment { uint64_t linkOrdinal; uint32_t passOrdinal; uint32_t segmentOrdinal; };
struct FilterInput { uint32_t passSourceId; uint32_t taxonomyKey; uint8_t stable; };
#pragma pack( pop )

static_assert( sizeof( IdentityRecord ) == 13 );
static_assert( sizeof( PassRecord ) == 18 );
static_assert( sizeof( LinkRecord ) == 24 );
static_assert( sizeof( PartialRecord ) == 58 );
static_assert( sizeof( SegmentRequest ) == 20 );
static_assert( sizeof( ResolvedSegment ) == 16 );
static_assert( sizeof( FilterInput ) == 9 );

void Check( const analysis::PagedRecordOptions& options )
{
    if( options.cancelled && options.cancelled() )
        throw std::runtime_error( "query_explicit_pass_builder_cancelled" );
}

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

uint32_t Partition( uint64_t key ) { return uint32_t( Mix( key ) & ( PartitionCount - 1 ) ); }

uint64_t FileBytes( const std::filesystem::path& path )
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size( analysis::AnalysisIoPath( path ), error );
    return error ? 0 : bytes;
}

void Remove( const std::filesystem::path& path,
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk )
{
    std::error_code error;
    analysis::AnalysisDiskRemove( path, disk, error );
    if( error ) throw std::runtime_error( "query_explicit_pass_builder_cleanup" );
}

template<typename T>
class FileWriter
{
public:
    FileWriter( std::filesystem::path path, analysis::PagedRecordOptions options )
        : m_path( std::move( path ) ), m_options( std::move( options ) )
    {
        std::filesystem::create_directories( analysis::AnalysisIoPath( m_path.parent_path() ) );
        m_output.open( analysis::AnalysisIoPath( m_path ), std::ios::binary );
        if( !m_output ) throw std::runtime_error( "query_explicit_pass_builder_output_open" );
        m_buffer.reserve( PageSize );
    }
    ~FileWriter() { try { Close(); } catch( ... ) {} }
    void Append( const T& value )
    {
        Check( m_options );
        m_buffer.push_back( value );
        if( m_buffer.size() == m_buffer.capacity() ) Flush();
    }
    void Close()
    {
        if( !m_output.is_open() ) return;
        Flush();
        m_output.flush();
        const bool good = bool( m_output );
        m_output.close();
        if( !good ) throw std::runtime_error( "query_explicit_pass_builder_output_flush" );
    }
private:
    void Flush()
    {
        if( m_buffer.empty() ) return;
        const auto bytes = uint64_t( m_buffer.size() * sizeof( T ) );
        analysis::AnalysisDiskGrow( m_options.disk, bytes );
        try
        {
            if( m_options.beforeFileGrowth ) m_options.beforeFileGrowth( m_path, m_bytes + bytes );
            m_output.write( reinterpret_cast<const char*>( m_buffer.data() ), std::streamsize( bytes ) );
            if( !m_output ) throw std::runtime_error( "query_explicit_pass_builder_output_write" );
            m_bytes += bytes;
            m_buffer.clear();
        }
        catch( ... )
        {
            const auto charged = m_bytes + bytes;
            const auto actual = FileBytes( m_path );
            if( actual < charged && m_options.disk && m_options.disk->usage )
                m_options.disk->usage->Release( charged - actual );
            m_bytes = actual;
            m_buffer.clear();
            m_output.close();
            throw;
        }
    }
    std::filesystem::path m_path;
    analysis::PagedRecordOptions m_options;
    std::ofstream m_output;
    std::vector<T> m_buffer;
    uint64_t m_bytes = 0;
};

template<typename T>
class PartitionWriter
{
public:
    PartitionWriter( std::filesystem::path root, std::string prefix,
        analysis::PagedRecordOptions options )
        : m_root( std::move( root ) ), m_prefix( std::move( prefix ) ), m_options( std::move( options ) )
    {}
    void Append( uint32_t partition, const T& value )
    {
        if( partition >= PartitionCount ) throw std::runtime_error( "query_explicit_pass_partition" );
        if( !m_outputs[partition] ) m_outputs[partition] = std::make_unique<FileWriter<T>>(
            Path( partition ), m_options );
        m_outputs[partition]->Append( value );
    }
    void Close()
    {
        for( auto& output : m_outputs ) if( output ) { output->Close(); output.reset(); }
    }
    std::filesystem::path Path( uint32_t partition ) const
    {
        return m_root / ( m_prefix + "-" + std::to_string( partition ) + ".bin" );
    }
private:
    std::filesystem::path m_root;
    std::string m_prefix;
    analysis::PagedRecordOptions m_options;
    std::array<std::unique_ptr<FileWriter<T>>, PartitionCount> m_outputs;
};

template<typename T>
std::vector<T> ReadAll( const std::filesystem::path& path )
{
    const auto bytes = FileBytes( path );
    if( bytes == 0 ) return {};
    if( bytes % sizeof( T ) != 0 ) throw std::runtime_error( "query_explicit_pass_builder_input_size" );
    std::vector<T> result;
    result.resize( size_t( bytes / sizeof( T ) ) );
    std::ifstream input( analysis::AnalysisIoPath( path ), std::ios::binary );
    input.read( reinterpret_cast<char*>( result.data() ), std::streamsize( bytes ) );
    if( !input || input.peek() != std::ifstream::traits_type::eof() )
        throw std::runtime_error( "query_explicit_pass_builder_input_read" );
    return result;
}

bool RelevantRelation( uint8_t relation )
{
    return relation == uint8_t( JnGfxRelation::RunsOnGpu ) ||
        ( relation >= uint8_t( JnGfxRelation::RecordedOnCommandList ) &&
          relation <= uint8_t( JnGfxRelation::ClassifiesAsTaxonomy ) );
}

uint64_t WorkspaceLimit( const analysis::PagedRecordOptions& options )
{
    if( !options.workspace ) return std::numeric_limits<uint64_t>::max();
    const auto snapshot = options.workspace->Snapshot();
    return snapshot.maximumBytes > snapshot.currentBytes ?
        snapshot.maximumBytes - snapshot.currentBytes : 0;
}

template<typename T>
void RequirePage( const std::vector<T>& page, uint64_t expected, const char* label )
{
    if( page.size() != expected ) throw std::runtime_error( std::string( label ) + " page is incomplete" );
}

}

QueryExplicitPassBuilderStatistics BuildQueryExplicitPassStore(
    const std::filesystem::path& root, const std::filesystem::path& temporaryRoot,
    const std::string& sourceFingerprint, const QueryExplicitPassBuildInput& input,
    analysis::PagedRecordOptions options )
{
    if( !input.scanEntities || !input.scanLinks || !input.scanGpuZones )
        throw std::invalid_argument( "explicit Pass builder input callbacks are required" );
    if( input.entityCount > UINT32_MAX || input.entityCount > QueryExplicitPassMissingSegment )
        throw std::runtime_error( "query_explicit_pass_entity_ordinal_width" );
    if( input.linkCount > LinkOrdinalMask )
        throw std::runtime_error( "query_explicit_pass_link_ordinal_width" );
    if( std::filesystem::exists( analysis::AnalysisIoPath( temporaryRoot ) ) )
        throw std::runtime_error( "query_explicit_pass_temporary_exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( temporaryRoot ) );
    QueryExplicitPassBuilderStatistics stats;
    try
    {
        PartitionWriter<IdentityRecord> identities( temporaryRoot, "identity", options );
        PartitionWriter<PassRecord> passes( temporaryRoot, "pass", options );
        PartitionWriter<LinkRecord> links( temporaryRoot, "link", options );
        for( uint64_t offset = 0; offset < input.entityCount; offset += PageSize )
        {
            Check( options );
            const auto take = std::min<uint64_t>( PageSize, input.entityCount - offset );
            const auto page = input.scanEntities( offset, size_t( take ) );
            RequirePage( page, take, "explicit Pass entity" );
            for( const auto& entity : page )
            {
                if( entity.ordinal != offset + ( &entity - page.data() ) || entity.ordinal > UINT32_MAX )
                    throw std::runtime_error( "query_explicit_pass_entity_ordinal" );
                const auto partition = Partition( entity.entityId );
                identities.Append( partition, { entity.entityId, uint32_t( entity.ordinal ), entity.kind } );
                if( entity.kind == uint8_t( JnGfxEntityKind::ExplicitGpuPass ) )
                {
                    passes.Append( partition, { entity.entityId, uint32_t( entity.ordinal ),
                        entity.gpuQueryId, entity.gpuContext, uint8_t( ( entity.flags & 2 ) != 0 ) } );
                    stats.passes++;
                }
            }
        }
        for( uint64_t offset = 0; offset < input.linkCount; offset += PageSize )
        {
            Check( options );
            const auto take = std::min<uint64_t>( PageSize, input.linkCount - offset );
            const auto page = input.scanLinks( offset, size_t( take ) );
            RequirePage( page, take, "explicit Pass link" );
            for( const auto& link : page )
            {
                if( link.ordinal != offset + ( &link - page.data() ) || link.ordinal > LinkOrdinalMask )
                    throw std::runtime_error( "query_explicit_pass_link_ordinal" );
                if( !RelevantRelation( link.relation ) ) continue;
                links.Append( Partition( link.sourceId ), { link.sourceId, link.targetId,
                    ( uint64_t( link.relation ) << 56 ) | link.ordinal } );
                stats.relevantLinks++;
            }
        }
        identities.Close(); passes.Close(); links.Close();

        PartitionWriter<PartialRecord> partials( temporaryRoot, "partial", options );
        PartitionWriter<SegmentRequest> requests( temporaryRoot, "request", options );
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            Check( options );
            const auto passPath = passes.Path( partition );
            const auto linkPath = links.Path( partition );
            const auto passCount = FileBytes( passPath ) / sizeof( PassRecord );
            const auto linkCount = FileBytes( linkPath ) / sizeof( LinkRecord );
            const auto estimated = passCount * ( sizeof( PassRecord ) + sizeof( PartialRecord ) + 64 ) +
                linkCount * sizeof( LinkRecord ) + 65536;
            if( estimated > WorkspaceLimit( options ) )
                throw std::runtime_error( "query_explicit_pass_partition_workspace" );
            analysis::AnalysisWorkspaceReservation memory( options.workspace, estimated );
            const auto passRows = ReadAll<PassRecord>( passPath );
            const auto linkRows = ReadAll<LinkRecord>( linkPath );
            std::vector<PartialRecord> rows;
            rows.reserve( passRows.size() );
            std::unordered_map<uint64_t, size_t> firstPass;
            firstPass.reserve( passRows.size() );
            for( const auto& pass : passRows )
            {
                PartialRecord row;
                row.projection.entityOrdinal = pass.ordinal;
                row.projection.segmentOrdinalAndStable = QueryExplicitPassMissingSegment;
                row.passSourceId = pass.passSourceId;
                row.legacyTaxonomy = pass.legacyTaxonomy;
                row.managed = pass.managed;
                firstPass.emplace( pass.entityId, rows.size() );
                rows.emplace_back( row );
            }
            for( const auto& link : linkRows )
            {
                const auto found = firstPass.find( link.sourceId );
                if( found == firstPass.end() ) continue;
                auto& row = rows[found->second];
                const auto relation = uint8_t( link.encodedRelationOrdinal >> 56 );
                const auto ordinal = link.encodedRelationOrdinal & LinkOrdinalMask;
                switch( JnGfxRelation( relation ) )
                {
                case JnGfxRelation::RunsOnGpu:
                    requests.Append( Partition( link.targetId ),
                        { link.targetId, ordinal, row.projection.entityOrdinal } );
                    stats.segmentRequests++;
                    break;
                case JnGfxRelation::RecordedOnCommandList: row.projection.commandListId = link.targetId; break;
                case JnGfxRelation::BelongsToFrame: row.projection.frameId = link.targetId; break;
                case JnGfxRelation::BelongsToCamera: row.projection.cameraId = link.targetId; break;
                case JnGfxRelation::BelongsToView: row.projection.viewId = link.targetId; break;
                case JnGfxRelation::ReferencesResources: row.projection.referenceToken = link.targetId; break;
                case JnGfxRelation::ClassifiesAsTaxonomy:
                    if( link.targetId != 0 && link.targetId <= UINT32_MAX )
                    {
                        row.projection.taxonomyId = uint32_t( link.targetId );
                        row.projection.segmentOrdinalAndStable |= QueryExplicitPassStableTaxonomyBit;
                    }
                    break;
                default: break;
                }
            }
            for( const auto& row : rows ) partials.Append( Partition( row.projection.entityOrdinal ), row );
            if( FileBytes( passPath ) != 0 ) Remove( passPath, options.disk );
            if( FileBytes( linkPath ) != 0 ) Remove( linkPath, options.disk );
        }
        partials.Close(); requests.Close();

        PartitionWriter<ResolvedSegment> resolved( temporaryRoot, "resolved", options );
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            Check( options );
            const auto identityPath = identities.Path( partition );
            const auto requestPath = requests.Path( partition );
            const auto identityCount = FileBytes( identityPath ) / sizeof( IdentityRecord );
            const auto requestCount = FileBytes( requestPath ) / sizeof( SegmentRequest );
            const auto estimated = identityCount * ( sizeof( IdentityRecord ) + 48 ) +
                requestCount * sizeof( SegmentRequest ) + 65536;
            if( estimated > WorkspaceLimit( options ) )
                throw std::runtime_error( "query_explicit_pass_segment_workspace" );
            analysis::AnalysisWorkspaceReservation memory( options.workspace, estimated );
            const auto identityRows = ReadAll<IdentityRecord>( identityPath );
            const auto requestRows = ReadAll<SegmentRequest>( requestPath );
            std::unordered_map<uint64_t, IdentityRecord> firstEntity;
            firstEntity.reserve( identityRows.size() );
            for( const auto& identity : identityRows ) firstEntity.emplace( identity.entityId, identity );
            for( const auto& request : requestRows )
            {
                const auto found = firstEntity.find( request.targetId );
                if( found == firstEntity.end() ||
                    found->second.kind != uint8_t( JnGfxEntityKind::GpuSegment ) ||
                    found->second.ordinal >= QueryExplicitPassMissingSegment ) continue;
                resolved.Append( Partition( request.passOrdinal ),
                    { request.linkOrdinal, request.passOrdinal, found->second.ordinal } );
                stats.resolvedSegments++;
            }
            if( FileBytes( identityPath ) != 0 ) Remove( identityPath, options.disk );
            if( FileBytes( requestPath ) != 0 ) Remove( requestPath, options.disk );
        }
        resolved.Close();

        std::array<std::filesystem::path, PartitionCount> runPaths;
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            Check( options );
            const auto partialPath = partials.Path( partition );
            const auto resolvedPath = resolved.Path( partition );
            const auto partialCount = FileBytes( partialPath ) / sizeof( PartialRecord );
            const auto resolvedCount = FileBytes( resolvedPath ) / sizeof( ResolvedSegment );
            const auto estimated = partialCount * ( sizeof( PartialRecord ) + sizeof( uint64_t ) + 48 ) +
                resolvedCount * sizeof( ResolvedSegment ) + 65536;
            if( estimated > WorkspaceLimit( options ) )
                throw std::runtime_error( "query_explicit_pass_projection_workspace" );
            analysis::AnalysisWorkspaceReservation memory( options.workspace, estimated );
            auto partialRows = ReadAll<PartialRecord>( partialPath );
            const auto resolvedRows = ReadAll<ResolvedSegment>( resolvedPath );
            std::unordered_map<uint32_t, size_t> byOrdinal;
            byOrdinal.reserve( partialRows.size() );
            for( size_t index = 0; index < partialRows.size(); index++ )
                byOrdinal.emplace( partialRows[index].projection.entityOrdinal, index );
            std::vector<uint64_t> best( partialRows.size(), 0 );
            std::vector<bool> haveBest( partialRows.size(), false );
            for( const auto& segment : resolvedRows )
            {
                const auto found = byOrdinal.find( segment.passOrdinal );
                if( found == byOrdinal.end() ) continue;
                const auto index = found->second;
                if( haveBest[index] && segment.linkOrdinal <= best[index] ) continue;
                best[index] = segment.linkOrdinal;
                haveBest[index] = true;
                const auto stable = partialRows[index].projection.segmentOrdinalAndStable &
                    QueryExplicitPassStableTaxonomyBit;
                partialRows[index].projection.segmentOrdinalAndStable = stable | segment.segmentOrdinal;
            }
            std::sort( partialRows.begin(), partialRows.end(), []( const auto& lhs, const auto& rhs ) {
                return lhs.projection.entityOrdinal < rhs.projection.entityOrdinal;
            } );
            runPaths[partition] = temporaryRoot / ( "run-" + std::to_string( partition ) + ".bin" );
            {
                FileWriter<PartialRecord> run( runPaths[partition], options );
                for( const auto& row : partialRows ) run.Append( row );
                run.Close();
            }
            if( FileBytes( partialPath ) != 0 ) Remove( partialPath, options.disk );
            if( FileBytes( resolvedPath ) != 0 ) Remove( resolvedPath, options.disk );
        }

        if( stats.passes > UINT64_MAX / 2 ) throw std::runtime_error( "query_explicit_pass_filter_count" );
        QueryExplicitPassStoreWriter store( root, sourceFingerprint, input.entityCount,
            input.gpuZoneCount, stats.passes, stats.passes * 2, options );
        struct Head { PartialRecord row; uint32_t partition; };
        const auto later = []( const Head& lhs, const Head& rhs ) {
            return lhs.row.projection.entityOrdinal > rhs.row.projection.entityOrdinal;
        };
        std::priority_queue<Head, std::vector<Head>, decltype( later )> queue( later );
        std::array<std::ifstream, PartitionCount> runInputs;
        const auto readRun = [&]( uint32_t partition, PartialRecord& row ) {
            auto& stream = runInputs[partition];
            stream.read( reinterpret_cast<char*>( &row ), sizeof( row ) );
            if( stream.gcount() == sizeof( row ) ) return true;
            if( stream.gcount() != 0 || !stream.eof() )
                throw std::runtime_error( "query_explicit_pass_run_read" );
            return false;
        };
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            if( FileBytes( runPaths[partition] ) == 0 ) continue;
            runInputs[partition].open( analysis::AnalysisIoPath( runPaths[partition] ), std::ios::binary );
            if( !runInputs[partition] ) throw std::runtime_error( "query_explicit_pass_run_open" );
            PartialRecord row;
            if( readRun( partition, row ) ) queue.push( { row, partition } );
        }
        const auto filterPath = temporaryRoot / "filter-input.bin";
        FileWriter<FilterInput> filterInput( filterPath, options );
        uint64_t projectionOrdinal = 0;
        uint32_t previousEntity = 0;
        bool firstProjection = true;
        while( !queue.empty() )
        {
            Check( options );
            auto head = queue.top(); queue.pop();
            if( !firstProjection && head.row.projection.entityOrdinal <= previousEntity )
                throw std::runtime_error( "query_explicit_pass_run_order" );
            store.AppendPass( head.row.projection, head.row.managed != 0 );
            const bool stable = head.row.projection.TaxonomyStable();
            filterInput.Append( { head.row.passSourceId,
                stable ? head.row.projection.taxonomyId : uint32_t( head.row.legacyTaxonomy ),
                uint8_t( stable ) } );
            previousEntity = head.row.projection.entityOrdinal;
            firstProjection = false;
            projectionOrdinal++;
            PartialRecord next;
            if( readRun( head.partition, next ) ) queue.push( { next, head.partition } );
        }
        filterInput.Close();
        if( projectionOrdinal != stats.passes ) throw std::runtime_error( "query_explicit_pass_projection_count" );
        store.SealPasses();
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            if( runInputs[partition].is_open() ) runInputs[partition].close();
            if( FileBytes( runPaths[partition] ) != 0 ) Remove( runPaths[partition], options.disk );
        }

        {
            std::ifstream filterStream( analysis::AnalysisIoPath( filterPath ), std::ios::binary );
            if( !filterStream && stats.passes != 0 ) throw std::runtime_error( "query_explicit_pass_filter_input_open" );
            std::array<FilterInput, PageSize> buffer;
            uint64_t offset = 0;
            while( offset < stats.passes )
            {
                Check( options );
                const auto take = size_t( std::min<uint64_t>( buffer.size(), stats.passes - offset ) );
                filterStream.read( reinterpret_cast<char*>( buffer.data() ),
                    std::streamsize( take * sizeof( FilterInput ) ) );
                if( !filterStream ) throw std::runtime_error( "query_explicit_pass_filter_input_read" );
                for( size_t index = 0; index < take; index++ )
                {
                    store.AppendFilter( buffer[index].passSourceId,
                        QueryExplicitPassFilterTag::PassSource, offset + index );
                    store.AppendFilter( buffer[index].taxonomyKey,
                        buffer[index].stable ? QueryExplicitPassFilterTag::Taxonomy :
                            QueryExplicitPassFilterTag::LegacyTaxonomy, offset + index );
                }
                offset += take;
            }
            if( stats.passes != 0 && filterStream.peek() != std::ifstream::traits_type::eof() )
                throw std::runtime_error( "query_explicit_pass_filter_input_size" );
        }
        if( FileBytes( filterPath ) != 0 ) Remove( filterPath, options.disk );

        const auto sortedZones = temporaryRoot / "gpu-zones-sorted.bin";
        const auto zoneScratch = temporaryRoot / "gpu-zone-sort";
        std::filesystem::create_directories( analysis::AnalysisIoPath( zoneScratch ) );
        std::string sortError;
        analysis::AnalysisExternalSortStatistics sortStats;
        const auto zoneBuffer = analysis::ChooseAnalysisExternalSortBufferRecords(
            input.gpuZoneCount, sizeof( analysis::AnalysisKeyMetadataRecord ), options.workspace );
        if( !analysis::SortAnalysisKeyMetadataRecordsGenerated( sortedZones, zoneScratch,
            "explicit-pass-zone", zoneBuffer,
            [&]( const auto& emit ) {
                for( uint64_t offset = 0; offset < input.gpuZoneCount; offset += PageSize )
                {
                    Check( options );
                    const auto take = std::min<uint64_t>( PageSize, input.gpuZoneCount - offset );
                    const auto page = input.scanGpuZones( offset, size_t( take ) );
                    RequirePage( page, take, "explicit Pass GPU Zone" );
                    for( const auto& zone : page )
                    {
                        if( zone.ordinal != offset + ( &zone - page.data() ) || zone.ordinal >= input.gpuZoneCount )
                            throw std::runtime_error( "query_explicit_pass_gpu_zone_ordinal" );
                        emit( { ( uint64_t( zone.context ) << 32 ) | zone.queryId,
                            uint64_t( zone.cpuStartNs ) ^ ( uint64_t( 1 ) << 63 ), zone.ordinal, 0, 0, {} } );
                    }
                }
            }, sortError, options.disk, options.workspace, options.cancelled, &sortStats ) )
            throw std::runtime_error( "query_explicit_pass_gpu_zone_sort: " + sortError );
        {
            std::ifstream zones( analysis::AnalysisIoPath( sortedZones ), std::ios::binary );
            if( !zones && input.gpuZoneCount != 0 ) throw std::runtime_error( "query_explicit_pass_gpu_zone_open" );
            analysis::AnalysisKeyMetadataRecord row;
            while( zones.read( reinterpret_cast<char*>( &row ), sizeof( row ) ) )
            {
                Check( options );
                store.AppendGpuZoneKey( { row.keyHigh,
                    int64_t( row.keyLow ^ ( uint64_t( 1 ) << 63 ) ), row.metadata } );
                stats.gpuZoneKeys++;
            }
            if( !zones.eof() ) throw std::runtime_error( "query_explicit_pass_gpu_zone_read" );
        }
        if( FileBytes( sortedZones ) != 0 ) Remove( sortedZones, options.disk );
        if( !analysis::CleanupAnalysisExternalSortFiles( zoneScratch,
            "explicit-pass-zone", sortError, options.disk ) )
            throw std::runtime_error( "query_explicit_pass_gpu_zone_cleanup: " + sortError );

        const auto storeStats = store.Commit();
        if( storeStats.passes != stats.passes || storeStats.gpuZoneKeys != input.gpuZoneCount ||
            storeStats.filterRecords != stats.passes * 2 )
            throw std::runtime_error( "query_explicit_pass_store_count" );
        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( temporaryRoot, options.disk, cleanupError, true );
        if( cleanupError ) throw std::runtime_error( "query_explicit_pass_temporary_cleanup" );
        return stats;
    }
    catch( ... )
    {
        std::error_code error;
        analysis::AnalysisDiskRemove( temporaryRoot, options.disk, error, true );
        throw;
    }
}

}
