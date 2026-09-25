#include "TracyQueryGpuPassResourceStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyQueryCatalogStore.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <list>
#include <stdexcept>
#include <unordered_map>

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

class SequentialLookup
{
public:
    explicit SequentialLookup( analysis::PagedRecordReader& reader )
        : m_reader( reader ), m_total( reader.Count() ) {}

    bool Next( analysis::AnalysisKeyMetadataRecord& row )
    {
        if( m_position == m_available )
        {
            if( m_nextOrdinal == m_total ) return false;
            m_available = size_t( std::min<uint64_t>( m_buffer.size(), m_total - m_nextOrdinal ) );
            m_reader.Read( m_nextOrdinal, m_buffer.data(), m_available );
            m_nextOrdinal += m_available;
            m_position = 0;
        }
        row = m_buffer[m_position++];
        return true;
    }

private:
    analysis::PagedRecordReader& m_reader;
    uint64_t m_total = 0, m_nextOrdinal = 0;
    std::array<analysis::AnalysisKeyMetadataRecord, 1024> m_buffer;
    size_t m_position = 0, m_available = 0;
};

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

class BuildLookupCache
{
public:
    explicit BuildLookupCache( std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace )
        : m_memory( std::move( workspace ) ) {}

    template<typename Loader>
    uint64_t Resolve( uint64_t key, Loader&& load )
    {
        const auto found = m_values.find( key );
        if( found != m_values.end() )
        {
            m_recent.splice( m_recent.end(), m_recent, found->second.recent );
            return found->second.value;
        }
        const auto value = load( key );
        if( m_values.size() == MaximumEntries )
        {
            m_values.erase( m_recent.front() );
            m_recent.pop_front();
            m_memory.Resize( m_memory.Bytes() - AccountedBytesPerEntry );
        }
        m_memory.Add( AccountedBytesPerEntry );
        try
        {
            m_recent.emplace_back( key );
            try { m_values.emplace( key, Entry { value, std::prev( m_recent.end() ) } ); }
            catch( ... ) { m_recent.pop_back(); throw; }
        }
        catch( ... )
        {
            m_memory.Resize( m_memory.Bytes() - AccountedBytesPerEntry );
            throw;
        }
        return value;
    }

    void Clear()
    {
        decltype( m_values ) {}.swap( m_values );
        m_recent.clear();
        m_memory.Resize( 0 );
    }

private:
    static constexpr size_t MaximumEntries = 1 << 20;
    // Includes map nodes, buckets, list nodes and transient rehash overhead.
    static constexpr uint64_t AccountedBytesPerEntry = 128;
    struct Entry { uint64_t value; std::list<uint64_t>::iterator recent; };
    analysis::AnalysisWorkspaceReservation m_memory;
    std::list<uint64_t> m_recent;
    std::unordered_map<uint64_t, Entry> m_values;
};

}

void QueryGpuPassResourceStore::Build( const std::filesystem::path& root,
    std::string sourceFingerprint, const QueryGpuPassResourceInput& input,
    analysis::PagedRecordOptions options, QueryGpuPassResourceBuildStatistics* statistics )
{
    if( statistics ) *statistics = {};
    auto phaseStarted = std::chrono::steady_clock::now();
    const auto finishPhase = [&]( uint64_t QueryGpuPassResourceBuildStatistics::*field ) {
        const auto now = std::chrono::steady_clock::now();
        if( statistics ) statistics->*field = uint64_t( std::chrono::duration_cast<
            std::chrono::milliseconds>( now - phaseStarted ).count() );
        phaseStarted = now;
    };
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
    const auto factSorted = scratch / "fact-sorted.work";
    const auto expandedSource = scratch / "expanded-source.work";
    const auto expandedSorted = scratch / "expanded-sorted.work";
    const auto rangeSource = scratch / "range-source.work";
    const auto rangeSorted = scratch / "range-sorted.work";
    const auto useSorted = scratch / "use-sorted.work";
    const auto baseSummarySource = scratch / "base-summary.work";
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
    const auto removeConsumed = [&]( const std::filesystem::path& path ) {
        std::error_code error;
        analysis::AnalysisDiskRemove( path, options.disk, error );
        if( error ) throw std::runtime_error( "GPU pass resource scratch cleanup failed: " +
            path.filename().string() + ": " + error.message() );
    };
    try
    {
        BuildLookupCache resourceAllocations( options.workspace );
        BuildLookupCache allocationEndBytes( options.workspace );
        const auto rangeCount = input.RangeCount();
        if( rangeCount > ( SIZE_MAX - 4096 ) / sizeof( uint64_t ) )
            throw std::runtime_error( "GPU pass range length fast path exceeds address space" );
        analysis::AnalysisWorkspaceReservation rangeMemory( options.workspace,
            rangeCount * sizeof( uint64_t ) + 4096 );
        std::vector<uint64_t> rangeLengths;
        rangeLengths.reserve( size_t( rangeCount ) );
        std::ofstream mappings( analysis::AnalysisIoPath( mappingSource ), std::ios::binary | std::ios::trunc );
        analysis::AnalysisKeyMetadataRunCollector factRuns( factSorted, scratch,
            "gpu-pass-resource-facts", 512 * 1024, options.disk,
            options.workspace, options.cancelled,
            statistics ? &statistics->factSort : nullptr );
        if( !mappings ) throw std::runtime_error( "GPU pass resource mapping source cannot be opened" );
        uint64_t mappingCount = 0;
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
        for( uint64_t ordinal = 0; ordinal < rangeCount; ordinal++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "GPU pass resource index cancelled" );
            const auto range = input.Range( ordinal );
            rangeLengths.emplace_back( range.lengthBytes );
            if( range.passInstanceId == 0 || range.resourceId == 0 ) continue;
            appendMapping( range.passInstanceId, range.passInstanceId );
            analysis::AnalysisKeyMetadataRecord fact {};
            fact.keyHigh = range.passInstanceId;
            fact.keyLow = range.resourceId;
            fact.metadata = ordinal;
            fact.kind = 1;
            factRuns.Emit( fact );
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
            factRuns.Emit( fact );
        }
        mappings.flush();
        if( !mappings ) throw std::runtime_error( "GPU pass mapping source flush failed" );
        mappings.close();
        finishPhase( &QueryGpuPassResourceBuildStatistics::inputMs );
        std::string sortError;
        if( !factRuns.Finish( sortError ) )
            throw std::runtime_error( "GPU pass resource fact sort failed: " + sortError );
        finishPhase( &QueryGpuPassResourceBuildStatistics::factSortMs );
        if( !analysis::SortAnalysisUInt64Pairs( mappingSource, mappingSorted, scratch,
                "gpu-pass-resource-mapping", mappingCount, 512 * 1024, sortError,
                options.disk, options.workspace, options.cancelled ) )
            throw std::runtime_error( "GPU pass mapping sort failed: " + sortError );
        removeConsumed( mappingSource );
        finishPhase( &QueryGpuPassResourceBuildStatistics::mappingSortMs );

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
            uint64_t mappedRows = 0;
            while( mappingInput.read( reinterpret_cast<char*>( &row ), sizeof( row ) ) )
            {
                if( ( mappedRows++ & 4095 ) == 0 && options.cancelled && options.cancelled() )
                    throw std::runtime_error( "GPU pass resource index cancelled" );
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
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->evidenceSort : nullptr ) )
            throw std::runtime_error( "GPU pass evidence mapping sort failed: " + sortError );
        removeConsumed( evidenceSource );
        finishPhase( &QueryGpuPassResourceBuildStatistics::evidenceSortMs );

        std::ifstream sortedFacts( analysis::AnalysisIoPath( factSorted ), std::ios::binary );
        std::ifstream replayFacts( analysis::AnalysisIoPath( factSorted ), std::ios::binary );
        std::ifstream sortedMappings( analysis::AnalysisIoPath( mappingSorted ), std::ios::binary );
        std::ofstream expanded( analysis::AnalysisIoPath( expandedSource ), std::ios::binary | std::ios::trunc );
        std::ofstream rangeLookups( analysis::AnalysisIoPath( rangeSource ), std::ios::binary | std::ios::trunc );
        if( !sortedFacts || !replayFacts || !sortedMappings || !expanded || !rangeLookups )
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
                        if( ( index & 4095 ) == 0 && options.cancelled && options.cancelled() )
                            throw std::runtime_error( "GPU pass resource index cancelled" );
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
                        if( source.kind == 1 )
                        {
                            analysis::AnalysisDiskGrow( options.disk, sizeof( detail ) );
                            rangeLookups.write( reinterpret_cast<const char*>( &detail ), sizeof( detail ) );
                            if( !rangeLookups ) throw std::runtime_error( "GPU pass detail lookup write failed" );
                        }
                        source.kind == 1 ? rangeLookupCount++ : useLookupCount++;
                        expandedCount++;
                    }
                    previousQueryPass = queryPassId;
                }
                haveMapping = bool( sortedMappings.read( reinterpret_cast<char*>( &mapping ), sizeof( mapping ) ) );
            }
        }
        if( !sortedMappings.eof() && !haveMapping ) throw std::runtime_error( "GPU pass mapping read failed" );
        expanded.flush(); rangeLookups.flush();
        if( !expanded || !rangeLookups ) throw std::runtime_error( "GPU pass expansion flush failed" );
        expanded.close(); rangeLookups.close();
        sortedFacts.close(); replayFacts.close(); sortedMappings.close();
        removeConsumed( mappingSorted );
        removeConsumed( factSorted );
        finishPhase( &QueryGpuPassResourceBuildStatistics::expansionMs );
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
                if( options.cancelled && options.cancelled() )
                    throw std::runtime_error( "GPU pass resource index cancelled" );
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
        if( !analysis::SortAnalysisKeyMetadataRecords( expandedSource, expandedSorted, scratch,
                "gpu-pass-resource-expanded", expandedCount, 512 * 1024, sortError,
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->expandedSort : nullptr ) )
            throw std::runtime_error( "GPU pass expanded index sort failed: " + sortError );
        removeConsumed( expandedSource );
        finishPhase( &QueryGpuPassResourceBuildStatistics::expandedSortMs );
        if( !analysis::SortAnalysisKeyMetadataRecords( rangeSource, rangeSorted, scratch,
                "gpu-pass-resource-ranges", rangeLookupCount, 512 * 1024, sortError,
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->rangeSort : nullptr ) )
            throw std::runtime_error( "GPU pass range index sort failed: " + sortError );
        removeConsumed( rangeSource );
        finishPhase( &QueryGpuPassResourceBuildStatistics::rangeSortMs );
        if( !analysis::SortAnalysisKeyMetadataRecordsGenerated( useSorted, scratch,
                "gpu-pass-resource-uses", 512 * 1024,
                [&]( const auto& emit ) {
                    std::ifstream source( analysis::AnalysisIoPath( expandedSorted ), std::ios::binary );
                    if( !source ) throw std::runtime_error( "GPU pass sorted facts cannot generate use lookup" );
                    uint64_t generated = 0;
                    analysis::AnalysisKeyMetadataRecord fact {};
                    while( source.read( reinterpret_cast<char*>( &fact ), sizeof( fact ) ) )
                    {
                        if( fact.kind != 2 ) continue;
                        analysis::AnalysisKeyMetadataRecord detail {};
                        detail.keyHigh = fact.keyHigh;
                        detail.keyLow = fact.metadata;
                        detail.metadata = fact.metadata;
                        emit( detail );
                        generated++;
                    }
                    if( !source.eof() || source.gcount() != 0 || generated != useLookupCount )
                        throw std::runtime_error( "GPU pass generated use lookup count mismatch" );
                }, sortError, options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->useSort : nullptr ) )
            throw std::runtime_error( "GPU pass use index sort failed: " + sortError );
        publishLookup( useSorted, useLookupOutput, "pass-resource-uses", useLookupCount );
        removeConsumed( useSorted );
        finishPhase( &QueryGpuPassResourceBuildStatistics::useSortMs );

        std::ifstream expandedFacts( analysis::AnalysisIoPath( expandedSorted ), std::ios::binary );
        std::ofstream baseSummaries( analysis::AnalysisIoPath( baseSummarySource ), std::ios::binary | std::ios::trunc );
        analysis::AnalysisKeyMetadataRunCollector allocationRuns( allocationSorted, scratch,
            "gpu-pass-resource-allocations", 512 * 1024, options.disk,
            options.workspace, options.cancelled,
            statistics ? &statistics->allocationSort : nullptr );
        if( !expandedFacts || !baseSummaries )
            throw std::runtime_error( "GPU pass summary files cannot be opened" );
        QueryGpuPassResourceSummary summary {};
        uint64_t activeResource = 0;
        bool haveSummary = false, haveResource = false, resourceRanged = false;
        uint64_t summaryCount = 0;
        const auto flushResource = [&] {
            if( !haveResource ) return;
            summary.resourceCount++;
            if( resourceRanged ) summary.rangedResourceCount++;
            const auto allocationId = resourceAllocations.Resolve( activeResource,
                [&]( uint64_t resourceId ) { return input.ResourceAllocation( resourceId ); } );
            if( allocationId != 0 )
            {
                analysis::AnalysisKeyMetadataRecord pair {};
                pair.keyHigh = summary.passId;
                pair.keyLow = allocationId;
                allocationRuns.Emit( pair );
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
        uint64_t summarizedFacts = 0;
        while( expandedFacts.read( reinterpret_cast<char*>( &expandedFact ), sizeof( expandedFact ) ) )
        {
            if( ( summarizedFacts++ & 4095 ) == 0 && options.cancelled && options.cancelled() )
                throw std::runtime_error( "GPU pass resource index cancelled" );
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
                if( expandedFact.metadata >= rangeLengths.size() )
                    throw std::runtime_error( "GPU pass range ordinal mismatch" );
                CheckedAdd( summary.directRangeBytes, rangeLengths[size_t( expandedFact.metadata )],
                    "GPU pass direct range bytes" );
                summary.rangeCount++;
                resourceRanged = true;
            }
            else if( expandedFact.kind == 2 ) summary.useCount++;
            else throw std::runtime_error( "GPU pass fact kind is invalid" );
        }
        if( !expandedFacts.eof() ) throw std::runtime_error( "GPU pass expanded fact read failed" );
        flushSummary();
        baseSummaries.flush();
        if( !baseSummaries ) throw std::runtime_error( "GPU pass summary flush failed" );
        baseSummaries.close(); expandedFacts.close();
        removeConsumed( expandedSorted );
        resourceAllocations.Clear();
        std::vector<uint64_t> {}.swap( rangeLengths );
        rangeMemory.Resize( 0 );
        finishPhase( &QueryGpuPassResourceBuildStatistics::summaryMs );
        if( !allocationRuns.Finish( sortError ) )
            throw std::runtime_error( "GPU pass allocation sort failed: " + sortError );
        finishPhase( &QueryGpuPassResourceBuildStatistics::allocationSortMs );

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
        uint64_t aggregatedPairs = 0;
        while( sortedAllocations.read( reinterpret_cast<char*>( &allocationPair ), sizeof( allocationPair ) ) )
        {
            if( ( aggregatedPairs++ & 4095 ) == 0 && options.cancelled && options.cancelled() )
                throw std::runtime_error( "GPU pass resource index cancelled" );
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
            CheckedAdd( aggregate.physicalBytes, allocationEndBytes.Resolve( allocationPair.keyLow,
                [&]( uint64_t allocationId ) { return input.AllocationBytesAtEnd( allocationId ); } ),
                "GPU pass referenced physical bytes" );
        }
        if( !sortedAllocations.eof() ) throw std::runtime_error( "GPU pass allocation pair read failed" );
        flushAggregate();
        allocationAggregates.flush();
        if( !allocationAggregates ) throw std::runtime_error( "GPU pass allocation aggregate flush failed" );
        allocationAggregates.close(); sortedAllocations.close();
        removeConsumed( allocationSorted );
        allocationEndBytes.Clear();
        finishPhase( &QueryGpuPassResourceBuildStatistics::allocationAggregateMs );

        analysis::PagedRecordWriter summaries( summaryOutput,
            Identity( sourceFingerprint, "pass-resource-summary" ), sizeof( QueryGpuPassResourceSummary ), options );
        std::ifstream baseInput( analysis::AnalysisIoPath( baseSummarySource ), std::ios::binary );
        std::ifstream aggregateInput( analysis::AnalysisIoPath( allocationAggregateSource ), std::ios::binary );
        if( !baseInput || !aggregateInput ) throw std::runtime_error( "GPU pass summary join files cannot be opened" );
        AllocationAggregate nextAggregate {};
        bool haveNextAggregate = bool( aggregateInput.read( reinterpret_cast<char*>( &nextAggregate ), sizeof( nextAggregate ) ) );
        for( uint64_t index = 0; index < summaryCount; index++ )
        {
            if( ( index & 4095 ) == 0 && options.cancelled && options.cancelled() )
                throw std::runtime_error( "GPU pass resource index cancelled" );
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
        removeConsumed( baseSummarySource );
        removeConsumed( allocationAggregateSource );

        publishLookup( rangeSorted, rangeLookupOutput, "pass-resource-ranges", rangeLookupCount );
        removeConsumed( rangeSorted );
        publishLookup( evidenceSorted, evidenceLookupOutput, "pass-resource-evidence-ids", evidenceLookupCount );
        removeConsumed( evidenceSorted );
        const auto cleanupError = cleanup( false );
        if( !cleanupError.empty() ) throw std::runtime_error( "GPU pass resource scratch cleanup failed: " + cleanupError );
        finishPhase( &QueryGpuPassResourceBuildStatistics::publishMs );
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

QueryGpuPassResourceReadStats QueryGpuPassResourceStoreReader::ReadStatistics() const
{
    return { m_impl->ranges.Statistics().blockLoads, m_impl->uses.Statistics().blockLoads };
}

void QueryGpuPassResourceStoreReader::ValidateAll() const
{
    m_impl->summaries.ValidateAll();
    m_impl->ranges.ValidateAll();
    m_impl->uses.ValidateAll();
    m_impl->evidence.ValidateAll();
    SequentialLookup ranges( m_impl->ranges ), uses( m_impl->uses );
    analysis::AnalysisKeyMetadataRecord range {}, use {}, previousRange {}, previousUse {};
    bool haveRange = ranges.Next( range ), haveUse = uses.Next( use );
    uint64_t rangeOrdinal = 0, useOrdinal = 0;
    const auto rawRangeCount = m_impl->catalog.RangeCount();
    const auto rawUseCount = m_impl->gpuReferences.UseCount();
    const auto checkOrder = []( const analysis::AnalysisKeyMetadataRecord& current,
        const analysis::AnalysisKeyMetadataRecord& previous, uint64_t ordinal, uint64_t rawCount,
        const char* label ) {
        if( current.keyHigh == 0 || current.metadata >= rawCount ||
            ( ordinal != 0 && ( current.keyHigh < previous.keyHigh ||
                ( current.keyHigh == previous.keyHigh && ( current.keyLow < previous.keyLow ||
                    ( current.keyLow == previous.keyLow && current.metadata < previous.metadata ) ) ) ) ) )
            throw std::runtime_error( std::string( label ) + " lookup order mismatch" );
    };
    uint64_t previousPass = 0;
    for( uint64_t index = 0; index < m_impl->summaries.Count(); index++ )
    {
        QueryGpuPassResourceSummary row {};
        m_impl->summaries.Read( index, &row, 1 );
        if( row.passId == 0 || ( index != 0 && row.passId <= previousPass ) ||
            row.rangedResourceCount > row.resourceCount )
            throw std::runtime_error( "GPU pass resource summary order mismatch" );
        if( ( haveRange && range.keyHigh < row.passId ) ||
            ( haveUse && use.keyHigh < row.passId ) )
            throw std::runtime_error( "GPU pass resource lookup has no summary" );
        uint64_t rangeCount = 0, useCount = 0;
        while( haveRange && range.keyHigh == row.passId )
        {
            checkOrder( range, previousRange, rangeOrdinal++, rawRangeCount, "GPU pass range" );
            previousRange = range;
            rangeCount++;
            haveRange = ranges.Next( range );
        }
        while( haveUse && use.keyHigh == row.passId )
        {
            checkOrder( use, previousUse, useOrdinal++, rawUseCount, "GPU pass use" );
            previousUse = use;
            useCount++;
            haveUse = uses.Next( use );
        }
        if( rangeCount != row.rangeCount || useCount != row.useCount )
            throw std::runtime_error( "GPU pass resource summary detail count mismatch" );
        previousPass = row.passId;
    }
    if( haveRange || haveUse )
        throw std::runtime_error( "GPU pass resource lookup has no summary" );
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
