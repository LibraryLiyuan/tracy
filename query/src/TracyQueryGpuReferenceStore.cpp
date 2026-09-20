#include "TracyQueryGpuReferenceStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

}

void QueryGpuReferenceStore::Build( const std::filesystem::path& root, std::string sourceFingerprint,
    const QueryGpuReferenceInput& input, analysis::PagedRecordOptions options )
{
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
    const auto usePassSource = scratch / "use-pass-source.work";
    const auto usePassSorted = scratch / "use-pass-sorted.work";
    const auto resourceSource = scratch / "resource-source.work";
    const auto resourceSorted = scratch / "resource-sorted.work";
    const auto catalogResourceSource = scratch / "catalog-resource-source.work";
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
                "gpu-reference-pass-events", passEventCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU reference pass event sort failed: " + sortError );

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

        std::ofstream usePasses( analysis::AnalysisIoPath( usePassSource ), std::ios::binary | std::ios::trunc );
        if( !usePasses ) throw std::runtime_error( "GPU reference use source cannot be opened" );
        uint64_t usePassCount = 0;
        for( uint64_t ordinal = 0; ordinal < input.UseCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU reference index cancelled" );
            const auto use = input.Use( ordinal );
            if( use.passId == 0 || use.resourceId == 0 ) continue;
            analysis::AnalysisKeyMetadataRecord row {};
            row.keyHigh = use.passId;
            row.metadata = ordinal;
            analysis::AnalysisDiskGrow( options.disk, sizeof( row ) );
            usePasses.write( reinterpret_cast<const char*>( &row ), sizeof( row ) );
            if( !usePasses ) throw std::runtime_error( "GPU reference use source write failed" );
            usePassCount++;
        }
        usePasses.flush();
        if( !usePasses ) throw std::runtime_error( "GPU reference use source flush failed" );
        usePasses.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( usePassSource, usePassSorted, scratch,
                "gpu-reference-uses-by-pass", usePassCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU reference use sort failed: " + sortError );

        analysis::PagedRecordReader passTimeReader( passTimesOutput,
            Identity( sourceFingerprint, "pass-times" ), sizeof( QueryGpuReferencePassTimeRecord ), options );
        analysis::PagedRecordWriter resolvedUses( usesOutput,
            Identity( sourceFingerprint, "resolved-uses" ), sizeof( QueryGpuReferenceResolvedUseRecord ), options );
        std::ofstream resourceLookup( analysis::AnalysisIoPath( resourceSource ), std::ios::binary | std::ios::trunc );
        std::ofstream catalogResourceLookup( analysis::AnalysisIoPath( catalogResourceSource ), std::ios::binary | std::ios::trunc );
        if( !resourceLookup || !catalogResourceLookup )
            throw std::runtime_error( "GPU reference resource lookup source cannot be opened" );
        std::ifstream sortedUses( analysis::AnalysisIoPath( usePassSorted ), std::ios::binary );
        if( !sortedUses ) throw std::runtime_error( "GPU reference sorted uses cannot be opened" );
        uint64_t passTimeOrdinal = 0;
        uint64_t resolvedOrdinal = 0;
        uint64_t resourceLookupCount = 0;
        uint64_t catalogResourceLookupCount = 0;
        analysis::AnalysisKeyMetadataRecord useIndex {};
        while( sortedUses.read( reinterpret_cast<char*>( &useIndex ), sizeof( useIndex ) ) )
        {
            QueryGpuReferencePassTimeRecord passTime {};
            while( passTimeOrdinal < passTimeReader.Count() )
            {
                passTimeReader.Read( passTimeOrdinal, &passTime, 1 );
                if( passTime.passId >= useIndex.keyHigh ) break;
                passTimeOrdinal++;
            }
            if( passTimeOrdinal >= passTimeReader.Count() || passTime.passId != useIndex.keyHigh ) continue;
            const auto use = input.Use( useIndex.metadata );
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
            lookup.keyLow = useIndex.metadata;
            lookup.metadata = resolvedOrdinal++;
            analysis::AnalysisDiskGrow( options.disk, sizeof( lookup ) );
            resourceLookup.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
            if( !resourceLookup ) throw std::runtime_error( "GPU reference resource lookup write failed" );
            resourceLookupCount++;
            const auto catalogResourceId = input.ResolveCatalogResource( resolved.resourceId, resolved.passEnd );
            if( catalogResourceId != 0 )
            {
                lookup.keyHigh = catalogResourceId;
                analysis::AnalysisDiskGrow( options.disk, sizeof( lookup ) );
                catalogResourceLookup.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
                if( !catalogResourceLookup )
                    throw std::runtime_error( "GPU reference Catalog resource lookup write failed" );
                catalogResourceLookupCount++;
            }
        }
        if( !sortedUses.eof() ) throw std::runtime_error( "GPU reference sorted use read failed" );
        resolvedUses.Commit();
        sortedUses.close();
        resourceLookup.flush();
        catalogResourceLookup.flush();
        if( !resourceLookup || !catalogResourceLookup )
            throw std::runtime_error( "GPU reference resource lookup flush failed" );
        resourceLookup.close();
        catalogResourceLookup.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( resourceSource, resourceSorted, scratch,
                "gpu-reference-uses-by-resource", resourceLookupCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU reference resource lookup sort failed: " + sortError );
        if( !analysis::SortAnalysisKeyMetadataRecords( catalogResourceSource, catalogResourceSorted, scratch,
                "gpu-reference-uses-by-catalog-resource", catalogResourceLookupCount, 512 * 1024,
                sortError, options.disk, options.workspace ) )
            throw std::runtime_error( "GPU reference Catalog resource lookup sort failed: " + sortError );
        analysis::PagedRecordWriter lookupWriter( lookupOutput,
            Identity( sourceFingerprint, "by-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
        std::ifstream sortedResources( analysis::AnalysisIoPath( resourceSorted ), std::ios::binary );
        if( !sortedResources ) throw std::runtime_error( "GPU reference sorted resource lookup cannot be opened" );
        std::array<analysis::AnalysisKeyMetadataRecord, 4096> buffer;
        while( sortedResources )
        {
            sortedResources.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
            const auto bytes = size_t( sortedResources.gcount() );
            if( bytes % sizeof( buffer[0] ) != 0 ) throw std::runtime_error( "GPU reference lookup is truncated" );
            if( bytes != 0 ) lookupWriter.Append( buffer.data(), bytes / sizeof( buffer[0] ) );
        }
        if( !sortedResources.eof() ) throw std::runtime_error( "GPU reference resource lookup read failed" );
        lookupWriter.Commit();
        sortedResources.close();
        analysis::PagedRecordWriter catalogLookupWriter( catalogLookupOutput,
            Identity( sourceFingerprint, "by-catalog-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
        std::ifstream sortedCatalogResources( analysis::AnalysisIoPath( catalogResourceSorted ), std::ios::binary );
        if( !sortedCatalogResources )
            throw std::runtime_error( "GPU reference sorted Catalog resource lookup cannot be opened" );
        while( sortedCatalogResources )
        {
            sortedCatalogResources.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
            const auto bytes = size_t( sortedCatalogResources.gcount() );
            if( bytes % sizeof( buffer[0] ) != 0 )
                throw std::runtime_error( "GPU reference Catalog lookup is truncated" );
            if( bytes != 0 ) catalogLookupWriter.Append( buffer.data(), bytes / sizeof( buffer[0] ) );
        }
        if( !sortedCatalogResources.eof() )
            throw std::runtime_error( "GPU reference Catalog resource lookup read failed" );
        catalogLookupWriter.Commit();
        sortedCatalogResources.close();
        const auto cleanupError = cleanup( false );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU reference scratch cleanup failed: " + cleanupError );
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
        : lookup( ResourceLookupPath( root ), Identity( source, "by-resource" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), options )
        , catalogLookup( CatalogResourceLookupPath( root ), Identity( source, "by-catalog-resource" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), options )
        , passTimes( PassTimesPath( root ), Identity( source, "pass-times" ),
            sizeof( QueryGpuReferencePassTimeRecord ), options )
        , uses( ResolvedUsesPath( root ), Identity( source, "resolved-uses" ),
            sizeof( QueryGpuReferenceResolvedUseRecord ), options )
    {}
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

void QueryGpuReferenceStoreReader::ValidateAll(
    const std::function<uint64_t( uint64_t, int64_t )>& resolveCatalogResource ) const
{
    m_impl->lookup.ValidateAll();
    m_impl->catalogLookup.ValidateAll();
    m_impl->passTimes.ValidateAll();
    m_impl->uses.ValidateAll();
    uint64_t previousPass = 0;
    for( uint64_t index = 0; index < m_impl->passTimes.Count(); index++ )
    {
        QueryGpuReferencePassTimeRecord row {};
        m_impl->passTimes.Read( index, &row, 1 );
        if( row.passId == 0 || row.end < row.start || ( index != 0 && row.passId <= previousPass ) )
            throw std::runtime_error( "GPU reference pass time order mismatch" );
        previousPass = row.passId;
    }
    analysis::AnalysisKeyMetadataRecord previous {};
    for( uint64_t index = 0; index < m_impl->lookup.Count(); index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        m_impl->lookup.Read( index, &row, 1 );
        if( row.keyHigh == 0 || row.metadata >= m_impl->uses.Count() ||
            ( index != 0 && ( row.keyHigh < previous.keyHigh ||
                ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                    ( row.keyLow == previous.keyLow && row.metadata < previous.metadata ) ) ) ) ) )
            throw std::runtime_error( "GPU reference resource lookup order mismatch" );
        QueryGpuReferenceResolvedUseRecord use {};
        m_impl->uses.Read( row.metadata, &use, 1 );
        if( use.resourceId != row.keyHigh ) throw std::runtime_error( "GPU reference resource lookup identity mismatch" );
        previous = row;
    }
    previous = {};
    for( uint64_t index = 0; index < m_impl->catalogLookup.Count(); index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        m_impl->catalogLookup.Read( index, &row, 1 );
        if( row.keyHigh == 0 || row.metadata >= m_impl->uses.Count() ||
            ( index != 0 && ( row.keyHigh < previous.keyHigh ||
                ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                    ( row.keyLow == previous.keyLow && row.metadata < previous.metadata ) ) ) ) ) )
            throw std::runtime_error( "GPU reference Catalog resource lookup order mismatch" );
        if( resolveCatalogResource )
        {
            QueryGpuReferenceResolvedUseRecord use {};
            m_impl->uses.Read( row.metadata, &use, 1 );
            if( resolveCatalogResource( use.resourceId, use.passEnd ) != row.keyHigh )
                throw std::runtime_error( "GPU reference Catalog resource lookup identity mismatch" );
        }
        previous = row;
    }
}

}
