#include "TracyQueryGhostZoneStore.hpp"

#include "TracyHash.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleStore.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueryResourceContext.hpp"

#include <cstring>
#include <cstdio>
#include <sstream>
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

struct BuildFrame
{
    uint64_t id = 0;
    uint64_t symAddr = 0;
    uint8_t inlineFrame = 0;
    bool reusable = false;
    uint64_t name = UINT64_MAX;
    uint64_t file = UINT64_MAX;
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
    uint64_t name = UINT64_MAX;
    uint64_t file = UINT64_MAX;
    uint32_t line = 0;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    uint8_t inlineFrame = 0;
    uint8_t reusable = 0;
    uint8_t reserved[2] = {};
};
#pragma pack( pop )

static_assert( sizeof( ScratchGhostZone ) == 72 );

#pragma pack( push, 1 )
struct ScratchGhostPatch
{
    int64_t endNs = 0;
    uint32_t childCount = 0;
};
#pragma pack( pop )
static_assert( sizeof( ScratchGhostPatch ) == 12 );

uint64_t CanonicalPointer( CallstackFrameId id )
{
    return ( id.idx & 0x3FFFFFFFFFFFFFFF ) | ( ( id.idx & 0x3000000000000000 ) << 2 );
}

void GrowScratch( const analysis::PagedRecordOptions& options, const std::filesystem::path& path,
    uint64_t growth, uint64_t proposedBytes )
{
    analysis::AnalysisDiskGrow( options.disk, growth );
    if( options.beforeFileGrowth ) options.beforeFileGrowth( path, proposedBytes );
}

uint64_t ResolveString( const QueryStringStoreReader& reader, const StringIdx& value )
{
    if( !value.Active() ) return UINT64_MAX;
    if( value.Idx() >= reader.StringCount() ) throw std::runtime_error( "ghost-zone global string reference" );
    return value.Idx();
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
            result.push_back( { entry.data, CanonicalPointer( entry ), 0, false, UINT64_MAX, UINT64_MAX, 0 } );
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

uint64_t BuildQueryGhostZoneStoreImpl( std::filesystem::path output,
    std::filesystem::path samplesPath, std::filesystem::path callstacksPath,
    std::filesystem::path callstackFramesPath, std::filesystem::path stringsPath,
    std::string sourceFingerprint, int64_t samplingPeriodNs, analysis::PagedRecordOptions options,
    QueryGhostZoneBuildStatistics* outputStatistics )
{
    const auto scratchPath = output / "build-patches.bin";
    const auto stagingPath = output / "build-static";
    std::filesystem::create_directories( analysis::AnalysisIoPath( output ) );
    GrowScratch( options, scratchPath, 0, 0 );
    std::fstream scratch( analysis::AnalysisIoPath( scratchPath ), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
    if( !scratch ) throw std::runtime_error( "ghost-zone scratch open" );
    const auto stagingIdentity = Identity( sourceFingerprint, "query-ghost-build-static-v1" );
    analysis::PagedRecordWriter staging( stagingPath, stagingIdentity, sizeof( StoredGhostZone ), options );

    QuerySampleStoreReader samples( samplesPath, sourceFingerprint, options );
    auto dictionaryOptions = options;
    if( dictionaryOptions.readerCacheBytes == 0 ) dictionaryOptions.readerCacheBytes = 64 * 1024 * 1024;
    QueryCallstackStoreReader callstacks( callstacksPath, sourceFingerprint, dictionaryOptions );
    QueryCallstackFrameStoreReader frames( callstackFramesPath, sourceFingerprint, dictionaryOptions );
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
        return 256 + uint64_t( path.capacity() ) * sizeof( BuildFrame );
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
        const auto ss = strings.RecordReadStatistics();
        const auto fs = frames.RecordReadStatistics();
        const auto cs = callstacks.RecordReadStatistics();
        statistics.stringBlockLoads = ss.blockLoads; statistics.stringDecodedBytes = ss.decodedBytes;
        statistics.frameBlockLoads = fs.blockLoads; statistics.frameDecodedBytes = fs.decodedBytes;
        statistics.callstackBlockLoads = cs.blockLoads; statistics.callstackDecodedBytes = cs.decodedBytes;
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
    struct FinalizedRecord { uint64_t ordinal; ScratchGhostPatch value; };
    constexpr size_t writeBatchRecords = 8192;
    analysis::AnalysisWorkspaceReservation batchMemory( options.workspace,
        writeBatchRecords * ( sizeof( FinalizedRecord ) + sizeof( ScratchGhostPatch ) ) + 4096 );
    std::vector<FinalizedRecord> pending;
    std::vector<ScratchGhostPatch> serialized;
    pending.reserve( writeBatchRecords ); serialized.reserve( writeBatchRecords );
    const auto flushPending = [&] {
        std::sort( pending.begin(), pending.end(), []( const auto& a, const auto& b ) { return a.ordinal < b.ordinal; } );
        for( size_t first = 0; first < pending.size(); ) {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "ghost-zone build cancelled" );
            size_t end = first + 1;
            while( end < pending.size() && pending[end].ordinal == pending[end - 1].ordinal + 1 ) ++end;
            serialized.clear();
            for( size_t i = first; i < end; ++i ) serialized.push_back( pending[i].value );
            scratch.seekp( std::streamoff( pending[first].ordinal * sizeof( ScratchGhostPatch ) ) );
            scratch.write( reinterpret_cast<const char*>( serialized.data() ), std::streamsize( serialized.size() * sizeof( ScratchGhostPatch ) ) );
            if( !scratch ) throw std::runtime_error( "ghost-zone scratch record write" );
            ++statistics.scratchWriteCalls;
            first = end;
        }
        pending.clear();
    };
    const auto writeRecord = [&]( uint64_t ordinal, const ScratchGhostZone& value ) {
        pending.push_back( { ordinal, { value.endNs, value.childCount } } );
        ++statistics.scratchRecordWrites;
        if( pending.size() == writeBatchRecords ) flushPending();
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
                record.name = frame.name;
                record.file = frame.file;
                if( record.parent != NoParent )
                    active.back().record.childCount++;
                GrowScratch( options, scratchPath, sizeof( ScratchGhostPatch ),
                    ( count + 1 ) * sizeof( ScratchGhostPatch ) );
                const StoredGhostZone staticRecord { record.startNs, record.endNs, record.thread,
                    record.parent, record.name, record.file, record.symAddr, record.line,
                    record.depth, 0, record.inlineFrame, {} };
                staging.Append( &staticRecord, 1 );
                active.push_back( { count++, record } );
            }
        }
        flushActiveFrom( 0 );
    }
    flushPending();
    reportProgress();
    staging.Commit();
    scratch.flush();
    if( !scratch ) throw std::runtime_error( "ghost-zone scratch flush" );
    scratch.close();

    {
        analysis::PagedRecordReader staticRows( stagingPath, stagingIdentity,
            sizeof( StoredGhostZone ), options );
        if( staticRows.Count() != count ) throw std::runtime_error( "ghost-zone static count mismatch" );
        std::ifstream patches( analysis::AnalysisIoPath( scratchPath ), std::ios::binary );
        if( !patches ) throw std::runtime_error( "ghost-zone patch scratch reopen" );
        QueryGhostZoneStoreWriter writer( output, sourceFingerprint, options );
        writer.Begin( count );
        constexpr size_t finalJoinRecords = 8192;
        analysis::AnalysisWorkspaceReservation finalJoinMemory( options.workspace,
            finalJoinRecords * ( sizeof( StoredGhostZone ) + sizeof( ScratchGhostPatch ) +
                sizeof( QueryGhostZoneFact ) ) + 4096 );
        std::vector<StoredGhostZone> staticRecords( finalJoinRecords );
        std::vector<ScratchGhostPatch> patchRecords( finalJoinRecords );
        std::vector<QueryGhostZoneFact> facts( finalJoinRecords );
        for( uint64_t ordinal = 0; ordinal < count; ordinal += finalJoinRecords )
        {
            if( options.cancelled && options.cancelled() ) throw std::runtime_error( "ghost-zone build cancelled" );
            const auto take = size_t( std::min<uint64_t>( finalJoinRecords, count - ordinal ) );
            staticRows.Read( ordinal, staticRecords.data(), take );
            patches.read( reinterpret_cast<char*>( patchRecords.data() ),
                std::streamsize( take * sizeof( ScratchGhostPatch ) ) );
            if( !patches ) throw std::runtime_error( "ghost-zone patch scratch sequential read" );
            for( size_t index = 0; index < take; index++ )
            {
                const auto& staticRecord = staticRecords[index];
                const auto& patch = patchRecords[index];
                auto& value = facts[index];
                value.thread = staticRecord.thread;
                value.parent = staticRecord.parent;
                value.startNs = staticRecord.startNs; value.endNs = patch.endNs;
                value.line = staticRecord.line;
                value.depth = staticRecord.depth; value.childCount = patch.childCount;
                value.inlineFrame = staticRecord.inlineFrame != 0;
                value.name = staticRecord.name;
                value.file = staticRecord.file;
                value.address = staticRecord.address;
            }
            writer.AppendBatch( ordinal, facts.data(), take );
            statistics.finalJoinBatches++;
        }
        writer.Commit();
    }
    std::error_code error;
    analysis::AnalysisDiskRemove( scratchPath, options.disk, error );
    if( error ) throw std::runtime_error( "ghost-zone patch scratch remove: " + error.message() );
    analysis::AnalysisDiskRemove( stagingPath, options.disk, error, true );
    if( error ) throw std::runtime_error( "ghost-zone static scratch remove: " + error.message() );
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
        , records( root / "records", Identity( source, "query-ghost-zones-v2-global-records" ), sizeof( StoredGhostZone ), options )
        , batchMemory( options.workspace )
    {}

    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    analysis::PagedRecordWriter records;
    analysis::AnalysisWorkspaceReservation batchMemory;
    std::vector<StoredGhostZone> batch;
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
    if( value.begun || value.committed )
        throw std::runtime_error( "ghost-zone store begin" );
    value.expected = count;
    value.begun = true;
}

void QueryGhostZoneStoreWriter::Append( uint64_t ordinal, const QueryGhostZoneFact& dto )
{
    AppendBatch( ordinal, &dto, 1 );
}

void QueryGhostZoneStoreWriter::AppendBatch( uint64_t ordinal, const QueryGhostZoneFact* values, size_t count )
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || ordinal != value.seen || count > value.expected - value.seen ||
        ( count != 0 && values == nullptr ) )
        throw std::runtime_error( "ghost-zone store append order" );
    if( count > value.batch.capacity() )
    {
        value.batchMemory.Resize( uint64_t( value.batch.capacity() + count ) * sizeof( StoredGhostZone ) );
        value.batch.reserve( count );
        value.batchMemory.Resize( uint64_t( value.batch.capacity() ) * sizeof( StoredGhostZone ) );
    }
    value.batch.clear();
    for( size_t index = 0; index < count; index++ )
    {
        const auto& dto = values[index];
        if( dto.parent != NoParent && dto.parent >= ordinal + index )
            throw std::runtime_error( "ghost-zone parent ordinal" );
        value.batch.push_back( { dto.startNs, dto.endNs, dto.thread, dto.parent, dto.name, dto.file,
            dto.address, dto.line, dto.depth, dto.childCount, uint8_t( dto.inlineFrame ), {} } );
    }
    value.records.Append( value.batch.data(), count );
    value.seen += count;
}

void QueryGhostZoneStoreWriter::Commit()
{
    auto& value = *m_impl;
    if( !value.begun || value.committed || value.seen != value.expected )
        throw std::runtime_error( "ghost-zone store commit" );
    value.records.Commit();
    value.committed = true;
}

uint64_t QueryGhostZoneStoreWriter::Count() const { return m_impl->seen; }

struct QueryGhostZoneStoreReader::Impl
{
    Impl( std::filesystem::path input, std::string identity, std::filesystem::path globalStrings, analysis::PagedRecordOptions storage )
        : root( std::move( input ) ), source( std::move( identity ) )
        , records( root / "records", Identity( source, "query-ghost-zones-v2-global-records" ), sizeof( StoredGhostZone ), storage )
        , strings( std::move(globalStrings), source, QueryDictionaryReadOptions(storage,false) )
    {}

    void CheckRecord( uint64_t ordinal, const StoredGhostZone& record ) const
    {
        if( ( record.parent != NoParent && record.parent >= ordinal ) ||
            ( record.name != UINT64_MAX && record.name >= strings.StringCount() ) ||
            ( record.file != UINT64_MAX && record.file >= strings.StringCount() ) ||
            record.inlineFrame > 1 || record.reserved[0] || record.reserved[1] || record.reserved[2] )
            throw std::runtime_error( "ghost-zone record directory invalid" );
    }

    std::string ReadString( uint64_t ordinal ) const
    {
        if( ordinal == UINT64_MAX ) return {};
        const auto bytes = strings.StringLength( ordinal );
        if( bytes > (1 << 20) ) throw QueryResourceError("response", "ghost_string_response_limit", bytes);
        return strings.ReadOrdinal( ordinal, 1 << 20 );
    }

    QueryGhostZoneRow Read( uint64_t ordinal, const StoredGhostZone& record,
        bool resolveStrings = true ) const
    {
        CheckRecord( ordinal, record );
        QueryGhostZoneRow result;
        result.ordinal = ordinal;
        result.thread = record.thread;
        if( record.parent != NoParent ) result.parent = record.parent;
        result.startNs = record.startNs;
        result.endNs = record.endNs;
        if( resolveStrings )
        {
            result.name = ReadString( record.name );
            result.file = ReadString( record.file );
        }
        result.line = record.line;
        std::ostringstream address; address << "0x" << std::hex << record.address;
        result.address = address.str();
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
    std::filesystem::path globalStrings, analysis::PagedRecordOptions options )
    : m_impl( std::make_unique<Impl>( std::move( root ), std::move( sourceFingerprint ), std::move(globalStrings), std::move( options ) ) )
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
        result.emplace_back( m_impl->Read( ordinal, record ) );
        if( result.size() == limit ) break;
    }
    return result;
}

std::vector<QueryGhostZoneRow> QueryGhostZoneStoreReader::ScanOrdinals( uint64_t offset, size_t limit,
    int64_t startNs, int64_t endNs ) const
{
    std::lock_guard lock( m_impl->mutex );
    if( limit > 4096 ) throw QueryResourceError( "response", "ghost_zone_page_limit", limit );
    std::vector<QueryGhostZoneRow> result;
    const auto total = m_impl->records.Count();
    if( offset >= total || limit == 0 ) return result;
    const auto take = size_t( std::min<uint64_t>( limit, total - offset ) );
    result.reserve( take );
    for( size_t index = 0; index < take; ++index )
    {
        const auto ordinal = offset + index;
        StoredGhostZone record {};
        m_impl->records.Read( ordinal, &record, 1 );
        result.emplace_back( m_impl->Read( ordinal, record,
            record.startNs < endNs && record.endNs > startNs ) );
    }
    return result;
}

analysis::PagedRecordReader::ReadStats QueryGhostZoneStoreReader::ReadStatistics() const
{
    std::lock_guard lock( m_impl->mutex );
    return m_impl->records.Statistics();
}

void QueryGhostZoneStoreReader::ValidateAll()
{
    std::lock_guard lock( m_impl->mutex );
    uint64_t ordinal = 0;
    m_impl->records.VisitValidated( [&]( const void* bytes, uint64_t count ) {
        const auto* records = static_cast<const StoredGhostZone*>( bytes );
        for( uint64_t i = 0; i < count; ++i ) m_impl->CheckRecord( ordinal++, records[i] );
    } );
}

}
