#include "TracyQueryExplicitPassStore.hpp"

#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyQueryCorrelationStore.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace tracy::query
{
namespace
{

constexpr size_t MaximumPage = 4096;

#pragma pack( push, 1 )
struct StoreHeader
{
    char magic[8] { 'J', 'N', 'P', 'A', 'S', 'S', '1', 0 };
    uint32_t version = 1;
    uint32_t projectionRecordBytes = sizeof( QueryExplicitPassRecord );
    uint32_t zoneRecordBytes = sizeof( QueryExplicitPassGpuZoneKey );
    uint32_t reserved = 0;
    uint64_t gfxEntityCount = 0;
    uint64_t gpuZoneCount = 0;
    uint64_t passes = 0;
    uint64_t managedPasses = 0;
    uint64_t gpuZoneKeys = 0;
    uint64_t filterRecords = 0;
    char sourceFingerprint[64] {};
};
#pragma pack( pop )

bool IsDigest( const std::string& value )
{
    return value.size() == 64 &&
        value.find_first_not_of( "0123456789abcdefABCDEF" ) == std::string::npos;
}

std::string Identity( std::string_view source, std::string_view name )
{
    if( !IsDigest( std::string( source ) ) )
        throw std::runtime_error( "query_explicit_pass_source_identity" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( name.data(), name.size() );
    return hash.FinalHex();
}

void Check( const analysis::PagedRecordOptions& options )
{
    if( options.cancelled && options.cancelled() )
        throw std::runtime_error( "query_explicit_pass_cancelled" );
}

uint64_t FileBytes( const std::filesystem::path& path )
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size( analysis::AnalysisIoPath( path ), error );
    return error ? 0 : bytes;
}

void WriteHeader( const std::filesystem::path& path, const StoreHeader& header,
    const analysis::PagedRecordOptions& options )
{
    analysis::Sha256Builder hash;
    hash.Update( &header, sizeof( header ) );
    const auto digest = hash.FinalHex();
    const auto bytes = uint64_t( sizeof( header ) + digest.size() );
    analysis::AnalysisDiskGrow( options.disk, bytes );
    try
    {
        if( options.beforeFileGrowth ) options.beforeFileGrowth( path, bytes );
        std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary );
        output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        output.write( digest.data(), std::streamsize( digest.size() ) );
        output.flush();
        if( !output ) throw std::runtime_error( "query_explicit_pass_header_write" );
    }
    catch( ... )
    {
        const auto actual = FileBytes( path );
        if( actual < bytes && options.disk && options.disk->usage ) options.disk->usage->Release( bytes - actual );
        throw;
    }
}

StoreHeader ReadHeader( const std::filesystem::path& path )
{
    std::ifstream input( analysis::AnalysisIoPath( path ), std::ios::binary );
    if( !input ) throw std::runtime_error( "query_explicit_pass_header_open" );
    StoreHeader header;
    std::array<char, 64> digest {};
    input.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    input.read( digest.data(), digest.size() );
    if( !input || input.peek() != std::ifstream::traits_type::eof() )
        throw std::runtime_error( "query_explicit_pass_header_size" );
    analysis::Sha256Builder hash;
    hash.Update( &header, sizeof( header ) );
    if( hash.FinalHex() != std::string( digest.data(), digest.size() ) )
        throw std::runtime_error( "query_explicit_pass_header_checksum" );
    return header;
}

QueryCorrelationEndpointTag FilterTag( QueryExplicitPassFilterTag tag )
{
    switch( tag )
    {
    case QueryExplicitPassFilterTag::PassSource: return QueryCorrelationEndpointTag::LinkSource;
    case QueryExplicitPassFilterTag::Taxonomy: return QueryCorrelationEndpointTag::LinkTarget;
    case QueryExplicitPassFilterTag::LegacyTaxonomy: return QueryCorrelationEndpointTag::EntitySelf;
    }
    throw std::runtime_error( "query_explicit_pass_filter_tag" );
}

QueryExplicitPassFilterTag FilterTag( QueryCorrelationEndpointTag tag )
{
    if( tag == QueryCorrelationEndpointTag::LinkSource ) return QueryExplicitPassFilterTag::PassSource;
    if( tag == QueryCorrelationEndpointTag::LinkTarget ) return QueryExplicitPassFilterTag::Taxonomy;
    if( tag == QueryCorrelationEndpointTag::EntitySelf ) return QueryExplicitPassFilterTag::LegacyTaxonomy;
    throw std::runtime_error( "query_explicit_pass_filter_tag" );
}

uint64_t Distance( int64_t left, int64_t right )
{
    return left >= right ? uint64_t( left ) - uint64_t( right ) : uint64_t( right ) - uint64_t( left );
}

}

struct QueryExplicitPassStoreWriter::Impl
{
    std::filesystem::path root;
    std::string fingerprint;
    analysis::PagedRecordOptions options;
    StoreHeader header;
    std::unique_ptr<analysis::PagedRecordWriter> passes;
    std::unique_ptr<analysis::PagedRecordWriter> modes;
    std::unique_ptr<analysis::PagedRecordWriter> zones;
    std::unique_ptr<QueryCorrelationStoreWriter> filters;
    uint8_t modeByte = 0;
    bool havePass = false;
    uint32_t lastEntityOrdinal = 0;
    bool haveZone = false;
    QueryExplicitPassGpuZoneKey lastZone;
    bool committed = false;
    bool passesSealed = false;

    Impl( std::filesystem::path value, std::string source, uint64_t entityCount,
        uint64_t zoneCount, uint64_t expectedPasses, uint64_t expectedFilters,
        analysis::PagedRecordOptions config )
        : root( std::move( value ) ), fingerprint( std::move( source ) ), options( std::move( config ) )
    {
        if( !IsDigest( fingerprint ) ) throw std::runtime_error( "query_explicit_pass_source_identity" );
        if( entityCount > UINT32_MAX ) throw std::runtime_error( "query_explicit_pass_entity_ordinal_width" );
        if( std::filesystem::exists( analysis::AnalysisIoPath( root ) ) )
            throw std::runtime_error( "query_explicit_pass_already_exists" );
        header.gfxEntityCount = entityCount;
        header.gpuZoneCount = zoneCount;
        std::memcpy( header.sourceFingerprint, fingerprint.data(), fingerprint.size() );
        passes = std::make_unique<analysis::PagedRecordWriter>( root / "passes",
            Identity( fingerprint, "explicit-pass-projections-v1" ), sizeof( QueryExplicitPassRecord ), options );
        modes = std::make_unique<analysis::PagedRecordWriter>( root / "mode-bits",
            Identity( fingerprint, "explicit-pass-mode-bits-v1" ), sizeof( uint8_t ), options );
        zones = std::make_unique<analysis::PagedRecordWriter>( root / "gpu-zone-time",
            Identity( fingerprint, "explicit-pass-gpu-zone-time-v1" ),
            sizeof( QueryExplicitPassGpuZoneKey ), options );
        filters = std::make_unique<QueryCorrelationStoreWriter>( root / "filters", fingerprint,
            expectedFilters, options );
        (void)expectedPasses;
    }

    ~Impl()
    {
        filters.reset();
        zones.reset();
        modes.reset();
        passes.reset();
        if( committed ) return;
        std::error_code error;
        analysis::AnalysisDiskRemove( root, options.disk, error, true );
    }
};

QueryExplicitPassStoreWriter::QueryExplicitPassStoreWriter( std::filesystem::path root,
    std::string sourceFingerprint, uint64_t gfxEntityCount, uint64_t gpuZoneCount,
    uint64_t expectedPasses, uint64_t expectedFilterRecords, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ),
        gfxEntityCount, gpuZoneCount, expectedPasses, expectedFilterRecords, std::move( options ) ) )
{}

QueryExplicitPassStoreWriter::~QueryExplicitPassStoreWriter() = default;

void QueryExplicitPassStoreWriter::AppendPass( const QueryExplicitPassRecord& record, bool managed )
{
    auto& value = *m_impl;
    if( value.committed || value.passesSealed ) throw std::runtime_error( "query_explicit_pass_writer_closed" );
    Check( value.options );
    const auto segment = record.SegmentOrdinal();
    if( record.entityOrdinal >= value.header.gfxEntityCount ||
        ( record.HasSegment() && ( segment >= value.header.gfxEntityCount || segment == QueryExplicitPassMissingSegment ) ) ||
        ( value.havePass && record.entityOrdinal <= value.lastEntityOrdinal ) )
        throw std::runtime_error( "query_explicit_pass_projection_order" );
    value.passes->Append( &record, 1 );
    if( managed )
    {
        value.modeByte |= uint8_t( 1u << ( value.header.passes & 7 ) );
        value.header.managedPasses++;
    }
    value.header.passes++;
    if( ( value.header.passes & 7 ) == 0 )
    {
        value.modes->Append( &value.modeByte, 1 );
        value.modeByte = 0;
    }
    value.lastEntityOrdinal = record.entityOrdinal;
    value.havePass = true;
}

void QueryExplicitPassStoreWriter::SealPasses()
{
    auto& value = *m_impl;
    if( value.committed || value.passesSealed )
        throw std::runtime_error( "query_explicit_pass_writer_closed" );
    Check( value.options );
    if( ( value.header.passes & 7 ) != 0 ) value.modes->Append( &value.modeByte, 1 );
    value.passes->Commit();
    value.modes->Commit();
    value.passesSealed = true;
}

void QueryExplicitPassStoreWriter::AppendFilter( uint64_t key, QueryExplicitPassFilterTag tag,
    uint64_t projectionOrdinal )
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_explicit_pass_writer_closed" );
    if( projectionOrdinal >= value.header.passes )
        throw std::runtime_error( "query_explicit_pass_filter_ordinal" );
    value.filters->Append( key, FilterTag( tag ), projectionOrdinal );
    value.header.filterRecords++;
}

void QueryExplicitPassStoreWriter::AppendGpuZoneKey( const QueryExplicitPassGpuZoneKey& record )
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_explicit_pass_writer_closed" );
    Check( value.options );
    if( record.zoneOrdinal >= value.header.gpuZoneCount ||
        ( value.haveZone && std::tie( record.queryKey, record.cpuStartNs, record.zoneOrdinal ) <=
            std::tie( value.lastZone.queryKey, value.lastZone.cpuStartNs, value.lastZone.zoneOrdinal ) ) )
        throw std::runtime_error( "query_explicit_pass_gpu_zone_order" );
    value.zones->Append( &record, 1 );
    value.lastZone = record;
    value.haveZone = true;
    value.header.gpuZoneKeys++;
}

QueryExplicitPassBuildStatistics QueryExplicitPassStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( value.committed ) throw std::runtime_error( "query_explicit_pass_writer_closed" );
    Check( value.options );
    if( !value.passesSealed ) SealPasses();
    value.zones->Commit();
    const auto filterStats = value.filters->Commit();
    if( filterStats.records != value.header.filterRecords )
        throw std::runtime_error( "query_explicit_pass_filter_count" );
    WriteHeader( value.root / "header.bin", value.header, value.options );
    value.committed = true;
    return { value.header.passes, value.header.managedPasses, value.header.gpuZoneKeys,
        value.header.filterRecords, filterStats.temporaryBytes };
}

struct QueryExplicitPassStoreReader::Impl
{
    std::filesystem::path root;
    std::string fingerprint;
    analysis::PagedRecordOptions options;
    StoreHeader header;
    analysis::PagedRecordReader passes;
    analysis::PagedRecordReader modes;
    analysis::PagedRecordReader zones;
    QueryCorrelationStoreReader filters;

    Impl( std::filesystem::path value, std::string source, uint64_t entityCount,
        uint64_t zoneCount, analysis::PagedRecordOptions config )
        : root( std::move( value ) ), fingerprint( std::move( source ) ), options( std::move( config ) )
        , header( ReadHeader( root / "header.bin" ) )
        , passes( root / "passes", Identity( fingerprint, "explicit-pass-projections-v1" ),
            sizeof( QueryExplicitPassRecord ), options )
        , modes( root / "mode-bits", Identity( fingerprint, "explicit-pass-mode-bits-v1" ),
            sizeof( uint8_t ), options )
        , zones( root / "gpu-zone-time", Identity( fingerprint, "explicit-pass-gpu-zone-time-v1" ),
            sizeof( QueryExplicitPassGpuZoneKey ), options )
        , filters( root / "filters", fingerprint, options )
    {
        if( std::memcmp( header.magic, "JNPASS1", 7 ) != 0 || header.version != 1 ||
            header.projectionRecordBytes != sizeof( QueryExplicitPassRecord ) ||
            header.zoneRecordBytes != sizeof( QueryExplicitPassGpuZoneKey ) ||
            header.gfxEntityCount != entityCount || header.gpuZoneCount != zoneCount ||
            header.passes != passes.Count() || header.gpuZoneKeys != zones.Count() ||
            modes.Count() != ( header.passes + 7 ) / 8 || header.managedPasses > header.passes ||
            std::string( header.sourceFingerprint, 64 ) != fingerprint ||
            filters.Count() != header.filterRecords )
            throw std::runtime_error( "query_explicit_pass_header" );
    }

    QueryExplicitPassGpuZoneKey Zone( uint64_t ordinal )
    {
        QueryExplicitPassGpuZoneKey result;
        zones.Read( ordinal, &result, 1 );
        return result;
    }

    uint64_t LowerZone( uint64_t key, int64_t time, uint64_t ordinal )
    {
        uint64_t begin = 0, end = header.gpuZoneKeys;
        while( begin < end )
        {
            Check( options );
            const auto middle = begin + ( end - begin ) / 2;
            const auto record = Zone( middle );
            if( std::tie( record.queryKey, record.cpuStartNs, record.zoneOrdinal ) <
                std::tie( key, time, ordinal ) ) begin = middle + 1;
            else end = middle;
        }
        return begin;
    }
};

QueryExplicitPassStoreReader::QueryExplicitPassStoreReader( std::filesystem::path root,
    std::string sourceFingerprint, uint64_t gfxEntityCount, uint64_t gpuZoneCount,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ),
        gfxEntityCount, gpuZoneCount, std::move( options ) ) )
{}

QueryExplicitPassStoreReader::~QueryExplicitPassStoreReader() = default;

uint64_t QueryExplicitPassStoreReader::Count() const { return m_impl->header.passes; }
uint64_t QueryExplicitPassStoreReader::ManagedCount() const { return m_impl->header.managedPasses; }
uint64_t QueryExplicitPassStoreReader::GpuZoneKeyCount() const { return m_impl->header.gpuZoneKeys; }

std::vector<QueryExplicitPassRecord> QueryExplicitPassStoreReader::ReadPasses(
    uint64_t offset, size_t limit ) const
{
    auto& value = *m_impl;
    Check( value.options );
    if( limit > MaximumPage ) throw std::runtime_error( "query_explicit_pass_page_limit" );
    if( limit == 0 || offset >= value.header.passes ) return {};
    const auto take = std::min<uint64_t>( limit, value.header.passes - offset );
    analysis::AnalysisWorkspaceReservation memory( value.options.workspace,
        take * sizeof( QueryExplicitPassRecord ) + 4096 );
    std::vector<QueryExplicitPassRecord> result;
    result.resize( size_t( take ) );
    value.passes.Read( offset, result.data(), take );
    return result;
}

bool QueryExplicitPassStoreReader::IsManaged( uint64_t projectionOrdinal ) const
{
    auto& value = *m_impl;
    Check( value.options );
    if( projectionOrdinal >= value.header.passes )
        throw std::out_of_range( "query_explicit_pass_projection_ordinal" );
    uint8_t bits = 0;
    value.modes.Read( projectionOrdinal / 8, &bits, 1 );
    return ( bits & uint8_t( 1u << ( projectionOrdinal & 7 ) ) ) != 0;
}

std::optional<QueryExplicitPassLocated> QueryExplicitPassStoreReader::FindByEntityOrdinal(
    uint32_t entityOrdinal ) const
{
    auto& value = *m_impl;
    uint64_t begin = 0, end = value.header.passes;
    while( begin < end )
    {
        Check( value.options );
        const auto middle = begin + ( end - begin ) / 2;
        QueryExplicitPassRecord record;
        value.passes.Read( middle, &record, 1 );
        if( record.entityOrdinal < entityOrdinal ) begin = middle + 1;
        else end = middle;
    }
    if( begin == value.header.passes ) return std::nullopt;
    QueryExplicitPassRecord record;
    value.passes.Read( begin, &record, 1 );
    if( record.entityOrdinal != entityOrdinal ) return std::nullopt;
    return QueryExplicitPassLocated { begin, record };
}

uint64_t QueryExplicitPassStoreReader::FilterCandidateCount( uint64_t key ) const
{
    return m_impl->filters.CandidateCount( key );
}

QueryExplicitPassFilterPage QueryExplicitPassStoreReader::ReadFilterCandidates(
    uint64_t key, uint64_t offset, size_t limit ) const
{
    const auto page = m_impl->filters.ReadCandidates( key, offset, limit );
    QueryExplicitPassFilterPage result;
    result.total = page.total;
    result.hasMore = page.hasMore;
    result.values.reserve( page.values.size() );
    for( const auto& candidate : page.values )
        result.values.push_back( { FilterTag( candidate.tag ), candidate.ordinal } );
    return result;
}

std::optional<uint64_t> QueryExplicitPassStoreReader::FindNearestGpuZone(
    uint64_t queryKey, int64_t cpuStartNs ) const
{
    auto& value = *m_impl;
    Check( value.options );
    const auto lower = value.LowerZone( queryKey, cpuStartNs, 0 );
    std::optional<QueryExplicitPassGpuZoneKey> best;
    const auto consider = [&]( const QueryExplicitPassGpuZoneKey& candidate ) {
        if( candidate.queryKey != queryKey ) return;
        if( !best || Distance( candidate.cpuStartNs, cpuStartNs ) < Distance( best->cpuStartNs, cpuStartNs ) ||
            ( Distance( candidate.cpuStartNs, cpuStartNs ) == Distance( best->cpuStartNs, cpuStartNs ) &&
                candidate.zoneOrdinal < best->zoneOrdinal ) ) best = candidate;
    };
    if( lower < value.header.gpuZoneKeys ) consider( value.Zone( lower ) );
    if( lower != 0 )
    {
        const auto previous = value.Zone( lower - 1 );
        if( previous.queryKey == queryKey )
        {
            const auto first = value.LowerZone( queryKey, previous.cpuStartNs, 0 );
            if( first < value.header.gpuZoneKeys ) consider( value.Zone( first ) );
        }
    }
    return best ? std::optional<uint64_t>( best->zoneOrdinal ) : std::nullopt;
}

void QueryExplicitPassStoreReader::ValidateAll()
{
    auto& value = *m_impl;
    Check( value.options );
    uint32_t lastEntity = 0;
    bool firstPass = true;
    value.passes.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const QueryExplicitPassRecord*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            const auto segment = rows[index].SegmentOrdinal();
            if( rows[index].entityOrdinal >= value.header.gfxEntityCount ||
                ( rows[index].HasSegment() && segment >= value.header.gfxEntityCount ) ||
                ( !firstPass && rows[index].entityOrdinal <= lastEntity ) )
                throw std::runtime_error( "query_explicit_pass_projection_order" );
            lastEntity = rows[index].entityOrdinal;
            firstPass = false;
        }
    } );
    uint64_t managed = 0, modeBytes = 0;
    value.modes.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const uint8_t*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            const auto ordinal = modeBytes++;
            uint8_t bits = rows[index];
            if( ordinal + 1 == value.modes.Count() && ( value.header.passes & 7 ) != 0 )
            {
                const auto validMask = uint8_t( ( 1u << ( value.header.passes & 7 ) ) - 1 );
                if( ( bits & ~validMask ) != 0 ) throw std::runtime_error( "query_explicit_pass_mode_tail" );
            }
            while( bits != 0 ) { managed += bits & 1; bits >>= 1; }
        }
    } );
    if( managed != value.header.managedPasses )
        throw std::runtime_error( "query_explicit_pass_mode_count" );
    QueryExplicitPassGpuZoneKey previous;
    bool firstZone = true;
    value.zones.VisitValidated( [&]( const void* data, uint64_t count ) {
        const auto* rows = static_cast<const QueryExplicitPassGpuZoneKey*>( data );
        for( uint64_t index = 0; index < count; index++ )
        {
            Check( value.options );
            if( rows[index].zoneOrdinal >= value.header.gpuZoneCount ||
                ( !firstZone && std::tie( rows[index].queryKey, rows[index].cpuStartNs, rows[index].zoneOrdinal ) <=
                    std::tie( previous.queryKey, previous.cpuStartNs, previous.zoneOrdinal ) ) )
                throw std::runtime_error( "query_explicit_pass_gpu_zone_order" );
            previous = rows[index];
            firstZone = false;
        }
    } );
    value.filters.ValidateAll( [&]( QueryCorrelationEndpointTag tag, uint64_t ordinal ) {
        (void)FilterTag( tag );
        if( ordinal >= value.header.passes )
            throw std::runtime_error( "query_explicit_pass_filter_ordinal" );
    } );
}

}
