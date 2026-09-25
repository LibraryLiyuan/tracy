#include "TracyAnalysisIoPath.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueryGpuPassResourceStore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace tracy;
using nlohmann::json;

namespace
{

constexpr uint32_t PartitionCount = 32;
constexpr size_t WriteBufferRecords = 4096;
constexpr uint32_t RangeBit = uint32_t( 1 ) << 31;

#pragma pack( push, 1 )
struct FusedFact
{
    uint32_t passOrdinal = 0;
    uint32_t resourceOrdinal = 0;
    uint32_t ordinalAndKind = 0;
};
#pragma pack( pop )

static_assert( sizeof( FusedFact ) == 12 );

uint64_t ParseCount( const char* text )
{
    uint64_t value = 0;
    const auto end = text + std::strlen( text );
    const auto parsed = std::from_chars( text, end, value );
    if( parsed.ec != std::errc() || parsed.ptr != end || value == 0 )
        throw std::runtime_error( "count must be a positive integer" );
    return value;
}

uint64_t Milliseconds( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

uint32_t Partition( uint32_t passOrdinal, uint64_t passCount )
{
    return uint32_t( std::min<uint64_t>( PartitionCount - 1,
        uint64_t( passOrdinal ) * PartitionCount / passCount ) );
}

std::string PartitionIdentity( uint32_t partition )
{
    std::string value( 64, 'a' );
    value[62] = "0123456789abcdef"[( partition >> 4 ) & 15];
    value[63] = "0123456789abcdef"[partition & 15];
    return value;
}

}

int main( int argc, char** argv )
{
    std::filesystem::path ownedRoot;
    bool ownsRoot = false;
    try
    {
        if( argc != 4 && argc != 5 )
            throw std::runtime_error( "usage: gpu-pass-fused-capacity USES RESOURCES ROOT [mixed|mixed-high|mixed-high-fanout]" );
        const auto useCount = ParseCount( argv[1] );
        const auto resourceCount = ParseCount( argv[2] );
        const auto distribution = argc == 5 ? std::string_view( argv[4] ) : std::string_view();
        const bool fanout = distribution == "mixed-high-fanout";
        const bool mixed = distribution == "mixed" || distribution == "mixed-high" || fanout;
        const bool highEntropy = distribution == "mixed-high" || fanout;
        if( argc == 5 && !mixed ) throw std::runtime_error( "unknown fused distribution" );
        const auto rangeCount = mixed ? useCount / 10 : 0;
        const auto basePassCount = ( useCount + 31 ) / 32;
        const auto extraPassCount = fanout ? ( basePassCount + 15 ) / 16 : 0;
        const auto passCount = basePassCount + extraPassCount;
        const auto maximumFactCount = ( useCount + rangeCount ) * ( fanout ? 2 : 1 );
        if( resourceCount > useCount || resourceCount > UINT32_MAX || passCount > UINT32_MAX ||
            maximumFactCount < useCount || maximumFactCount > UINT64_MAX / sizeof( FusedFact ) ||
            useCount >= RangeBit || rangeCount >= RangeBit )
            throw std::runtime_error( "invalid fused capacity scale" );
        const std::filesystem::path root = argv[3];
        if( std::filesystem::exists( root ) ) throw std::runtime_error( "capacity root already exists" );
        std::filesystem::create_directories( root / "partitions" );
        ownedRoot = root;
        ownsRoot = true;

        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 8ull * 1024 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            4ull * 1024 * 1024 * 1024, 2ull * 1024 * 1024 * 1024 );
        analysis::PagedRecordOptions options;
        options.disk = disk;
        options.workspace = workspace;
        options.blockBytes = 4 * 1024 * 1024;

        const auto started = std::chrono::steady_clock::now();
        options.blockBytes = 1024 * 1024;
        std::array<std::unique_ptr<analysis::PagedRecordWriter>, PartitionCount> partitionWriters;
        std::array<std::vector<FusedFact>, PartitionCount> buffers;
        analysis::AnalysisWorkspaceReservation writeMemory( workspace,
            PartitionCount * WriteBufferRecords * sizeof( FusedFact ) );
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            partitionWriters[partition] = std::make_unique<analysis::PagedRecordWriter>(
                root / "partitions" / std::to_string( partition ), PartitionIdentity( partition ),
                uint32_t( sizeof( FusedFact ) ), options );
            buffers[partition].reserve( WriteBufferRecords );
        }
        const auto flush = [&]( uint32_t partition ) {
            auto& values = buffers[partition];
            if( values.empty() ) return;
            partitionWriters[partition]->Append( values.data(), values.size() );
            values.clear();
        };
        uint64_t expandedUseCount = 0, expandedRangeCount = 0, expandedFactCount = 0;
        const auto appendFact = [&]( uint32_t passOrdinal, uint32_t resourceOrdinal,
            uint32_t ordinalAndKind ) {
            const auto partition = Partition( passOrdinal, passCount );
            buffers[partition].push_back( { passOrdinal, resourceOrdinal, ordinalAndKind } );
            if( buffers[partition].size() == WriteBufferRecords ) flush( partition );
            expandedFactCount++;
            ( ordinalAndKind & RangeBit ) != 0 ? expandedRangeCount++ : expandedUseCount++;
        };
        for( uint64_t ordinal = 0; ordinal < useCount; ordinal++ )
        {
            const auto evidencePassOrdinal = uint32_t( ordinal % basePassCount );
            const auto resourceOrdinal = highEntropy ? Mix( ordinal ^ 0x1020304050607080ull ) % resourceCount :
                ordinal % resourceCount;
            appendFact( evidencePassOrdinal, uint32_t( resourceOrdinal ), uint32_t( ordinal ) );
            if( fanout && evidencePassOrdinal % 16 == 0 )
                appendFact( uint32_t( basePassCount + evidencePassOrdinal / 16 ),
                    uint32_t( resourceOrdinal ), uint32_t( ordinal ) );
        }
        for( uint64_t ordinal = 0; ordinal < rangeCount; ordinal++ )
        {
            const auto evidencePassOrdinal = uint32_t( ordinal % basePassCount );
            const auto resourceOrdinal = highEntropy ? Mix( ordinal ^ 0x8877665544332211ull ) % resourceCount :
                ( ordinal * 17 ) % resourceCount;
            appendFact( evidencePassOrdinal, uint32_t( resourceOrdinal ), RangeBit | uint32_t( ordinal ) );
            if( fanout && evidencePassOrdinal % 16 == 0 )
                appendFact( uint32_t( basePassCount + evidencePassOrdinal / 16 ),
                    uint32_t( resourceOrdinal ), RangeBit | uint32_t( ordinal ) );
        }
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            flush( partition );
            partitionWriters[partition]->Commit();
            partitionWriters[partition].reset();
        }
        for( auto& values : buffers ) std::vector<FusedFact>().swap( values );
        writeMemory.Resize( 0 );
        const auto partitionMs = Milliseconds( started );
        const auto partitionBytes = usage->Current();

        const std::string summaryIdentity( 64, '7' );
        const std::string useIdentity( 64, '8' );
        const std::string rangeIdentity( 64, '9' );
        const std::string evidenceIdentity( 64, 'e' );
        std::string summaryHash, useHash, rangeHash, evidenceHash;
        uint64_t emittedSummaries = 0, emittedUses = 0, emittedRanges = 0;
        uint64_t summaryUseTotal = 0, summaryRangeTotal = 0, summaryAllocationTotal = 0;
        const auto processingStarted = std::chrono::steady_clock::now();
        {
            analysis::PagedRecordWriter summaries( root / "summaries", summaryIdentity,
                sizeof( query::QueryGpuPassResourceSummary ), options );
            analysis::PagedRecordWriter uses( root / "uses", useIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            analysis::PagedRecordWriter ranges( root / "ranges", rangeIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            analysis::PagedRecordWriter evidence( root / "evidence", evidenceIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            analysis::AnalysisWorkspaceReservation outputBufferMemory( workspace,
                2 * WriteBufferRecords * sizeof( analysis::AnalysisKeyMetadataRecord ) );
            std::vector<analysis::AnalysisKeyMetadataRecord> useBuffer;
            std::vector<analysis::AnalysisKeyMetadataRecord> rangeBuffer;
            useBuffer.reserve( WriteBufferRecords );
            rangeBuffer.reserve( WriteBufferRecords );
            for( uint32_t partition = 0; partition < PartitionCount; partition++ )
            {
                const auto path = root / "partitions" / std::to_string( partition );
                size_t count = 0;
                analysis::AnalysisWorkspaceReservation partitionMemory( workspace );
                std::vector<FusedFact> facts;
                {
                    analysis::PagedRecordReader input( path, PartitionIdentity( partition ),
                        sizeof( FusedFact ), options );
                    if( input.Count() > SIZE_MAX ) throw std::runtime_error( "fused partition count overflow" );
                    count = size_t( input.Count() );
                    partitionMemory.Resize( count * ( sizeof( FusedFact ) + sizeof( uint64_t ) ) );
                    facts.resize( count );
                    if( count != 0 ) input.Read( 0, facts.data(), count );
                }
                size_t comparisons = 0;
                std::sort( facts.begin(), facts.end(), [&]( const auto& lhs, const auto& rhs ) {
                    if( ( ++comparisons & 4095 ) == 0 &&
                        std::chrono::steady_clock::now() - started > std::chrono::minutes( 2 ) )
                        throw std::runtime_error( "fused capacity time limit" );
                    if( lhs.passOrdinal != rhs.passOrdinal ) return lhs.passOrdinal < rhs.passOrdinal;
                    if( lhs.resourceOrdinal != rhs.resourceOrdinal ) return lhs.resourceOrdinal < rhs.resourceOrdinal;
                    return lhs.ordinalAndKind < rhs.ordinalAndKind;
                } );

                for( size_t begin = 0; begin < facts.size(); )
                {
                    size_t end = begin + 1;
                    while( end < facts.size() && facts[end].passOrdinal == facts[begin].passOrdinal ) end++;
                    query::QueryGpuPassResourceSummary summary {};
                    summary.passId = uint64_t( facts[begin].passOrdinal ) + 1;
                    std::vector<uint64_t> useOrdinals;
                    std::vector<uint64_t> rangeOrdinals;
                    useOrdinals.reserve( end - begin ); rangeOrdinals.reserve( end - begin );
                    for( size_t resourceBegin = begin; resourceBegin < end; )
                    {
                        size_t resourceEnd = resourceBegin + 1;
                        while( resourceEnd < end && facts[resourceEnd].resourceOrdinal == facts[resourceBegin].resourceOrdinal )
                            resourceEnd++;
                        summary.resourceCount++;
                        summary.allocationCount++;
                        summary.referencedPhysicalBytes += 64;
                        bool ranged = false;
                        for( size_t index = resourceBegin; index < resourceEnd; index++ )
                        {
                            if( ( facts[index].ordinalAndKind & RangeBit ) != 0 )
                            {
                                ranged = true;
                                summary.rangeCount++;
                                summary.directRangeBytes += 64;
                                rangeOrdinals.emplace_back( facts[index].ordinalAndKind & ~RangeBit );
                            }
                            else
                            {
                                summary.useCount++;
                                useOrdinals.emplace_back( facts[index].ordinalAndKind );
                            }
                        }
                        if( ranged ) summary.rangedResourceCount++;
                        resourceBegin = resourceEnd;
                    }
                    std::sort( useOrdinals.begin(), useOrdinals.end() );
                    for( const auto ordinal : useOrdinals )
                    {
                        analysis::AnalysisKeyMetadataRecord value {};
                        value.keyHigh = summary.passId;
                        value.keyLow = ordinal;
                        value.metadata = ordinal;
                        useBuffer.emplace_back( value );
                        if( useBuffer.size() == WriteBufferRecords )
                        {
                            uses.Append( useBuffer.data(), useBuffer.size() );
                            useBuffer.clear();
                        }
                    }
                    std::sort( rangeOrdinals.begin(), rangeOrdinals.end() );
                    for( const auto ordinal : rangeOrdinals )
                    {
                        analysis::AnalysisKeyMetadataRecord value {};
                        value.keyHigh = summary.passId;
                        value.keyLow = ordinal;
                        value.metadata = ordinal;
                        rangeBuffer.emplace_back( value );
                        if( rangeBuffer.size() == WriteBufferRecords )
                        {
                            ranges.Append( rangeBuffer.data(), rangeBuffer.size() );
                            rangeBuffer.clear();
                        }
                    }
                    summaries.Append( &summary, 1 );
                    emittedSummaries++;
                    emittedUses += summary.useCount;
                    emittedRanges += summary.rangeCount;
                    summaryUseTotal += summary.useCount;
                    summaryRangeTotal += summary.rangeCount;
                    summaryAllocationTotal += summary.allocationCount;
                    begin = end;
                }
                facts.clear();
                std::error_code removeError;
                analysis::AnalysisDiskRemove( path, disk, removeError, true );
                if( removeError ) throw std::runtime_error( "fused partition cleanup failed" );
            }
            if( !useBuffer.empty() ) uses.Append( useBuffer.data(), useBuffer.size() );
            if( !rangeBuffer.empty() ) ranges.Append( rangeBuffer.data(), rangeBuffer.size() );
            for( uint64_t ordinal = 0; ordinal < basePassCount; ordinal++ )
            {
                analysis::AnalysisKeyMetadataRecord value {};
                value.keyHigh = ordinal + 1;
                value.keyLow = ordinal + 1;
                evidence.Append( &value, 1 );
            }
            if( fanout ) for( uint64_t ordinal = 0; ordinal < extraPassCount; ordinal++ )
            {
                analysis::AnalysisKeyMetadataRecord value {};
                value.keyHigh = basePassCount + ordinal + 1;
                value.keyLow = ordinal * 16 + 1;
                evidence.Append( &value, 1 );
            }
            summaries.Commit();
            uses.Commit();
            ranges.Commit();
            evidence.Commit();
            summaryHash = summaries.ContentHash();
            useHash = uses.ContentHash();
            rangeHash = ranges.ContentHash();
            evidenceHash = evidence.ContentHash();
        }
        const auto processingMs = Milliseconds( processingStarted );
        const auto buildMs = Milliseconds( started );
        if( emittedSummaries != passCount || emittedUses != expandedUseCount ||
            emittedRanges != expandedRangeCount || summaryUseTotal != expandedUseCount ||
            summaryRangeTotal != expandedRangeCount || expandedFactCount != expandedUseCount + expandedRangeCount )
            throw std::runtime_error( "fused capacity output count mismatch" );

        const auto validationStarted = std::chrono::steady_clock::now();
        {
            analysis::PagedRecordReader summaries( root / "summaries", summaryIdentity,
                sizeof( query::QueryGpuPassResourceSummary ), options );
            analysis::PagedRecordReader uses( root / "uses", useIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            analysis::PagedRecordReader ranges( root / "ranges", rangeIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            analysis::PagedRecordReader evidence( root / "evidence", evidenceIdentity,
                sizeof( analysis::AnalysisKeyMetadataRecord ), options );
            summaries.ValidateAll();
            uses.ValidateAll();
            ranges.ValidateAll();
            evidence.ValidateAll();
            if( summaries.Count() != passCount || uses.Count() != expandedUseCount ||
                ranges.Count() != expandedRangeCount || evidence.Count() != passCount )
                throw std::runtime_error( "fused capacity published count mismatch" );
            uint64_t previousPass = 0, verifiedUses = 0, verifiedRanges = 0, verifiedAllocations = 0;
            for( uint64_t ordinal = 0; ordinal < summaries.Count(); ordinal++ )
            {
                query::QueryGpuPassResourceSummary row {};
                summaries.Read( ordinal, &row, 1 );
                if( row.passId <= previousPass || row.useCount == 0 ||
                    row.allocationCount != row.resourceCount ||
                    row.referencedPhysicalBytes != row.allocationCount * 64 )
                    throw std::runtime_error( "fused capacity summary semantics changed" );
                previousPass = row.passId;
                verifiedUses += row.useCount;
                verifiedRanges += row.rangeCount;
                verifiedAllocations += row.allocationCount;
            }
            if( verifiedUses != expandedUseCount || verifiedRanges != expandedRangeCount ||
                verifiedAllocations != summaryAllocationTotal )
                throw std::runtime_error( "fused capacity summary totals changed" );
            const auto validateLookup = [&]( analysis::PagedRecordReader& reader, uint64_t maximumOrdinal )
            {
                analysis::AnalysisKeyMetadataRecord previous {};
                for( uint64_t ordinal = 0; ordinal < reader.Count(); ordinal++ )
                {
                    analysis::AnalysisKeyMetadataRecord value {};
                    reader.Read( ordinal, &value, 1 );
                    if( value.metadata >= maximumOrdinal || ( ordinal != 0 &&
                        ( value.keyHigh < previous.keyHigh ||
                            ( value.keyHigh == previous.keyHigh && value.keyLow <= previous.keyLow ) ) ) )
                        throw std::runtime_error( "fused capacity lookup order changed" );
                    previous = value;
                }
            };
            validateLookup( uses, useCount );
            validateLookup( ranges, std::max<uint64_t>( 1, rangeCount ) );
            analysis::AnalysisKeyMetadataRecord previousEvidence {};
            for( uint64_t ordinal = 0; ordinal < evidence.Count(); ordinal++ )
            {
                analysis::AnalysisKeyMetadataRecord value {};
                evidence.Read( ordinal, &value, 1 );
                if( value.keyHigh != ordinal + 1 || value.keyLow == 0 || value.keyLow > basePassCount ||
                    ( ordinal != 0 && value.keyHigh <= previousEvidence.keyHigh ) )
                    throw std::runtime_error( "fused capacity evidence order changed" );
                previousEvidence = value;
            }
        }
        const auto validateMs = Milliseconds( validationStarted );
        const auto diskSnapshot = usage->Snapshot();
        const auto workspaceSnapshot = workspace->Snapshot();
        const auto result = json {
            { "uses", useCount }, { "ranges", rangeCount }, { "expanded_uses", expandedUseCount },
            { "expanded_ranges", expandedRangeCount }, { "expanded_facts", expandedFactCount },
            { "evidence_passes", basePassCount }, { "query_passes", passCount },
            { "mapping_fanout", fanout ? "one_plus_one_per_16" : "one_to_one" },
            { "resource_keys", resourceCount }, { "distribution", fanout ? "mixed-high-fanout" : highEntropy ? "mixed-high" : mixed ? "mixed" : "uses-only" },
            { "partitions", PartitionCount }, { "partition_record_bytes", sizeof( FusedFact ) },
            { "partition_bytes", partitionBytes }, { "partition_ms", partitionMs },
            { "processing_ms", processingMs }, { "build_ms", buildMs }, { "validate_ms", validateMs },
            { "summary_records", emittedSummaries }, { "use_records", emittedUses },
            { "range_records", emittedRanges },
            { "allocation_pairs_after_local_dedup", summaryAllocationTotal },
            { "summary_sha256", summaryHash },
            { "use_sha256", useHash },
            { "range_sha256", rangeHash },
            { "evidence_sha256", evidenceHash },
            { "final_bytes", usage->Current() }, { "disk_peak_bytes", diskSnapshot.peakBytes },
            { "workspace_peak_bytes", workspaceSnapshot.peakBytes }
        };

        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( root, disk, cleanupError, true );
        if( cleanupError || usage->Current() != 0 || workspace->Snapshot().currentBytes != 0 )
            throw std::runtime_error( "fused capacity cleanup or accounting failed" );
        ownsRoot = false;
        std::cout << result.dump( 2 ) << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        if( ownsRoot )
        {
            std::error_code ignored;
            std::filesystem::remove_all( ownedRoot, ignored );
        }
        return 1;
    }
}
