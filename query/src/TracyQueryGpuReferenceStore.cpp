#include "TracyQueryGpuReferenceStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace tracy::query
{
namespace
{

struct QueryGpuReferencePassTimeRecord
{
    uint64_t passId = 0;
    int64_t start = 0;
    int64_t end = 0;
};

static_assert( sizeof( QueryGpuReferencePassTimeRecord ) == 24 );

std::string Identity( std::string_view source, std::string_view kind )
{
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( kind.data(), kind.size() );
    constexpr std::string_view version = "query-gpu-reference-v1";
    hash.Update( version.data(), version.size() );
    return hash.FinalHex();
}

std::filesystem::path ResourceLookupPath( const std::filesystem::path& root ) { return root / "by-resource"; }
std::filesystem::path CatalogResourceLookupPath( const std::filesystem::path& root ) { return root / "by-catalog-resource"; }
std::filesystem::path PassTimesPath( const std::filesystem::path& root ) { return root / "pass-times"; }
std::filesystem::path ResolvedUsesPath( const std::filesystem::path& root ) { return root / "resolved-uses"; }

class ValidationSortedRows
{
public:
    ValidationSortedRows( const std::filesystem::path& path,
        const std::shared_ptr<analysis::AnalysisWorkspaceBudget>& workspace )
        : m_memory( workspace, sizeof( analysis::AnalysisKeyMetadataRecord ) * 4096 )
        , m_rows( 4096 )
        , m_input( analysis::AnalysisIoPath( path ), std::ios::binary )
    {
        if( !m_input ) throw std::runtime_error( "GPU reference validation sort output cannot be opened" );
    }

    bool Next( analysis::AnalysisKeyMetadataRecord& row )
    {
        if( m_position == m_count )
        {
            m_input.read( reinterpret_cast<char*>( m_rows.data() ),
                std::streamsize( m_rows.size() * sizeof( m_rows[0] ) ) );
            const auto bytes = size_t( m_input.gcount() );
            if( bytes == 0 )
            {
                if( !m_input.eof() ) throw std::runtime_error( "GPU reference validation sort read failed" );
                return false;
            }
            if( bytes % sizeof( m_rows[0] ) != 0 )
                throw std::runtime_error( "GPU reference validation sort is truncated" );
            m_position = 0;
            m_count = bytes / sizeof( m_rows[0] );
        }
        row = m_rows[m_position++];
        return true;
    }

private:
    analysis::AnalysisWorkspaceReservation m_memory;
    std::vector<analysis::AnalysisKeyMetadataRecord> m_rows;
    std::ifstream m_input;
    size_t m_position = 0;
    size_t m_count = 0;
};

}

void QueryGpuReferenceStore::Build( const std::filesystem::path& root, std::string sourceFingerprint,
    const QueryGpuReferenceInput& input, analysis::PagedRecordOptions options,
    QueryGpuReferenceBuildStatistics* statistics )
{
    if( statistics ) *statistics = {};
    auto phaseStarted = std::chrono::steady_clock::now();
    const auto finishPhase = [&]( uint64_t QueryGpuReferenceBuildStatistics::*field ) {
        const auto now = std::chrono::steady_clock::now();
        if( statistics ) statistics->*field = uint64_t( std::chrono::duration_cast<
            std::chrono::milliseconds>( now - phaseStarted ).count() );
        phaseStarted = now;
    };
    if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "GPU reference source identity must be SHA-256" );
    const auto lookupOutput = ResourceLookupPath( root );
    const auto catalogLookupOutput = CatalogResourceLookupPath( root );
    const auto passTimesOutput = PassTimesPath( root );
    const auto usesOutput = ResolvedUsesPath( root );
    const auto scratch = root / "reverse-scratch";
    if( std::filesystem::exists( analysis::AnalysisIoPath( lookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( catalogLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( passTimesOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( usesOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( scratch ) ) )
        throw std::runtime_error( "GPU reference reverse index already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratch ) );
    const auto passEventsSource = scratch / "pass-events-source.work";
    const auto passEventsSorted = scratch / "pass-events-sorted.work";
    const auto usePassSorted = scratch / "use-pass-sorted.work";
    const auto resourceSource = scratch / "resource-source.work";
    const auto resourceSorted = scratch / "resource-sorted.work";
    const auto catalogResourceSorted = scratch / "catalog-resource-sorted.work";
    const auto cleanup = [&]( bool outputs ) {
        std::string failure;
        const auto remove = [&]( const std::filesystem::path& path ) {
            std::error_code error;
            analysis::AnalysisDiskRemove( path, options.disk, error, true );
            if( error && failure.empty() ) failure = error.message();
        };
        remove( scratch );
        if( outputs ) { remove( lookupOutput ); remove( catalogLookupOutput ); remove( passTimesOutput ); remove( usesOutput ); }
        return failure;
    };
    const auto removeConsumed = [&]( const std::filesystem::path& path ) {
        std::error_code error;
        analysis::AnalysisDiskRemove( path, options.disk, error );
        if( error ) throw std::runtime_error( "GPU reference scratch cleanup failed: " +
            path.filename().string() + ": " + error.message() );
    };
    try
    {
        std::ofstream passEvents( analysis::AnalysisIoPath( passEventsSource ), std::ios::binary | std::ios::trunc );
        if( !passEvents ) throw std::runtime_error( "GPU reference pass event source cannot be opened" );
        uint64_t passEventCount = 0;
        const auto appendPassEvent = [&]( uint64_t passId, uint64_t kind, int64_t time ) {
            if( passId == 0 ) return;
            analysis::AnalysisKeyMetadataRecord row {};
            row.keyHigh = passId;
            row.keyLow = kind;
            row.metadata = std::bit_cast<uint64_t>( time );
            analysis::AnalysisDiskGrow( options.disk, sizeof( row ) );
            passEvents.write( reinterpret_cast<const char*>( &row ), sizeof( row ) );
            if( !passEvents ) throw std::runtime_error( "GPU reference pass event write failed" );
            passEventCount++;
        };
        for( uint64_t ordinal = 0; ordinal < input.PassCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU reference index cancelled" );
            const auto row = input.Pass( ordinal );
            appendPassEvent( row.passId, 0, row.time );
        }
        for( uint64_t ordinal = 0; ordinal < input.EndCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU reference index cancelled" );
            const auto row = input.End( ordinal );
            appendPassEvent( row.passId, 1, row.time );
        }
        passEvents.flush();
        if( !passEvents ) throw std::runtime_error( "GPU reference pass event flush failed" );
        passEvents.close();
        std::string sortError;
        if( !analysis::SortAnalysisKeyMetadataRecords( passEventsSource, passEventsSorted, scratch,
                "gpu-reference-pass-events", passEventCount,
                analysis::ChooseAnalysisExternalSortBufferRecords( passEventCount,
                    sizeof( analysis::AnalysisKeyMetadataRecord ), options.workspace ), sortError,
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->passEventSort : nullptr ) )
            throw std::runtime_error( "GPU reference pass event sort failed: " + sortError );
        removeConsumed( passEventsSource );
        finishPhase( &QueryGpuReferenceBuildStatistics::passEventSortMs );

        analysis::PagedRecordWriter passTimes( passTimesOutput,
            Identity( sourceFingerprint, "pass-times" ), sizeof( QueryGpuReferencePassTimeRecord ), options );
        std::ifstream sortedPassEvents( analysis::AnalysisIoPath( passEventsSorted ), std::ios::binary );
        if( !sortedPassEvents ) throw std::runtime_error( "GPU reference sorted pass events cannot be opened" );
        analysis::AnalysisKeyMetadataRecord event {};
        bool haveEvent = bool( sortedPassEvents.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) );
        while( haveEvent )
        {
            const auto passId = event.keyHigh;
            bool haveStart = false;
            int64_t start = 0;
            int64_t end = 0;
            while( haveEvent && event.keyHigh == passId )
            {
                const auto time = std::bit_cast<int64_t>( event.metadata );
                if( event.keyLow == 0 && !haveStart ) { start = time; end = time; haveStart = true; }
                else if( event.keyLow == 1 && haveStart ) end = std::max( end, time );
                haveEvent = bool( sortedPassEvents.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) );
            }
            if( !haveEvent && !sortedPassEvents.eof() )
                throw std::runtime_error( "GPU reference sorted pass event read failed" );
            if( haveStart )
            {
                const QueryGpuReferencePassTimeRecord row { passId, start, end };
                passTimes.Append( &row, 1 );
            }
        }
        passTimes.Commit();
        sortedPassEvents.close();
        removeConsumed( passEventsSorted );
        finishPhase( &QueryGpuReferenceBuildStatistics::passTimePublishMs );

        if( !analysis::SortAnalysisUInt64PairsGenerated( usePassSorted, scratch,
                "gpu-reference-uses-by-pass",
                analysis::ChooseAnalysisExternalSortBufferRecords( input.UseCount(),
                    sizeof( analysis::AnalysisUInt64Pair ), options.workspace ),
                [&]( const auto& emit ) {
                    for( uint64_t ordinal = 0; ordinal < input.UseCount(); ordinal++ )
                    {
                        if( options.cancelled && options.cancelled() )
                            throw std::runtime_error( "GPU reference index cancelled" );
                        const auto use = input.Use( ordinal );
                        if( use.passId == 0 || use.resourceId == 0 ) continue;
                        emit( analysis::AnalysisUInt64Pair { use.passId, ordinal } );
                    }
                }, sortError, options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->usePassSort : nullptr ) )
            throw std::runtime_error( "GPU reference use sort failed: " + sortError );
        finishPhase( &QueryGpuReferenceBuildStatistics::usePassSortMs );

        analysis::PagedRecordReader passTimeReader( passTimesOutput,
            Identity( sourceFingerprint, "pass-times" ), sizeof( QueryGpuReferencePassTimeRecord ), options );
        analysis::PagedRecordWriter resolvedUses( usesOutput,
            Identity( sourceFingerprint, "resolved-uses" ), sizeof( QueryGpuReferenceResolvedUseRecord ), options );
        std::ofstream resourceLookup( analysis::AnalysisIoPath( resourceSource ), std::ios::binary | std::ios::trunc );
        if( !resourceLookup )
            throw std::runtime_error( "GPU reference resource lookup source cannot be opened" );
        std::ifstream sortedUses( analysis::AnalysisIoPath( usePassSorted ), std::ios::binary );
        if( !sortedUses ) throw std::runtime_error( "GPU reference sorted uses cannot be opened" );
        uint64_t passTimeOrdinal = 0;
        uint64_t resolvedOrdinal = 0;
        uint64_t resourceLookupCount = 0;
        analysis::AnalysisUInt64Pair useIndex {};
        while( sortedUses.read( reinterpret_cast<char*>( &useIndex ), sizeof( useIndex ) ) )
        {
            QueryGpuReferencePassTimeRecord passTime {};
            while( passTimeOrdinal < passTimeReader.Count() )
            {
                passTimeReader.Read( passTimeOrdinal, &passTime, 1 );
                if( passTime.passId >= useIndex.key ) break;
                passTimeOrdinal++;
            }
            if( passTimeOrdinal >= passTimeReader.Count() || passTime.passId != useIndex.key ) continue;
            const auto use = input.Use( useIndex.value );
            QueryGpuReferenceResolvedUseRecord resolved {};
            resolved.resourceId = use.resourceId;
            resolved.passId = use.passId;
            resolved.passStart = passTime.start;
            resolved.passEnd = passTime.end;
            resolved.usageMask = use.usageMask;
            resolved.resourceSetId = use.resourceSetId;
            resolved.flags = use.flags;
            resolved.encoding = use.encoding;
            resolvedUses.Append( &resolved, 1 );
            analysis::AnalysisKeyMetadataRecord lookup {};
            lookup.keyHigh = resolved.resourceId;
            lookup.keyLow = useIndex.value;
            lookup.metadata = resolvedOrdinal++;
            analysis::AnalysisDiskGrow( options.disk, sizeof( lookup ) );
            resourceLookup.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
            if( !resourceLookup ) throw std::runtime_error( "GPU reference resource lookup write failed" );
            resourceLookupCount++;
        }
        if( !sortedUses.eof() ) throw std::runtime_error( "GPU reference sorted use read failed" );
        resolvedUses.Commit();
        sortedUses.close();
        removeConsumed( usePassSorted );
        resourceLookup.flush();
        if( !resourceLookup )
            throw std::runtime_error( "GPU reference resource lookup flush failed" );
        resourceLookup.close();
        finishPhase( &QueryGpuReferenceBuildStatistics::resolveUsesMs );
        if( !analysis::SortAnalysisKeyMetadataRecords( resourceSource, resourceSorted, scratch,
                "gpu-reference-uses-by-resource", resourceLookupCount,
                analysis::ChooseAnalysisExternalSortBufferRecords( resourceLookupCount,
                    sizeof( analysis::AnalysisKeyMetadataRecord ), options.workspace ), sortError,
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->resourceSort : nullptr ) )
            throw std::runtime_error( "GPU reference resource lookup sort failed: " + sortError );
        finishPhase( &QueryGpuReferenceBuildStatistics::resourceSortMs );
        std::array<analysis::AnalysisKeyMetadataRecord, 4096> buffer;
        {
            analysis::PagedRecordWriter writer( lookupOutput,
                Identity( sourceFingerprint, "by-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            std::ifstream sorted( analysis::AnalysisIoPath( resourceSorted ), std::ios::binary );
            if( !sorted ) throw std::runtime_error( "GPU reference sorted resource lookup cannot be opened" );
            while( sorted )
            {
                sorted.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
                const auto bytes = size_t( sorted.gcount() );
                if( bytes % sizeof( buffer[0] ) != 0 ) throw std::runtime_error( "GPU reference lookup is truncated" );
                if( bytes != 0 ) writer.Append( buffer.data(), bytes / sizeof( buffer[0] ) );
            }
            if( !sorted.eof() ) throw std::runtime_error( "GPU reference resource lookup read failed" );
            writer.Commit();
        }
        removeConsumed( resourceSorted );
        finishPhase( &QueryGpuReferenceBuildStatistics::resourcePublishMs );

        uint64_t catalogResourceLookupCount = 0;
        if( !analysis::SortAnalysisKeyMetadataRecordsGenerated( catalogResourceSorted, scratch,
                "gpu-reference-uses-by-catalog-resource",
                analysis::ChooseAnalysisExternalSortBufferRecords( resourceLookupCount,
                    sizeof( analysis::AnalysisKeyMetadataRecord ), options.workspace ),
                [&]( const auto& emit ) {
                    std::ifstream source( analysis::AnalysisIoPath( resourceSource ), std::ios::binary );
                    if( !source ) throw std::runtime_error( "GPU reference resource source cannot be reopened" );
                    analysis::PagedRecordReader uses( usesOutput, Identity( sourceFingerprint, "resolved-uses" ),
                        sizeof( QueryGpuReferenceResolvedUseRecord ), options );
                    uint64_t ordinal = 0;
                    analysis::AnalysisKeyMetadataRecord lookup {};
                    while( source.read( reinterpret_cast<char*>( &lookup ), sizeof( lookup ) ) )
                    {
                        if( options.cancelled && options.cancelled() )
                            throw std::runtime_error( "GPU reference index cancelled" );
                        if( lookup.metadata != ordinal )
                            throw std::runtime_error( "GPU reference resource source ordinal changed" );
                        QueryGpuReferenceResolvedUseRecord resolved {};
                        uses.Read( ordinal++, &resolved, 1 );
                        if( lookup.keyHigh != resolved.resourceId )
                            throw std::runtime_error( "GPU reference resource source identity changed" );
                        const auto catalogResourceId = input.ResolveCatalogResource(
                            resolved.resourceId, resolved.passEnd );
                        if( catalogResourceId == 0 ) continue;
                        lookup.keyHigh = catalogResourceId;
                        emit( lookup );
                        catalogResourceLookupCount++;
                    }
                    if( !source.eof() || source.gcount() != 0 || ordinal != resourceLookupCount )
                        throw std::runtime_error( "GPU reference resource source count changed" );
                    finishPhase( &QueryGpuReferenceBuildStatistics::catalogReplayMs );
                }, sortError, options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->catalogSort : nullptr ) )
            throw std::runtime_error( "GPU reference Catalog resource lookup sort failed: " + sortError );
        removeConsumed( resourceSource );
        finishPhase( &QueryGpuReferenceBuildStatistics::catalogSortMs );
        analysis::PagedRecordWriter catalogLookupWriter( catalogLookupOutput,
            Identity( sourceFingerprint, "by-catalog-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
        std::ifstream sortedCatalogResources( analysis::AnalysisIoPath( catalogResourceSorted ), std::ios::binary );
        if( !sortedCatalogResources )
            throw std::runtime_error( "GPU reference sorted Catalog resource lookup cannot be opened" );
        uint64_t catalogRowsWritten = 0;
        while( sortedCatalogResources )
        {
            sortedCatalogResources.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
            const auto bytes = size_t( sortedCatalogResources.gcount() );
            if( bytes % sizeof( buffer[0] ) != 0 )
                throw std::runtime_error( "GPU reference Catalog lookup is truncated" );
            if( bytes != 0 )
            {
                catalogLookupWriter.Append( buffer.data(), bytes / sizeof( buffer[0] ) );
                catalogRowsWritten += bytes / sizeof( buffer[0] );
            }
        }
        if( !sortedCatalogResources.eof() )
            throw std::runtime_error( "GPU reference Catalog resource lookup read failed" );
        if( catalogRowsWritten != catalogResourceLookupCount )
            throw std::runtime_error( "GPU reference sorted Catalog resource lookup count changed" );
        catalogLookupWriter.Commit();
        sortedCatalogResources.close();
        removeConsumed( catalogResourceSorted );
        const auto cleanupError = cleanup( false );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU reference scratch cleanup failed: " + cleanupError );
        finishPhase( &QueryGpuReferenceBuildStatistics::catalogPublishMs );
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        const auto cleanupError = cleanup( true );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU reference cleanup failed: " + cleanupError );
        std::rethrow_exception( failure );
    }
}

struct QueryGpuReferenceStoreReader::Impl
{
    Impl( std::filesystem::path root, std::string source, analysis::PagedRecordOptions options )
        : root( std::move( root ) )
        , options( std::move( options ) )
        , lookup( ResourceLookupPath( this->root ), Identity( source, "by-resource" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), this->options )
        , catalogLookup( CatalogResourceLookupPath( this->root ), Identity( source, "by-catalog-resource" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), this->options )
        , passTimes( PassTimesPath( this->root ), Identity( source, "pass-times" ),
            sizeof( QueryGpuReferencePassTimeRecord ), this->options )
        , uses( ResolvedUsesPath( this->root ), Identity( source, "resolved-uses" ),
            sizeof( QueryGpuReferenceResolvedUseRecord ), this->options )
    {}
    std::filesystem::path root;
    analysis::PagedRecordOptions options;
    analysis::PagedRecordReader lookup;
    analysis::PagedRecordReader catalogLookup;
    analysis::PagedRecordReader passTimes;
    analysis::PagedRecordReader uses;
};

QueryGpuReferenceStoreReader::QueryGpuReferenceStoreReader( std::filesystem::path root,
    std::string sourceFingerprint, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGpuReferenceStoreReader::~QueryGpuReferenceStoreReader() = default;

QueryGpuReferenceUsePage QueryGpuReferenceStoreReader::UsesByResource(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryGpuReferenceUsePage result;
    if( resourceId == 0 || limit == 0 ) return result;
    auto& lookup = m_impl->lookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->uses.Count() )
            throw std::runtime_error( "GPU reference resource lookup identity mismatch" );
        QueryGpuReferenceResolvedUseRecord value {};
        m_impl->uses.Read( row.metadata, &value, 1 );
        if( value.resourceId != resourceId )
            throw std::runtime_error( "GPU reference resource lookup identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

QueryGpuReferenceUsePage QueryGpuReferenceStoreReader::UsesByCatalogResource(
    uint64_t resourceId, uint64_t offset, size_t limit ) const
{
    QueryGpuReferenceUsePage result;
    if( resourceId == 0 || limit == 0 ) return result;
    auto& lookup = m_impl->catalogLookup;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < resourceId || ( upper && row.keyHigh == resourceId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != resourceId || row.metadata >= m_impl->uses.Count() )
            throw std::runtime_error( "GPU reference Catalog resource lookup identity mismatch" );
        QueryGpuReferenceResolvedUseRecord value {};
        m_impl->uses.Read( row.metadata, &value, 1 );
        result.values.emplace_back( value );
    }
    return result;
}

QueryGpuReferenceUsePage QueryGpuReferenceStoreReader::UsesByPass(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryGpuReferenceUsePage result;
    if( passId == 0 || limit == 0 ) return result;
    auto& uses = m_impl->uses;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = uses.Count();
        QueryGpuReferenceResolvedUseRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            uses.Read( middle, &row, 1 );
            if( row.passId < passId || ( upper && row.passId == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false );
    const auto rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        QueryGpuReferenceResolvedUseRecord value {};
        uses.Read( index, &value, 1 );
        if( value.passId != passId )
            throw std::runtime_error( "GPU reference pass range identity mismatch" );
        result.values.emplace_back( value );
    }
    return result;
}

uint64_t QueryGpuReferenceStoreReader::UseCount() const
{
    return m_impl->uses.Count();
}

QueryGpuReferenceResolvedUseRecord QueryGpuReferenceStoreReader::UseAt( uint64_t ordinal ) const
{
    if( ordinal >= m_impl->uses.Count() ) throw std::out_of_range( "GPU reference use ordinal" );
    QueryGpuReferenceResolvedUseRecord result {};
    m_impl->uses.Read( ordinal, &result, 1 );
    return result;
}

QueryGpuReferenceValidationStats QueryGpuReferenceStoreReader::ValidateAll(
    const std::function<uint64_t( uint64_t, int64_t )>& resolveCatalogResource,
    const std::filesystem::path& scratchRoot ) const
{
    using Record = analysis::AnalysisKeyMetadataRecord;
    const auto scratch = scratchRoot.empty() ? m_impl->root / "validation-scratch" : scratchRoot;
    if( std::filesystem::exists( analysis::AnalysisIoPath( scratch ) ) )
        throw std::runtime_error( "GPU reference validation scratch already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratch ) );
    const auto cleanup = [&]() {
        std::error_code error;
        analysis::AnalysisDiskRemove( scratch, m_impl->options.disk, error, true );
        if( error ) throw std::runtime_error( "GPU reference validation scratch cleanup failed: " + error.message() );
    };
    const auto removeFile = [&]( const std::filesystem::path& path ) {
        std::error_code error;
        analysis::AnalysisDiskRemove( path, m_impl->options.disk, error );
        if( error ) throw std::runtime_error( "GPU reference validation scratch removal failed: " + error.message() );
    };
    const auto before = m_impl->uses.Statistics();
    try
    {
        uint64_t previousPass = 0, passOrdinal = 0;
        m_impl->passTimes.VisitValidated( [&]( const void* data, uint64_t count ) {
            const auto* bytes = static_cast<const char*>( data );
            for( uint64_t index = 0; index < count; index++, passOrdinal++ )
            {
                QueryGpuReferencePassTimeRecord row {};
                std::memcpy( &row, bytes + index * sizeof( row ), sizeof( row ) );
                if( row.passId == 0 || row.end < row.start ||
                    ( passOrdinal != 0 && row.passId <= previousPass ) )
                    throw std::runtime_error( "GPU reference pass time order mismatch" );
                previousPass = row.passId;
            }
        } );
        previousPass = 0;
        uint64_t useOrdinal = 0;
        m_impl->uses.VisitValidated( [&]( const void* data, uint64_t count ) {
            const auto* bytes = static_cast<const char*>( data );
            for( uint64_t index = 0; index < count; index++, useOrdinal++ )
            {
                QueryGpuReferenceResolvedUseRecord row {};
                std::memcpy( &row, bytes + index * sizeof( row ), sizeof( row ) );
                if( row.passId == 0 || row.resourceId == 0 || row.passEnd < row.passStart ||
                    ( useOrdinal != 0 && row.passId < previousPass ) )
                    throw std::runtime_error( "GPU reference resolved use order mismatch" );
                previousPass = row.passId;
            }
        } );

        const auto writeByUseOrdinal = [&]( analysis::PagedRecordReader& source,
            const std::filesystem::path& path, const char* orderError ) {
            std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "GPU reference validation source cannot be opened" );
            analysis::AnalysisWorkspaceReservation memory( m_impl->options.workspace,
                uint64_t( m_impl->options.blockBytes ) + 4096 );
            std::vector<Record> reordered;
            Record previous {};
            uint64_t ordinal = 0;
            source.VisitValidated( [&]( const void* data, uint64_t count ) {
                const auto* bytes = static_cast<const char*>( data );
                reordered.resize( size_t( count ) );
                for( uint64_t index = 0; index < count; index++, ordinal++ )
                {
                    Record row {};
                    std::memcpy( &row, bytes + index * sizeof( row ), sizeof( row ) );
                    if( row.keyHigh == 0 || row.metadata >= m_impl->uses.Count() ||
                        ( ordinal != 0 && ( row.keyHigh < previous.keyHigh ||
                            ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                                ( row.keyLow == previous.keyLow && row.metadata < previous.metadata ) ) ) ) ) )
                        throw std::runtime_error( orderError );
                    reordered[size_t( index )].keyHigh = row.metadata;
                    reordered[size_t( index )].metadata = row.keyHigh;
                    previous = row;
                }
                const auto length = count * sizeof( Record );
                analysis::AnalysisDiskGrow( m_impl->options.disk, length );
                output.write( reinterpret_cast<const char*>( reordered.data() ), std::streamsize( length ) );
                if( !output ) throw std::runtime_error( "GPU reference validation source write failed" );
            } );
            output.flush();
            if( !output ) throw std::runtime_error( "GPU reference validation source flush failed" );
        };
        const auto sortByUseOrdinal = [&]( const std::filesystem::path& source,
            const std::filesystem::path& sorted, const char* prefix, uint64_t count ) {
            std::string error;
            if( !analysis::SortAnalysisKeyMetadataRecords( source, sorted, scratch, prefix, count,
                    512 * 1024, error, m_impl->options.disk, m_impl->options.workspace,
                    m_impl->options.cancelled ) )
                throw std::runtime_error( "GPU reference validation sort failed: " + error );
            removeFile( source );
        };

        const auto resourceSource = scratch / "resource-source.work";
        const auto resourceSorted = scratch / "resource-sorted.work";
        writeByUseOrdinal( m_impl->lookup, resourceSource, "GPU reference resource lookup order mismatch" );
        sortByUseOrdinal( resourceSource, resourceSorted, "gpu-reference-validation-resource",
            m_impl->lookup.Count() );
        {
            ValidationSortedRows sorted( resourceSorted, m_impl->options.workspace );
            uint64_t ordinal = 0;
            m_impl->uses.VisitValidated( [&]( const void* data, uint64_t count ) {
                const auto* bytes = static_cast<const char*>( data );
                for( uint64_t index = 0; index < count; index++, ordinal++ )
                {
                    if( ( ordinal & 4095 ) == 0 && m_impl->options.cancelled && m_impl->options.cancelled() )
                        throw std::runtime_error( "GPU reference validation cancelled" );
                    Record lookup {};
                    if( !sorted.Next( lookup ) || lookup.keyHigh != ordinal )
                        throw std::runtime_error( "GPU reference resource lookup ordinal mismatch" );
                    QueryGpuReferenceResolvedUseRecord use {};
                    std::memcpy( &use, bytes + index * sizeof( use ), sizeof( use ) );
                    if( lookup.metadata != use.resourceId )
                        throw std::runtime_error( "GPU reference resource lookup identity mismatch" );
                }
            } );
            Record extra {};
            if( sorted.Next( extra ) ) throw std::runtime_error( "GPU reference resource lookup duplicate ordinal" );
        }
        removeFile( resourceSorted );

        const auto catalogSource = scratch / "catalog-source.work";
        const auto catalogSorted = scratch / "catalog-sorted.work";
        writeByUseOrdinal( m_impl->catalogLookup, catalogSource,
            "GPU reference Catalog resource lookup order mismatch" );
        sortByUseOrdinal( catalogSource, catalogSorted, "gpu-reference-validation-catalog",
            m_impl->catalogLookup.Count() );
        {
            ValidationSortedRows sorted( catalogSorted, m_impl->options.workspace );
            Record next {};
            bool haveNext = sorted.Next( next );
            uint64_t ordinal = 0;
            m_impl->uses.VisitValidated( [&]( const void* data, uint64_t count ) {
                const auto* bytes = static_cast<const char*>( data );
                for( uint64_t index = 0; index < count; index++, ordinal++ )
                {
                    if( ( ordinal & 4095 ) == 0 && m_impl->options.cancelled && m_impl->options.cancelled() )
                        throw std::runtime_error( "GPU reference validation cancelled" );
                    QueryGpuReferenceResolvedUseRecord use {};
                    std::memcpy( &use, bytes + index * sizeof( use ), sizeof( use ) );
                    const auto expected = resolveCatalogResource ?
                        resolveCatalogResource( use.resourceId, use.passEnd ) : 0;
                    if( haveNext && next.keyHigh < ordinal )
                        throw std::runtime_error( "GPU reference Catalog lookup ordinal mismatch" );
                    if( haveNext && next.keyHigh == ordinal )
                    {
                        if( next.metadata == 0 || ( resolveCatalogResource && next.metadata != expected ) )
                            throw std::runtime_error( "GPU reference Catalog resource lookup identity mismatch" );
                        haveNext = sorted.Next( next );
                        if( haveNext && next.keyHigh == ordinal )
                            throw std::runtime_error( "GPU reference Catalog lookup duplicate ordinal" );
                    }
                    else if( resolveCatalogResource && expected != 0 )
                        throw std::runtime_error( "GPU reference Catalog lookup missing ordinal" );
                }
            } );
            if( haveNext ) throw std::runtime_error( "GPU reference Catalog lookup ordinal out of range" );
        }
        removeFile( catalogSorted );
        cleanup();
        const auto after = m_impl->uses.Statistics();
        QueryGpuReferenceValidationStats result;
        result.useBlockLoads = after.blockLoads - before.blockLoads;
        result.useDecodedBytes = after.decodedBytes - before.decodedBytes;
        result.sortedReferenceCount = m_impl->lookup.Count() + m_impl->catalogLookup.Count();
        return result;
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        cleanup();
        std::rethrow_exception( failure );
    }
}

}
