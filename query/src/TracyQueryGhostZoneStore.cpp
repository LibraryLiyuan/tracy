#include "TracyQueryGhostZoneStore.hpp"

#include "TracyHash.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleStore.hpp"
#include "TracyPagedRecordStore.hpp"

#include <cstring>
#include <cstdio>
#include <charconv>
#include <limits>
#include <fstream>
#include <list>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

namespace tracy::query
{
namespace
{

#pragma pack( push, 1 )
struct StoredGhostZone
{
    int64_t startNs;
    int64_t endNs;
    uint64_t thread;
    uint64_t parent;
    uint64_t name;
    uint64_t file;
    uint64_t address;
    uint32_t line;
    uint32_t depth;
    uint32_t childCount;
    uint8_t inlineFrame;
    uint8_t reserved[3];
};
#pragma pack( pop )

static_assert( sizeof( StoredGhostZone ) == QueryGhostZoneStoreRecordBytes );
constexpr uint64_t NoParent = std::numeric_limits<uint64_t>::max();

std::string Identity( std::string_view source, std::string_view domain )
{
    if( source.size() != 64 ) throw std::runtime_error( "ghost-zone store source identity must be SHA-256" );
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( domain.data(), domain.size() );
    return hash.FinalHex();
}

void WriteString( QueryStringStoreWriter& writer, uint64_t ordinal, std::string_view value )
{
    writer.StringBegin( ordinal, ordinal, value.size() );
    if( !value.empty() ) writer.StringBytes( ordinal, 0, value.data(), value.size() );
    writer.StringEnd( ordinal );
}

struct BuildFrame
{
    uint64_t id = 0;
    uint64_t symAddr = 0;
    uint8_t inlineFrame = 0;
    bool reusable = false;
    std::string name;
    std::string file;
    uint32_t line = 0;
};

#pragma pack( push, 1 )
struct ScratchGhostZone
{
    uint64_t thread = 0;
    uint64_t parent = NoParent;
    int64_t startNs = 0;
    int64_t endNs = 0;
    uint64_t symAddr = 0;
    uint64_t nameOffset = 0;
    uint64_t fileOffset = 0;
    uint64_t addressOffset = 0;
    uint32_t nameBytes = 0;
    uint32_t fileBytes = 0;
    uint32_t addressBytes = 0;
    uint32_t line = 0;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    uint8_t inlineFrame = 0;
    uint8_t reusable = 0;
    uint8_t reserved[2] = {};
};
#pragma pack( pop )

static_assert( sizeof( ScratchGhostZone ) == 92 );

uint64_t CanonicalPointer( CallstackFrameId id )
{
    return ( id.idx & 0x3FFFFFFFFFFFFFFF ) | ( ( id.idx & 0x3000000000000000 ) << 2 );
}

std::string Ref( std::string_view fingerprint, std::string_view kind, uint64_t value )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << value;
    return out.str();
}

void GrowScratch( const analysis::PagedRecordOptions& options, const std::filesystem::path& path,
    uint64_t growth, uint64_t proposedBytes )
{
    analysis::AnalysisDiskGrow( options.disk, growth );
    if( options.beforeFileGrowth ) options.beforeFileGrowth( path, proposedBytes );
}

std::string ResolveString( const QueryStringStoreReader& reader, const StringIdx& value )
{
    return value.Active() ? reader.ReadOrdinal( value.Idx(), 1 << 20 ) : std::string();
}

std::vector<BuildFrame> BuildPath( const QueryCallstackStoreReader& callstacks,
    const QueryCallstackFrameStoreReader& frames, const QueryStringStoreReader& strings, uint32_t callstack )
{
    std::vector<BuildFrame> result;
    const auto stack = callstacks.Frames( callstack );
    for( size_t stackIndex = stack.size(); stackIndex != 0; stackIndex-- )
    {
        const auto entry = stack[stackIndex - 1];
        const auto definition = frames.Find( entry );
        if( !definition )
        {
            result.push_back( { entry.data, CanonicalPointer( entry ), 0, false, {}, {}, 0 } );
            continue;
        }
        for( size_t index = definition->frames.size(); index != 0; index-- )
        {
            const auto& frame = definition->frames[index - 1];
            result.push_back( { entry.data, CanonicalPointer( entry ), uint8_t( index < definition->frames.size() ), true,
                ResolveString( strings, frame.name ), ResolveString( strings, frame.file ), frame.line } );
        }
    }
    return result;
}

void ReadScratchString( std::ifstream& input, uint64_t offset, uint32_t bytes, std::string& output )
{
    output.resize( bytes );
    if( bytes == 0 ) return;
    input.seekg( std::streamoff( offset ) );
    input.read( output.data(), std::streamsize( bytes ) );
    if( !input ) throw std::runtime_error( "ghost-zone scratch string read" );
}

uint64_t ParseRefOrdinal( std::string_view ref, const char* kind )
{
    const auto marker = ref.rfind( ':' );
    if( marker == std::string_view::npos || ref.find( std::string( ":" ) + kind + ":" ) == std::string_view::npos )
        throw std::runtime_error( "ghost-zone entity reference" );
    uint64_t value = 0;
    const auto parsed = std::from_chars( ref.data() + marker + 1, ref.data() + ref.size(), value, 16 );
    if( parsed.ec != std::errc() || parsed.ptr != ref.data() + ref.size() )
        throw std::runtime_error( "ghost-zone entity ordinal" );
    return value;
}

uint64_t BuildQueryGhostZoneStoreImpl( std::filesystem::path output,
    std::filesystem::path samplesPath, std::filesystem::path callstacksPath,
    std::filesystem::path callstackFramesPath, std::filesystem::path stringsPath,
    std::string sourceFingerprint, int64_t samplingPeriodNs, analysis::PagedRecordOptions options,
    QueryGhostZoneBuildStatistics* outputStatistics )
{
    const auto scratchPath = output / "build-scratch.bin";
    const auto stringsScratchPath = output / "build-strings.bin";
    std::filesystem::create_directories( analysis::AnalysisIoPath( output ) );
    GrowScratch( options, scratchPath, 0, 0 );
    GrowScratch( options, stringsScratchPath, 0, 0 );
    std::fstream scratch( analysis::AnalysisIoPath( scratchPath ), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
    std::ofstream scratchStrings( analysis::AnalysisIoPath( stringsScratchPath ), std::ios::binary | std::ios::trunc );
    if( !scratch || !scratchStrings ) throw std::runtime_error( "ghost-zone scratch open" );

    QuerySampleStoreReader samples( samplesPath, sourceFingerprint, options );
    QueryCallstackStoreReader callstacks( callstacksPath, sourceFingerprint, options );
    QueryCallstackFrameStoreReader frames( callstackFramesPath, sourceFingerprint, options );
    QueryStringStoreReader strings( stringsPath, sourceFingerprint, options );
    std::vector<QuerySampleMainSpan> spans = samples.MainSpans();
    std::sort( spans.begin(), spans.end(), []( const auto& lhs, const auto& rhs ) { return lhs.thread < rhs.thread; } );

    uint64_t count = 0;
    const auto period = samplingPeriodNs;
    struct ActiveGhostZone
    {
        uint64_t ordinal = 0;
        ScratchGhostZone record;
    };
    std::vector<ActiveGhostZone> active;
    analysis::AnalysisWorkspaceReservation activeMemory( options.workspace );
    if( outputStatistics ) *outputStatistics = {};
    QueryGhostZoneBuildStatistics localStatistics;
    auto& statistics = outputStatistics ? *outputStatistics : localStatistics;
    constexpr uint64_t pathCacheBudget = 8 * 1024 * 1024;
    constexpr size_t maximumPathCacheEntries = 16384;
    analysis::AnalysisWorkspaceReservation pathCacheMemory( options.workspace, pathCacheBudget );
    struct CachedPath
    {
        uint32_t callstack = 0;
        uint64_t footprint = 0;
        std::vector<BuildFrame> frames;
    };
    std::list<CachedPath> pathCache;
    std::unordered_map<uint32_t, std::list<CachedPath>::iterator> pathIndex;
    pathIndex.reserve( maximumPathCacheEntries );
    uint64_t pathCacheBytes = 0;
    const auto pathFootprint = []( const std::vector<BuildFrame>& path ) {
        uint64_t bytes = 256 + uint64_t( path.size() ) * sizeof( BuildFrame );
        for( const auto& frame : path ) bytes += frame.name.size() + frame.file.size();
        return bytes;
    };
    const auto resolvePath = [&]( uint32_t callstack, std::vector<BuildFrame>& uncached ) -> const std::vector<BuildFrame>& {
        statistics.pathRequests++;
        if( const auto found = pathIndex.find( callstack ); found != pathIndex.end() )
        {
            statistics.pathCacheHits++;
            pathCache.splice( pathCache.begin(), pathCache, found->second );
            found->second = pathCache.begin();
            return found->second->frames;
        }
        statistics.pathResolutions++;
        uncached = BuildPath( callstacks, frames, strings, callstack );
        const auto footprint = pathFootprint( uncached );
        while( !pathCache.empty() &&
            ( pathCache.size() >= maximumPathCacheEntries || footprint > pathCacheBudget - pathCacheBytes ) )
        {
            pathCacheBytes -= pathCache.back().footprint;
            pathIndex.erase( pathCache.back().callstack );
            pathCache.pop_back();
            statistics.pathCacheEvictions++;
        }
        if( footprint <= pathCacheBudget - pathCacheBytes )
        {
            pathCache.push_front( { callstack, footprint, std::move( uncached ) } );
            pathIndex[callstack] = pathCache.begin();
            pathCacheBytes += footprint;
            statistics.pathCachePeakBytes = std::max( statistics.pathCachePeakBytes, pathCacheBytes );
            return pathCache.front().frames;
        }
        return uncached;
    };
    const auto writeRecord = [&]( uint64_t ordinal, const ScratchGhostZone& value ) {
        scratch.seekp( std::streamoff( ordinal * sizeof( ScratchGhostZone ) ) );
        scratch.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !scratch ) throw std::runtime_error( "ghost-zone scratch record write" );
        statistics.scratchRecordWrites++;
    };
    const auto flushActiveFrom = [&]( size_t first ) {
        for( size_t index = active.size(); index > first; index-- )
            writeRecord( active[index - 1].ordinal, active[index - 1].record );
        active.resize( first );
    };
    uint64_t processedSamples = 0;
    uint64_t totalSamples = 0;
    for( const auto& span : spans ) totalSamples += span.count;
    const auto reportProgress = [&] {
        fprintf( stderr, "Query ghost progress samples=%llu/%llu zones=%llu resolutions=%llu hits=%llu scratch_writes=%llu\n",
            (unsigned long long)processedSamples, (unsigned long long)totalSamples, (unsigned long long)count,
            (unsigned long long)statistics.pathResolutions, (unsigned long long)statistics.pathCacheHits,
            (unsigned long long)statistics.scratchRecordWrites );
        fflush( stderr );
    };
    reportProgress();
    for( const auto& span : spans )
    {
        active.clear();
        const bool skipZeroTime = span.count != 0 &&
            samples.ReadPhysical( span.first, span.thread, QuerySampleKind::Main ).timeNs == 0;
        for( uint64_t index = 0; index < span.count; index++ )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "ghost-zone build cancelled" );
            if( ++processedSamples % 1000000 == 0 ) reportProgress();
            const auto sample = samples.ReadPhysical( span.first + index, span.thread, QuerySampleKind::Main );
            if( skipZeroTime && sample.timeNs == 0 ) continue;
            std::vector<BuildFrame> uncachedPath;
            const auto& path = resolvePath( sample.callstack, uncachedPath );
            if( path.size() > active.capacity() )
            {
                // Include both old and replacement allocations during vector growth.
                activeMemory.Resize( ( uint64_t( active.capacity() ) + path.size() ) * sizeof( ActiveGhostZone ) );
                active.reserve( path.size() );
                activeMemory.Resize( uint64_t( active.capacity() ) * sizeof( ActiveGhostZone ) );
            }
            if( !active.empty() )
            {
                const auto previousEnd = active.front().record.endNs;
                if( previousEnd > sample.timeNs )
                {
                    for( auto& value : active )
                    {
                        if( value.record.endNs != previousEnd ) break;
                        value.record.endNs = sample.timeNs;
                    }
                }
            }
            const auto referenceEnd = active.empty() ? int64_t( 0 ) : active.front().record.endNs;
            size_t common = 0;
            while( common < active.size() && common < path.size() )
            {
                const auto& record = active[common].record;
                if( record.endNs != referenceEnd ) break;
                if( record.reusable == 0 ) break;
                if( record.symAddr != path[common].symAddr ) break;
                common++;
            }
            for( size_t i = 0; i < common; i++ )
                active[i].record.endNs = sample.timeNs + period;
            flushActiveFrom( common );
            for( size_t i = common; i < path.size(); i++ )
            {
                const auto& frame = path[i];
                ScratchGhostZone record;
                record.thread = span.thread;
                record.parent = active.empty() ? NoParent : active.back().ordinal;
                record.startNs = sample.timeNs;
                record.endNs = sample.timeNs + period;
                record.symAddr = frame.symAddr;
                record.depth = uint32_t( i );
                record.inlineFrame = frame.inlineFrame;
                record.reusable = frame.reusable;
                record.line = frame.line;
                record.addressOffset = std::numeric_limits<uint64_t>::max();
                const auto append = [&]( const std::string& value, uint64_t& offset, uint32_t& bytes ) {
                    offset = uint64_t( scratchStrings.tellp() );
                    bytes = uint32_t( value.size() );
                    GrowScratch( options, stringsScratchPath, bytes, offset + bytes );
                    if( bytes ) scratchStrings.write( value.data(), std::streamsize( bytes ) );
                };
                append( frame.name, record.nameOffset, record.nameBytes );
                append( frame.file, record.fileOffset, record.fileBytes );
                std::ostringstream address; address << "0x" << std::hex << frame.symAddr;
                append( address.str(), record.addressOffset, record.addressBytes );
                if( record.parent != NoParent )
                    active.back().record.childCount++;
                GrowScratch( options, scratchPath, sizeof( ScratchGhostZone ),
                    ( count + 1 ) * sizeof( ScratchGhostZone ) );
                active.push_back( { count++, record } );
            }
        }
        flushActiveFrom( 0 );
    }
    reportProgress();
    scratch.flush(); scratchStrings.flush();
    if( !scratch || !scratchStrings ) throw std::runtime_error( "ghost-zone scratch flush" );
    scratch.close(); scratchStrings.close();

    QueryGhostZoneStoreWriter writer( output, sourceFingerprint, options );
    writer.Begin( count );
    std::ifstream recordsInput( analysis::AnalysisIoPath( scratchPath ), std::ios::binary );
    std::ifstream stringsInput( analysis::AnalysisIoPath( stringsScratchPath ), std::ios::binary );
    if( !recordsInput || !stringsInput ) throw std::runtime_error( "ghost-zone scratch reopen" );
    for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
    {
        if( options.cancelled && options.cancelled() ) throw std::runtime_error( "ghost-zone build cancelled" );
        ScratchGhostZone record;
        recordsInput.read( reinterpret_cast<char*>( &record ), sizeof( record ) );
        if( !recordsInput ) throw std::runtime_error( "ghost-zone scratch sequential read" );
        analysis::GhostZoneDto value;
        value.ref = Ref( sourceFingerprint, "ghost-zone", ordinal );
        value.threadRef = Ref( sourceFingerprint, "thread", record.thread );
        if( record.parent != NoParent ) value.parentRef = Ref( sourceFingerprint, "ghost-zone", record.parent );
        value.startNs = record.startNs; value.endNs = record.endNs; value.line = record.line;
        value.depth = record.depth; value.childCount = record.childCount; value.inlineFrame = record.inlineFrame != 0;
        ReadScratchString( stringsInput, record.nameOffset, record.nameBytes, value.name );
        ReadScratchString( stringsInput, record.fileOffset, record.fileBytes, value.file );
        ReadScratchString( stringsInput, record.addressOffset, record.addressBytes, value.address );
        writer.Append( ordinal, value );
    }
    writer.Commit();
    recordsInput.close();
    stringsInput.close();
    std::error_code error;
    analysis::AnalysisDiskRemove( scratchPath, options.disk, error );
    if( error ) throw std::runtime_error( "ghost-zone scratch remove: " + error.message() );
    error.clear();
    analysis::AnalysisDiskRemove( stringsScratchPath, options.disk, error );
    if( error ) throw std::runtime_error( "ghost-zone string scratch remove: " + error.message() );
    return count;
}

}

uint64_t BuildQueryGhostZoneStore( std::filesystem::path output,
    std::filesystem::path samplesPath, std::filesystem::path callstacksPath,
    std::filesystem::path callstackFramesPath, std::filesystem::path stringsPath,
    std::string sourceFingerprint, int64_t samplingPeriodNs, analysis::PagedRecordOptions options,
    QueryGhostZoneBuildStatistics* statistics )
{
    return BuildQueryGhostZoneStoreImpl( std::move( output ), std::move( samplesPath ), std::move( callstacksPath ),
        std::move( callstackFramesPath ), std::move( stringsPath ), std::move( sourceFingerprint ), samplingPeriodNs,
        std::move( options ), statistics );
}

struct QueryGhostZoneStoreWriter::Impl
{
    Impl( std::filesystem::path output, std::string identity, analysis::PagedRecordOptions storage )
        : root( std::move( output ) ), source( std::move( identity ) ), options( std::move( storage ) )
        , records( root / "records", Identity( source, "query-ghost-zones-v1-records" ), sizeof( StoredGhostZone ), options )
        , strings( root / "strings", source, options )
    {}

    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    analysis::PagedRecordWriter records;
    QueryStringStoreWriter strings;
    uint64_t expected = 0;
    uint64_t seen = 0;
    bool begun = false;
    bool committed = false;
};

QueryGhostZoneStoreWriter::QueryGhostZoneStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGhostZoneStoreWriter::~QueryGhostZoneStoreWriter() = default;

void QueryGhostZoneStoreWriter::Begin( uint64_t count )
{
    auto& value = *m_impl;
    if( value.begun || value.committed || count > ( std::numeric_limits<uint64_t>::max() / 3 ) )
        throw std::runtime_error( "ghost-zone store begin" );
    value.expected = count;
    value.strings.StringsBegin( count * 3 );
    value.begun = true;
}

void QueryGhostZoneStoreWriter::Append( uint64_t ordinal, const analysis::GhostZoneDto& dto )
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || ordinal != value.seen || ordinal >= value.expected )
        throw std::runtime_error( "ghost-zone store append order" );
    const auto stringBase = ordinal * 3;
    WriteString( value.strings, stringBase, dto.name );
    WriteString( value.strings, stringBase + 1, dto.file );
    WriteString( value.strings, stringBase + 2, dto.address );
    uint64_t parent = NoParent;
    if( dto.parentRef )
    {
        parent = ParseRefOrdinal( *dto.parentRef, "ghost-zone" );
        if( parent >= ordinal )
            throw std::runtime_error( "ghost-zone parent ordinal" );
    }
    StoredGhostZone record { dto.startNs, dto.endNs, 0, parent, stringBase, stringBase + 1,
        stringBase + 2, dto.line, dto.depth, dto.childCount, uint8_t( dto.inlineFrame ), {} };
    // The thread reference is deliberately parsed only at the sidecar boundary;
    // Worker entity refs are decimal ordinals by protocol.
    record.thread = ParseRefOrdinal( dto.threadRef, "thread" );
    value.records.Append( &record, 1 );
    value.seen++;
}

void QueryGhostZoneStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || value.seen != value.expected )
        throw std::runtime_error( "ghost-zone store commit" );
    value.strings.StringsEnd();
    for( size_t kind = 0; kind < 3; kind++ )
    {
        const auto pointerKind = QueryStringPointerKind( kind );
        value.strings.PointerTableBegin( pointerKind, 0 );
        value.strings.PointerTableEnd( pointerKind );
    }
    value.strings.Commit();
    value.records.Commit();
    value.committed = true;
}

uint64_t QueryGhostZoneStoreWriter::Count() const { return m_impl->seen; }

struct QueryGhostZoneStoreReader::Impl
{
    Impl( std::filesystem::path input, std::string identity, analysis::PagedRecordOptions storage )
        : root( std::move( input ) ), source( std::move( identity ) )
        , records( root / "records", Identity( source, "query-ghost-zones-v1-records" ), sizeof( StoredGhostZone ), storage )
        , strings( root / "strings", source, storage )
    {
        if( strings.StringCount() != records.Count() * 3 ) throw std::runtime_error( "ghost-zone string count mismatch" );
    }

    QueryGhostZoneRow Read( uint64_t ordinal ) const
    {
        StoredGhostZone record;
        records.Read( ordinal, &record, 1 );
        if( ( record.parent != NoParent && record.parent >= ordinal ) ||
            record.name >= strings.StringCount() || record.file >= strings.StringCount() ||
            record.address >= strings.StringCount() )
            throw std::runtime_error( "ghost-zone record directory invalid" );
        QueryGhostZoneRow result;
        result.ordinal = ordinal;
        result.thread = record.thread;
        if( record.parent != NoParent ) result.parent = record.parent;
        result.startNs = record.startNs;
        result.endNs = record.endNs;
        result.name = strings.ReadOrdinal( record.name, strings.StringLength( record.name ) );
        result.file = strings.ReadOrdinal( record.file, strings.StringLength( record.file ) );
        result.line = record.line;
        result.address = strings.ReadOrdinal( record.address, strings.StringLength( record.address ) );
        result.depth = record.depth;
        result.childCount = record.childCount;
        result.inlineFrame = record.inlineFrame != 0;
        return result;
    }

    std::filesystem::path root;
    std::string source;
    mutable analysis::PagedRecordReader records;
    mutable QueryStringStoreReader strings;
    mutable std::mutex mutex;
};

QueryGhostZoneStoreReader::QueryGhostZoneStoreReader( std::filesystem::path root, std::string sourceFingerprint,
    analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move( options ) ) )
{}

QueryGhostZoneStoreReader::~QueryGhostZoneStoreReader() = default;
uint64_t QueryGhostZoneStoreReader::Count() const { return m_impl->records.Count(); }

std::vector<QueryGhostZoneRow> QueryGhostZoneStoreReader::Scan( int64_t startNs, int64_t endNs,
    uint64_t offset, size_t limit ) const
{
    std::lock_guard lock( m_impl->mutex );
    std::vector<QueryGhostZoneRow> result;
    if( startNs >= endNs || limit == 0 ) return result;
    result.reserve( limit );
    for( uint64_t ordinal = 0; ordinal < m_impl->records.Count(); ordinal++ )
    {
        StoredGhostZone record;
        m_impl->records.Read( ordinal, &record, 1 );
        if( record.startNs >= endNs || record.endNs <= startNs ) continue;
        if( offset != 0 ) { offset--; continue; }
        result.emplace_back( m_impl->Read( ordinal ) );
        if( result.size() == limit ) break;
    }
    return result;
}

void QueryGhostZoneStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    m_impl->records.ValidateAll();
    m_impl->strings.ValidateAll();
    for( uint64_t ordinal = 0; ordinal < m_impl->records.Count(); ordinal++ ) (void)m_impl->Read( ordinal );
}

}
