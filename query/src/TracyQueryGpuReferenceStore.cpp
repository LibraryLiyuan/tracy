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
#include <future>
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

constexpr uint32_t ReferenceUsePartitionCount = 32;
constexpr size_t ReferenceUseWriteBufferRecords = 4096;
constexpr uint64_t ReferenceUseTargetPartitionRecords = 4ull * 1024 * 1024;

#pragma pack( push, 1 )
struct QueryGpuReferencePartitionUse
{
    uint64_t resourceId = 0;
    uint32_t passOrdinal = 0;
    uint32_t sourceUseOrdinal = 0;
    uint32_t usageMask = 0;
    uint32_t resourceSetId = 0;
    uint8_t flags = 0;
    uint8_t encoding = 0;
    uint32_t resourceOrdinal = std::numeric_limits<uint32_t>::max();
};
#pragma pack( pop )

static_assert( sizeof( QueryGpuReferencePartitionUse ) == 30 );

#pragma pack( push, 1 )
struct QueryGpuReferenceLookupPartitionFact
{
    uint32_t resourceOrdinal = 0;
    uint32_t sourceUseOrdinal = 0;
    uint32_t resolvedUseOrdinal = 0;
};
#pragma pack( pop )

static_assert( sizeof( QueryGpuReferenceLookupPartitionFact ) == 12 );

std::string Identity( std::string_view source, std::string_view kind )
{
    analysis::Sha256Builder hash;
    hash.Update( source.data(), source.size() );
    hash.Update( kind.data(), kind.size() );
    constexpr std::string_view version = "query-gpu-reference-v1";
    hash.Update( version.data(), version.size() );
    return hash.FinalHex();
}

std::string UsePartitionIdentity( std::string_view source, uint32_t partition )
{
    return Identity( source, "use-partition-" + std::to_string( partition ) );
}

std::string LookupPartitionIdentity(
    std::string_view source, std::string_view kind, uint32_t partition )
{
    return Identity( source, std::string( kind ) + "-" + std::to_string( partition ) );
}

uint64_t MixReferenceId( uint64_t value )
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

std::filesystem::path ResourceLookupPath( const std::filesystem::path& root ) { return root / "by-resource"; }
std::filesystem::path CatalogResourceLookupPath( const std::filesystem::path& root ) { return root / "by-catalog-resource"; }
std::filesystem::path PassTimesPath( const std::filesystem::path& root ) { return root / "pass-times"; }
std::filesystem::path ResolvedUsesPath( const std::filesystem::path& root ) { return root / "resolved-uses"; }

class ValidationSortedPairs
{
public:
    ValidationSortedPairs( const std::filesystem::path& path,
        const std::shared_ptr<analysis::AnalysisWorkspaceBudget>& workspace )
        : m_memory( workspace, sizeof( analysis::AnalysisUInt64Pair ) * 4096 )
        , m_rows( 4096 )
        , m_input( analysis::AnalysisIoPath( path ), std::ios::binary )
    {
        if( !m_input ) throw std::runtime_error( "GPU reference validation sort output cannot be opened" );
    }

    bool Next( analysis::AnalysisUInt64Pair& row )
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
    std::vector<analysis::AnalysisUInt64Pair> m_rows;
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
    if( input.UseCount() > std::numeric_limits<uint32_t>::max() )
        throw std::runtime_error( "GPU reference compact lookup exceeds 32-bit Use ordinal capacity" );
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
    const auto usePartitions = scratch / "use-partitions";
    std::filesystem::create_directories( analysis::AnalysisIoPath( usePartitions ) );
    const auto passEventsSource = scratch / "pass-events-source.work";
    const auto passEventsSorted = scratch / "pass-events-sorted.work";
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

        analysis::PagedRecordReader passTimeReader( passTimesOutput,
            Identity( sourceFingerprint, "pass-times" ), sizeof( QueryGpuReferencePassTimeRecord ), options );
        if( passTimeReader.Count() > std::numeric_limits<uint32_t>::max() ||
            passTimeReader.Count() > SIZE_MAX / sizeof( QueryGpuReferencePassTimeRecord ) )
            throw std::runtime_error( "GPU reference Pass ordinal exceeds 32-bit capacity" );
        analysis::AnalysisWorkspaceReservation passTimeMemory( options.workspace,
            passTimeReader.Count() * sizeof( QueryGpuReferencePassTimeRecord ) );
        std::vector<QueryGpuReferencePassTimeRecord> passTimeRows( size_t( passTimeReader.Count() ) );
        for( uint64_t ordinal = 0; ordinal < passTimeReader.Count(); ordinal += 4096 )
            passTimeReader.Read( ordinal, passTimeRows.data() + size_t( ordinal ),
                std::min<uint64_t>( 4096, passTimeReader.Count() - ordinal ) );
        struct PassOrdinalEntry
        {
            uint64_t passId = 0;
            uint32_t ordinal = 0;
            uint32_t reserved = 0;
        };
        size_t passHashCapacity = 1;
        while( passHashCapacity < passTimeRows.size() * 2 )
        {
            if( passHashCapacity > std::numeric_limits<size_t>::max() / 2 )
                throw std::runtime_error( "GPU reference Pass hash exceeds address space" );
            passHashCapacity *= 2;
        }
        analysis::AnalysisWorkspaceReservation passHashMemory( options.workspace,
            uint64_t( passHashCapacity ) * sizeof( PassOrdinalEntry ) );
        std::vector<PassOrdinalEntry> passHash( passHashCapacity );
        for( uint32_t ordinal = 0; ordinal < passTimeRows.size(); ordinal++ )
        {
            const auto passId = passTimeRows[ordinal].passId;
            const auto mask = passHash.size() - 1;
            auto slot = size_t( MixReferenceId( passId ) ) & mask;
            while( passHash[slot].passId != 0 )
            {
                if( passHash[slot].passId == passId )
                    throw std::runtime_error( "GPU reference Pass hash contains a duplicate" );
                slot = ( slot + 1 ) & mask;
            }
            passHash[slot] = { passId, ordinal, 0 };
        }
        const auto findPassOrdinal = [&]( uint64_t passId ) -> std::optional<uint32_t> {
            if( passId == 0 ) return std::nullopt;
            const auto mask = passHash.size() - 1;
            auto slot = size_t( MixReferenceId( passId ) ) & mask;
            while( passHash[slot].passId != 0 )
            {
                if( passHash[slot].passId == passId ) return passHash[slot].ordinal;
                slot = ( slot + 1 ) & mask;
            }
            return std::nullopt;
        };

        const auto partitionsForUses = std::max<uint64_t>( 1,
            ( input.UseCount() + ReferenceUseTargetPartitionRecords - 1 ) /
                ReferenceUseTargetPartitionRecords );
        const auto partitionCount = uint32_t( std::max<uint64_t>( 1,
            std::min<uint64_t>( { ReferenceUsePartitionCount, partitionsForUses,
                std::max<uint64_t>( 1, passTimeRows.size() ) } ) ) );
        auto partitionOptions = options;
        partitionOptions.blockBytes = 1024 * 1024;
        std::array<std::unique_ptr<analysis::PagedRecordWriter>, ReferenceUsePartitionCount> partitionWriters;
        std::array<std::vector<QueryGpuReferencePartitionUse>, ReferenceUsePartitionCount> partitionBuffers;
        analysis::AnalysisWorkspaceReservation partitionBufferMemory( options.workspace,
            uint64_t( partitionCount ) * ReferenceUseWriteBufferRecords * sizeof( QueryGpuReferencePartitionUse ) );
        for( uint32_t partition = 0; partition < partitionCount; partition++ )
        {
            partitionBuffers[partition].reserve( ReferenceUseWriteBufferRecords );
            partitionWriters[partition] = std::make_unique<analysis::PagedRecordWriter>(
                usePartitions / std::to_string( partition ),
                UsePartitionIdentity( sourceFingerprint, partition ),
                uint32_t( sizeof( QueryGpuReferencePartitionUse ) ), partitionOptions );
        }
        const auto flushPartition = [&]( uint32_t partition ) {
            auto& values = partitionBuffers[partition];
            if( values.empty() ) return;
            partitionWriters[partition]->Append( values.data(), values.size() );
            values.clear();
        };
        uint64_t partitionRecords = 0;
        bool referenceOrdinalsComplete = input.ReferenceResourceOrdinalCount() != 0 &&
            input.ReferenceResourceOrdinalCount() <= std::numeric_limits<uint32_t>::max();
        for( uint64_t ordinal = 0; ordinal < input.UseCount(); ordinal++ )
        {
            if( ( ordinal & 4095 ) == 0 && options.cancelled && options.cancelled() )
                throw std::runtime_error( "GPU reference index cancelled" );
            const auto use = input.Use( ordinal );
            if( use.passId == 0 || use.resourceId == 0 ) continue;
            const auto passOrdinal = findPassOrdinal( use.passId );
            if( !passOrdinal ) continue;
            const auto resourceOrdinal = referenceOrdinalsComplete ?
                input.FindReferenceResourceOrdinal( use.resourceId ) : std::nullopt;
            if( referenceOrdinalsComplete && !resourceOrdinal ) referenceOrdinalsComplete = false;
            const auto partition = passTimeRows.empty() ? 0 : uint32_t( std::min<uint64_t>(
                partitionCount - 1, uint64_t( *passOrdinal ) * partitionCount / passTimeRows.size() ) );
            partitionBuffers[partition].push_back( { use.resourceId, *passOrdinal,
                uint32_t( ordinal ), use.usageMask, use.resourceSetId, use.flags, use.encoding,
                resourceOrdinal.value_or( std::numeric_limits<uint32_t>::max() ) } );
            if( partitionBuffers[partition].size() == ReferenceUseWriteBufferRecords )
                flushPartition( partition );
            partitionRecords++;
        }
        for( uint32_t partition = 0; partition < partitionCount; partition++ )
        {
            flushPartition( partition );
            partitionWriters[partition]->Commit();
            partitionWriters[partition].reset();
            std::vector<QueryGpuReferencePartitionUse>().swap( partitionBuffers[partition] );
        }
        partitionBufferMemory.Resize( 0 );
        std::vector<PassOrdinalEntry>().swap( passHash );
        passHashMemory.Resize( 0 );
        if( statistics ) statistics->usePartitionRecords = partitionRecords;
        finishPhase( &QueryGpuReferenceBuildStatistics::usePartitionWriteMs );

        analysis::PagedRecordWriter resolvedUses( usesOutput,
            Identity( sourceFingerprint, "resolved-uses" ), sizeof( QueryGpuReferenceResolvedUseRecord ), options );
        analysis::AnalysisWorkspaceReservation resolvedBufferMemory( options.workspace,
            ReferenceUseWriteBufferRecords * sizeof( QueryGpuReferenceResolvedUseRecord ) );
        std::vector<QueryGpuReferenceResolvedUseRecord> resolvedBuffer;
        resolvedBuffer.reserve( ReferenceUseWriteBufferRecords );
        const auto flushResolved = [&] {
            if( resolvedBuffer.empty() ) return;
            resolvedUses.Append( resolvedBuffer.data(), resolvedBuffer.size() );
            resolvedBuffer.clear();
        };
        const auto referenceResourceCount = input.ReferenceResourceOrdinalCount();
        const auto catalogResourceCount = input.CatalogResourceOrdinalCount();
        const bool ordinalLookups = referenceOrdinalsComplete &&
            referenceResourceCount != 0 && catalogResourceCount != 0 &&
            referenceResourceCount <= std::numeric_limits<uint32_t>::max() &&
            catalogResourceCount <= std::numeric_limits<uint32_t>::max();
        std::unique_ptr<analysis::AnalysisUInt64PairRunCollector> resourceRuns, catalogRuns;
        if( !ordinalLookups )
        {
            resourceRuns = std::make_unique<analysis::AnalysisUInt64PairRunCollector>(
                resourceSorted, scratch, "gpu-reference-uses-by-resource",
                analysis::ChooseAnalysisExternalSortBufferRecords( input.UseCount(),
                    sizeof( QueryGpuReferenceLookupRecord ), options.workspace ),
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->resourceSort : nullptr );
            catalogRuns = std::make_unique<analysis::AnalysisUInt64PairRunCollector>(
                catalogResourceSorted, scratch, "gpu-reference-uses-by-catalog-resource",
                analysis::ChooseAnalysisExternalSortBufferRecords( input.UseCount(),
                    sizeof( QueryGpuReferenceLookupRecord ), options.workspace ),
                options.disk, options.workspace, options.cancelled,
                statistics ? &statistics->catalogSort : nullptr );
        }
        const auto lookupPartitions = scratch / "lookup-partitions";
        const auto resourcePartitions = lookupPartitions / "resource";
        const auto catalogPartitions = lookupPartitions / "catalog";
        const auto lookupPartitionCount = uint32_t( std::max<uint64_t>( 1,
            std::min<uint64_t>( ReferenceUsePartitionCount,
                ( input.UseCount() + ReferenceUseTargetPartitionRecords - 1 ) /
                    ReferenceUseTargetPartitionRecords ) ) );
        std::array<std::unique_ptr<analysis::PagedRecordWriter>, ReferenceUsePartitionCount> resourceWriters;
        std::array<std::unique_ptr<analysis::PagedRecordWriter>, ReferenceUsePartitionCount> catalogWriters;
        std::array<std::vector<QueryGpuReferenceLookupPartitionFact>, ReferenceUsePartitionCount> resourceBuffers;
        std::array<std::vector<QueryGpuReferenceLookupPartitionFact>, ReferenceUsePartitionCount> catalogBuffers;
        analysis::AnalysisWorkspaceReservation lookupBufferMemory( options.workspace );
        if( ordinalLookups )
        {
            std::filesystem::create_directories( analysis::AnalysisIoPath( resourcePartitions ) );
            std::filesystem::create_directories( analysis::AnalysisIoPath( catalogPartitions ) );
            lookupBufferMemory.Resize( uint64_t( lookupPartitionCount ) * 2 *
                ReferenceUseWriteBufferRecords * sizeof( QueryGpuReferenceLookupPartitionFact ) );
            for( uint32_t partition = 0; partition < lookupPartitionCount; partition++ )
            {
                resourceBuffers[partition].reserve( ReferenceUseWriteBufferRecords );
                catalogBuffers[partition].reserve( ReferenceUseWriteBufferRecords );
                resourceWriters[partition] = std::make_unique<analysis::PagedRecordWriter>(
                    resourcePartitions / std::to_string( partition ),
                    LookupPartitionIdentity( sourceFingerprint, "resource-lookup", partition ),
                    uint32_t( sizeof( QueryGpuReferenceLookupPartitionFact ) ), partitionOptions );
                catalogWriters[partition] = std::make_unique<analysis::PagedRecordWriter>(
                    catalogPartitions / std::to_string( partition ),
                    LookupPartitionIdentity( sourceFingerprint, "catalog-lookup", partition ),
                    uint32_t( sizeof( QueryGpuReferenceLookupPartitionFact ) ), partitionOptions );
            }
        }
        const auto appendLookupFact = [&]( bool catalog, QueryGpuReferenceLookupPartitionFact fact,
            uint64_t dictionaryCount ) {
            const auto partition = uint32_t( std::min<uint64_t>( lookupPartitionCount - 1,
                uint64_t( fact.resourceOrdinal ) * lookupPartitionCount / dictionaryCount ) );
            auto& buffer = catalog ? catalogBuffers[partition] : resourceBuffers[partition];
            buffer.emplace_back( fact );
            if( buffer.size() == ReferenceUseWriteBufferRecords )
            {
                auto& writer = catalog ? catalogWriters[partition] : resourceWriters[partition];
                writer->Append( buffer.data(), buffer.size() );
                buffer.clear();
            }
        };
        uint64_t resolvedOrdinal = 0;
        uint64_t resourceLookupCount = 0;
        uint64_t catalogResourceLookupCount = 0;
        struct LoadedUsePartition
        {
            explicit LoadedUsePartition( std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace )
                : memory( std::move( workspace ) ) {}
            analysis::AnalysisWorkspaceReservation memory;
            std::vector<QueryGpuReferencePartitionUse> uses;
            uint64_t readMs = 0;
            uint64_t sortMs = 0;
        };
        const auto loadUsePartition = [&]( uint32_t partition ) {
            LoadedUsePartition result( options.workspace );
            const auto path = usePartitions / std::to_string( partition );
            auto started = std::chrono::steady_clock::now();
            {
                analysis::PagedRecordReader reader( path,
                    UsePartitionIdentity( sourceFingerprint, partition ),
                    sizeof( QueryGpuReferencePartitionUse ), partitionOptions );
                if( reader.Count() > SIZE_MAX / sizeof( QueryGpuReferencePartitionUse ) )
                    throw std::runtime_error( "GPU reference Use partition exceeds address space" );
                result.memory.Resize( reader.Count() * sizeof( QueryGpuReferencePartitionUse ) );
                result.uses.resize( size_t( reader.Count() ) );
                if( !result.uses.empty() ) reader.Read( 0, result.uses.data(), result.uses.size() );
            }
            result.readMs = uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started ).count() );
            started = std::chrono::steady_clock::now();
            std::sort( result.uses.begin(), result.uses.end(), []( const auto& lhs, const auto& rhs ) {
                if( lhs.passOrdinal != rhs.passOrdinal ) return lhs.passOrdinal < rhs.passOrdinal;
                return lhs.sourceUseOrdinal < rhs.sourceUseOrdinal;
            } );
            result.sortMs = uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started ).count() );
            return result;
        };
        std::future<LoadedUsePartition> prefetched;
        if( partitionCount > 1 )
            prefetched = std::async( std::launch::async, loadUsePartition, 0 );
        for( uint32_t partition = 0; partition < partitionCount; partition++ )
        {
            const auto path = usePartitions / std::to_string( partition );
            auto loaded = partitionCount == 1 ? loadUsePartition( partition ) : prefetched.get();
            if( partition + 1 < partitionCount )
                prefetched = std::async( std::launch::async, loadUsePartition, partition + 1 );
            if( statistics )
            {
                statistics->usePartitionReadMs += loaded.readMs;
                statistics->usePartitionSortMs += loaded.sortMs;
            }
            auto& uses = loaded.uses;
            auto substageStarted = std::chrono::steady_clock::now();
            for( const auto& use : uses )
            {
                if( ( resolvedOrdinal & 4095 ) == 0 && options.cancelled && options.cancelled() )
                    throw std::runtime_error( "GPU reference index cancelled" );
                if( use.passOrdinal >= passTimeRows.size() )
                    throw std::runtime_error( "GPU reference Use partition Pass ordinal mismatch" );
                const auto& passTime = passTimeRows[use.passOrdinal];
                QueryGpuReferenceResolvedUseRecord resolved {};
                resolved.resourceId = use.resourceId;
                resolved.passId = passTime.passId;
                resolved.passStart = passTime.start;
                resolved.passEnd = passTime.end;
                resolved.usageMask = use.usageMask;
                resolved.resourceSetId = use.resourceSetId;
                resolved.flags = use.flags;
                resolved.encoding = use.encoding;
                resolvedBuffer.emplace_back( resolved );
                if( resolvedBuffer.size() == ReferenceUseWriteBufferRecords ) flushResolved();
                if( resolvedOrdinal > std::numeric_limits<uint32_t>::max() )
                    throw std::runtime_error( "GPU reference compact lookup ordinal overflow" );
                auto lookup = QueryGpuReferenceLookupRecord::Make( resolved.resourceId,
                    use.sourceUseOrdinal, uint32_t( resolvedOrdinal++ ) );
                if( ordinalLookups )
                {
                    if( use.resourceOrdinal >= referenceResourceCount )
                        throw std::runtime_error( "GPU reference Resource ordinal contract changed" );
                    appendLookupFact( false, { use.resourceOrdinal, use.sourceUseOrdinal,
                        lookup.ResolvedUseOrdinal() }, referenceResourceCount );
                }
                else resourceRuns->Emit( { lookup.resourceId, lookup.packedOrdinals } );
                resourceLookupCount++;
                if( ordinalLookups )
                {
                    const auto catalogOrdinal = input.ResolveCatalogResourceOrdinal(
                        resolved.resourceId, resolved.passEnd );
                    if( catalogOrdinal )
                    {
                        if( *catalogOrdinal >= catalogResourceCount )
                            throw std::runtime_error( "GPU reference Catalog Resource ordinal contract changed" );
                        appendLookupFact( true, { *catalogOrdinal, use.sourceUseOrdinal,
                            lookup.ResolvedUseOrdinal() }, catalogResourceCount );
                        catalogResourceLookupCount++;
                    }
                }
                else
                {
                    const auto catalogResourceId = input.ResolveCatalogResource(
                        resolved.resourceId, resolved.passEnd );
                    if( catalogResourceId != 0 )
                    {
                        lookup.resourceId = catalogResourceId;
                        catalogRuns->Emit( { lookup.resourceId, lookup.packedOrdinals } );
                        catalogResourceLookupCount++;
                    }
                }
            }
            if( statistics ) statistics->useResolveEmitMs += uint64_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - substageStarted ).count() );
            std::vector<QueryGpuReferencePartitionUse>().swap( uses );
            loaded.memory.Resize( 0 );
            std::error_code removeError;
            analysis::AnalysisDiskRemove( path, options.disk, removeError, true );
            if( removeError ) throw std::runtime_error( "GPU reference Use partition cleanup failed: " +
                removeError.message() );
        }
        flushResolved();
        resolvedUses.Commit();
        std::vector<QueryGpuReferenceResolvedUseRecord>().swap( resolvedBuffer );
        resolvedBufferMemory.Resize( 0 );
        if( ordinalLookups )
        {
            for( uint32_t partition = 0; partition < lookupPartitionCount; partition++ )
            {
                if( !resourceBuffers[partition].empty() )
                    resourceWriters[partition]->Append( resourceBuffers[partition].data(),
                        resourceBuffers[partition].size() );
                if( !catalogBuffers[partition].empty() )
                    catalogWriters[partition]->Append( catalogBuffers[partition].data(),
                        catalogBuffers[partition].size() );
                resourceWriters[partition]->Commit();
                catalogWriters[partition]->Commit();
                resourceWriters[partition].reset();
                catalogWriters[partition].reset();
                std::vector<QueryGpuReferenceLookupPartitionFact>().swap( resourceBuffers[partition] );
                std::vector<QueryGpuReferenceLookupPartitionFact>().swap( catalogBuffers[partition] );
            }
            lookupBufferMemory.Resize( 0 );
            if( statistics ) statistics->lookupPartitionRecords =
                resourceLookupCount + catalogResourceLookupCount;
        }
        std::vector<QueryGpuReferencePassTimeRecord>().swap( passTimeRows );
        passTimeMemory.Resize( 0 );
        finishPhase( &QueryGpuReferenceBuildStatistics::resolveUsesMs );
        if( ordinalLookups )
        {
            const auto publishPartitions = [&]( bool catalog, const std::filesystem::path& partitionRoot,
                const std::filesystem::path& output, std::string_view identityKind,
                uint64_t dictionaryCount, uint64_t expectedCount ) {
                analysis::PagedRecordWriter writer( output, Identity( sourceFingerprint, identityKind ),
                    sizeof( analysis::AnalysisKeyMetadataRecord ), options );
                std::array<analysis::AnalysisKeyMetadataRecord, 4096> outputBuffer;
                uint64_t written = 0;
                uint32_t activeResourceOrdinal = std::numeric_limits<uint32_t>::max();
                uint64_t activeResourceId = 0;
                for( uint32_t partition = 0; partition < lookupPartitionCount; partition++ )
                {
                    if( options.cancelled && options.cancelled() )
                        throw std::runtime_error( "GPU reference index cancelled" );
                    const auto path = partitionRoot / std::to_string( partition );
                    std::vector<QueryGpuReferenceLookupPartitionFact> facts;
                    analysis::AnalysisWorkspaceReservation factMemory( options.workspace );
                    {
                        analysis::PagedRecordReader reader( path,
                            LookupPartitionIdentity( sourceFingerprint,
                                catalog ? "catalog-lookup" : "resource-lookup", partition ),
                            sizeof( QueryGpuReferenceLookupPartitionFact ), partitionOptions );
                        if( reader.Count() > SIZE_MAX / sizeof( QueryGpuReferenceLookupPartitionFact ) )
                            throw std::runtime_error( "GPU reference lookup partition exceeds address space" );
                        factMemory.Resize( reader.Count() * sizeof( QueryGpuReferenceLookupPartitionFact ) );
                        facts.resize( size_t( reader.Count() ) );
                        if( !facts.empty() ) reader.Read( 0, facts.data(), facts.size() );
                    }
                    std::sort( facts.begin(), facts.end(), []( const auto& lhs, const auto& rhs ) {
                        if( lhs.resourceOrdinal != rhs.resourceOrdinal )
                            return lhs.resourceOrdinal < rhs.resourceOrdinal;
                        if( lhs.sourceUseOrdinal != rhs.sourceUseOrdinal )
                            return lhs.sourceUseOrdinal < rhs.sourceUseOrdinal;
                        return lhs.resolvedUseOrdinal < rhs.resolvedUseOrdinal;
                    } );
                    for( size_t begin = 0; begin < facts.size(); )
                    {
                        const auto count = std::min( outputBuffer.size(), facts.size() - begin );
                        for( size_t index = 0; index < count; index++ )
                        {
                            const auto& fact = facts[begin + index];
                            if( fact.resourceOrdinal >= dictionaryCount )
                                throw std::runtime_error( "GPU reference lookup partition Resource ordinal mismatch" );
                            if( fact.resourceOrdinal != activeResourceOrdinal )
                            {
                                activeResourceOrdinal = fact.resourceOrdinal;
                                activeResourceId = catalog ?
                                    input.CatalogResourceIdAtOrdinal( fact.resourceOrdinal ) :
                                    input.ReferenceResourceIdAtOrdinal( fact.resourceOrdinal );
                            }
                            auto& row = outputBuffer[index];
                            row = {};
                            row.keyHigh = activeResourceId;
                            if( row.keyHigh == 0 )
                                throw std::runtime_error( "GPU reference lookup partition Resource identity mismatch" );
                            row.keyLow = fact.sourceUseOrdinal;
                            row.metadata = fact.resolvedUseOrdinal;
                        }
                        writer.Append( outputBuffer.data(), count );
                        written += count;
                        begin += count;
                    }
                    std::vector<QueryGpuReferenceLookupPartitionFact>().swap( facts );
                    factMemory.Resize( 0 );
                    std::error_code removeError;
                    analysis::AnalysisDiskRemove( path, options.disk, removeError, true );
                    if( removeError ) throw std::runtime_error(
                        "GPU reference lookup partition cleanup failed: " + removeError.message() );
                }
                if( written != expectedCount )
                    throw std::runtime_error( "GPU reference lookup partition count mismatch" );
                writer.Commit();
            };
            auto resourcePublish = std::async( std::launch::async, [&] {
                publishPartitions( false, resourcePartitions, lookupOutput, "by-resource",
                    referenceResourceCount, resourceLookupCount );
            } );
            publishPartitions( true, catalogPartitions, catalogLookupOutput, "by-catalog-resource",
                catalogResourceCount, catalogResourceLookupCount );
            resourcePublish.get();
            const auto cleanupError = cleanup( false );
            if( !cleanupError.empty() )
                throw std::runtime_error( "GPU reference scratch cleanup failed: " + cleanupError );
            finishPhase( &QueryGpuReferenceBuildStatistics::lookupPartitionPublishMs );
            return;
        }
        if( !resourceRuns->Finish( sortError ) )
            throw std::runtime_error( "GPU reference resource lookup sort failed: " + sortError );
        finishPhase( &QueryGpuReferenceBuildStatistics::resourceSortMs );
        std::array<QueryGpuReferenceLookupRecord, 4096> compactBuffer;
        std::array<analysis::AnalysisKeyMetadataRecord, 4096> publishBuffer;
        const auto expandLookup = [&]( size_t count ) {
            for( size_t index = 0; index < count; index++ )
            {
                publishBuffer[index] = {};
                publishBuffer[index].keyHigh = compactBuffer[index].resourceId;
                publishBuffer[index].keyLow = compactBuffer[index].SourceUseOrdinal();
                publishBuffer[index].metadata = compactBuffer[index].ResolvedUseOrdinal();
            }
        };
        {
            analysis::PagedRecordWriter writer( lookupOutput,
                Identity( sourceFingerprint, "by-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            std::ifstream sorted( analysis::AnalysisIoPath( resourceSorted ), std::ios::binary );
            if( !sorted ) throw std::runtime_error( "GPU reference sorted resource lookup cannot be opened" );
            uint64_t resourceRowsWritten = 0;
            while( sorted )
            {
                sorted.read( reinterpret_cast<char*>( compactBuffer.data() ), std::streamsize( sizeof( compactBuffer ) ) );
                const auto bytes = size_t( sorted.gcount() );
                if( bytes % sizeof( compactBuffer[0] ) != 0 ) throw std::runtime_error( "GPU reference lookup is truncated" );
                if( bytes != 0 )
                {
                    const auto count = bytes / sizeof( compactBuffer[0] );
                    expandLookup( count );
                    writer.Append( publishBuffer.data(), count );
                    resourceRowsWritten += count;
                }
            }
            if( !sorted.eof() ) throw std::runtime_error( "GPU reference resource lookup read failed" );
            if( resourceRowsWritten != resourceLookupCount )
                throw std::runtime_error( "GPU reference sorted resource lookup count changed" );
            writer.Commit();
        }
        removeConsumed( resourceSorted );
        finishPhase( &QueryGpuReferenceBuildStatistics::resourcePublishMs );

        if( !catalogRuns->Finish( sortError ) )
            throw std::runtime_error( "GPU reference Catalog resource lookup sort failed: " + sortError );
        finishPhase( &QueryGpuReferenceBuildStatistics::catalogSortMs );
        analysis::PagedRecordWriter catalogLookupWriter( catalogLookupOutput,
            Identity( sourceFingerprint, "by-catalog-resource" ), sizeof( analysis::AnalysisKeyMetadataRecord ), options );
        std::ifstream sortedCatalogResources( analysis::AnalysisIoPath( catalogResourceSorted ), std::ios::binary );
        if( !sortedCatalogResources )
            throw std::runtime_error( "GPU reference sorted Catalog resource lookup cannot be opened" );
        uint64_t catalogRowsWritten = 0;
        while( sortedCatalogResources )
        {
            sortedCatalogResources.read( reinterpret_cast<char*>( compactBuffer.data() ),
                std::streamsize( sizeof( compactBuffer ) ) );
            const auto bytes = size_t( sortedCatalogResources.gcount() );
            if( bytes % sizeof( compactBuffer[0] ) != 0 )
                throw std::runtime_error( "GPU reference Catalog lookup is truncated" );
            if( bytes != 0 )
            {
                const auto count = bytes / sizeof( compactBuffer[0] );
                expandLookup( count );
                catalogLookupWriter.Append( publishBuffer.data(), count );
                catalogRowsWritten += count;
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
    QueryGpuReferenceValidationStats result;
    const auto validationStarted = std::chrono::steady_clock::now();
    auto phaseStarted = validationStarted;
    const auto finishPhase = [&]( uint64_t QueryGpuReferenceValidationStats::*field ) {
        const auto now = std::chrono::steady_clock::now();
        result.*field = uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
            now - phaseStarted ).count() );
        phaseStarted = now;
    };
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
        finishPhase( &QueryGpuReferenceValidationStats::passAndUseValidationMs );

        const auto sortByUseOrdinal = [&]( analysis::PagedRecordReader& source,
            const std::filesystem::path& sorted, const char* prefix, const char* orderError,
            analysis::AnalysisExternalSortStatistics& statistics ) {
            std::string error;
            const auto count = source.Count();
            if( !analysis::SortAnalysisUInt64PairsGenerated( sorted, scratch, prefix,
                    analysis::ChooseAnalysisExternalSortBufferRecords( count,
                        sizeof( analysis::AnalysisUInt64Pair ), m_impl->options.workspace ),
                    [&]( const auto& emit ) {
                        Record previous {};
                        uint64_t ordinal = 0;
                        source.VisitValidated( [&]( const void* data, uint64_t blockCount ) {
                            const auto* bytes = static_cast<const char*>( data );
                            for( uint64_t index = 0; index < blockCount; index++, ordinal++ )
                            {
                                Record row {};
                                std::memcpy( &row, bytes + index * sizeof( row ), sizeof( row ) );
                                if( row.keyHigh == 0 || row.metadata >= m_impl->uses.Count() ||
                                    ( ordinal != 0 && ( row.keyHigh < previous.keyHigh ||
                                        ( row.keyHigh == previous.keyHigh && ( row.keyLow < previous.keyLow ||
                                            ( row.keyLow == previous.keyLow &&
                                                row.metadata < previous.metadata ) ) ) ) ) )
                                    throw std::runtime_error( orderError );
                                emit( analysis::AnalysisUInt64Pair { row.metadata, row.keyHigh } );
                                previous = row;
                            }
                        } );
                    }, error, m_impl->options.disk, m_impl->options.workspace,
                    m_impl->options.cancelled, &statistics ) )
                throw std::runtime_error( "GPU reference validation sort failed: " + error );
        };

        const auto resourceSorted = scratch / "resource-sorted.work";
        finishPhase( &QueryGpuReferenceValidationStats::resourceSourceMs );
        sortByUseOrdinal( m_impl->lookup, resourceSorted, "gpu-reference-validation-resource",
            "GPU reference resource lookup order mismatch", result.resourceSort );
        finishPhase( &QueryGpuReferenceValidationStats::resourceSortMs );
        {
            ValidationSortedPairs sorted( resourceSorted, m_impl->options.workspace );
            uint64_t ordinal = 0;
            m_impl->uses.VisitValidated( [&]( const void* data, uint64_t count ) {
                const auto* bytes = static_cast<const char*>( data );
                for( uint64_t index = 0; index < count; index++, ordinal++ )
                {
                    if( ( ordinal & 4095 ) == 0 && m_impl->options.cancelled && m_impl->options.cancelled() )
                        throw std::runtime_error( "GPU reference validation cancelled" );
                    analysis::AnalysisUInt64Pair lookup {};
                    if( !sorted.Next( lookup ) || lookup.key != ordinal )
                        throw std::runtime_error( "GPU reference resource lookup ordinal mismatch" );
                    QueryGpuReferenceResolvedUseRecord use {};
                    std::memcpy( &use, bytes + index * sizeof( use ), sizeof( use ) );
                    if( lookup.value != use.resourceId )
                        throw std::runtime_error( "GPU reference resource lookup identity mismatch" );
                }
            } );
            analysis::AnalysisUInt64Pair extra {};
            if( sorted.Next( extra ) ) throw std::runtime_error( "GPU reference resource lookup duplicate ordinal" );
        }
        removeFile( resourceSorted );
        finishPhase( &QueryGpuReferenceValidationStats::resourceCompareMs );

        const auto catalogSorted = scratch / "catalog-sorted.work";
        finishPhase( &QueryGpuReferenceValidationStats::catalogSourceMs );
        sortByUseOrdinal( m_impl->catalogLookup, catalogSorted, "gpu-reference-validation-catalog",
            "GPU reference Catalog resource lookup order mismatch", result.catalogSort );
        finishPhase( &QueryGpuReferenceValidationStats::catalogSortMs );
        {
            ValidationSortedPairs sorted( catalogSorted, m_impl->options.workspace );
            analysis::AnalysisUInt64Pair next {};
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
                    if( haveNext && next.key < ordinal )
                        throw std::runtime_error( "GPU reference Catalog lookup ordinal mismatch" );
                    if( haveNext && next.key == ordinal )
                    {
                        if( next.value == 0 || ( resolveCatalogResource && next.value != expected ) )
                            throw std::runtime_error( "GPU reference Catalog resource lookup identity mismatch" );
                        haveNext = sorted.Next( next );
                        if( haveNext && next.key == ordinal )
                            throw std::runtime_error( "GPU reference Catalog lookup duplicate ordinal" );
                    }
                    else if( resolveCatalogResource && expected != 0 )
                        throw std::runtime_error( "GPU reference Catalog lookup missing ordinal" );
                }
            } );
            if( haveNext ) throw std::runtime_error( "GPU reference Catalog lookup ordinal out of range" );
        }
        removeFile( catalogSorted );
        finishPhase( &QueryGpuReferenceValidationStats::catalogCompareMs );
        cleanup();
        finishPhase( &QueryGpuReferenceValidationStats::cleanupMs );
        const auto after = m_impl->uses.Statistics();
        result.useBlockLoads = after.blockLoads - before.blockLoads;
        result.useDecodedBytes = after.decodedBytes - before.decodedBytes;
        result.sortedReferenceCount = m_impl->lookup.Count() + m_impl->catalogLookup.Count();
        result.totalMs = uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - validationStarted ).count() );
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
