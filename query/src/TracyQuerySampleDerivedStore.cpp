#include "TracyQuerySampleDerivedStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleView.hpp"

#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace tracy::query
{
namespace
{

#pragma pack( push, 1 )
struct StoredSymbolStats
{
    uint64_t address = 0;
    uint64_t childSamples = 0;
    uint32_t inclusiveSamples = 0;
    uint32_t exclusiveSamples = 0;
    uint64_t reserved = 0;
};

struct StoredSummary
{
    uint64_t records = 0;
    uint64_t childSampleSymbols = 0;
    uint64_t childSamples = 0;
};
#pragma pack( pop )

static_assert( sizeof( StoredSymbolStats ) == 32 );
static_assert( sizeof( StoredSummary ) == 24 );

enum class ContributionKind : uint8_t
{
    Inclusive = 0,
    Exclusive = 1,
    Child = 2
};

std::string Identity( std::string_view source, std::string_view domain )
{
    if( source.size() != 64 ) throw std::runtime_error( "sample derived source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

uint64_t CanonicalPointer( CallstackFrameId id )
{
    return ( id.idx & 0x3FFFFFFFFFFFFFFF ) | ( ( id.idx & 0x3000000000000000 ) << 2 );
}

void CheckCancelled( const analysis::PagedRecordOptions& options )
{
    if( options.cancelled && options.cancelled() ) throw std::runtime_error( "sample derived build cancelled" );
}

}

uint64_t BuildQuerySampleDerivedStore( std::filesystem::path output,
    std::filesystem::path samplesPath, std::filesystem::path callstacksPath,
    std::filesystem::path callstackFramesPath, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
{
    const auto scratchRoot = output / "build-scratch";
    const auto sourcePath = scratchRoot / "contributions.work";
    const auto sortedPath = scratchRoot / "contributions-sorted.work";
    if( std::filesystem::exists( analysis::AnalysisIoPath( output ) ) )
        throw std::runtime_error( "sample derived store already exists" );
    std::filesystem::create_directories( analysis::AnalysisIoPath( scratchRoot ) );

    try
    {
        QuerySampleViewReader samples( samplesPath, sourceFingerprint, options );
        QueryCallstackStoreReader callstacks( callstacksPath, sourceFingerprint, options );
        QueryCallstackFrameStoreReader frames( callstackFramesPath, sourceFingerprint, options );
        std::ofstream contributions( analysis::AnalysisIoPath( sourcePath ), std::ios::binary | std::ios::trunc );
        if( !contributions ) throw std::runtime_error( "sample derived contribution open" );
        uint64_t contributionCount = 0;
        const auto append = [&]( uint64_t address, ContributionKind kind, bool subtract ) {
            analysis::AnalysisKeyMetadataRecord value;
            value.keyHigh = address;
            value.metadata = 1;
            value.kind = uint8_t( kind );
            value.auxiliary = subtract ? 1 : 0;
            analysis::AnalysisDiskGrow( options.disk, sizeof( value ) );
            if( options.beforeFileGrowth ) options.beforeFileGrowth( sourcePath, ( contributionCount + 1 ) * sizeof( value ) );
            contributions.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
            if( !contributions ) throw std::runtime_error( "sample derived contribution write" );
            contributionCount++;
        };
        const auto appendSymbolContributions = [&]( uint32_t callstack, bool subtract ) {
            if( callstack == 0 ) return;
            const auto stack = callstacks.Frames( callstack );
            if( stack.empty() ) return;
            for( const auto entry : stack )
            {
                const auto definition = frames.Find( entry );
                if( !definition || definition->frames.empty() ) return;
            }
            for( size_t stackIndex = 0; stackIndex < stack.size(); stackIndex++ )
            {
                const auto definition = frames.Find( stack[stackIndex] );
                if( stackIndex == 0 )
                {
                    append( definition->frames.front().symAddr, ContributionKind::Exclusive, subtract );
                    for( size_t frame = 1; frame < definition->frames.size(); frame++ )
                        append( definition->frames[frame].symAddr, ContributionKind::Inclusive, subtract );
                }
                else
                {
                    for( const auto& frame : definition->frames )
                        append( frame.symAddr, ContributionKind::Inclusive, subtract );
                }
            }
        };
        samples.Visit( [&]( const QuerySampleRow& sample ) {
            CheckCancelled( options );
            const bool context = sample.kind == QuerySampleKind::ContextSwitch;
            appendSymbolContributions( sample.callstack, context );
            if( !context && sample.callstack != 0 )
            {
                const auto stack = callstacks.Frames( sample.callstack );
                for( size_t index = 1; index < stack.size(); index++ )
                    append( CanonicalPointer( stack[index] ), ContributionKind::Child, false );
            }
            return true;
        } );
        contributions.flush();
        if( !contributions ) throw std::runtime_error( "sample derived contribution flush" );
        contributions.close();

        std::string sortError;
        if( !analysis::SortAnalysisKeyMetadataRecords( sourcePath, sortedPath, scratchRoot,
                "sample-derived", contributionCount, 128 * 1024, sortError, options.disk, options.workspace ) )
            throw std::runtime_error( "sample derived contribution sort: " + sortError );

        analysis::PagedRecordWriter records( output / "records",
            Identity( sourceFingerprint, "query-sample-symbol-statistics-v1" ), sizeof( StoredSymbolStats ), options );
        std::ifstream sorted( analysis::AnalysisIoPath( sortedPath ), std::ios::binary );
        if( !sorted ) throw std::runtime_error( "sample derived sorted contribution open" );
        StoredSummary summary;
        StoredSymbolStats current;
        bool haveCurrent = false;
        uint64_t inclusive = 0;
        uint64_t exclusive = 0;
        const auto flush = [&]() {
            if( !haveCurrent ) return;
            if( inclusive > std::numeric_limits<uint32_t>::max() || exclusive > std::numeric_limits<uint32_t>::max() )
                throw std::runtime_error( "sample derived symbol count overflow" );
            current.inclusiveSamples = uint32_t( inclusive );
            current.exclusiveSamples = uint32_t( exclusive );
            if( current.inclusiveSamples != 0 || current.exclusiveSamples != 0 || current.childSamples != 0 )
            {
                records.Append( &current, 1 );
                if( current.childSamples != 0 )
                {
                    summary.childSampleSymbols++;
                    if( current.childSamples > std::numeric_limits<uint64_t>::max() - summary.childSamples )
                        throw std::runtime_error( "sample derived child count overflow" );
                    summary.childSamples += current.childSamples;
                }
            }
        };
        analysis::AnalysisKeyMetadataRecord contribution;
        while( sorted.read( reinterpret_cast<char*>( &contribution ), sizeof( contribution ) ) )
        {
            CheckCancelled( options );
            if( !haveCurrent || current.address != contribution.keyHigh )
            {
                flush();
                current = {};
                current.address = contribution.keyHigh;
                inclusive = 0;
                exclusive = 0;
                haveCurrent = true;
            }
            const bool subtract = contribution.auxiliary != 0;
            auto apply = [&]( uint64_t& value ) {
                if( subtract )
                {
                    if( value == 0 ) throw std::runtime_error( "sample derived contribution underflow" );
                    value--;
                }
                else
                {
                    if( value == std::numeric_limits<uint64_t>::max() )
                        throw std::runtime_error( "sample derived contribution overflow" );
                    value++;
                }
            };
            switch( ContributionKind( contribution.kind ) )
            {
            case ContributionKind::Inclusive: apply( inclusive ); break;
            case ContributionKind::Exclusive: apply( exclusive ); break;
            case ContributionKind::Child:
                if( subtract ) throw std::runtime_error( "sample derived child subtraction" );
                if( current.childSamples == std::numeric_limits<uint64_t>::max() )
                    throw std::runtime_error( "sample derived child count overflow" );
                current.childSamples++;
                break;
            default: throw std::runtime_error( "sample derived contribution kind" );
            }
        }
        if( !sorted.eof() ) throw std::runtime_error( "sample derived sorted contribution read" );
        flush();
        records.Commit();
        summary.records = records.Count();
        analysis::PagedRecordWriter metadata( output / "metadata",
            Identity( sourceFingerprint, "query-sample-symbol-statistics-summary-v1" ), sizeof( StoredSummary ), options );
        metadata.Append( &summary, 1 );
        metadata.Commit();
        sorted.close();

        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( scratchRoot, options.disk, cleanupError, true );
        if( cleanupError ) throw std::runtime_error( "sample derived scratch cleanup: " + cleanupError.message() );
        return summary.records;
    }
    catch( ... )
    {
        const auto failure = std::current_exception();
        std::error_code ignored;
        analysis::AnalysisDiskRemove( output, options.disk, ignored, true );
        std::rethrow_exception( failure );
    }
}

struct QuerySampleDerivedStoreReader::Impl
{
    Impl( std::filesystem::path input, std::string identity, analysis::PagedRecordOptions storage )
        : records( input / "records", Identity( identity, "query-sample-symbol-statistics-v1" ),
            sizeof( StoredSymbolStats ), storage )
        , metadata( input / "metadata", Identity( identity, "query-sample-symbol-statistics-summary-v1" ),
            sizeof( StoredSummary ), storage )
    {
        if( metadata.Count() != 1 ) throw std::runtime_error( "sample derived summary count" );
        metadata.Read( 0, &summary, 1 );
        if( summary.records != records.Count() ) throw std::runtime_error( "sample derived record count mismatch" );
    }

    mutable analysis::PagedRecordReader records;
    mutable analysis::PagedRecordReader metadata;
    StoredSummary summary;
    mutable std::mutex mutex;
};

QuerySampleDerivedStoreReader::QuerySampleDerivedStoreReader( std::filesystem::path root,
    std::string sourceFingerprint, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QuerySampleDerivedStoreReader::~QuerySampleDerivedStoreReader() = default;
uint64_t QuerySampleDerivedStoreReader::Count() const { return m_impl->summary.records; }
uint64_t QuerySampleDerivedStoreReader::ChildSampleSymbolCount() const { return m_impl->summary.childSampleSymbols; }
uint64_t QuerySampleDerivedStoreReader::ChildSampleCount() const { return m_impl->summary.childSamples; }

std::optional<QuerySampleSymbolStatsRow> QuerySampleDerivedStoreReader::Find( uint64_t address ) const
{
    std::lock_guard lock( m_impl->mutex );
    uint64_t begin = 0;
    uint64_t end = m_impl->records.Count();
    while( begin < end )
    {
        const auto middle = begin + ( end - begin ) / 2;
        StoredSymbolStats value;
        m_impl->records.Read( middle, &value, 1 );
        if( value.address < address ) begin = middle + 1;
        else end = middle;
    }
    if( begin == m_impl->records.Count() ) return std::nullopt;
    StoredSymbolStats value;
    m_impl->records.Read( begin, &value, 1 );
    if( value.address != address ) return std::nullopt;
    return QuerySampleSymbolStatsRow { value.address, value.inclusiveSamples,
        value.exclusiveSamples, value.childSamples };
}

void QuerySampleDerivedStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->records.ValidateAll();
    m_impl->metadata.ValidateAll();
    uint64_t previous = 0;
    uint64_t childSymbols = 0;
    uint64_t childSamples = 0;
    for( uint64_t index = 0; index < m_impl->records.Count(); index++ )
    {
        StoredSymbolStats value;
        m_impl->records.Read( index, &value, 1 );
        if( ( index != 0 && value.address <= previous ) ||
            ( value.inclusiveSamples == 0 && value.exclusiveSamples == 0 && value.childSamples == 0 ) )
            throw std::runtime_error( "sample derived record order" );
        previous = value.address;
        if( value.childSamples != 0 )
        {
            childSymbols++;
            if( value.childSamples > std::numeric_limits<uint64_t>::max() - childSamples )
                throw std::runtime_error( "sample derived child summary overflow" );
            childSamples += value.childSamples;
        }
    }
    if( childSymbols != m_impl->summary.childSampleSymbols || childSamples != m_impl->summary.childSamples )
        throw std::runtime_error( "sample derived child summary mismatch" );
}

}
