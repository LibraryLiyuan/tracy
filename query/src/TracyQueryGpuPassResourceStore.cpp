#include "TracyQueryGpuPassResourceStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyQueryCatalogStore.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace tracy::query
{
namespace
{

struct AllocationAggregate
{
    uint64_t passId = 0;
    uint64_t allocationCount = 0;
    uint64_t physicalBytes = 0;
};

static_assert( sizeof( AllocationAggregate ) == 24 );

std::string Identity( std::string_view source, std::string_view kind )
{
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( kind.data(), kind.size() );
    constexpr std::string_view version = "query-gpu-pass-resource-v1";
    hash.Update( version.data(), version.size() );
    return hash.FinalHex();
}

std::filesystem::path SummaryPath( const std::filesystem::path& root ) { return root / "pass-resource-summary"; }
std::filesystem::path RangeLookupPath( const std::filesystem::path& root ) { return root / "pass-resource-ranges"; }
std::filesystem::path UseLookupPath( const std::filesystem::path& root ) { return root / "pass-resource-uses"; }
std::filesystem::path EvidenceLookupPath( const std::filesystem::path& root ) { return root / "pass-resource-evidence-ids"; }

void CheckedAdd( uint64_t& target, uint64_t value, const char* label )
{
    if( value > std::numeric_limits<uint64_t>::max() - target )
        throw std::runtime_error( std::string( label ) + " overflow" );
    target += value;
}

}

void QueryGpuPassResourceStore::Build( const std::filesystem::path& root,
    std::string sourceFingerprint, const QueryGpuPassResourceInput& input,
    analysis::PagedRecordOptions options )
{
    if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "GPU pass resource identity must be SHA-256" );
    const auto summaryOutput = SummaryPath( root );
    const auto rangeLookupOutput = RangeLookupPath( root );
    const auto useLookupOutput = UseLookupPath( root );
    const auto evidenceLookupOutput = EvidenceLookupPath( root );
    const auto scratch = root / "pass-resource-scratch";
    if( std::filesystem::exists( analysis::AnalysisIoPath( summaryOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( rangeLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( useLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( evidenceLookupOutput ) ) ||
        std::filesystem::exists( analysis::AnalysisIoPath( scratch ) ) )
        throw std::runtime_error( "GPU pass resource index already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratch ) );
    const auto mappingSource = scratch / "mapping-source.work";
    const auto mappingSorted = scratch / "mapping-sorted.work";
    const auto factSource = scratch / "fact-source.work";
    const auto factSorted = scratch / "fact-sorted.work";
    const auto expandedSource = scratch / "expanded-source.work";
    const auto expandedSorted = scratch / "expanded-sorted.work";
    const auto rangeSource = scratch / "range-source.work";
    const auto rangeSorted = scratch / "range-sorted.work";
    const auto useSource = scratch / "use-source.work";
    const auto useSorted = scratch / "use-sorted.work";
    const auto baseSummarySource = scratch / "base-summary.work";
    const auto allocationSource = scratch / "allocation-source.work";
    const auto allocationSorted = scratch / "allocation-sorted.work";
    const auto allocationAggregateSource = scratch / "allocation-aggregate.work";
    const auto evidenceSource = scratch / "evidence-source.work";
    const auto evidenceSorted = scratch / "evidence-sorted.work";
    const auto cleanup = [&]( bool outputs ) {
        std::string failure;
        const auto remove = [&]( const std::filesystem::path& path ) {
            std::error_code error;
            analysis::AnalysisDiskRemove( path, options.disk, error, true );
            if( error && failure.empty() ) failure = error.message();
        };
        remove( scratch );
        if( outputs ) { remove( summaryOutput ); remove( rangeLookupOutput ); remove( useLookupOutput ); remove( evidenceLookupOutput ); }
        return failure;
    };
    try
    {
        std::ofstream mappings( analysis::AnalysisIoPath( mappingSource ), std::ios::binary | std::ios::trunc );
        std::ofstream facts( analysis::AnalysisIoPath( factSource ), std::ios::binary | std::ios::trunc );
        if( !mappings || !facts ) throw std::runtime_error( "GPU pass resource sources cannot be opened" );
        uint64_t mappingCount = 0;
        uint64_t factCount = 0;
        const auto appendMapping = [&]( uint64_t evidencePassId, uint64_t queryPassId ) {
            if( evidencePassId == 0 || queryPassId == 0 ) return;
            const analysis::AnalysisUInt64Pair row { evidencePassId, queryPassId };
            analysis::AnalysisDiskGrow( options.disk, sizeof( row ) );
            mappings.write( reinterpret_cast<const char*>( &row ), sizeof( row ) );
            if( !mappings ) throw std::runtime_error( "GPU pass mapping write failed" );
            mappingCount++;
        };
        for( uint64_t ordinal = 0; ordinal < input.MappingCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU pass resource index cancelled" );
            const auto row = input.Mapping( ordinal );
            appendMapping( row.evidencePassId, row.queryPassId );
        }
        for( uint64_t ordinal = 0; ordinal < input.RangeCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU pass resource index cancelled" );
            const auto range = input.Range( ordinal );
            if( range.passInstanceId == 0 || range.resourceId == 0 ) continue;
            appendMapping( range.passInstanceId, range.passInstanceId );
            analysis::AnalysisKeyMetadataRecord fact {};
            fact.keyHigh = range.passInstanceId;
            fact.keyLow = range.resourceId;
            fact.metadata = ordinal;
            fact.kind = 1;
            analysis::AnalysisDiskGrow( options.disk, sizeof( fact ) );
            facts.write( reinterpret_cast<const char*>( &fact ), sizeof( fact ) );
            if( !facts ) throw std::runtime_error( "GPU pass range fact write failed" );
            factCount++;
        }
        for( uint64_t ordinal = 0; ordinal < input.UseCount(); ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU pass resource index cancelled" );
            const auto use = input.Use( ordinal );
            const auto resourceId = input.ResolveCatalogResource( use.resourceId, use.passEnd );
            if( use.passId == 0 || resourceId == 0 ) continue;
            analysis::AnalysisKeyMetadataRecord fact {};
            fact.keyHigh = use.passId;
            fact.keyLow = resourceId;
            fact.metadata = ordinal;
            fact.kind = 2;
            analysis::AnalysisDiskGrow( options.disk, sizeof( fact ) );
            facts.write( reinterpret_cast<const char*>( &fact ), sizeof( fact ) );
            if( !facts ) throw std::runtime_error( "GPU pass use fact write failed" );
            factCount++;
        }
        mappings.flush(); facts.flush();
        if( !mappings || !facts ) throw std::runtime_error( "GPU pass resource source flush failed" );
        mappings.close(); facts.close();
        std::string sortError;
        if( !analysis::SortAnalysisUInt64Pairs( mappingSource, mappingSorted, scratch,
                "gpu-pass-resource-mapping", mappingCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU pass mapping sort failed: " + sortError );
        if( !analysis::SortAnalysisKeyMetadataRecords( factSource, factSorted, scratch,
                "gpu-pass-resource-facts", factCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU pass resource fact sort failed: " + sortError );

        uint64_t evidenceLookupCount = 0;
        {
            std::ifstream mappingInput( analysis::AnalysisIoPath( mappingSorted ), std::ios::binary );
            std::ofstream evidenceOutput( analysis::AnalysisIoPath( evidenceSource ),
                std::ios::binary | std::ios::trunc );
            if( !mappingInput || !evidenceOutput )
                throw std::runtime_error( "GPU pass evidence mapping files cannot be opened" );
            analysis::AnalysisUInt64Pair previous {};
            bool havePrevious = false;
            analysis::AnalysisUInt64Pair row {};
            while( mappingInput.read( reinterpret_cast<char*>( &row ), sizeof( row ) ) )
            {
                if( havePrevious && row.key == previous.key && row.value == previous.value ) continue;
                analysis::AnalysisKeyMetadataRecord inverted {};
                inverted.keyHigh = row.value;
                inverted.keyLow = row.key;
                analysis::AnalysisDiskGrow( options.disk, sizeof( inverted ) );
                evidenceOutput.write( reinterpret_cast<const char*>( &inverted ), sizeof( inverted ) );
                if( !evidenceOutput ) throw std::runtime_error( "GPU pass evidence mapping write failed" );
                previous = row;
                havePrevious = true;
                evidenceLookupCount++;
            }
            if( !mappingInput.eof() ) throw std::runtime_error( "GPU pass evidence mapping read failed" );
            evidenceOutput.flush();
            if( !evidenceOutput ) throw std::runtime_error( "GPU pass evidence mapping flush failed" );
        }
        if( !analysis::SortAnalysisKeyMetadataRecords( evidenceSource, evidenceSorted, scratch,
                "gpu-pass-resource-evidence", evidenceLookupCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU pass evidence mapping sort failed: " + sortError );

        std::ifstream sortedFacts( analysis::AnalysisIoPath( factSorted ), std::ios::binary );
        std::ifstream replayFacts( analysis::AnalysisIoPath( factSorted ), std::ios::binary );
        std::ifstream sortedMappings( analysis::AnalysisIoPath( mappingSorted ), std::ios::binary );
        std::ofstream expanded( analysis::AnalysisIoPath( expandedSource ), std::ios::binary | std::ios::trunc );
        std::ofstream rangeLookups( analysis::AnalysisIoPath( rangeSource ), std::ios::binary | std::ios::trunc );
        std::ofstream useLookups( analysis::AnalysisIoPath( useSource ), std::ios::binary | std::ios::trunc );
        if( !sortedFacts || !replayFacts || !sortedMappings || !expanded || !rangeLookups || !useLookups )
            throw std::runtime_error( "GPU pass resource expansion files cannot be opened" );
        analysis::AnalysisKeyMetadataRecord fact {};
        bool haveFact = bool( sortedFacts.read( reinterpret_cast<char*>( &fact ), sizeof( fact ) ) );
        analysis::AnalysisUInt64Pair mapping {};
        bool haveMapping = bool( sortedMappings.read( reinterpret_cast<char*>( &mapping ), sizeof( mapping ) ) );
        uint64_t expandedCount = 0, rangeLookupCount = 0, useLookupCount = 0;
        while( haveFact )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU pass resource index cancelled" );
            const auto evidencePassId = fact.keyHigh;
            const auto groupStart = sortedFacts.tellg() - std::streamoff( sizeof( fact ) );
            uint64_t groupCount = 0;
            do
            {
                groupCount++;
                haveFact = bool( sortedFacts.read( reinterpret_cast<char*>( &fact ), sizeof( fact ) ) );
            }
            while( haveFact && fact.keyHigh == evidencePassId );
            if( !haveFact && !sortedFacts.eof() ) throw std::runtime_error( "GPU pass fact group read failed" );
            while( haveMapping && mapping.key < evidencePassId )
                haveMapping = bool( sortedMappings.read( reinterpret_cast<char*>( &mapping ), sizeof( mapping ) ) );
            uint64_t previousQueryPass = 0;
            while( haveMapping && mapping.key == evidencePassId )
            {
                const auto queryPassId = mapping.value;
                if( queryPassId != 0 && queryPassId != previousQueryPass )
                {
                    replayFacts.clear();
                    replayFacts.seekg( groupStart );
                    if( !replayFacts ) throw std::runtime_error( "GPU pass fact replay seek failed" );
                    for( uint64_t index = 0; index < groupCount; index++ )
                    {
                        analysis::AnalysisKeyMetadataRecord source {};
                        replayFacts.read( reinterpret_cast<char*>( &source ), sizeof( source ) );
                        if( !replayFacts || source.keyHigh != evidencePassId )
                            throw std::runtime_error( "GPU pass fact replay changed" );
                        source.keyHigh = queryPassId;
                        analysis::AnalysisDiskGrow( options.disk, sizeof( source ) );
                        expanded.write( reinterpret_cast<const char*>( &source ), sizeof( source ) );
                        if( !expanded ) throw std::runtime_error( "GPU pass expanded fact write failed" );
                        analysis::AnalysisKeyMetadataRecord detail {};
                        detail.keyHigh = queryPassId;
                        detail.keyLow = source.metadata;
                        detail.metadata = source.metadata;
                        auto& output = source.kind == 1 ? rangeLookups : useLookups;
                        analysis::AnalysisDiskGrow( options.disk, sizeof( detail ) );
                        output.write( reinterpret_cast<const char*>( &detail ), sizeof( detail ) );
                        if( !output ) throw std::runtime_error( "GPU pass detail lookup write failed" );
                        source.kind == 1 ? rangeLookupCount++ : useLookupCount++;
                        expandedCount++;
                    }
                    previousQueryPass = queryPassId;
                }
                haveMapping = bool( sortedMappings.read( reinterpret_cast<char*>( &mapping ), sizeof( mapping ) ) );
            }
        }
        if( !sortedMappings.eof() && !haveMapping ) throw std::runtime_error( "GPU pass mapping read failed" );
        expanded.flush(); rangeLookups.flush(); useLookups.flush();
        if( !expanded || !rangeLookups || !useLookups ) throw std::runtime_error( "GPU pass expansion flush failed" );
        expanded.close(); rangeLookups.close(); useLookups.close();
        sortedFacts.close(); replayFacts.close(); sortedMappings.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( expandedSource, expandedSorted, scratch,
                "gpu-pass-resource-expanded", expandedCount, 512 * 1024, sortError,
                options.disk, options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( rangeSource, rangeSorted, scratch,
                "gpu-pass-resource-ranges", rangeLookupCount, 512 * 1024, sortError,
                options.disk, options.workspace ) ||
            !analysis::SortAnalysisKeyMetadataRecords( useSource, useSorted, scratch,
                "gpu-pass-resource-uses", useLookupCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU pass expanded index sort failed: " + sortError );

        std::ifstream expandedFacts( analysis::AnalysisIoPath( expandedSorted ), std::ios::binary );
        std::ofstream baseSummaries( analysis::AnalysisIoPath( baseSummarySource ), std::ios::binary | std::ios::trunc );
        std::ofstream allocations( analysis::AnalysisIoPath( allocationSource ), std::ios::binary | std::ios::trunc );
        if( !expandedFacts || !baseSummaries || !allocations )
            throw std::runtime_error( "GPU pass summary files cannot be opened" );
        QueryGpuPassResourceSummary summary {};
        uint64_t activeResource = 0;
        bool haveSummary = false, haveResource = false, resourceRanged = false;
        uint64_t allocationPairCount = 0, summaryCount = 0;
        const auto flushResource = [&] {
            if( !haveResource ) return;
            summary.resourceCount++;
            if( resourceRanged ) summary.rangedResourceCount++;
            const auto allocationId = input.ResourceAllocation( activeResource );
            if( allocationId != 0 )
            {
                analysis::AnalysisKeyMetadataRecord pair {};
                pair.keyHigh = summary.passId;
                pair.keyLow = allocationId;
                analysis::AnalysisDiskGrow( options.disk, sizeof( pair ) );
                allocations.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
                if( !allocations ) throw std::runtime_error( "GPU pass allocation pair write failed" );
                allocationPairCount++;
            }
            haveResource = false;
            resourceRanged = false;
        };
        const auto flushSummary = [&] {
            if( !haveSummary ) return;
            flushResource();
            analysis::AnalysisDiskGrow( options.disk, sizeof( summary ) );
            baseSummaries.write( reinterpret_cast<const char*>( &summary ), sizeof( summary ) );
            if( !baseSummaries ) throw std::runtime_error( "GPU pass base summary write failed" );
            summaryCount++;
            haveSummary = false;
        };
        analysis::AnalysisKeyMetadataRecord expandedFact {};
        while( expandedFacts.read( reinterpret_cast<char*>( &expandedFact ), sizeof( expandedFact ) ) )
        {
            if( !haveSummary || summary.passId != expandedFact.keyHigh )
            {
                flushSummary();
                summary = {};
                summary.passId = expandedFact.keyHigh;
                haveSummary = true;
            }
            if( !haveResource || activeResource != expandedFact.keyLow )
            {
                flushResource();
                activeResource = expandedFact.keyLow;
                haveResource = true;
            }
            if( expandedFact.kind == 1 )
            {
                const auto range = input.Range( expandedFact.metadata );
                CheckedAdd( summary.directRangeBytes, range.lengthBytes, "GPU pass direct range bytes" );
                summary.rangeCount++;
                resourceRanged = true;
            }
            else if( expandedFact.kind == 2 ) summary.useCount++;
            else throw std::runtime_error( "GPU pass fact kind is invalid" );
        }
        if( !expandedFacts.eof() ) throw std::runtime_error( "GPU pass expanded fact read failed" );
        flushSummary();
        baseSummaries.flush(); allocations.flush();
        if( !baseSummaries || !allocations ) throw std::runtime_error( "GPU pass summary flush failed" );
        baseSummaries.close(); allocations.close(); expandedFacts.close();
        if( !analysis::SortAnalysisKeyMetadataRecords( allocationSource, allocationSorted, scratch,
                "gpu-pass-resource-allocations", allocationPairCount, 512 * 1024, sortError,
                options.disk, options.workspace ) )
            throw std::runtime_error( "GPU pass allocation sort failed: " + sortError );

        std::ifstream sortedAllocations( analysis::AnalysisIoPath( allocationSorted ), std::ios::binary );
        std::ofstream allocationAggregates( analysis::AnalysisIoPath( allocationAggregateSource ),
            std::ios::binary | std::ios::trunc );
        if( !sortedAllocations || !allocationAggregates )
            throw std::runtime_error( "GPU pass allocation aggregate files cannot be opened" );
        AllocationAggregate aggregate {};
        uint64_t previousAllocation = 0;
        bool haveAggregate = false;
        const auto flushAggregate = [&] {
            if( !haveAggregate ) return;
            analysis::AnalysisDiskGrow( options.disk, sizeof( aggregate ) );
            allocationAggregates.write( reinterpret_cast<const char*>( &aggregate ), sizeof( aggregate ) );
            if( !allocationAggregates ) throw std::runtime_error( "GPU pass allocation aggregate write failed" );
            haveAggregate = false;
        };
        analysis::AnalysisKeyMetadataRecord allocationPair {};
        while( sortedAllocations.read( reinterpret_cast<char*>( &allocationPair ), sizeof( allocationPair ) ) )
        {
            if( !haveAggregate || aggregate.passId != allocationPair.keyHigh )
            {
                flushAggregate();
                aggregate = {};
                aggregate.passId = allocationPair.keyHigh;
                previousAllocation = 0;
                haveAggregate = true;
            }
            if( allocationPair.keyLow == previousAllocation ) continue;
            previousAllocation = allocationPair.keyLow;
            aggregate.allocationCount++;
            CheckedAdd( aggregate.physicalBytes, input.AllocationBytesAtEnd( allocationPair.keyLow ),
                "GPU pass referenced physical bytes" );
        }
        if( !sortedAllocations.eof() ) throw std::runtime_error( "GPU pass allocation pair read failed" );
        flushAggregate();
        allocationAggregates.flush();
        if( !allocationAggregates ) throw std::runtime_error( "GPU pass allocation aggregate flush failed" );
        allocationAggregates.close(); sortedAllocations.close();

        analysis::PagedRecordWriter summaries( summaryOutput,
            Identity( sourceFingerprint, "pass-resource-summary" ), sizeof( QueryGpuPassResourceSummary ), options );
        std::ifstream baseInput( analysis::AnalysisIoPath( baseSummarySource ), std::ios::binary );
        std::ifstream aggregateInput( analysis::AnalysisIoPath( allocationAggregateSource ), std::ios::binary );
        if( !baseInput || !aggregateInput ) throw std::runtime_error( "GPU pass summary join files cannot be opened" );
        AllocationAggregate nextAggregate {};
        bool haveNextAggregate = bool( aggregateInput.read( reinterpret_cast<char*>( &nextAggregate ), sizeof( nextAggregate ) ) );
        for( uint64_t index = 0; index < summaryCount; index++ )
        {
            QueryGpuPassResourceSummary row {};
            baseInput.read( reinterpret_cast<char*>( &row ), sizeof( row ) );
            if( !baseInput ) throw std::runtime_error( "GPU pass base summary read failed" );
            while( haveNextAggregate && nextAggregate.passId < row.passId )
                haveNextAggregate = bool( aggregateInput.read( reinterpret_cast<char*>( &nextAggregate ), sizeof( nextAggregate ) ) );
            if( haveNextAggregate && nextAggregate.passId == row.passId )
            {
                row.allocationCount = nextAggregate.allocationCount;
                row.referencedPhysicalBytes = nextAggregate.physicalBytes;
                haveNextAggregate = bool( aggregateInput.read( reinterpret_cast<char*>( &nextAggregate ), sizeof( nextAggregate ) ) );
            }
            summaries.Append( &row, 1 );
        }
        summaries.Commit();
        baseInput.close(); aggregateInput.close();

        const auto publishLookup = [&]( const std::filesystem::path& sorted,
            const std::filesystem::path& output, std::string_view kind, uint64_t expectedCount ) {
            analysis::PagedRecordWriter writer( output, Identity( sourceFingerprint, kind ),
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            std::ifstream inputFile( analysis::AnalysisIoPath( sorted ), std::ios::binary );
            if( !inputFile ) throw std::runtime_error( "GPU pass detail sorted file cannot be opened" );
            std::array<analysis::AnalysisKeyMetadataRecord, 4096> buffer;
            uint64_t written = 0;
            while( inputFile )
            {
                inputFile.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( sizeof( buffer ) ) );
                const auto bytes = size_t( inputFile.gcount() );
                if( bytes % sizeof( buffer[0] ) != 0 ) throw std::runtime_error( "GPU pass detail lookup is truncated" );
                const auto count = bytes / sizeof( buffer[0] );
                if( count != 0 ) writer.Append( buffer.data(), count );
                written += count;
            }
            if( !inputFile.eof() || written != expectedCount )
                throw std::runtime_error( "GPU pass detail lookup count mismatch" );
            writer.Commit();
        };
        publishLookup( rangeSorted, rangeLookupOutput, "pass-resource-ranges", rangeLookupCount );
        publishLookup( useSorted, useLookupOutput, "pass-resource-uses", useLookupCount );
        publishLookup( evidenceSorted, evidenceLookupOutput, "pass-resource-evidence-ids", evidenceLookupCount );
        const auto cleanupError = cleanup( false );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU pass resource scratch cleanup failed: " + cleanupError );
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        const auto cleanupError = cleanup( true );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU pass resource cleanup failed: " + cleanupError );
        std::rethrow_exception( failure );
    }
}

struct QueryGpuPassResourceStoreReader::Impl
{
    Impl( std::filesystem::path root, std::filesystem::path catalogRoot,
        std::filesystem::path gpuReferenceRoot, std::string source,
        analysis::PagedRecordOptions options )
        : summaries( SummaryPath( root ), Identity( source, "pass-resource-summary" ),
            sizeof( QueryGpuPassResourceSummary ), options )
        , ranges( RangeLookupPath( root ), Identity( source, "pass-resource-ranges" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), options )
        , uses( UseLookupPath( root ), Identity( source, "pass-resource-uses" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), options )
        , evidence( EvidenceLookupPath( root ), Identity( source, "pass-resource-evidence-ids" ),
            sizeof( analysis::AnalysisKeyMetadataRecord ), options )
        , catalog( std::move( catalogRoot ), source, options )
        , gpuReferences( std::move( gpuReferenceRoot ), std::move( source ), options )
    {}
    analysis::PagedRecordReader summaries;
    analysis::PagedRecordReader ranges;
    analysis::PagedRecordReader uses;
    analysis::PagedRecordReader evidence;
    QueryCatalogStoreReader catalog;
    QueryGpuReferenceStoreReader gpuReferences;
};

QueryGpuPassResourceStoreReader::QueryGpuPassResourceStoreReader( std::filesystem::path root,
    std::filesystem::path catalogRoot, std::filesystem::path gpuReferenceRoot,
    std::string sourceFingerprint, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( catalogRoot ),
        std::move( gpuReferenceRoot ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGpuPassResourceStoreReader::~QueryGpuPassResourceStoreReader() = default;

uint64_t QueryGpuPassResourceStoreReader::SummaryCount() const
{
    return m_impl->summaries.Count();
}

QueryGpuPassResourceSummary QueryGpuPassResourceStoreReader::SummaryAt( uint64_t ordinal ) const
{
    if( ordinal >= m_impl->summaries.Count() )
        throw std::out_of_range( "GPU pass summary ordinal" );
    QueryGpuPassResourceSummary result {};
    m_impl->summaries.Read( ordinal, &result, 1 );
    return result;
}

std::optional<QueryGpuPassResourceSummary> QueryGpuPassResourceStoreReader::Summary( uint64_t passId ) const
{
    if( passId == 0 ) return std::nullopt;
    uint64_t first = 0, length = m_impl->summaries.Count();
    QueryGpuPassResourceSummary row {};
    while( length != 0 )
    {
        const auto half = length / 2;
        const auto middle = first + half;
        m_impl->summaries.Read( middle, &row, 1 );
        if( row.passId < passId ) { first = middle + 1; length -= half + 1; }
        else length = half;
    }
    if( first >= m_impl->summaries.Count() ) return std::nullopt;
    m_impl->summaries.Read( first, &row, 1 );
    return row.passId == passId ? std::optional( row ) : std::nullopt;
}

QueryGpuPassRangePage QueryGpuPassResourceStoreReader::Ranges(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryGpuPassRangePage result;
    if( passId == 0 || limit == 0 ) return result;
    auto& lookup = m_impl->ranges;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < passId || ( upper && row.keyHigh == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false ), rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != passId ) throw std::runtime_error( "GPU pass range lookup identity mismatch" );
        const auto value = m_impl->catalog.RangeAt( row.metadata );
        result.values.push_back( { value.record, value.generation } );
    }
    return result;
}

QueryGpuPassUsePage QueryGpuPassResourceStoreReader::Uses(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryGpuPassUsePage result;
    if( passId == 0 || limit == 0 ) return result;
    auto& lookup = m_impl->uses;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < passId || ( upper && row.keyHigh == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false ), rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != passId ) throw std::runtime_error( "GPU pass use lookup identity mismatch" );
        result.values.emplace_back( m_impl->gpuReferences.UseAt( row.metadata ) );
    }
    return result;
}

QueryGpuPassIdPage QueryGpuPassResourceStoreReader::EvidencePassIds(
    uint64_t passId, uint64_t offset, size_t limit ) const
{
    QueryGpuPassIdPage result;
    if( passId == 0 || limit == 0 ) return result;
    auto& lookup = m_impl->evidence;
    const auto lowerBound = [&]( bool upper ) {
        uint64_t begin = 0, count = lookup.Count();
        analysis::AnalysisKeyMetadataRecord row {};
        while( count != 0 )
        {
            const auto half = count / 2;
            const auto middle = begin + half;
            lookup.Read( middle, &row, 1 );
            if( row.keyHigh < passId || ( upper && row.keyHigh == passId ) )
            { begin = middle + 1; count -= half + 1; }
            else count = half;
        }
        return begin;
    };
    const auto rangeBegin = lowerBound( false ), rangeEnd = lowerBound( true );
    result.total = rangeEnd - rangeBegin;
    const auto begin = rangeBegin + std::min<uint64_t>( offset, result.total );
    const auto end = begin + std::min<uint64_t>( limit, rangeEnd - begin );
    result.values.reserve( size_t( end - begin ) );
    for( auto index = begin; index < end; index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        lookup.Read( index, &row, 1 );
        if( row.keyHigh != passId || row.keyLow == 0 )
            throw std::runtime_error( "GPU pass evidence lookup identity mismatch" );
        result.values.emplace_back( row.keyLow );
    }
    return result;
}

void QueryGpuPassResourceStoreReader::ValidateAll() const
{
    m_impl->summaries.ValidateAll();
    m_impl->ranges.ValidateAll();
    m_impl->uses.ValidateAll();
    m_impl->evidence.ValidateAll();
    uint64_t previousPass = 0;
    for( uint64_t index = 0; index < m_impl->summaries.Count(); index++ )
    {
        QueryGpuPassResourceSummary row {};
        m_impl->summaries.Read( index, &row, 1 );
        if( row.passId == 0 || ( index != 0 && row.passId <= previousPass ) ||
            row.rangedResourceCount > row.resourceCount )
            throw std::runtime_error( "GPU pass resource summary order mismatch" );
        if( Ranges( row.passId, 0, 1 ).total != row.rangeCount ||
            Uses( row.passId, 0, 1 ).total != row.useCount )
            throw std::runtime_error( "GPU pass resource summary detail count mismatch" );
        previousPass = row.passId;
    }
    const auto validateLookup = [&]( analysis::PagedRecordReader& lookup, uint64_t rawCount,
        std::string_view label ) {
        analysis::AnalysisKeyMetadataRecord previous {};
        for( uint64_t index = 0; index < lookup.Count(); index++ )
        {
            analysis::AnalysisKeyMetadataRecord row {};
            lookup.Read( index, &row, 1 );
            if( row.keyHigh == 0 || row.metadata >= rawCount ||
                ( index != 0 && ( row.keyHigh < previous.keyHigh ||
                    ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                        ( row.keyLow == previous.keyLow && row.metadata < previous.metadata ) ) ) ) ) )
                throw std::runtime_error( std::string( label ) + " lookup order mismatch" );
            previous = row;
        }
    };
    validateLookup( m_impl->ranges, m_impl->catalog.RangeCount(), "GPU pass range" );
    validateLookup( m_impl->uses, m_impl->gpuReferences.UseCount(), "GPU pass use" );
    analysis::AnalysisKeyMetadataRecord previousEvidence {};
    for( uint64_t index = 0; index < m_impl->evidence.Count(); index++ )
    {
        analysis::AnalysisKeyMetadataRecord row {};
        m_impl->evidence.Read( index, &row, 1 );
        if( row.keyHigh == 0 || row.keyLow == 0 ||
            ( index != 0 && ( row.keyHigh < previousEvidence.keyHigh ||
                ( row.keyHigh == previousEvidence.keyHigh && row.keyLow <= previousEvidence.keyLow ) ) ) )
            throw std::runtime_error( "GPU pass evidence lookup order mismatch" );
        previousEvidence = row;
    }
}

}
