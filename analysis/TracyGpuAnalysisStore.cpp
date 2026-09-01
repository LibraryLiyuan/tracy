#include "TracyGpuAnalysisStore.hpp"
#include "TracyGpuAnalysisPath.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <Windows.h>
#  undef FindResource
#else
#  include <unistd.h>
#endif

namespace tracy::analysis
{
std::optional<GpuAnalysisStoreManifest> LoadStoreManifestImpl(
    const std::filesystem::path& root, bool requireComplete, std::string& error );

struct GpuAnalysisStoreReader::InclusiveCache
{
    struct Entry
    {
        std::shared_ptr<const std::vector<uint64_t>> resources;
        uint64_t lastUse = 0;
    };
    static constexpr uint64_t MaximumBytes = 512ull * 1024 * 1024;
    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> entries;
    uint64_t bytes = 0;
    uint64_t clock = 0;
};

namespace
{

constexpr uint64_t StoreManifestMagic = 0x314d5453474e4aull; // JNGSTM1
constexpr uint64_t RelationMagic = 0x31584449474e4aull; // JNGIDX1
constexpr uint64_t PassPageMagic = 0x31534150474e4aull; // JNGPAS1
constexpr uint64_t ResourceSummaryPageMagic = 0x31525352474e4aull; // JNGRSR1
constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

#pragma pack( push, 1 )
struct RelationHeader
{
    uint64_t magic = RelationMagic;
    uint32_t schema = GpuAnalysisStoreSchemaVersion;
    uint8_t kind = 0;
    uint8_t reserved[3] {};
    uint64_t firstIndex = 0;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t checksum = 0;
};

struct PassPageHeader
{
    uint64_t magic = PassPageMagic;
    uint32_t schema = GpuAnalysisStoreSchemaVersion;
    uint32_t recordCount = 0;
    uint64_t dictionaryCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t checksum = 0;
};

struct PassPageRecord
{
    uint64_t passId = 0;
    uint64_t parentPassId = 0;
    uint64_t frameId = 0;
    uint64_t commandListId = 0;
    int64_t startNs = 0;
    int64_t endNs = 0;
    uint64_t directRangeBytes = 0;
    uint64_t directPhysicalBytes = 0;
    uint64_t inclusivePhysicalBytes = 0;
    uint32_t unknownRangeResourceCount = 0;
    uint32_t nameBytes = 0;
    uint32_t directSetId = 0;
    uint32_t inclusiveSetId = 0;
    uint32_t evidenceCount = 0;
    uint8_t complete = 0;
    uint8_t truncated = 0;
    uint8_t reserved[2] {};
};

struct ResourceSummaryPageHeader
{
    uint64_t magic = ResourceSummaryPageMagic;
    uint32_t schema = GpuAnalysisStoreSchemaVersion;
    uint32_t recordCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t checksum = 0;
};

struct ResourceSummaryPageRecord
{
    uint64_t generation = 0;
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
    uint64_t allocationOffsetBytes = 0;
    uint64_t createTime = 0;
    uint64_t destroyTime = 0;
    uint64_t nameHash = 0;
    uint64_t viewCount = 0;
    uint64_t logicalBindingCount = 0;
    uint64_t partCount = 0;
    uint64_t rangeCount = 0;
    uint64_t relationCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t allocationResourceCount = 0;
    uint32_t createCallsiteId = 0;
    uint32_t definitionRevision = 0;
    uint32_t nameOriginalLength = 0;
    uint32_t nameBytes = 0;
    uint16_t primaryKind = 0;
    uint8_t resourceClass = 0;
    uint8_t memoryDomain = 0;
    uint8_t allocationKind = 0;
    uint8_t nameProvenance = 0;
    uint8_t stackProvenance = 0;
    uint8_t exactness = 0;
    uint8_t openBoundary = 0;
    uint8_t aliveAtEnd = 0;
    uint8_t hasAliasGroup = 0;
    uint8_t reserved[5] {};
};
#pragma pack( pop )

uint64_t HashUpdate( uint64_t hash, const void* data, size_t size )
{
    const auto* bytes = static_cast<const uint8_t*>( data );
    for( size_t i = 0; i < size; ++i ) { hash ^= bytes[i]; hash *= FnvPrime; }
    return hash;
}

template<typename T> void AppendPod( std::vector<uint8_t>& output, const T& value )
{
    static_assert( std::is_trivially_copyable_v<T> ); const auto begin = output.size(); output.resize( begin + sizeof( value ) );
    std::memcpy( output.data() + begin, &value, sizeof( value ) );
}

void AppendBytes( std::vector<uint8_t>& output, const void* data, size_t size )
{
    const auto begin = output.size(); output.resize( begin + size ); if( size ) std::memcpy( output.data() + begin, data, size );
}

void AppendVarint( std::vector<uint8_t>& output, uint64_t value )
{
    while( value >= 0x80 ) { output.push_back( uint8_t( value ) | 0x80 ); value >>= 7; } output.push_back( uint8_t( value ) );
}

bool ReadVarint( const uint8_t*& cursor, const uint8_t* end, uint64_t& value )
{
    value = 0; unsigned shift = 0;
    while( cursor != end && shift < 64 ) { const auto byte = *cursor++; value |= uint64_t( byte & 0x7f ) << shift; if( ( byte & 0x80 ) == 0 ) return true; shift += 7; }
    return false;
}

uint64_t FileChecksum( const std::filesystem::path& path, uint64_t& bytes, std::string& error )
{
    std::ifstream in( GpuAnalysisIoPath( path ), std::ios::binary );
    if( !in ) { error = "store_checksum_open_failed"; return 0; }
    std::vector<uint8_t> buffer( 1024 * 1024 );
    uint64_t hash = FnvOffset; bytes = 0;
    while( in )
    {
        in.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 ) { hash = HashUpdate( hash, buffer.data(), size_t( count ) ); bytes += uint64_t( count ); }
    }
    if( !in.eof() ) { error = "store_checksum_read_failed"; return 0; }
    return hash;
}

bool AtomicReplace( const std::filesystem::path& source, const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    DWORD lastError = ERROR_SUCCESS;
    for( unsigned attempt = 0; attempt < 8; ++attempt )
    {
        const auto ioTarget = GpuAnalysisIoPath( target ); const auto ioSource = GpuAnalysisIoPath( source );
        const bool targetExists = GetFileAttributesW( ioTarget.c_str() ) != INVALID_FILE_ATTRIBUTES;
        const bool replaced = targetExists
            ? ReplaceFileW( ioTarget.c_str(), ioSource.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr ) != FALSE
            : MoveFileExW( ioSource.c_str(), ioTarget.c_str(), MOVEFILE_WRITE_THROUGH ) != FALSE;
        if( replaced ) return true;
        lastError = GetLastError();
        if( lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION &&
            lastError != ERROR_UNABLE_TO_REMOVE_REPLACED && lastError != ERROR_UNABLE_TO_MOVE_REPLACEMENT &&
            lastError != ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 ) break;
        Sleep( 1u << attempt );
    }
    error = "store_atomic_replace_failed:" + std::to_string( lastError ); return false;
#else
    std::error_code ec; std::filesystem::rename( source, target, ec );
    if( !ec ) return true; error = "store_atomic_replace_failed:" + ec.message(); return false;
#endif
}

GpuAnalysisSnapshot OverviewOf( const GpuAnalysisSnapshot& source )
{
    GpuAnalysisSnapshot out;
    out.manifest = source.manifest;
    out.engineKnownPhysicalBytes = source.engineKnownPhysicalBytes;
    out.engineKnownPhysicalPeakBytes = source.engineKnownPhysicalPeakBytes;
    out.engineKnownPhysicalPeakTimeNs = source.engineKnownPhysicalPeakTimeNs;
    out.allocationCreateCount = source.allocationCreateCount;
    out.allocationDestroyCount = source.allocationDestroyCount;
    out.allocatedPhysicalBytes = source.allocatedPhysicalBytes;
    out.freedPhysicalBytes = source.freedPhysicalBytes;
    out.residentPhysicalBytes = source.residentPhysicalBytes;
    out.logicalCapacityBytes = source.logicalCapacityBytes;
    return out;
}

uint64_t ResourceBytes( const GpuResourceAnalysisRecord& value )
{
    uint64_t bytes = sizeof( value ) + value.name.size() + value.history.size() * sizeof( size_t ) +
        value.views.size() * sizeof( GpuViewAnalysisRecord ) + value.parts.size() * sizeof( GpuPartAnalysisRecord ) +
        value.relations.size() * sizeof( GpuRelationAnalysisRecord ) + value.ranges.size() * sizeof( GpuRangeAnalysisRecord ) +
        value.virtualGeometry.size() * sizeof( GpuVgAnalysisRecord );
    for( const auto& logical : value.logicals ) bytes += sizeof( logical ) + logical.name.size();
    return bytes;
}

uint64_t AllocationBytes( const GpuAllocationAnalysisRecord& value )
{
    return sizeof( value ) + value.history.size() * sizeof( size_t ) + value.resources.size() * sizeof( uint64_t );
}

uint64_t PassBytes( const GpuPassWorkingSet& value )
{
    return sizeof( value ) + value.name.size() +
        ( value.directResources.size() + value.inclusiveResources.size() ) * sizeof( uint64_t ) +
        value.detailedEvidence.size() * sizeof( GpuDetailedEvidenceAnalysisRecord );
}

uint64_t ChurnBytes( const GpuChurnCandidate& value ) { return sizeof( value ) + value.reason.size(); }

std::string GenerationName()
{
    static std::atomic<uint64_t> sequence { 0 };
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch() ).count();
#ifdef _WIN32
    const auto processId = GetCurrentProcessId();
#else
    const auto processId = getpid();
#endif
    return "g" + std::to_string( now ) + "-p" + std::to_string( processId ) +
        "-s" + std::to_string( sequence.fetch_add( 1, std::memory_order_relaxed ) );
}

bool PublishGenerationDirectory( const std::filesystem::path& staging,
    const std::filesystem::path& committed, std::string& error )
{
    std::error_code existsError;
    if( std::filesystem::exists( GpuAnalysisIoPath( committed ), existsError ) )
    {
        error = "store_generation_collision";
        return false;
    }
    if( existsError )
    {
        error = "store_generation_publish_probe_failed:" + existsError.message();
        return false;
    }
#ifdef _WIN32
    DWORD lastError = ERROR_SUCCESS;
    for( unsigned attempt = 0; attempt < 8; ++attempt )
    {
        if( MoveFileExW( GpuAnalysisIoPath( staging ).c_str(), GpuAnalysisIoPath( committed ).c_str(), MOVEFILE_WRITE_THROUGH ) ) return true;
        lastError = GetLastError();
        if( lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION ) break;
        Sleep( 1u << attempt );
    }
    error = "store_generation_publish_failed:" + std::to_string( lastError );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( staging, committed, ec );
    if( !ec ) return true;
    error = "store_generation_publish_failed:" + ec.message();
    return false;
#endif
}

bool SaveStoreManifest( const std::filesystem::path& root, const GpuAnalysisStoreManifest& value, std::string& error )
{
    const auto target = root / "store-manifest"; auto temporary = target; temporary += ".tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "store_manifest_open_failed"; return false; }
    out << "magic " << StoreManifestMagic << '\n';
    out << "schema " << value.schema << '\n';
    out << "algorithm " << std::quoted( value.algorithmId ) << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "trace_sha256 " << std::quoted( value.traceSha256 ) << '\n';
    out << "trace_size " << value.traceSize << '\n';
#define W( field ) out << #field " " << value.field << '\n'
    W( resourceCount ); W( allocationCount ); W( passCount ); W( passSummaryCount ); W( stablePassSummaryCount ); W( residencyCount ); W( churnCount );
    W( resourcePassRelationCount ); W( framePassRelationCount ); W( passChildRelationCount ); W( rangeCount );
    W( logicalCount ); W( catalogRelationCount ); W( sourceGapResourceCount );
    W( sourceGapReferenceCount ); W( totalBytes );
#undef W
    out << "complete " << value.complete << '\n';
    out << "reason " << std::quoted( value.reason ) << '\n';
    out << "type_count " << value.typeSummaries.size() << '\n';
    for( const auto& type : value.typeSummaries ) out << "type " << type.primaryKind << ' ' << type.liveResourceCount << ' ' << type.resourceCapacityBytes << '\n';
    out << "page_count " << value.pages.size() << '\n';
    for( const auto& page : value.pages )
        out << "page " << unsigned( page.kind ) << ' ' << page.firstIndex << ' ' << page.recordCount << ' '
            << page.firstKey << ' ' << page.lastKey << ' ' << page.fileBytes << ' ' << page.checksum << ' '
            << std::quoted( page.relativePath.generic_string() ) << '\n';
    out.flush(); if( !out ) { error = "store_manifest_write_failed"; return false; }
    out.close(); return AtomicReplace( temporary, target, error );
}

template<typename T, typename SizeFn, typename KeyFn>
bool WriteCachePages( const std::filesystem::path& root, GpuAnalysisStorePageKind kind, const char* directory,
    const std::vector<T>& input, const GpuAnalysisSnapshot& overview, const GpuAnalysisCacheIdentity& identity,
    SizeFn sizeFn, KeyFn keyFn, std::vector<GpuAnalysisStorePage>& pages, uint64_t& totalBytes,
    const GpuAnalysisSidecarControl& control, const std::function<bool()>& checkpoint, std::string& error )
{
    std::vector<size_t> order( input.size() ); std::iota( order.begin(), order.end(), 0 );
    std::stable_sort( order.begin(), order.end(), [&]( size_t lhs, size_t rhs ) { return keyFn( input[lhs] ) < keyFn( input[rhs] ); } );
    std::error_code ec; const auto pageRoot = root / directory; std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_page_directory_failed:" + ec.message(); return false; }
    size_t first = 0; uint32_t pageIndex = 0;
    for( const auto& page : pages ) if( page.kind == kind ) { first += size_t( page.recordCount ); ++pageIndex; }
    if( first > order.size() ) { error = "store_resume_record_count_mismatch"; return false; }
    while( first < order.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        GpuAnalysisSnapshot page = OverviewOf( overview ); uint64_t estimated = 0; const auto begin = first;
        while( first < order.size() && ( first == begin || estimated < control.targetDerivedPageBytes ) )
        {
            const auto& value = input[order[first++]]; estimated += sizeFn( value );
            if constexpr( std::is_same_v<T, GpuResourceAnalysisRecord> ) page.resources.push_back( value );
            else if constexpr( std::is_same_v<T, GpuAllocationAnalysisRecord> ) page.allocations.push_back( value );
            else if constexpr( std::is_same_v<T, GpuPassWorkingSet> ) page.passes.push_back( value );
            else if constexpr( std::is_same_v<T, GpuResidencyInterval> ) page.residency.push_back( value );
            else if constexpr( std::is_same_v<T, GpuChurnCandidate> ) page.churnCandidates.push_back( value );
        }
        std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' ) << pageIndex++ << ".bin";
        const auto path = pageRoot / fileName.str();
        if( !SaveGpuAnalysisCache( path, identity, page, error ) ) return false;
        uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error ); if( !error.empty() ) return false;
        if( totalBytes > control.maximumSidecarBytes - fileBytes ) { error = "sidecar_size_limit"; return false; }
        const auto firstKey = keyFn( input[order[begin]] ); const auto lastKey = keyFn( input[order[first - 1]] );
        pages.push_back( { kind, begin, first - begin, firstKey, lastKey, fileBytes, checksum, std::filesystem::relative( path, root ) } );
        totalBytes += fileBytes;
        if( !checkpoint() ) return false;
        if( control.progress ) control.progress( 0.f, "store-page-committed" );
    }
    return true;
}

bool WritePassPages( const std::filesystem::path& root, const std::vector<GpuPassWorkingSet>& input,
    std::vector<GpuAnalysisStorePage>& pages, uint64_t& totalBytes, const GpuAnalysisSidecarControl& control,
    const std::function<bool()>& checkpoint, std::string& error )
{
    std::vector<size_t> order( input.size() ); std::iota( order.begin(), order.end(), 0 );
    std::stable_sort( order.begin(), order.end(), [&]( size_t lhs, size_t rhs ) { return input[lhs].passId < input[rhs].passId; } );
    size_t first = 0; uint32_t pageIndex = 0;
    for( const auto& page : pages ) if( page.kind == GpuAnalysisStorePageKind::Pass ) { first += size_t( page.recordCount ); ++pageIndex; }
    if( first > order.size() ) { error = "store_resume_pass_count_mismatch"; return false; }
    std::error_code ec; const auto pageRoot = root / "passes"; std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_pass_directory_failed:" + ec.message(); return false; }
    while( first < order.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        const auto begin = first; uint64_t estimate = 0;
        while( first < order.size() && ( first == begin || estimate < control.targetDerivedPageBytes ) ) estimate += PassBytes( input[order[first++]] );

        std::vector<std::vector<uint64_t>> dictionary;
        std::unordered_map<uint64_t, std::vector<uint32_t>> dictionaryByHash;
        const auto intern = [&]( const std::vector<uint64_t>& value ) -> uint32_t {
            uint64_t hash = FnvOffset;
            const uint64_t count = value.size(); hash = HashUpdate( hash, &count, sizeof( count ) );
            if( !value.empty() ) hash = HashUpdate( hash, value.data(), value.size() * sizeof( uint64_t ) );
            auto& candidates = dictionaryByHash[hash];
            for( const auto id : candidates ) if( dictionary[id] == value ) return id;
            const auto id = uint32_t( dictionary.size() ); dictionary.push_back( value ); candidates.push_back( id ); return id;
        };
        std::vector<PassPageRecord> records; records.reserve( first - begin );
        for( auto index = begin; index < first; ++index )
        {
            const auto& value = input[order[index]]; PassPageRecord record;
            record.passId = value.passId; record.parentPassId = value.parentPassId; record.frameId = value.frameId; record.commandListId = value.commandListId;
            record.startNs = value.startNs; record.endNs = value.endNs; record.directRangeBytes = value.directRangeBytes;
            record.directPhysicalBytes = value.directPhysicalBytes; record.inclusivePhysicalBytes = value.inclusivePhysicalBytes;
            record.unknownRangeResourceCount = value.unknownRangeResourceCount; record.nameBytes = uint32_t( value.name.size() );
            record.directSetId = intern( value.directResources ); record.inclusiveSetId = intern( value.inclusiveResources );
            record.evidenceCount = uint32_t( value.detailedEvidence.size() );
            record.complete = value.complete; record.truncated = value.truncated; records.push_back( record );
        }
        std::vector<uint8_t> payload; AppendPod( payload, uint64_t( dictionary.size() ) );
        for( const auto& set : dictionary )
        {
            std::vector<uint8_t> delta; delta.reserve( set.size() * 2 ); uint64_t previous = 0;
            for( const auto value : set ) { AppendVarint( delta, value - previous ); previous = value; }
            const uint8_t encoding = delta.size() < set.size() * sizeof( uint64_t ) ? 1 : 0;
            const uint32_t count = uint32_t( set.size() ); const uint64_t bytes = encoding ? delta.size() : set.size() * sizeof( uint64_t );
            AppendPod( payload, count ); AppendPod( payload, encoding ); const uint8_t reserved[3] {}; AppendBytes( payload, reserved, sizeof( reserved ) );
            AppendPod( payload, bytes ); if( encoding ) AppendBytes( payload, delta.data(), delta.size() ); else AppendBytes( payload, set.data(), size_t( bytes ) );
        }
        AppendPod( payload, uint64_t( records.size() ) );
        for( size_t index = 0; index < records.size(); ++index )
        { AppendPod( payload, records[index] ); const auto& value = input[order[begin + index]]; AppendBytes( payload, value.name.data(), value.name.size() );
          AppendBytes( payload, value.detailedEvidence.data(), value.detailedEvidence.size() * sizeof( GpuDetailedEvidenceAnalysisRecord ) ); }
        PassPageHeader header; header.recordCount = uint32_t( records.size() ); header.dictionaryCount = dictionary.size();
        header.payloadBytes = payload.size(); header.checksum = HashUpdate( FnvOffset, payload.data(), payload.size() );
        std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' ) << pageIndex++ << ".bin";
        const auto path = pageRoot / fileName.str(); auto temporary = path; temporary += ".tmp";
        { std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc ); out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
          out.write( reinterpret_cast<const char*>( payload.data() ), std::streamsize( payload.size() ) ); out.flush();
          if( !out ) { error = "store_pass_write_failed"; return false; } }
        if( !AtomicReplace( temporary, path, error ) ) return false;
        uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error ); if( !error.empty() ) return false;
        if( totalBytes > control.maximumSidecarBytes - fileBytes ) { error = "sidecar_size_limit"; return false; }
        pages.push_back( { GpuAnalysisStorePageKind::Pass, begin, first - begin, input[order[begin]].passId,
            input[order[first - 1]].passId, fileBytes, checksum, std::filesystem::relative( path, root ) } );
        totalBytes += fileBytes; if( !checkpoint() ) return false; if( control.progress ) control.progress( 0.f, "store-page-committed" );
    }
    return true;
}

template<typename T>
bool WriteRelationPages( const std::filesystem::path& root, GpuAnalysisStorePageKind kind, const char* directory,
    std::vector<T>& values, std::vector<GpuAnalysisStorePage>& pages, uint64_t& totalBytes,
    const GpuAnalysisSidecarControl& control, const std::function<bool()>& checkpoint, std::string& error )
{
    static_assert( std::is_trivially_copyable_v<T> );
    std::sort( values.begin(), values.end(), []( const auto& lhs, const auto& rhs ) {
        const uint64_t lhsKey = [] ( const auto& v ) { if constexpr( std::is_same_v<T, GpuAnalysisResourcePassEntry> ) return v.resourceId; else return v.frameId; }( lhs );
        const uint64_t rhsKey = [] ( const auto& v ) { if constexpr( std::is_same_v<T, GpuAnalysisResourcePassEntry> ) return v.resourceId; else return v.frameId; }( rhs );
        if( lhsKey != rhsKey ) return lhsKey < rhsKey; return lhs.passId < rhs.passId;
    } );
    const auto keyOf = []( const T& value ) { if constexpr( std::is_same_v<T, GpuAnalysisResourcePassEntry> ) return value.resourceId; else return value.frameId; };
    std::error_code ec; const auto pageRoot = root / directory; std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_relation_directory_failed:" + ec.message(); return false; }
    const uint64_t perPage = std::max<uint64_t>( 1, control.targetDerivedPageBytes / sizeof( T ) );
    uint64_t first = 0; uint32_t pageIndex = 0;
    for( const auto& page : pages ) if( page.kind == kind ) { first += page.recordCount; ++pageIndex; }
    if( first > values.size() ) { error = "store_resume_relation_count_mismatch"; return false; }
    while( first < values.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        const auto count = std::min<uint64_t>( perPage, values.size() - first );
        const auto payloadBytes = count * sizeof( T );
        RelationHeader header; header.kind = uint8_t( kind ); header.firstIndex = first; header.recordCount = count;
        header.payloadBytes = payloadBytes; header.checksum = HashUpdate( FnvOffset, values.data() + first, size_t( payloadBytes ) );
        std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' ) << pageIndex++ << ".bin";
        const auto path = pageRoot / fileName.str(); auto temporary = path; temporary += ".tmp";
        std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
        if( !out ) { error = "store_relation_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( reinterpret_cast<const char*>( values.data() + first ), std::streamsize( payloadBytes ) ); out.flush();
        if( !out ) { error = "store_relation_write_failed"; return false; } out.close();
        if( !AtomicReplace( temporary, path, error ) ) return false;
        uint64_t fileBytes = 0; const auto fileChecksum = FileChecksum( path, fileBytes, error ); if( !error.empty() ) return false;
        if( totalBytes > control.maximumSidecarBytes - fileBytes ) { error = "sidecar_size_limit"; return false; }
        pages.push_back( { kind, first, count, keyOf( values[size_t( first )] ), keyOf( values[size_t( first + count - 1 )] ),
            fileBytes, fileChecksum, std::filesystem::relative( path, root ) } );
        totalBytes += fileBytes; first += count;
        if( !checkpoint() ) return false;
        if( control.progress ) control.progress( 0.f, "store-page-committed" );
    }
    return true;
}

bool VerifyPageFile( const std::filesystem::path& path, const GpuAnalysisStorePage& page, std::string& error )
{
    uint64_t bytes = 0; const auto checksum = FileChecksum( path, bytes, error );
    if( !error.empty() ) return false;
    if( bytes != page.fileBytes || checksum != page.checksum ) { error = "store_page_checksum_mismatch"; return false; }
    return true;
}

template<typename T>
bool LoadRelationPage( const std::filesystem::path& path, const GpuAnalysisStorePage& page,
    std::vector<T>& values, std::string& error )
{
    if( !VerifyPageFile( path, page, error ) ) return false;
    std::ifstream in( GpuAnalysisIoPath( path ), std::ios::binary ); RelationHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) || header.magic != RelationMagic ||
        header.schema != GpuAnalysisStoreSchemaVersion || header.kind != uint8_t( page.kind ) ||
        header.recordCount != page.recordCount || header.payloadBytes != header.recordCount * sizeof( T ) )
    { error = "store_relation_header_mismatch"; return false; }
    values.resize( size_t( header.recordCount ) );
    if( !in.read( reinterpret_cast<char*>( values.data() ), std::streamsize( header.payloadBytes ) ) )
    { error = "store_relation_payload_failed"; return false; }
    if( HashUpdate( FnvOffset, values.data(), size_t( header.payloadBytes ) ) != header.checksum )
    { error = "store_relation_payload_checksum_mismatch"; return false; }
    return true;
}

const GpuAnalysisStorePage* NthPage( const GpuAnalysisStoreManifest& manifest, GpuAnalysisStorePageKind kind, size_t index )
{
    for( const auto& page : manifest.pages ) if( page.kind == kind ) { if( index-- == 0 ) return &page; }
    return nullptr;
}

const GpuAnalysisStorePage* NthPage( const std::vector<GpuAnalysisStorePage>& pages,
    GpuAnalysisStorePageKind kind, size_t index )
{
    for( const auto& page : pages ) if( page.kind == kind )
    { if( index-- == 0 ) return &page; }
    return nullptr;
}

size_t CountPages( const GpuAnalysisStoreManifest& manifest, GpuAnalysisStorePageKind kind )
{
    return size_t( std::count_if( manifest.pages.begin(), manifest.pages.end(), [&]( const auto& page ) { return page.kind == kind; } ) );
}

template<typename Entry, typename KeyFn>
bool RelationIds( const std::filesystem::path& root, const GpuAnalysisStoreManifest& manifest,
    GpuAnalysisStorePageKind kind, uint64_t key, size_t offset, size_t limit,
    std::vector<uint64_t>& passIds, bool& hasMore, KeyFn keyFn, std::string& error )
{
    hasMore = false; size_t skipped = 0;
    for( const auto& page : manifest.pages )
    {
        if( page.kind != kind || key < page.firstKey || key > page.lastKey ) continue;
        std::vector<Entry> values;
        if( !LoadRelationPage( root / page.relativePath, page, values, error ) ) return false;
        const auto first = std::lower_bound( values.begin(), values.end(), key,
            [&]( const Entry& value, uint64_t candidate ) { return keyFn( value ) < candidate; } );
        for( auto it = first; it != values.end() && keyFn( *it ) == key; ++it )
        {
            if( skipped++ < offset ) continue;
            if( passIds.size() == limit ) { hasMore = true; return true; }
            passIds.push_back( it->passId );
        }
    }
    return true;
}

GpuAnalysisPassSummary PassSummaryOf( const GpuPassWorkingSet& pass,
    const GpuAnalysisPassTaxonomyEntry* taxonomy = nullptr )
{
    GpuAnalysisPassSummary value;
    value.passId = pass.passId;
    value.parentPassId = pass.parentPassId;
    value.frameId = pass.frameId;
    value.directResourceCount = pass.directResources.size();
    value.inclusiveResourceCount = pass.inclusiveResources.size();
    value.directPhysicalBytes = pass.directPhysicalBytes;
    value.inclusivePhysicalBytes = pass.inclusivePhysicalBytes;
    value.directResourceHash = GpuAnalysisResourceSetHash( pass.directResources );
    value.inclusiveResourceHash = GpuAnalysisResourceSetHash( pass.inclusiveResources );
    value.directRangeBytes = pass.directRangeBytes;
    value.unknownRangeResourceCount = pass.unknownRangeResourceCount;
    if( taxonomy )
    {
        value.taxonomyId = taxonomy->taxonomyId;
        value.taxonomyLevel = taxonomy->taxonomyLevel;
        value.taxonomyFlags = taxonomy->flags;
    }
    value.complete = pass.complete;
    value.truncated = pass.truncated;
    return value;
}

}

uint64_t GpuAnalysisResourceSetHash( const std::vector<uint64_t>& resources )
{
    uint64_t hash = FnvOffset;
    const uint64_t count = resources.size();
    hash = HashUpdate( hash, &count, sizeof( count ) );
    if( !resources.empty() )
        hash = HashUpdate( hash, resources.data(), resources.size() * sizeof( uint64_t ) );
    return hash;
}

bool WriteGpuAnalysisDerivedStore( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error )
{
    return WriteGpuAnalysisDerivedStoreAt( GpuAnalysisDerivedPath( sidecarPath ),
        identity, snapshot, control, generation, writtenBytes, error );
}

bool WriteSortedPassBatch( const std::filesystem::path& root,
    const std::vector<GpuPassWorkingSet>& input, uint64_t firstIndex,
    uint32_t pageIndex, std::vector<GpuAnalysisStorePage>& pages,
    uint64_t& totalBytes, const GpuAnalysisSidecarControl& control,
    const std::function<bool()>& checkpoint, std::string& error )
{
    if( input.empty() ) return true;
    if( !std::is_sorted( input.begin(), input.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    } ) ) { error = "store_spool_pass_order_invalid"; return false; }
    if( const auto* existing = NthPage( pages, GpuAnalysisStorePageKind::Pass,
        pageIndex ) )
    {
        if( existing->firstIndex != firstIndex || existing->recordCount != input.size() ||
            existing->firstKey != input.front().passId ||
            existing->lastKey != input.back().passId )
        { error = "store_resume_pass_page_mismatch"; return false; }
        return true;
    }
    std::error_code ec; const auto pageRoot = root / "passes";
    std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_pass_directory_failed:" + ec.message(); return false; }
    std::vector<std::vector<uint64_t>> dictionary;
    std::unordered_map<uint64_t, std::vector<uint32_t>> dictionaryByHash;
    const auto intern = [&]( const std::vector<uint64_t>& value ) -> uint32_t {
        uint64_t hash = FnvOffset; const uint64_t count = value.size();
        hash = HashUpdate( hash, &count, sizeof( count ) );
        if( !value.empty() ) hash = HashUpdate( hash, value.data(), value.size() * sizeof( uint64_t ) );
        auto& candidates = dictionaryByHash[hash];
        for( const auto id : candidates ) if( dictionary[id] == value ) return id;
        const auto id = uint32_t( dictionary.size() ); dictionary.push_back( value );
        candidates.push_back( id ); return id;
    };
    std::vector<PassPageRecord> records; records.reserve( input.size() );
    for( const auto& value : input )
    {
        PassPageRecord record;
        record.passId = value.passId; record.parentPassId = value.parentPassId;
        record.frameId = value.frameId; record.commandListId = value.commandListId;
        record.startNs = value.startNs; record.endNs = value.endNs;
        record.directRangeBytes = value.directRangeBytes;
        record.directPhysicalBytes = value.directPhysicalBytes;
        record.inclusivePhysicalBytes = value.inclusivePhysicalBytes;
        record.unknownRangeResourceCount = value.unknownRangeResourceCount;
        record.nameBytes = uint32_t( value.name.size() );
        record.directSetId = intern( value.directResources );
        // Session Pass pages persist only authoritative Direct members. Exact
        // Inclusive members are reconstructed from Parent->Child + Direct and
        // verified against PassSummary count/hash.
        static const std::vector<uint64_t> EmptySet;
        record.inclusiveSetId = intern( EmptySet );
        record.evidenceCount = uint32_t( value.detailedEvidence.size() );
        record.complete = value.complete; record.truncated = value.truncated;
        records.push_back( record );
    }
    std::vector<uint8_t> payload; AppendPod( payload, uint64_t( dictionary.size() ) );
    for( const auto& set : dictionary )
    {
        std::vector<uint8_t> delta; delta.reserve( set.size() * 2 ); uint64_t previous = 0;
        for( const auto value : set ) { AppendVarint( delta, value - previous ); previous = value; }
        const uint8_t encoding = delta.size() < set.size() * sizeof( uint64_t ) ? 1 : 0;
        const uint32_t count = uint32_t( set.size() );
        const uint64_t bytes = encoding ? delta.size() : set.size() * sizeof( uint64_t );
        AppendPod( payload, count ); AppendPod( payload, encoding );
        const uint8_t reserved[3] {}; AppendBytes( payload, reserved, sizeof( reserved ) );
        AppendPod( payload, bytes );
        if( encoding ) AppendBytes( payload, delta.data(), delta.size() );
        else AppendBytes( payload, set.data(), size_t( bytes ) );
    }
    AppendPod( payload, uint64_t( records.size() ) );
    for( size_t index = 0; index < records.size(); ++index )
    {
        AppendPod( payload, records[index] );
        AppendBytes( payload, input[index].name.data(), input[index].name.size() );
        AppendBytes( payload, input[index].detailedEvidence.data(),
            input[index].detailedEvidence.size() * sizeof( GpuDetailedEvidenceAnalysisRecord ) );
    }
    PassPageHeader header; header.recordCount = uint32_t( records.size() );
    header.dictionaryCount = dictionary.size(); header.payloadBytes = payload.size();
    header.checksum = HashUpdate( FnvOffset, payload.data(), payload.size() );
    std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
    const auto path = pageRoot / fileName.str(); auto temporary = path; temporary += ".tmp";
    { std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
      out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
      out.write( reinterpret_cast<const char*>( payload.data() ), std::streamsize( payload.size() ) );
      out.flush(); if( !out ) { error = "store_pass_write_failed"; return false; } }
    if( !AtomicReplace( temporary, path, error ) ) return false;
    uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error );
    if( !error.empty() ) return false;
    if( totalBytes > control.maximumSidecarBytes - fileBytes )
    { error = "sidecar_size_limit"; return false; }
    pages.push_back( { GpuAnalysisStorePageKind::Pass, firstIndex, input.size(),
        input.front().passId, input.back().passId, fileBytes, checksum,
        std::filesystem::relative( path, root ) } );
    totalBytes += fileBytes;
    if( !checkpoint() ) return false;
    if( control.progress ) control.progress( 0.f, "store-page-committed" );
    return true;
}

template<typename T, typename Compare>
bool WriteSortedRun( const std::filesystem::path& path, std::vector<T>& values,
    Compare compare, std::string& error, bool deduplicate = true )
{
    static_assert( std::is_trivially_copyable_v<T> );
    std::sort( values.begin(), values.end(), compare );
    if( deduplicate ) values.erase( std::unique( values.begin(), values.end(),
        []( const T& lhs, const T& rhs ) {
            return std::memcmp( &lhs, &rhs, sizeof( T ) ) == 0;
        } ), values.end() );
    std::ofstream out( GpuAnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "store_relation_run_open_failed"; return false; }
    if( !values.empty() ) out.write( reinterpret_cast<const char*>( values.data() ),
        std::streamsize( values.size() * sizeof( T ) ) );
    out.flush(); if( !out ) { error = "store_relation_run_write_failed"; return false; }
    return true;
}

template<typename T, typename Compare, typename KeyFn>
bool MergeRelationRuns( const std::filesystem::path& root,
    GpuAnalysisStorePageKind kind, const char* directory,
    const std::vector<std::filesystem::path>& runs, Compare compare, KeyFn keyOf,
    std::vector<GpuAnalysisStorePage>& pages, uint64_t& totalBytes,
    const GpuAnalysisSidecarControl& control, const std::function<bool()>& checkpoint,
    uint64_t& mergedCount, std::string& error, bool deduplicate = true )
{
    struct Cursor { std::ifstream in; T value {}; bool valid = false; };
    struct Node { T value {}; size_t run = 0; };
    struct Later { Compare compare; bool operator()( const Node& lhs, const Node& rhs ) const {
        return compare( rhs.value, lhs.value );
    } };
    std::vector<Cursor> cursors( runs.size() );
    std::priority_queue<Node, std::vector<Node>, Later> heap( Later { compare } );
    for( size_t i = 0; i < runs.size(); ++i )
    {
        cursors[i].in.open( GpuAnalysisIoPath( runs[i] ), std::ios::binary );
        if( !cursors[i].in ) { error = "store_relation_run_read_open_failed"; return false; }
        cursors[i].valid = bool( cursors[i].in.read(
            reinterpret_cast<char*>( &cursors[i].value ), sizeof( T ) ) );
        if( cursors[i].valid ) heap.push( { cursors[i].value, i } );
    }
    std::error_code ec; const auto pageRoot = root / directory;
    std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_relation_directory_failed:" + ec.message(); return false; }
    const size_t perPage = size_t( std::max<uint64_t>( 1,
        control.targetDerivedPageBytes / sizeof( T ) ) );
    std::vector<T> page; page.reserve( perPage );
    uint32_t pageIndex = 0; uint64_t firstIndex = 0; mergedCount = 0;
    T previous {}; bool havePrevious = false;
    const auto flush = [&]() -> bool {
        if( page.empty() ) return true;
        const auto committedPageIndex = pageIndex++;
        if( const auto* existing = NthPage( pages, kind, committedPageIndex ) )
        {
            if( existing->firstIndex != firstIndex ||
                existing->recordCount != page.size() ||
                existing->firstKey != keyOf( page.front() ) ||
                existing->lastKey != keyOf( page.back() ) )
            { error = "store_resume_relation_page_mismatch"; return false; }
            firstIndex += page.size(); page.clear(); return true;
        }
        RelationHeader header; header.kind = uint8_t( kind ); header.firstIndex = firstIndex;
        header.recordCount = page.size(); header.payloadBytes = page.size() * sizeof( T );
        header.checksum = HashUpdate( FnvOffset, page.data(), size_t( header.payloadBytes ) );
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << committedPageIndex << ".bin";
        const auto path = pageRoot / name.str(); auto temporary = path; temporary += ".tmp";
        std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
        if( !out ) { error = "store_relation_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( reinterpret_cast<const char*>( page.data() ), std::streamsize( header.payloadBytes ) );
        out.flush(); if( !out ) { error = "store_relation_write_failed"; return false; }
        out.close(); if( !AtomicReplace( temporary, path, error ) ) return false;
        uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error );
        if( !error.empty() ) return false;
        pages.push_back( { kind, firstIndex, page.size(), keyOf( page.front() ),
            keyOf( page.back() ), fileBytes, checksum, std::filesystem::relative( path, root ) } );
        totalBytes += fileBytes; firstIndex += page.size(); page.clear();
        if( !checkpoint() ) return false;
        if( control.progress ) control.progress( 0.f, "store-page-committed" );
        return true;
    };
    while( !heap.empty() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        const auto node = heap.top(); heap.pop();
        if( !deduplicate || !havePrevious ||
            std::memcmp( &previous, &node.value, sizeof( T ) ) != 0 )
        {
            page.push_back( node.value ); previous = node.value; havePrevious = true; ++mergedCount;
            if( page.size() == perPage && !flush() ) return false;
        }
        auto& cursor = cursors[node.run];
        cursor.valid = bool( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) );
        if( cursor.valid ) heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() ) { error = "store_relation_run_read_failed"; return false; }
    }
    return flush();
}

bool WriteGpuAnalysisDerivedStoreAt( const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error )
{
    error.clear(); writtenBytes = 0;
    std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( algorithmRoot ), ec );
    if( ec ) { error = "store_algorithm_directory_failed:" + ec.message(); return false; }
    GpuAnalysisStoreManifest manifest;
    std::filesystem::path staging;

    // Recover the newest compatible incomplete generation. Every page is
    // atomically published and checksummed before it enters this checkpoint,
    // so a cancelled/crashed builder only repeats the uncommitted page.
    std::vector<std::filesystem::directory_entry> candidates;
    for( const auto& entry : std::filesystem::directory_iterator( GpuAnalysisIoPath( algorithmRoot ), ec ) )
        if( entry.is_directory() && entry.path().filename().string().ends_with( ".building" ) ) candidates.push_back( entry );
    std::sort( candidates.begin(), candidates.end(), []( const auto& lhs, const auto& rhs ) { return lhs.last_write_time() > rhs.last_write_time(); } );
    for( const auto& candidate : candidates )
    {
        std::string resumeError; auto saved = LoadStoreManifestImpl( candidate.path(), false, resumeError ); bool valid = bool( saved );
        if( valid ) valid = saved->traceSha256 == identity.sha256 && saved->traceSize == identity.fileSize &&
            saved->resourceCount == snapshot.resources.size() && saved->allocationCount == snapshot.allocations.size() &&
            saved->passCount == snapshot.passes.size() && saved->residencyCount == snapshot.residency.size() &&
            saved->churnCount == snapshot.churnCandidates.size();
        if( valid ) for( const auto& page : saved->pages ) if( !VerifyPageFile( candidate.path() / page.relativePath, page, resumeError ) ) { valid = false; break; }
        if( valid ) { manifest = std::move( *saved ); staging = candidate.path(); generation = manifest.generation; writtenBytes = manifest.totalBytes; break; }
        std::error_code ignored; std::filesystem::remove_all( GpuAnalysisIoPath( candidate.path() ), ignored );
    }
    if( staging.empty() )
    {
        for( unsigned attempt = 0; attempt < 256; ++attempt )
        {
            generation = GenerationName(); staging = algorithmRoot / ( generation + ".building" );
            if( std::filesystem::create_directory( GpuAnalysisIoPath( staging ), ec ) ) break;
            if( ec ) { error = "store_generation_directory_failed:" + ec.message(); return false; }
            staging.clear();
        }
        if( staging.empty() ) { error = "store_generation_name_exhausted"; return false; }
        manifest.generation = generation; manifest.traceSha256 = identity.sha256; manifest.traceSize = identity.fileSize;
        manifest.resourceCount = snapshot.resources.size(); manifest.allocationCount = snapshot.allocations.size();
        manifest.passCount = snapshot.passes.size(); manifest.residencyCount = snapshot.residency.size(); manifest.churnCount = snapshot.churnCandidates.size();
    }
    std::map<uint16_t, GpuAnalysisTypeSummary> typeSummaries;
    for( const auto& resource : snapshot.resources ) if( resource.aliveAtEnd )
    { auto& value = typeSummaries[resource.primaryKind]; value.primaryKind = resource.primaryKind; ++value.liveResourceCount; value.resourceCapacityBytes += resource.capacityBytes; }
    manifest.typeSummaries.clear(); for( const auto& [_, value] : typeSummaries ) manifest.typeSummaries.push_back( value );
    const auto committed = algorithmRoot / generation;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize, std::string( GpuAnalysisAlgorithmId ) + "-store1" };
    const auto progress = [&]( float value, const char* stage ) { if( control.progress ) control.progress( value, stage ); };
    const auto checkpoint = [&]() { manifest.totalBytes = writtenBytes; manifest.complete = false; return SaveStoreManifest( staging, manifest, error ); };

    if( CountPages( manifest, GpuAnalysisStorePageKind::Metadata ) == 0 )
    {
        auto overview = OverviewOf( snapshot ); const auto metadataPath = staging / "metadata.bin";
        if( !SaveGpuAnalysisCache( metadataPath, cacheIdentity, overview, error ) ) return false;
        uint64_t metadataBytes = 0; const auto metadataChecksum = FileChecksum( metadataPath, metadataBytes, error ); if( !error.empty() ) return false;
        manifest.pages.push_back( { GpuAnalysisStorePageKind::Metadata, 0, 1, 0, 0, metadataBytes, metadataChecksum,
            std::filesystem::relative( metadataPath, staging ) } ); writtenBytes += metadataBytes;
        if( !checkpoint() ) return false;
    }

    progress( .05f, "store-resources" );
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Resource, "resources", snapshot.resources, snapshot, cacheIdentity,
        ResourceBytes, []( const auto& value ) { return value.resourceId; }, manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
    progress( .22f, "store-allocations" );
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Allocation, "allocations", snapshot.allocations, snapshot, cacheIdentity,
        AllocationBytes, []( const auto& value ) { return value.allocationId; }, manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
    progress( .38f, "store-passes" );
    if( !WritePassPages( staging, snapshot.passes, manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Residency, "residency", snapshot.residency, snapshot, cacheIdentity,
        []( const auto& ) { return uint64_t( sizeof( GpuResidencyInterval ) ); }, []( const auto& value ) { return value.allocationId; },
        manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Churn, "churn", snapshot.churnCandidates, snapshot, cacheIdentity,
        ChurnBytes, []( const auto& value ) { return value.resourceId ? value.resourceId : value.allocationId; },
        manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;

    progress( .62f, "store-resource-pass-index" );
    std::vector<GpuAnalysisResourcePassEntry> resourcePass;
    uint64_t relationEstimate = 0; for( const auto& pass : snapshot.passes ) relationEstimate += pass.directResources.size() + pass.inclusiveResources.size();
    if( relationEstimate > uint64_t( std::numeric_limits<size_t>::max() ) ) { error = "store_relation_count_limit"; return false; }
    resourcePass.reserve( size_t( relationEstimate ) );
    for( const auto& pass : snapshot.passes )
    {
        for( const auto resourceId : pass.directResources ) resourcePass.push_back( { resourceId, pass.passId, 0, {} } );
        for( const auto resourceId : pass.inclusiveResources )
            if( !std::binary_search( pass.directResources.begin(), pass.directResources.end(), resourceId ) )
                resourcePass.push_back( { resourceId, pass.passId, 1, {} } );
    }
    std::sort( resourcePass.begin(), resourcePass.end(), []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId; return lhs.inclusive < rhs.inclusive;
    } );
    resourcePass.erase( std::unique( resourcePass.begin(), resourcePass.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.resourceId == rhs.resourceId && lhs.passId == rhs.passId && lhs.inclusive == rhs.inclusive;
    } ), resourcePass.end() );
    manifest.resourcePassRelationCount = resourcePass.size();
    if( !WriteRelationPages( staging, GpuAnalysisStorePageKind::ResourcePassIndex, "resource-pass", resourcePass,
        manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;

    progress( .80f, "store-frame-pass-index" );
    std::vector<GpuAnalysisFramePassEntry> framePass; framePass.reserve( snapshot.passes.size() );
    for( const auto& pass : snapshot.passes ) framePass.push_back( { pass.frameId, pass.passId } );
    manifest.framePassRelationCount = framePass.size();
    if( !WriteRelationPages( staging, GpuAnalysisStorePageKind::FramePassIndex, "frame-pass", framePass,
        manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;

    manifest.totalBytes = writtenBytes; manifest.complete = true; manifest.reason = snapshot.manifest.reason;
    if( !SaveStoreManifest( staging, manifest, error ) ) return false;
    if( !PublishGenerationDirectory( staging, committed, error ) ) return false;
    auto currentTmp = algorithmRoot / "current.tmp"; const auto current = algorithmRoot / "current";
    { std::ofstream out( GpuAnalysisIoPath( currentTmp ), std::ios::binary | std::ios::trunc ); out << generation << '\n'; }
    if( !AtomicReplace( currentTmp, current, error ) ) return false;

    // Keep the current and previous successful generation. Open/mapped older
    // generations are retired lazily when Windows refuses removal.
    std::vector<std::filesystem::directory_entry> generations;
    for( const auto& entry : std::filesystem::directory_iterator( GpuAnalysisIoPath( algorithmRoot ), ec ) )
        if( entry.is_directory() && entry.path().filename().string().find( ".building" ) == std::string::npos ) generations.push_back( entry );
    std::sort( generations.begin(), generations.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.last_write_time() > rhs.last_write_time();
    } );
    for( size_t i = 2; i < generations.size(); ++i ) { std::error_code ignored; std::filesystem::remove_all( GpuAnalysisIoPath( generations[i] ), ignored ); }
    progress( 1.f, "store-complete" ); return true;
}

template<typename T, typename Compare>
class MergedRunCursor
{
    struct Cursor { std::ifstream in; T value {}; };
    struct Node { T value {}; size_t run = 0; };
    struct Later
    {
        Compare compare;
        bool operator()( const Node& lhs, const Node& rhs ) const
        { return compare( rhs.value, lhs.value ); }
    };
public:
    explicit MergedRunCursor( Compare compare )
        : m_compare( std::move( compare ) ), m_heap( Later { m_compare } ) {}

    bool Open( const std::vector<std::filesystem::path>& runs, std::string& error )
    {
        m_cursors.resize( runs.size() );
        for( size_t i = 0; i < runs.size(); ++i )
        {
            auto& cursor = m_cursors[i];
            cursor.in.open( GpuAnalysisIoPath( runs[i] ), std::ios::binary );
            if( !cursor.in ) { error = "store_enrichment_run_open_failed"; return false; }
            if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) )
                m_heap.push( { cursor.value, i } );
            else if( !cursor.in.eof() )
            { error = "store_enrichment_run_read_failed"; return false; }
        }
        return true;
    }

    bool Empty() const { return m_heap.empty(); }
    const T& Front() const { return m_heap.top().value; }
    bool Pop( T& value, std::string& error )
    {
        if( m_heap.empty() ) return false;
        const auto node = m_heap.top(); m_heap.pop(); value = node.value;
        auto& cursor = m_cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) )
            m_heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() )
        { error = "store_enrichment_run_read_failed"; return false; }
        return true;
    }
private:
    Compare m_compare;
    std::vector<Cursor> m_cursors;
    std::priority_queue<Node, std::vector<Node>, Later> m_heap;
};

GpuAnalysisResourceSummary ResourceSummaryOf( const GpuResourceAnalysisRecord& value,
    uint64_t allocationResourceCount )
{
    GpuAnalysisResourceSummary result;
    result.generation = value.generation;
    result.resourceId = value.resourceId;
    result.allocationId = value.allocationId;
    result.capacityBytes = value.capacityBytes;
    result.allocationOffsetBytes = value.allocationOffsetBytes;
    result.createTime = value.createTime;
    result.destroyTime = value.destroyTime;
    result.nameHash = value.nameHash;
    result.createCallsiteId = value.createCallsiteId;
    result.definitionRevision = value.definitionRevision;
    result.nameOriginalLength = value.nameOriginalLength;
    result.viewCount = value.views.size();
    result.logicalBindingCount = value.logicals.size();
    result.partCount = value.parts.size();
    result.rangeCount = value.ranges.size();
    result.relationCount = value.relations.size();
    result.vgRecordCount = value.virtualGeometry.size();
    result.primaryKind = value.primaryKind;
    result.resourceClass = value.resourceClass;
    result.memoryDomain = value.memoryDomain;
    result.allocationKind = value.allocationKind;
    result.nameProvenance = value.nameProvenance;
    result.stackProvenance = value.stackProvenance;
    result.exactness = value.exactness;
    result.openBoundary = value.openBoundary;
    result.aliveAtEnd = value.aliveAtEnd;
    result.name = value.name;
    result.hasAliasGroup = std::any_of( value.logicals.begin(), value.logicals.end(),
        []( const auto& item ) { return item.value.aliasGroupId != 0; } );
    result.allocationResourceCount = allocationResourceCount;
    return result;
}

bool WriteResourceSummaryPage( const std::filesystem::path& root,
    const std::vector<GpuAnalysisResourceSummary>& values, uint64_t firstIndex,
    uint32_t pageIndex, std::vector<GpuAnalysisStorePage>& pages,
    uint64_t& totalBytes, const GpuAnalysisSidecarControl& control,
    const std::function<bool()>& checkpoint, std::string& error )
{
    if( values.empty() ) return true;
    if( const auto* existing = NthPage( pages,
        GpuAnalysisStorePageKind::ResourceSummary, pageIndex ) )
    {
        if( existing->firstIndex != firstIndex ||
            existing->recordCount != values.size() ||
            existing->firstKey != values.front().resourceId ||
            existing->lastKey != values.back().resourceId )
        { error = "store_resume_resource_summary_page_mismatch"; return false; }
        return true;
    }
    if( values.size() > std::numeric_limits<uint32_t>::max() )
    { error = "store_resource_summary_record_count_overflow"; return false; }
    std::vector<uint8_t> payload;
    payload.reserve( values.size() * sizeof( ResourceSummaryPageRecord ) );
    for( const auto& value : values )
    {
        if( value.name.size() > std::numeric_limits<uint32_t>::max() )
        { error = "store_resource_summary_name_too_large"; return false; }
        ResourceSummaryPageRecord record;
        record.generation = value.generation;
        record.resourceId = value.resourceId;
        record.allocationId = value.allocationId;
        record.capacityBytes = value.capacityBytes;
        record.allocationOffsetBytes = value.allocationOffsetBytes;
        record.createTime = value.createTime;
        record.destroyTime = value.destroyTime;
        record.nameHash = value.nameHash;
        record.viewCount = value.viewCount;
        record.logicalBindingCount = value.logicalBindingCount;
        record.partCount = value.partCount;
        record.rangeCount = value.rangeCount;
        record.relationCount = value.relationCount;
        record.vgRecordCount = value.vgRecordCount;
        record.allocationResourceCount = value.allocationResourceCount;
        record.createCallsiteId = value.createCallsiteId;
        record.definitionRevision = value.definitionRevision;
        record.nameOriginalLength = value.nameOriginalLength;
        record.nameBytes = uint32_t( value.name.size() );
        record.primaryKind = value.primaryKind;
        record.resourceClass = value.resourceClass;
        record.memoryDomain = value.memoryDomain;
        record.allocationKind = value.allocationKind;
        record.nameProvenance = value.nameProvenance;
        record.stackProvenance = value.stackProvenance;
        record.exactness = value.exactness;
        record.openBoundary = value.openBoundary;
        record.aliveAtEnd = value.aliveAtEnd;
        record.hasAliasGroup = value.hasAliasGroup;
        AppendPod( payload, record );
        AppendBytes( payload, value.name.data(), value.name.size() );
    }
    ResourceSummaryPageHeader header;
    header.recordCount = uint32_t( values.size() );
    header.payloadBytes = payload.size();
    header.checksum = HashUpdate( FnvOffset, payload.data(), payload.size() );
    std::error_code ec;
    const auto pageRoot = root / "resource-summary";
    std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_resource_summary_directory_failed:" + ec.message(); return false; }
    std::ostringstream fileName;
    fileName << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
    const auto path = pageRoot / fileName.str(); auto temporary = path; temporary += ".tmp";
    {
        std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
        if( !out ) { error = "store_resource_summary_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( reinterpret_cast<const char*>( payload.data() ), std::streamsize( payload.size() ) );
        out.flush();
        if( !out ) { error = "store_resource_summary_write_failed"; return false; }
    }
    if( !AtomicReplace( temporary, path, error ) ) return false;
    uint64_t fileBytes = 0;
    const auto checksum = FileChecksum( path, fileBytes, error );
    if( !error.empty() ) return false;
    if( totalBytes > control.maximumSidecarBytes - fileBytes )
    { error = "sidecar_size_limit"; return false; }
    pages.push_back( { GpuAnalysisStorePageKind::ResourceSummary, firstIndex,
        uint64_t( values.size() ), values.front().resourceId, values.back().resourceId,
        fileBytes, checksum, std::filesystem::relative( path, root ) } );
    totalBytes += fileBytes;
    return checkpoint();
}

bool WriteEnrichedResourcePages( const std::filesystem::path& root,
    const GpuAnalysisSnapshot& catalogSnapshot, const GpuAnalysisPassSpool& passSpool,
    const std::vector<JnGpuCatalogStringData>& catalogStrings,
    const GpuAnalysisCacheIdentity& identity, std::vector<GpuAnalysisStorePage>& pages,
    uint64_t& totalBytes, const GpuAnalysisSidecarControl& control,
    const std::function<bool()>& checkpoint, std::string& error )
{
    const auto logicalCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        if( lhs.record.logicalResourceId != rhs.record.logicalResourceId )
            return lhs.record.logicalResourceId < rhs.record.logicalResourceId;
        return lhs.generation < rhs.generation;
    };
    const auto relationCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        if( lhs.record.sourceId != rhs.record.sourceId ) return lhs.record.sourceId < rhs.record.sourceId;
        if( lhs.record.targetId != rhs.record.targetId ) return lhs.record.targetId < rhs.record.targetId;
        return lhs.generation < rhs.generation;
    };
    MergedRunCursor<GpuAnalysisLogicalStoreEntry, decltype( logicalCompare )> logicals( logicalCompare );
    MergedRunCursor<GpuAnalysisCatalogRelationStoreEntry, decltype( relationCompare )> relations( relationCompare );
    if( !logicals.Open( passSpool.logicalRuns, error ) ||
        !relations.Open( passSpool.catalogRelationRuns, error ) ) return false;

    struct StringKey
    {
        uint64_t generation = 0; uint32_t id = 0;
        bool operator==( const StringKey& rhs ) const
        { return generation == rhs.generation && id == rhs.id; }
    };
    struct StringHash
    {
        size_t operator()( const StringKey& value ) const
        { return size_t( value.generation ^ ( uint64_t( value.id ) * 0x9e3779b97f4a7c15ull ) ); }
    };
    std::unordered_map<StringKey, const std::string*, StringHash> strings;
    strings.reserve( catalogStrings.size() );
    for( const auto& value : catalogStrings )
        strings.emplace( StringKey { value.generation, value.header.stringId }, &value.value );
    const auto stringFor = [&]( uint64_t generation, uint32_t id ) -> std::string {
        if( id == 0 ) return {};
        const auto found = strings.find( { generation, id } );
        return found == strings.end() ? std::string() : *found->second;
    };

    std::vector<size_t> order( catalogSnapshot.resources.size() );
    std::iota( order.begin(), order.end(), size_t( 0 ) );
    std::stable_sort( order.begin(), order.end(), [&]( size_t lhs, size_t rhs ) {
        return catalogSnapshot.resources[lhs].resourceId < catalogSnapshot.resources[rhs].resourceId;
    } );
    std::error_code ec; const auto pageRoot = root / "resources";
    std::filesystem::create_directories( GpuAnalysisIoPath( pageRoot ), ec );
    if( ec ) { error = "store_resource_directory_failed:" + ec.message(); return false; }
    uint64_t logicalCount = 0, relationCount = 0;
    size_t first = 0; uint32_t pageIndex = 0;
    while( first < order.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        GpuAnalysisSnapshot page = OverviewOf( catalogSnapshot );
        const auto begin = first; uint64_t estimated = 0;
        while( first < order.size() && ( first == begin || estimated < control.targetDerivedPageBytes ) )
        {
            const auto& resource = catalogSnapshot.resources[order[first++]];
            estimated += ResourceBytes( resource ); page.resources.push_back( resource );
        }
        const auto firstKey = page.resources.front().resourceId;
        const auto lastKey = page.resources.back().resourceId;
        const auto findResource = [&]( uint64_t id ) -> GpuResourceAnalysisRecord* {
            const auto found = std::lower_bound( page.resources.begin(), page.resources.end(), id,
                []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
            return found == page.resources.end() || found->resourceId != id ? nullptr : &*found;
        };
        while( !logicals.Empty() && logicals.Front().resourceId <= lastKey )
        {
            GpuAnalysisLogicalStoreEntry value;
            if( !logicals.Pop( value, error ) ) return false;
            auto* resource = findResource( value.resourceId );
            if( !resource || value.resourceId < firstKey )
            { error = "store_logical_resource_missing"; return false; }
            resource->logicals.push_back( { value.generation, value.record,
                stringFor( value.nameGeneration, value.record.nameId ) } );
            ++logicalCount;
        }
        while( !relations.Empty() && relations.Front().resourceId <= lastKey )
        {
            GpuAnalysisCatalogRelationStoreEntry value;
            if( !relations.Pop( value, error ) ) return false;
            auto* resource = findResource( value.resourceId );
            if( !resource || value.resourceId < firstKey )
            { error = "store_catalog_relation_resource_missing"; return false; }
            resource->relations.push_back( { value.generation, value.record } );
            ++relationCount;
        }
        const auto committedPageIndex = pageIndex++;
        std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' )
            << committedPageIndex << ".bin";
        const auto path = pageRoot / fileName.str();
        if( !SaveGpuAnalysisCache( path, identity, page, error ) ) return false;
        uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error );
        if( !error.empty() ) return false;
        if( totalBytes > control.maximumSidecarBytes - fileBytes )
        { error = "sidecar_size_limit"; return false; }
        pages.push_back( { GpuAnalysisStorePageKind::Resource, begin, page.resources.size(),
            firstKey, lastKey, fileBytes, checksum, std::filesystem::relative( path, root ) } );
        totalBytes += fileBytes; if( !checkpoint() ) return false;
        std::vector<GpuAnalysisResourceSummary> summaries;
        summaries.reserve( page.resources.size() );
        for( const auto& resource : page.resources )
        {
            const auto* allocation = catalogSnapshot.FindAllocation( resource.allocationId );
            summaries.push_back( ResourceSummaryOf( resource,
                allocation ? allocation->resources.size() : 0 ) );
        }
        if( !WriteResourceSummaryPage( root, summaries, begin, committedPageIndex,
            pages, totalBytes, control, checkpoint, error ) ) return false;
        if( control.progress ) control.progress( 0.f, "store-resource-enrichment-page" );
    }
    if( !logicals.Empty() || logicalCount != passSpool.logicalCount )
    { error = "store_logical_count_mismatch"; return false; }
    if( !relations.Empty() || relationCount != passSpool.catalogRelationCount )
    { error = "store_catalog_relation_count_mismatch"; return false; }
    return true;
}

bool WriteGpuAnalysisDerivedStoreFromPassSpoolAt(
    const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisSnapshot& catalogSnapshot,
    const GpuAnalysisPassSpool& passSpool,
    const std::vector<JnGpuCatalogStringData>& catalogStrings,
    const GpuAnalysisSidecarControl& control,
    std::string& generation, uint64_t& writtenBytes, std::string& error )
{
    error.clear(); writtenBytes = 0;
    if( !catalogSnapshot.passes.empty() )
    { error = "store_catalog_snapshot_contains_passes"; return false; }
    std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( algorithmRoot ), ec );
    if( ec ) { error = "store_algorithm_directory_failed:" + ec.message(); return false; }
    std::filesystem::path staging;
    for( unsigned attempt = 0; attempt < 256; ++attempt )
    {
        generation = GenerationName(); staging = algorithmRoot / ( generation + ".building" );
        if( std::filesystem::create_directory( GpuAnalysisIoPath( staging ), ec ) ) break;
        if( ec ) { error = "store_generation_directory_failed:" + ec.message(); return false; }
        staging.clear();
    }
    if( staging.empty() ) { error = "store_generation_name_exhausted"; return false; }

    GpuAnalysisStoreManifest manifest;
    manifest.generation = generation; manifest.traceSha256 = identity.sha256;
    manifest.traceSize = identity.fileSize;
    manifest.resourceCount = catalogSnapshot.resources.size();
    manifest.allocationCount = catalogSnapshot.allocations.size();
    manifest.passCount = passSpool.passCount;
    manifest.residencyCount = catalogSnapshot.residency.size();
    manifest.churnCount = catalogSnapshot.churnCandidates.size();
    manifest.logicalCount = passSpool.logicalCount;
    manifest.catalogRelationCount = passSpool.catalogRelationCount;
    manifest.sourceGapResourceCount = passSpool.sourceGapResourceCount;
    manifest.sourceGapReferenceCount = passSpool.sourceGapReferenceCount;
    std::map<uint16_t, GpuAnalysisTypeSummary> typeSummaries;
    for( const auto& resource : catalogSnapshot.resources ) if( resource.aliveAtEnd )
    { auto& value = typeSummaries[resource.primaryKind]; value.primaryKind = resource.primaryKind;
      ++value.liveResourceCount; value.resourceCapacityBytes += resource.capacityBytes; }
    for( const auto& [_, value] : typeSummaries ) manifest.typeSummaries.push_back( value );
    const auto committed = algorithmRoot / generation;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-store1" };
    const GpuAnalysisCacheIdentity spoolIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-pass-spool1" };
    const auto checkpoint = [&]() {
        manifest.totalBytes = writtenBytes; manifest.complete = false;
        return SaveStoreManifest( staging, manifest, error );
    };

    auto overview = OverviewOf( catalogSnapshot );
    const auto metadataPath = staging / "metadata.bin";
    if( !SaveGpuAnalysisCache( metadataPath, cacheIdentity, overview, error ) ) return false;
    uint64_t metadataBytes = 0;
    const auto metadataChecksum = FileChecksum( metadataPath, metadataBytes, error );
    if( !error.empty() ) return false;
    manifest.pages.push_back( { GpuAnalysisStorePageKind::Metadata, 0, 1, 0, 0,
        metadataBytes, metadataChecksum, std::filesystem::relative( metadataPath, staging ) } );
    writtenBytes += metadataBytes; if( !checkpoint() ) return false;

    if( !WriteEnrichedResourcePages( staging, catalogSnapshot, passSpool,
        catalogStrings, cacheIdentity, manifest.pages, writtenBytes, control,
        checkpoint, error ) ) return false;
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Allocation, "allocations",
        catalogSnapshot.allocations, catalogSnapshot, cacheIdentity, AllocationBytes,
        []( const auto& value ) { return value.allocationId; }, manifest.pages,
        writtenBytes, control, checkpoint, error ) ) return false;
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Residency, "residency",
        catalogSnapshot.residency, catalogSnapshot, cacheIdentity,
        []( const auto& ) { return uint64_t( sizeof( GpuResidencyInterval ) ); },
        []( const auto& value ) { return value.allocationId; }, manifest.pages,
        writtenBytes, control, checkpoint, error ) ) return false;
    if( !WriteCachePages( staging, GpuAnalysisStorePageKind::Churn, "churn",
        catalogSnapshot.churnCandidates, catalogSnapshot, cacheIdentity, ChurnBytes,
        []( const auto& value ) { return value.resourceId ? value.resourceId : value.allocationId; },
        manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;

    const auto runRoot = staging / "relation-runs";
    std::filesystem::create_directories( GpuAnalysisIoPath( runRoot ), ec );
    if( ec ) { error = "store_relation_run_directory_failed:" + ec.message(); return false; }
    const auto taxonomyCompare = []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    };
    MergedRunCursor<GpuAnalysisPassTaxonomyEntry, decltype( taxonomyCompare )>
        taxonomy( taxonomyCompare );
    if( !taxonomy.Open( passSpool.taxonomyRuns, error ) ) return false;
    uint64_t consumedTaxonomy = 0;
    std::vector<std::filesystem::path> resourceRuns, frameRuns, summaryRuns, childRuns;
    uint64_t passCount = 0; uint64_t previousPassId = 0;
    for( uint64_t pageIndex = 0; pageIndex < passSpool.pageCount; ++pageIndex )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
        const auto pagePath = passSpool.root / name.str();
        auto pageSnapshot = LoadGpuAnalysisCache( pagePath, spoolIdentity, error );
        if( !pageSnapshot ) { error = "store_spool_page_invalid:" + error; return false; }
        auto& passes = pageSnapshot->passes;
        if( !std::is_sorted( passes.begin(), passes.end(), []( const auto& lhs, const auto& rhs ) {
                return lhs.passId < rhs.passId;
            } ) || ( !passes.empty() && previousPassId != 0 && passes.front().passId <= previousPassId ) )
        { error = "store_spool_global_pass_order_invalid"; return false; }
        if( !passes.empty() ) previousPassId = passes.back().passId;
        if( !WriteSortedPassBatch( staging, passes, passCount, uint32_t( pageIndex ),
            manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
        passCount += passes.size();

        std::vector<GpuAnalysisResourcePassEntry> resourceRelations;
        std::vector<GpuAnalysisFramePassEntry> frameRelations;
        std::vector<GpuAnalysisPassChildEntry> childRelations;
        std::vector<GpuAnalysisPassSummary> summaries;
        uint64_t relationCount = 0;
        for( const auto& pass : passes )
            relationCount += pass.directResources.size() + pass.inclusiveResources.size();
        if( relationCount > std::numeric_limits<size_t>::max() )
        { error = "store_spool_relation_count_limit"; return false; }
        resourceRelations.reserve( size_t( relationCount ) ); frameRelations.reserve( passes.size() );
        summaries.reserve( passes.size() );
        for( const auto& pass : passes )
        {
            GpuAnalysisPassTaxonomyEntry taxonomyValue;
            const GpuAnalysisPassTaxonomyEntry* taxonomyPtr = nullptr;
            if( passSpool.taxonomyCount != 0 )
            {
                if( taxonomy.Empty() || taxonomy.Front().passId != pass.passId ||
                    !taxonomy.Pop( taxonomyValue, error ) || taxonomyValue.frameId != pass.frameId )
                { error = "store_pass_taxonomy_order_mismatch"; return false; }
                taxonomyPtr = &taxonomyValue; ++consumedTaxonomy;
            }
            for( const auto resourceId : pass.directResources )
                resourceRelations.push_back( { resourceId, pass.passId, 0, {} } );
            for( const auto resourceId : pass.inclusiveResources )
                if( !std::binary_search( pass.directResources.begin(),
                    pass.directResources.end(), resourceId ) )
                    resourceRelations.push_back( { resourceId, pass.passId, 1, {} } );
            frameRelations.push_back( { pass.frameId, pass.passId } );
            if( pass.parentPassId != 0 )
                childRelations.push_back( { pass.parentPassId, pass.passId } );
            summaries.push_back( PassSummaryOf( pass, taxonomyPtr ) );
        }
        const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
            if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
            if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
            return lhs.inclusive < rhs.inclusive;
        };
        const auto frameCompare = []( const auto& lhs, const auto& rhs ) {
            if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId;
            return lhs.passId < rhs.passId;
        };
        auto resourceRun = runRoot / ( "resource-" + name.str() );
        auto frameRun = runRoot / ( "frame-" + name.str() );
        auto summaryRun = runRoot / ( "summary-" + name.str() );
        auto childRun = runRoot / ( "child-" + name.str() );
        if( !WriteSortedRun( resourceRun, resourceRelations, resourceCompare, error ) ||
            !WriteSortedRun( frameRun, frameRelations, frameCompare, error ) ||
            !WriteSortedRun( summaryRun, summaries,
                []( const auto& lhs, const auto& rhs ) { return lhs.passId < rhs.passId; }, error ) ||
            !WriteSortedRun( childRun, childRelations, []( const auto& lhs, const auto& rhs ) {
                if( lhs.parentPassId != rhs.parentPassId ) return lhs.parentPassId < rhs.parentPassId;
                return lhs.childPassId < rhs.childPassId;
            }, error ) ) return false;
        resourceRuns.push_back( std::move( resourceRun ) );
        frameRuns.push_back( std::move( frameRun ) );
        summaryRuns.push_back( std::move( summaryRun ) );
        childRuns.push_back( std::move( childRun ) );
        if( control.progress ) control.progress( float( pageIndex + 1 ) /
            float( std::max<uint64_t>( 1, passSpool.pageCount ) ), "store-pass-spool" );
    }
    if( passCount != passSpool.passCount )
    { error = "store_spool_pass_count_mismatch"; return false; }
    if( consumedTaxonomy != passSpool.taxonomyCount || !taxonomy.Empty() )
    { error = "store_pass_taxonomy_count_mismatch"; return false; }

    const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
        return lhs.inclusive < rhs.inclusive;
    };
    const auto frameCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId;
        return lhs.passId < rhs.passId;
    };
    if( !MergeRelationRuns<GpuAnalysisResourcePassEntry>( staging,
        GpuAnalysisStorePageKind::ResourcePassIndex, "resource-pass", resourceRuns,
        resourceCompare, []( const auto& value ) { return value.resourceId; },
        manifest.pages, writtenBytes, control, checkpoint,
        manifest.resourcePassRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisFramePassEntry>( staging,
        GpuAnalysisStorePageKind::FramePassIndex, "frame-pass", frameRuns,
        frameCompare, []( const auto& value ) { return value.frameId; },
        manifest.pages, writtenBytes, control, checkpoint,
        manifest.framePassRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisPassSummary>( staging,
        GpuAnalysisStorePageKind::PassSummary, "pass-summary", summaryRuns,
        []( const auto& lhs, const auto& rhs ) { return lhs.passId < rhs.passId; },
        []( const auto& value ) { return value.passId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.passSummaryCount, error ) ) return false;
    if( manifest.passSummaryCount != manifest.passCount )
    { error = "store_pass_summary_count_mismatch"; return false; }
    if( !MergeRelationRuns<GpuAnalysisPassChildEntry>( staging,
        GpuAnalysisStorePageKind::PassChildIndex, "pass-child", childRuns,
        []( const auto& lhs, const auto& rhs ) {
            if( lhs.parentPassId != rhs.parentPassId ) return lhs.parentPassId < rhs.parentPassId;
            return lhs.childPassId < rhs.childPassId;
        }, []( const auto& value ) { return value.parentPassId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.passChildRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisStablePassSummary>( staging,
        GpuAnalysisStorePageKind::StablePassSummary, "stable-pass-summary",
        passSpool.stableSummaryRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId;
            return lhs.taxonomyId < rhs.taxonomyId;
        }, []( const auto& value ) { return value.frameId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.stablePassSummaryCount,
        error, false ) ) return false;
    if( manifest.stablePassSummaryCount != passSpool.stableSummaryCount )
    { error = "store_stable_pass_summary_count_mismatch"; return false; }
    const auto rangeCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.passInstanceId != rhs.record.passInstanceId )
            return lhs.record.passInstanceId < rhs.record.passInstanceId;
        if( lhs.record.offsetBytes != rhs.record.offsetBytes )
            return lhs.record.offsetBytes < rhs.record.offsetBytes;
        if( lhs.record.firstSubresource != rhs.record.firstSubresource )
            return lhs.record.firstSubresource < rhs.record.firstSubresource;
        return lhs.record.usageMask < rhs.record.usageMask;
    };
    if( !MergeRelationRuns<GpuAnalysisRangeStoreEntry>( staging,
        GpuAnalysisStorePageKind::Range, "ranges", passSpool.rangeRuns,
        rangeCompare, []( const auto& value ) { return value.resourceId; },
        manifest.pages, writtenBytes, control, checkpoint,
        manifest.rangeCount, error, false ) ) return false;
    if( manifest.rangeCount != passSpool.rangeCount )
    { error = "store_spool_range_count_mismatch"; return false; }
    for( const auto& path : resourceRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : frameRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : summaryRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : childRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : passSpool.rangeRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( runRoot ), ec );

    manifest.totalBytes = writtenBytes; manifest.complete = true;
    manifest.reason = catalogSnapshot.manifest.reason;
    if( !SaveStoreManifest( staging, manifest, error ) ) return false;
    if( !PublishGenerationDirectory( staging, committed, error ) ) return false;
    auto currentTmp = algorithmRoot / "current.tmp"; const auto current = algorithmRoot / "current";
    { std::ofstream out( GpuAnalysisIoPath( currentTmp ), std::ios::binary | std::ios::trunc );
      out << generation << '\n'; if( !out ) { error = "store_current_write_failed"; return false; } }
    if( !AtomicReplace( currentTmp, current, error ) ) return false;
    if( control.progress ) control.progress( 1.f, "store-complete" );
    return true;
}

bool WriteGpuAnalysisDerivedStoreFromCatalogAndPassSpoolsAt(
    const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisCatalogSpool& catalogSpool,
    const std::vector<GpuResourceAnalysisRecord>& appendedResources,
    const GpuAnalysisPassSpool& passSpool,
    const std::vector<JnGpuCatalogStringData>& catalogStrings,
    const GpuAnalysisSidecarControl& control,
    std::string& generation, uint64_t& writtenBytes, std::string& error,
    GpuAnalysisStoreWriteStats* writeStats )
{
    error.clear(); writtenBytes = 0;
    if( writeStats ) *writeStats = {};
    std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( algorithmRoot ), ec );
    if( ec ) { error = "store_algorithm_directory_failed:" + ec.message(); return false; }
    const auto expectedResourceCount = catalogSpool.resourceCount + appendedResources.size();
    std::filesystem::path staging;
    GpuAnalysisStoreManifest manifest;
    std::vector<std::filesystem::directory_entry> candidates;
    for( const auto& entry : std::filesystem::directory_iterator(
        GpuAnalysisIoPath( algorithmRoot ), ec ) )
        if( entry.is_directory() &&
            entry.path().filename().string().ends_with( ".building" ) )
            candidates.push_back( entry );
    if( ec ) { error = "store_generation_scan_failed:" + ec.message(); return false; }
    std::sort( candidates.begin(), candidates.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.last_write_time() > rhs.last_write_time();
    } );
    for( const auto& candidate : candidates )
    {
        std::string resumeError;
        auto saved = LoadStoreManifestImpl( candidate.path(), false, resumeError );
        bool valid = bool( saved ) && saved->traceSha256 == identity.sha256 &&
            saved->traceSize == identity.fileSize &&
            saved->resourceCount == expectedResourceCount &&
            saved->allocationCount == catalogSpool.allocationCount &&
            saved->passCount == passSpool.passCount &&
            saved->residencyCount == catalogSpool.residencyCount &&
            saved->churnCount == catalogSpool.churnCount &&
            saved->logicalCount == passSpool.logicalCount &&
            saved->catalogRelationCount == passSpool.catalogRelationCount &&
            saved->sourceGapResourceCount == passSpool.sourceGapResourceCount &&
            saved->sourceGapReferenceCount == passSpool.sourceGapReferenceCount;
        if( valid ) for( const auto& page : saved->pages )
            if( !VerifyPageFile( candidate.path() / page.relativePath,
                page, resumeError ) ) { valid = false; break; }
        if( valid )
        {
            manifest = std::move( *saved ); staging = candidate.path();
            generation = manifest.generation; writtenBytes = manifest.totalBytes;
            if( writeStats ) writeStats->resumedPages = manifest.pages.size();
            break;
        }
        std::error_code ignored;
        std::filesystem::remove_all( GpuAnalysisIoPath( candidate.path() ), ignored );
    }
    if( staging.empty() )
    {
        for( unsigned attempt = 0; attempt < 256; ++attempt )
        {
            ec.clear(); generation = GenerationName();
            staging = algorithmRoot / ( generation + ".building" );
            if( std::filesystem::create_directory( GpuAnalysisIoPath( staging ), ec ) ) break;
            if( ec ) { error = "store_generation_directory_failed:" + ec.message(); return false; }
            staging.clear();
        }
        if( staging.empty() ) { error = "store_generation_name_exhausted"; return false; }
        manifest.generation = generation; manifest.traceSha256 = identity.sha256;
        manifest.traceSize = identity.fileSize;
        manifest.resourceCount = expectedResourceCount;
        manifest.allocationCount = catalogSpool.allocationCount;
        manifest.passCount = passSpool.passCount;
        manifest.residencyCount = catalogSpool.residencyCount;
        manifest.churnCount = catalogSpool.churnCount;
        manifest.logicalCount = passSpool.logicalCount;
        manifest.catalogRelationCount = passSpool.catalogRelationCount;
        manifest.sourceGapResourceCount = passSpool.sourceGapResourceCount;
        manifest.sourceGapReferenceCount = passSpool.sourceGapReferenceCount;
        manifest.typeSummaries = catalogSpool.typeSummaries;
    }
    const auto committed = algorithmRoot / generation;
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-store1" };
    const GpuAnalysisCacheIdentity catalogIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-catalog-spool1" };
    const GpuAnalysisCacheIdentity passIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-pass-spool1" };
    const auto checkpoint = [&]() {
        manifest.totalBytes = writtenBytes; manifest.complete = false;
        if( !SaveStoreManifest( staging, manifest, error ) ) return false;
        if( writeStats ) writeStats->committedPages = manifest.pages.size();
        return true;
    };
    auto overview = catalogSpool.overview;
    overview.manifest.reason = catalogSpool.overview.manifest.reason;
    if( CountPages( manifest, GpuAnalysisStorePageKind::Metadata ) == 0 )
    {
        const auto metadataPath = staging / "metadata.bin";
        if( !SaveGpuAnalysisCache( metadataPath, cacheIdentity, overview, error ) ) return false;
        uint64_t metadataBytes = 0;
        const auto metadataChecksum = FileChecksum( metadataPath, metadataBytes, error );
        if( !error.empty() ) return false;
        manifest.pages.push_back( { GpuAnalysisStorePageKind::Metadata, 0, 1, 0, 0,
            metadataBytes, metadataChecksum, std::filesystem::relative( metadataPath, staging ) } );
        writtenBytes += metadataBytes; if( !checkpoint() ) return false;
    }

    struct StringKey { uint64_t generation; uint32_t id;
        bool operator==( const StringKey& rhs ) const { return generation == rhs.generation && id == rhs.id; } };
    struct StringHash { size_t operator()( const StringKey& value ) const
        { return size_t( value.generation ^ ( uint64_t( value.id ) * 0x9e3779b97f4a7c15ull ) ); } };
    std::unordered_map<StringKey, const std::string*, StringHash> strings;
    strings.reserve( catalogStrings.size() );
    for( const auto& value : catalogStrings ) strings.emplace(
        StringKey { value.generation, value.header.stringId }, &value.value );
    GpuAnalysisCatalogStringReader spoolStrings;
    if( !spoolStrings.Open( catalogSpool, error ) ) return false;
    const auto stringFor = [&]( uint64_t gen, uint32_t id ) -> std::string {
        const auto found = strings.find( { gen, id } );
        return found == strings.end() ? spoolStrings.Find( gen, id ) : *found->second;
    };
    const auto logicalCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        if( lhs.record.logicalResourceId != rhs.record.logicalResourceId )
            return lhs.record.logicalResourceId < rhs.record.logicalResourceId;
        return lhs.generation < rhs.generation;
    };
    const auto relationCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        if( lhs.record.sourceId != rhs.record.sourceId ) return lhs.record.sourceId < rhs.record.sourceId;
        if( lhs.record.targetId != rhs.record.targetId ) return lhs.record.targetId < rhs.record.targetId;
        return lhs.generation < rhs.generation;
    };
    MergedRunCursor<GpuAnalysisLogicalStoreEntry, decltype( logicalCompare )> logicals( logicalCompare );
    MergedRunCursor<GpuAnalysisCatalogRelationStoreEntry, decltype( relationCompare )> relations( relationCompare );
    const auto viewCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.value.resourceId != rhs.value.resourceId ) return lhs.value.resourceId < rhs.value.resourceId;
        if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
        return lhs.generation < rhs.generation;
    };
    const auto partCompare = viewCompare;
    const auto vgCompare = viewCompare;
    MergedRunCursor<GpuViewAnalysisRecord, decltype( viewCompare )> views( viewCompare );
    MergedRunCursor<GpuPartAnalysisRecord, decltype( partCompare )> parts( partCompare );
    MergedRunCursor<GpuVgAnalysisRecord, decltype( vgCompare )> virtualGeometry( vgCompare );
    if( !logicals.Open( passSpool.logicalRuns, error ) ||
        !relations.Open( passSpool.catalogRelationRuns, error ) ||
        !views.Open( passSpool.viewRuns, error ) ||
        !parts.Open( passSpool.partRuns, error ) ||
        !virtualGeometry.Open( passSpool.virtualGeometryRuns, error ) ) return false;

    GpuAnalysisCatalogAllocationLookupReader allocationLookup;
    if( !allocationLookup.Open( catalogSpool, error ) ) return false;

    const auto resourceRoot = staging / "resources";
    std::filesystem::create_directories( GpuAnalysisIoPath( resourceRoot ), ec );
    if( ec ) { error = "store_resource_directory_failed:" + ec.message(); return false; }
    uint64_t resourceIndex = 0, resourcePageIndex = 0, logicalCount = 0, relationCount = 0;
    uint64_t viewCount = 0, partCount = 0, vgCount = 0;
    const auto writeResourcePage = [&]( GpuAnalysisSnapshot& page ) -> bool {
        if( page.resources.empty() ) return true;
        const auto firstKey = page.resources.front().resourceId;
        const auto lastKey = page.resources.back().resourceId;
        const auto findResource = [&]( uint64_t id ) -> GpuResourceAnalysisRecord* {
            const auto found = std::lower_bound( page.resources.begin(), page.resources.end(), id,
                []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
            return found == page.resources.end() || found->resourceId != id ? nullptr : &*found;
        };
        while( !logicals.Empty() && logicals.Front().resourceId <= lastKey )
        {
            GpuAnalysisLogicalStoreEntry value; if( !logicals.Pop( value, error ) ) return false;
            auto* resource = findResource( value.resourceId );
            if( !resource || value.resourceId < firstKey ) { error = "store_logical_resource_missing"; return false; }
            resource->logicals.push_back( { value.generation, value.record,
                stringFor( value.nameGeneration, value.record.nameId ) } ); ++logicalCount;
        }
        while( !relations.Empty() && relations.Front().resourceId <= lastKey )
        {
            GpuAnalysisCatalogRelationStoreEntry value; if( !relations.Pop( value, error ) ) return false;
            auto* resource = findResource( value.resourceId );
            if( !resource || value.resourceId < firstKey ) { error = "store_catalog_relation_resource_missing"; return false; }
            resource->relations.push_back( { value.generation, value.record } ); ++relationCount;
        }
        while( !views.Empty() && views.Front().value.resourceId <= lastKey )
        {
            GpuViewAnalysisRecord value; if( !views.Pop( value, error ) ) return false;
            auto* resource = findResource( value.value.resourceId );
            if( !resource || value.value.resourceId < firstKey ) { error = "store_view_resource_missing"; return false; }
            resource->views.push_back( value ); ++viewCount;
        }
        while( !parts.Empty() && parts.Front().value.resourceId <= lastKey )
        {
            GpuPartAnalysisRecord value; if( !parts.Pop( value, error ) ) return false;
            auto* resource = findResource( value.value.resourceId );
            if( !resource || value.value.resourceId < firstKey ) { error = "store_part_resource_missing"; return false; }
            resource->parts.push_back( value ); ++partCount;
        }
        while( !virtualGeometry.Empty() && virtualGeometry.Front().value.resourceId <= lastKey )
        {
            GpuVgAnalysisRecord value; if( !virtualGeometry.Pop( value, error ) ) return false;
            auto* resource = findResource( value.value.resourceId );
            if( !resource || value.value.resourceId < firstKey ) { error = "store_vg_resource_missing"; return false; }
            resource->virtualGeometry.push_back( value ); ++vgCount;
        }
        const auto* existing = NthPage( manifest.pages,
            GpuAnalysisStorePageKind::Resource, size_t( resourcePageIndex ) );
        if( existing )
        {
            if( existing->firstIndex != resourceIndex ||
                existing->recordCount != page.resources.size() ||
                existing->firstKey != firstKey || existing->lastKey != lastKey )
            { error = "store_resume_resource_page_mismatch"; return false; }
        }
        else
        {
            std::ostringstream fileName; fileName << std::setw( 6 ) << std::setfill( '0' )
                << resourcePageIndex << ".bin";
            const auto path = resourceRoot / fileName.str();
            if( !SaveGpuAnalysisCache( path, cacheIdentity, page, error ) ) return false;
            uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error );
            if( !error.empty() ) return false;
            manifest.pages.push_back( { GpuAnalysisStorePageKind::Resource, resourceIndex,
                page.resources.size(), firstKey, lastKey, fileBytes, checksum,
                std::filesystem::relative( path, staging ) } );
            writtenBytes += fileBytes; if( !checkpoint() ) return false;
        }
        std::vector<GpuAnalysisResourceSummary> summaries; summaries.reserve( page.resources.size() );
        for( const auto& resource : page.resources ) summaries.push_back(
            ResourceSummaryOf( resource, allocationLookup.ResourceCount( resource.allocationId ) ) );
        if( !WriteResourceSummaryPage( staging, summaries, resourceIndex,
            uint32_t( resourcePageIndex ), manifest.pages, writtenBytes, control,
            checkpoint, error ) ) return false;
        resourceIndex += page.resources.size(); ++resourcePageIndex; return true;
    };
    for( const auto& sourcePage : catalogSpool.pages )
    {
        if( sourcePage.kind != GpuAnalysisStorePageKind::Resource ) continue;
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        auto page = LoadGpuAnalysisCache( catalogSpool.root / sourcePage.relativePath,
            catalogIdentity, error );
        if( !page ) { error = "store_catalog_resource_page_invalid:" + error; return false; }
        if( !writeResourcePage( *page ) ) return false;
    }
    if( !appendedResources.empty() )
    {
        GpuAnalysisSnapshot page = overview; page.resources = appendedResources;
        std::sort( page.resources.begin(), page.resources.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.resourceId < rhs.resourceId;
        } );
        if( !writeResourcePage( page ) ) return false;
    }
    if( !logicals.Empty() || logicalCount != passSpool.logicalCount )
    { error = "store_logical_count_mismatch"; return false; }
    if( !relations.Empty() || relationCount != passSpool.catalogRelationCount )
    { error = "store_catalog_relation_count_mismatch"; return false; }
    if( !views.Empty() || viewCount != passSpool.viewCount )
    { error = "store_view_count_mismatch"; return false; }
    if( !parts.Empty() || partCount != passSpool.partCount )
    { error = "store_part_count_mismatch"; return false; }
    if( !virtualGeometry.Empty() || vgCount != passSpool.virtualGeometryCount )
    { error = "store_vg_count_mismatch"; return false; }

    const auto copyCatalogPages = [&]( GpuAnalysisStorePageKind kind, const char* directory ) -> bool {
        uint64_t firstIndex = 0; uint64_t pageIndex = 0;
        const auto root = staging / directory;
        std::filesystem::create_directories( GpuAnalysisIoPath( root ), ec );
        if( ec ) { error = "store_catalog_copy_directory_failed:" + ec.message(); return false; }
        for( const auto& sourcePage : catalogSpool.pages )
        {
            if( sourcePage.kind != kind ) continue;
            auto snapshot = LoadGpuAnalysisCache( catalogSpool.root / sourcePage.relativePath,
                catalogIdentity, error );
            if( !snapshot ) { error = "store_catalog_page_invalid:" + error; return false; }
            const auto* existing = NthPage( manifest.pages, kind, size_t( pageIndex ) );
            if( existing )
            {
                if( existing->firstIndex != firstIndex ||
                    existing->recordCount != sourcePage.recordCount ||
                    existing->firstKey != sourcePage.firstKey ||
                    existing->lastKey != sourcePage.lastKey )
                { error = "store_resume_catalog_page_mismatch"; return false; }
            }
            else
            {
                std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
                const auto path = root / name.str();
                if( !SaveGpuAnalysisCache( path, cacheIdentity, *snapshot, error ) ) return false;
                uint64_t fileBytes = 0; const auto checksum = FileChecksum( path, fileBytes, error );
                if( !error.empty() ) return false;
                manifest.pages.push_back( { kind, firstIndex, sourcePage.recordCount,
                    sourcePage.firstKey, sourcePage.lastKey, fileBytes, checksum,
                    std::filesystem::relative( path, staging ) } );
                writtenBytes += fileBytes;
                if( !checkpoint() ) return false;
            }
            firstIndex += sourcePage.recordCount; ++pageIndex;
        }
        return true;
    };
    if( !copyCatalogPages( GpuAnalysisStorePageKind::Allocation, "allocations" ) ||
        !copyCatalogPages( GpuAnalysisStorePageKind::Residency, "residency" ) ||
        !copyCatalogPages( GpuAnalysisStorePageKind::Churn, "churn" ) ) return false;

    const auto runRoot = staging / "relation-runs";
    std::filesystem::create_directories( GpuAnalysisIoPath( runRoot ), ec );
    if( ec ) { error = "store_relation_run_directory_failed:" + ec.message(); return false; }
    const auto taxonomyCompare = []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    };
    MergedRunCursor<GpuAnalysisPassTaxonomyEntry, decltype( taxonomyCompare )>
        taxonomy( taxonomyCompare );
    if( !taxonomy.Open( passSpool.taxonomyRuns, error ) ) return false;
    uint64_t consumedTaxonomy = 0;
    std::vector<std::filesystem::path> resourceRuns, frameRuns, summaryRuns, childRuns;
    uint64_t passCount = 0; uint64_t previousPassId = 0;
    for( uint64_t pageIndex = 0; pageIndex < passSpool.pageCount; ++pageIndex )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
        auto pageSnapshot = LoadGpuAnalysisCache( passSpool.root / name.str(), passIdentity, error );
        if( !pageSnapshot ) { error = "store_spool_page_invalid:" + error; return false; }
        auto& passes = pageSnapshot->passes;
        if( !std::is_sorted( passes.begin(), passes.end(), []( const auto& lhs, const auto& rhs ) { return lhs.passId < rhs.passId; } ) ||
            ( !passes.empty() && previousPassId != 0 && passes.front().passId <= previousPassId ) )
        { error = "store_spool_global_pass_order_invalid"; return false; }
        if( !passes.empty() ) previousPassId = passes.back().passId;
        if( !WriteSortedPassBatch( staging, passes, passCount, uint32_t( pageIndex ),
            manifest.pages, writtenBytes, control, checkpoint, error ) ) return false;
        passCount += passes.size();
        std::vector<GpuAnalysisResourcePassEntry> resourceRelations;
        std::vector<GpuAnalysisFramePassEntry> frameRelations;
        std::vector<GpuAnalysisPassChildEntry> childRelations;
        std::vector<GpuAnalysisPassSummary> summaries;
        summaries.reserve( passes.size() );
        for( const auto& pass : passes )
        {
            GpuAnalysisPassTaxonomyEntry taxonomyValue;
            const GpuAnalysisPassTaxonomyEntry* taxonomyPtr = nullptr;
            if( passSpool.taxonomyCount != 0 )
            {
                if( taxonomy.Empty() || taxonomy.Front().passId != pass.passId ||
                    !taxonomy.Pop( taxonomyValue, error ) || taxonomyValue.frameId != pass.frameId )
                { error = "store_pass_taxonomy_order_mismatch"; return false; }
                taxonomyPtr = &taxonomyValue; ++consumedTaxonomy;
            }
            for( const auto id : pass.directResources ) resourceRelations.push_back( { id, pass.passId, 0, {} } );
            for( const auto id : pass.inclusiveResources )
                if( !std::binary_search( pass.directResources.begin(), pass.directResources.end(), id ) )
                    resourceRelations.push_back( { id, pass.passId, 1, {} } );
            frameRelations.push_back( { pass.frameId, pass.passId } );
            if( pass.parentPassId != 0 ) childRelations.push_back( { pass.parentPassId, pass.passId } );
            summaries.push_back( PassSummaryOf( pass, taxonomyPtr ) );
        }
        const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
            if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
            if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId; return lhs.inclusive < rhs.inclusive;
        };
        const auto frameCompare = []( const auto& lhs, const auto& rhs ) {
            if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId; return lhs.passId < rhs.passId;
        };
        auto resourceRun = runRoot / ( "resource-" + name.str() );
        auto frameRun = runRoot / ( "frame-" + name.str() );
        auto summaryRun = runRoot / ( "summary-" + name.str() );
        auto childRun = runRoot / ( "child-" + name.str() );
        if( !WriteSortedRun( resourceRun, resourceRelations, resourceCompare, error ) ||
            !WriteSortedRun( frameRun, frameRelations, frameCompare, error ) ||
            !WriteSortedRun( summaryRun, summaries,
                []( const auto& lhs, const auto& rhs ) { return lhs.passId < rhs.passId; }, error ) ||
            !WriteSortedRun( childRun, childRelations, []( const auto& lhs, const auto& rhs ) {
                if( lhs.parentPassId != rhs.parentPassId ) return lhs.parentPassId < rhs.parentPassId;
                return lhs.childPassId < rhs.childPassId;
            }, error ) ) return false;
        resourceRuns.push_back( resourceRun ); frameRuns.push_back( frameRun );
        summaryRuns.push_back( summaryRun );
        childRuns.push_back( childRun );
    }
    if( passCount != passSpool.passCount ) { error = "store_spool_pass_count_mismatch"; return false; }
    if( consumedTaxonomy != passSpool.taxonomyCount || !taxonomy.Empty() )
    { error = "store_pass_taxonomy_count_mismatch"; return false; }
    const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId; return lhs.inclusive < rhs.inclusive;
    };
    const auto frameCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId; return lhs.passId < rhs.passId;
    };
    if( !MergeRelationRuns<GpuAnalysisResourcePassEntry>( staging,
        GpuAnalysisStorePageKind::ResourcePassIndex, "resource-pass", resourceRuns,
        resourceCompare, []( const auto& value ) { return value.resourceId; },
        manifest.pages, writtenBytes, control, checkpoint,
        manifest.resourcePassRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisFramePassEntry>( staging,
        GpuAnalysisStorePageKind::FramePassIndex, "frame-pass", frameRuns,
        frameCompare, []( const auto& value ) { return value.frameId; },
        manifest.pages, writtenBytes, control, checkpoint,
        manifest.framePassRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisPassSummary>( staging,
        GpuAnalysisStorePageKind::PassSummary, "pass-summary", summaryRuns,
        []( const auto& lhs, const auto& rhs ) { return lhs.passId < rhs.passId; },
        []( const auto& value ) { return value.passId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.passSummaryCount, error ) ) return false;
    if( manifest.passSummaryCount != manifest.passCount )
    { error = "store_pass_summary_count_mismatch"; return false; }
    if( !MergeRelationRuns<GpuAnalysisPassChildEntry>( staging,
        GpuAnalysisStorePageKind::PassChildIndex, "pass-child", childRuns,
        []( const auto& lhs, const auto& rhs ) {
            if( lhs.parentPassId != rhs.parentPassId ) return lhs.parentPassId < rhs.parentPassId;
            return lhs.childPassId < rhs.childPassId;
        }, []( const auto& value ) { return value.parentPassId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.passChildRelationCount, error ) ) return false;
    if( !MergeRelationRuns<GpuAnalysisStablePassSummary>( staging,
        GpuAnalysisStorePageKind::StablePassSummary, "stable-pass-summary",
        passSpool.stableSummaryRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId;
            return lhs.taxonomyId < rhs.taxonomyId;
        }, []( const auto& value ) { return value.frameId; }, manifest.pages,
        writtenBytes, control, checkpoint, manifest.stablePassSummaryCount,
        error, false ) ) return false;
    if( manifest.stablePassSummaryCount != passSpool.stableSummaryCount )
    { error = "store_stable_pass_summary_count_mismatch"; return false; }
    const auto rangeCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.passInstanceId != rhs.record.passInstanceId ) return lhs.record.passInstanceId < rhs.record.passInstanceId;
        if( lhs.record.offsetBytes != rhs.record.offsetBytes ) return lhs.record.offsetBytes < rhs.record.offsetBytes;
        if( lhs.record.firstSubresource != rhs.record.firstSubresource ) return lhs.record.firstSubresource < rhs.record.firstSubresource;
        return lhs.record.usageMask < rhs.record.usageMask;
    };
    if( !MergeRelationRuns<GpuAnalysisRangeStoreEntry>( staging,
        GpuAnalysisStorePageKind::Range, "ranges", passSpool.rangeRuns,
        rangeCompare, []( const auto& value ) { return value.resourceId; },
        manifest.pages, writtenBytes, control, checkpoint, manifest.rangeCount, error, false ) ) return false;
    if( manifest.rangeCount != passSpool.rangeCount ) { error = "store_spool_range_count_mismatch"; return false; }
    for( const auto& path : resourceRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : frameRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : summaryRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    for( const auto& path : childRuns ) std::filesystem::remove( GpuAnalysisIoPath( path ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( runRoot ), ec );
    manifest.totalBytes = writtenBytes; manifest.complete = true;
    manifest.reason = catalogSpool.overview.manifest.reason;
    if( !SaveStoreManifest( staging, manifest, error ) ) return false;
    if( !PublishGenerationDirectory( staging, committed, error ) ) return false;
    auto currentTmp = algorithmRoot / "current.tmp"; const auto current = algorithmRoot / "current";
    { std::ofstream out( GpuAnalysisIoPath( currentTmp ), std::ios::binary | std::ios::trunc );
      out << generation << '\n'; if( !out ) { error = "store_current_write_failed"; return false; } }
    if( !AtomicReplace( currentTmp, current, error ) ) return false;
    if( writeStats ) writeStats->committedPages = manifest.pages.size();
    if( control.progress ) control.progress( 1.f, "store-complete" );
    return true;
}

bool BuildGpuAnalysisResourceSummariesAt( const std::filesystem::path& algorithmRoot,
    std::string_view expectedTraceSha256, uint64_t expectedTraceSize,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& logicalBytes, std::string& error )
{
    error.clear(); generation.clear(); logicalBytes = 0;
    auto source = GpuAnalysisStoreReader::OpenAt( algorithmRoot,
        expectedTraceSha256, expectedTraceSize, error );
    if( !source ) return false;
    uint64_t existingSummaries = 0;
    for( const auto& page : source->Manifest().pages )
        if( page.kind == GpuAnalysisStorePageKind::ResourceSummary )
            existingSummaries += page.recordCount;
    if( existingSummaries != 0 )
    {
        if( existingSummaries != source->Manifest().resourceCount )
        { error = "store_resource_summary_count_mismatch"; return false; }
        generation = source->Manifest().generation;
        logicalBytes = source->Manifest().totalBytes;
        if( control.progress ) control.progress( 1.f, "resource-summaries-reused" );
        return true;
    }

    std::error_code ec;
    std::filesystem::path staging;
    for( unsigned attempt = 0; attempt < 256; ++attempt )
    {
        generation = GenerationName(); staging = algorithmRoot / ( generation + ".building" );
        if( std::filesystem::create_directory( GpuAnalysisIoPath( staging ), ec ) ) break;
        if( ec ) { error = "store_generation_directory_failed:" + ec.message(); return false; }
        staging.clear();
    }
    if( staging.empty() ) { error = "store_generation_name_exhausted"; return false; }

    auto manifest = source->Manifest();
    const auto sourceRoot = algorithmRoot / manifest.generation;
    manifest.generation = generation;
    manifest.complete = false;
    manifest.pages.clear();
    logicalBytes = 0;
    for( const auto& page : source->Manifest().pages )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        if( page.kind == GpuAnalysisStorePageKind::ResourceSummary ) continue;
        const auto from = sourceRoot / page.relativePath;
        const auto to = staging / page.relativePath;
        std::filesystem::create_directories( GpuAnalysisIoPath( to.parent_path() ), ec );
        if( ec ) { error = "store_summary_link_directory_failed:" + ec.message(); return false; }
        std::filesystem::create_hard_link( GpuAnalysisIoPath( from ), GpuAnalysisIoPath( to ), ec );
        if( ec ) { error = "store_summary_hardlink_failed:" + ec.message(); return false; }
        manifest.pages.push_back( page );
        if( logicalBytes > control.maximumSidecarBytes - page.fileBytes )
        { error = "sidecar_size_limit"; return false; }
        logicalBytes += page.fileBytes;
    }
    const auto checkpoint = [&]() {
        manifest.totalBytes = logicalBytes;
        manifest.complete = false;
        return SaveStoreManifest( staging, manifest, error );
    };
    if( !checkpoint() ) return false;

    std::vector<std::pair<uint64_t, uint64_t>> allocationResourceCounts;
    allocationResourceCounts.reserve( size_t( std::min<uint64_t>(
        source->Manifest().allocationCount, std::numeric_limits<size_t>::max() ) ) );
    for( size_t pageIndex = 0; pageIndex < source->AllocationPageCount(); ++pageIndex )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::vector<GpuAllocationAnalysisRecord> allocations;
        if( !source->LoadAllocationPage( pageIndex, allocations, error ) ) return false;
        for( const auto& allocation : allocations )
            allocationResourceCounts.emplace_back( allocation.allocationId,
                uint64_t( allocation.resources.size() ) );
        if( control.progress ) control.progress( .15f * float( pageIndex + 1 ) /
            float( std::max<size_t>( 1, source->AllocationPageCount() ) ),
            "resource-summary-allocation-counts" );
    }
    std::sort( allocationResourceCounts.begin(), allocationResourceCounts.end() );
    const auto allocationCountFor = [&]( uint64_t allocationId ) -> uint64_t {
        const auto found = std::lower_bound( allocationResourceCounts.begin(),
            allocationResourceCounts.end(), allocationId,
            []( const auto& value, uint64_t id ) { return value.first < id; } );
        return found != allocationResourceCounts.end() && found->first == allocationId ?
            found->second : 0;
    };

    uint64_t summaryRecords = 0;
    size_t resourcePageIndex = 0;
    for( const auto& page : source->Manifest().pages )
    {
        if( page.kind != GpuAnalysisStorePageKind::Resource ) continue;
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::vector<GpuResourceAnalysisRecord> resources;
        if( !source->LoadResourcePage( resourcePageIndex, resources, error ) ) return false;
        if( resources.size() != page.recordCount )
        { error = "store_resource_summary_source_count_mismatch"; return false; }
        std::vector<GpuAnalysisResourceSummary> summaries;
        summaries.reserve( resources.size() );
        for( const auto& resource : resources )
            summaries.push_back( ResourceSummaryOf( resource,
                allocationCountFor( resource.allocationId ) ) );
        if( !WriteResourceSummaryPage( staging, summaries, page.firstIndex,
            uint32_t( resourcePageIndex ), manifest.pages, logicalBytes,
            control, checkpoint, error ) ) return false;
        summaryRecords += summaries.size();
        ++resourcePageIndex;
        if( control.progress ) control.progress( .15f + .8f * float( resourcePageIndex ) /
            float( std::max<size_t>( 1, source->ResourcePageCount() ) ),
            "resource-summary-pages" );
    }
    if( summaryRecords != manifest.resourceCount )
    { error = "store_resource_summary_count_mismatch"; return false; }
    manifest.totalBytes = logicalBytes;
    manifest.complete = true;
    if( !SaveStoreManifest( staging, manifest, error ) ) return false;
    const auto committed = algorithmRoot / generation;
    if( !PublishGenerationDirectory( staging, committed, error ) ) return false;
    auto currentTmp = algorithmRoot / "current.tmp";
    const auto current = algorithmRoot / "current";
    {
        std::ofstream out( GpuAnalysisIoPath( currentTmp ), std::ios::binary | std::ios::trunc );
        out << generation << '\n';
        if( !out ) { error = "store_current_write_failed"; return false; }
    }
    if( !AtomicReplace( currentTmp, current, error ) ) return false;
    if( control.progress ) control.progress( 1.f, "resource-summaries-complete" );
    return true;
}

std::optional<GpuAnalysisStoreManifest> LoadStoreManifestImpl(
    const std::filesystem::path& root, bool requireComplete, std::string& error )
{
    error.clear(); std::ifstream in( GpuAnalysisIoPath( root / "store-manifest" ), std::ios::binary );
    if( !in ) { error = "gpu_analysis_store_manifest_not_found"; return std::nullopt; }
    GpuAnalysisStoreManifest result; uint64_t magic = 0; size_t expectedPages = 0, expectedTypes = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic; else if( key == "schema" ) in >> result.schema;
        else if( key == "algorithm" ) in >> std::quoted( result.algorithmId ); else if( key == "generation" ) in >> std::quoted( result.generation );
        else if( key == "trace_sha256" ) in >> std::quoted( result.traceSha256 ); else if( key == "trace_size" ) in >> result.traceSize;
#define R( field ) else if( key == #field ) in >> result.field
        R( resourceCount ); R( allocationCount ); R( passCount ); R( passSummaryCount ); R( stablePassSummaryCount ); R( residencyCount ); R( churnCount );
        R( resourcePassRelationCount ); R( framePassRelationCount ); R( passChildRelationCount ); R( rangeCount );
        R( logicalCount ); R( catalogRelationCount ); R( sourceGapResourceCount );
        R( sourceGapReferenceCount ); R( totalBytes );
#undef R
        else if( key == "complete" ) in >> result.complete; else if( key == "reason" ) in >> std::quoted( result.reason );
        else if( key == "type_count" ) in >> expectedTypes;
        else if( key == "type" ) { GpuAnalysisTypeSummary value; in >> value.primaryKind >> value.liveResourceCount >> value.resourceCapacityBytes; result.typeSummaries.push_back( value ); }
        else if( key == "page_count" ) in >> expectedPages;
        else if( key == "page" )
        {
            unsigned kind = 0; std::string path; GpuAnalysisStorePage page;
            in >> kind >> page.firstIndex >> page.recordCount >> page.firstKey >> page.lastKey >> page.fileBytes >> page.checksum >> std::quoted( path );
            page.kind = GpuAnalysisStorePageKind( kind ); page.relativePath = std::filesystem::u8path( path ); result.pages.push_back( std::move( page ) );
        }
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "gpu_analysis_store_manifest_corrupt:" + key; return std::nullopt; }
    }
    if( magic != StoreManifestMagic || result.schema != GpuAnalysisStoreSchemaVersion || result.algorithmId != GpuAnalysisAlgorithmId ||
        result.pages.size() != expectedPages || result.typeSummaries.size() != expectedTypes || ( requireComplete && !result.complete ) )
    { error = "gpu_analysis_store_manifest_mismatch"; return std::nullopt; }
    return result;
}

std::optional<GpuAnalysisStoreManifest> LoadGpuAnalysisStoreManifest(
    const std::filesystem::path& root, std::string& error )
{
    return LoadStoreManifestImpl( root, true, error );
}

std::shared_ptr<GpuAnalysisStoreReader> GpuAnalysisStoreReader::Open( const std::filesystem::path& tracePath,
    bool strongIdentity, GpuAnalysisSidecarManifest* sidecarManifest, std::string& error )
{
    const auto sidecarPath = GpuAnalysisSidecarPath( tracePath );
    auto sidecar = LoadGpuAnalysisSidecarManifest( sidecarPath, error ); if( !sidecar ) return {};
    std::string identityReason; sidecar->identityState = VerifyGpuAnalysisIdentity( tracePath, sidecar->identity, strongIdentity, identityReason );
    if( sidecar->identityState == GpuAnalysisIdentityState::Mismatch ) { error = identityReason; return {}; }
    if( sidecarManifest ) *sidecarManifest = *sidecar;
    if( !sidecar->derivedComplete || sidecar->derivedGeneration.empty() ) { error = "gpu_analysis_derived_building"; return {}; }
    const auto root = GpuAnalysisDerivedPath( sidecarPath ) / sidecar->derivedGeneration;
    auto store = LoadGpuAnalysisStoreManifest( root, error ); if( !store ) return {};
    if( store->traceSha256 != sidecar->identity.sha256 || store->traceSize != sidecar->identity.fileSize )
    { error = "gpu_analysis_store_identity_mismatch"; return {}; }
    const auto* metadata = NthPage( *store, GpuAnalysisStorePageKind::Metadata, 0 );
    if( !metadata || !VerifyPageFile( root / metadata->relativePath, *metadata, error ) ) return {};
    GpuAnalysisCacheIdentity cacheIdentity { store->traceSha256, store->traceSize, std::string( GpuAnalysisAlgorithmId ) + "-store1" };
    auto overview = LoadGpuAnalysisCache( root / metadata->relativePath, cacheIdentity, error ); if( !overview ) return {};
    auto reader = std::shared_ptr<GpuAnalysisStoreReader>( new GpuAnalysisStoreReader );
    reader->m_root = root; reader->m_identity = std::move( cacheIdentity ); reader->m_manifest = std::move( *store ); reader->m_overview = std::move( *overview );
    reader->m_inclusiveCache = std::make_shared<GpuAnalysisStoreReader::InclusiveCache>();
    return reader;
}

std::shared_ptr<GpuAnalysisStoreReader> GpuAnalysisStoreReader::OpenAt(
    const std::filesystem::path& algorithmRoot, std::string_view expectedTraceSha256,
    uint64_t expectedTraceSize, std::string& error )
{
    error.clear();
    std::ifstream current( GpuAnalysisIoPath( algorithmRoot / "current" ), std::ios::binary );
    std::string generation;
    if( !current || !std::getline( current, generation ) || generation.empty() )
    { error = "gpu_analysis_derived_current_missing"; return {}; }
    if( generation.back() == '\r' ) generation.pop_back();
    if( generation.empty() || generation.find( '/' ) != std::string::npos ||
        generation.find( '\\' ) != std::string::npos || generation.find( ".." ) != std::string::npos )
    { error = "gpu_analysis_derived_current_invalid"; return {}; }
    const auto root = algorithmRoot / generation;
    auto store = LoadGpuAnalysisStoreManifest( root, error );
    if( !store ) return {};
    if( store->traceSha256 != expectedTraceSha256 || store->traceSize != expectedTraceSize )
    { error = "gpu_analysis_store_identity_mismatch"; return {}; }
    const auto* metadata = NthPage( *store, GpuAnalysisStorePageKind::Metadata, 0 );
    if( !metadata || !VerifyPageFile( root / metadata->relativePath, *metadata, error ) ) return {};
    GpuAnalysisCacheIdentity cacheIdentity { store->traceSha256, store->traceSize,
        std::string( GpuAnalysisAlgorithmId ) + "-store1" };
    auto overview = LoadGpuAnalysisCache( root / metadata->relativePath, cacheIdentity, error );
    if( !overview ) return {};
    auto reader = std::shared_ptr<GpuAnalysisStoreReader>( new GpuAnalysisStoreReader );
    reader->m_root = root;
    reader->m_identity = std::move( cacheIdentity );
    reader->m_manifest = std::move( *store );
    reader->m_overview = std::move( *overview );
    reader->m_inclusiveCache = std::make_shared<GpuAnalysisStoreReader::InclusiveCache>();
    return reader;
}

size_t GpuAnalysisStoreReader::ResourcePageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Resource ); }
size_t GpuAnalysisStoreReader::ResourceSummaryPageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::ResourceSummary ); }
size_t GpuAnalysisStoreReader::AllocationPageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Allocation ); }
size_t GpuAnalysisStoreReader::PassPageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Pass ); }

bool GpuAnalysisStoreReader::LoadResourcePage( size_t index, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const
{
    const auto* page = NthPage( m_manifest, GpuAnalysisStorePageKind::Resource, index ); if( !page ) { error = "resource_page_out_of_range"; return false; }
    if( !VerifyPageFile( m_root / page->relativePath, *page, error ) ) return false;
    auto snapshot = LoadGpuAnalysisCache( m_root / page->relativePath, m_identity, error ); if( !snapshot ) return false;
    out = std::move( snapshot->resources ); return true;
}

bool GpuAnalysisStoreReader::LoadResourceSummaryPage( size_t index,
    std::vector<GpuAnalysisResourceSummary>& out, std::string& error ) const
{
    error.clear();
    const auto* page = NthPage( m_manifest, GpuAnalysisStorePageKind::ResourceSummary, index );
    if( !page ) { error = "resource_summary_page_out_of_range"; return false; }
    if( !VerifyPageFile( m_root / page->relativePath, *page, error ) ) return false;
    std::ifstream in( GpuAnalysisIoPath( m_root / page->relativePath ), std::ios::binary );
    ResourceSummaryPageHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != ResourceSummaryPageMagic ||
        header.schema != GpuAnalysisStoreSchemaVersion ||
        header.recordCount != page->recordCount ||
        header.payloadBytes > GpuAnalysisMaximumTemporaryBytes ||
        header.payloadBytes > page->fileBytes - std::min<uint64_t>( page->fileBytes, sizeof( header ) ) )
    { error = "resource_summary_page_header_mismatch"; return false; }
    std::vector<uint8_t> payload( size_t( header.payloadBytes ) );
    if( !in.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) ) ||
        HashUpdate( FnvOffset, payload.data(), payload.size() ) != header.checksum )
    { error = "resource_summary_page_payload_corrupt"; return false; }
    const uint8_t* cursor = payload.data(); const uint8_t* end = cursor + payload.size();
    out.clear(); out.reserve( header.recordCount );
    for( uint32_t index = 0; index < header.recordCount; ++index )
    {
        if( size_t( end - cursor ) < sizeof( ResourceSummaryPageRecord ) )
        { error = "resource_summary_page_record_truncated"; return false; }
        ResourceSummaryPageRecord record;
        std::memcpy( &record, cursor, sizeof( record ) ); cursor += sizeof( record );
        if( record.nameBytes > uint64_t( end - cursor ) )
        { error = "resource_summary_page_name_truncated"; return false; }
        GpuAnalysisResourceSummary value;
        value.generation = record.generation;
        value.resourceId = record.resourceId;
        value.allocationId = record.allocationId;
        value.capacityBytes = record.capacityBytes;
        value.allocationOffsetBytes = record.allocationOffsetBytes;
        value.createTime = record.createTime;
        value.destroyTime = record.destroyTime;
        value.nameHash = record.nameHash;
        value.viewCount = record.viewCount;
        value.logicalBindingCount = record.logicalBindingCount;
        value.partCount = record.partCount;
        value.rangeCount = record.rangeCount;
        value.relationCount = record.relationCount;
        value.vgRecordCount = record.vgRecordCount;
        value.allocationResourceCount = record.allocationResourceCount;
        value.createCallsiteId = record.createCallsiteId;
        value.definitionRevision = record.definitionRevision;
        value.nameOriginalLength = record.nameOriginalLength;
        value.primaryKind = record.primaryKind;
        value.resourceClass = record.resourceClass;
        value.memoryDomain = record.memoryDomain;
        value.allocationKind = record.allocationKind;
        value.nameProvenance = record.nameProvenance;
        value.stackProvenance = record.stackProvenance;
        value.exactness = record.exactness;
        value.openBoundary = record.openBoundary != 0;
        value.aliveAtEnd = record.aliveAtEnd != 0;
        value.hasAliasGroup = record.hasAliasGroup != 0;
        value.name.assign( reinterpret_cast<const char*>( cursor ), record.nameBytes );
        cursor += record.nameBytes;
        out.push_back( std::move( value ) );
    }
    if( cursor != end ) { error = "resource_summary_page_trailing_data"; return false; }
    return true;
}

bool GpuAnalysisStoreReader::LoadAllocationPage( size_t index, std::vector<GpuAllocationAnalysisRecord>& out, std::string& error ) const
{
    const auto* page = NthPage( m_manifest, GpuAnalysisStorePageKind::Allocation, index ); if( !page ) { error = "allocation_page_out_of_range"; return false; }
    if( !VerifyPageFile( m_root / page->relativePath, *page, error ) ) return false;
    auto snapshot = LoadGpuAnalysisCache( m_root / page->relativePath, m_identity, error ); if( !snapshot ) return false;
    out = std::move( snapshot->allocations ); return true;
}

bool GpuAnalysisStoreReader::LoadPassPage( size_t index, std::vector<GpuPassWorkingSet>& out, std::string& error ) const
{
    const auto* page = NthPage( m_manifest, GpuAnalysisStorePageKind::Pass, index ); if( !page ) { error = "pass_page_out_of_range"; return false; }
    if( !VerifyPageFile( m_root / page->relativePath, *page, error ) ) return false;
    std::ifstream in( GpuAnalysisIoPath( m_root / page->relativePath ), std::ios::binary ); PassPageHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) || header.magic != PassPageMagic ||
        header.schema != GpuAnalysisStoreSchemaVersion || header.recordCount != page->recordCount ||
        header.payloadBytes > page->fileBytes || header.payloadBytes > GpuAnalysisMaximumTemporaryBytes )
    { error = "pass_page_header_mismatch"; return false; }
    std::vector<uint8_t> payload( size_t( header.payloadBytes ) );
    if( !in.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) ) ||
        HashUpdate( FnvOffset, payload.data(), payload.size() ) != header.checksum )
    { error = "pass_page_payload_corrupt"; return false; }
    const uint8_t* cursor = payload.data(); const uint8_t* end = cursor + payload.size();
    auto take = [&]( void* target, size_t bytes ) {
        if( bytes > size_t( end - cursor ) ) return false; if( bytes ) std::memcpy( target, cursor, bytes ); cursor += bytes; return true;
    };
    auto pod = [&]( auto& value ) { return take( &value, sizeof( value ) ); };
    uint64_t dictionaryCount = 0; if( !pod( dictionaryCount ) || dictionaryCount != header.dictionaryCount || dictionaryCount > 100000000 )
    { error = "pass_page_dictionary_corrupt"; return false; }
    std::vector<std::vector<uint64_t>> decodedSets; decodedSets.resize( size_t( dictionaryCount ) );
    for( auto& set : decodedSets )
    {
        uint32_t count = 0; uint8_t encoding = 0; uint8_t reserved[3]; uint64_t bytes = 0;
        if( !pod( count ) || !pod( encoding ) || !take( reserved, sizeof( reserved ) ) || !pod( bytes ) || bytes > uint64_t( end - cursor ) )
        { error = "pass_page_dictionary_entry_corrupt"; return false; }
        set.reserve( count ); const auto* setEnd = cursor + bytes;
        if( encoding == 0 )
        {
            if( bytes != uint64_t( count ) * sizeof( uint64_t ) ) { error = "pass_page_raw_set_size_mismatch"; return false; }
            set.resize( count ); if( !take( set.data(), size_t( bytes ) ) ) return false;
        }
        else if( encoding == 1 )
        {
            uint64_t previous = 0; for( uint32_t item = 0; item < count; ++item )
            { uint64_t delta = 0; if( !ReadVarint( cursor, setEnd, delta ) || previous > std::numeric_limits<uint64_t>::max() - delta )
                { error = "pass_page_delta_set_corrupt"; return false; } previous += delta; set.push_back( previous ); }
            if( cursor != setEnd ) { error = "pass_page_delta_set_trailing_data"; return false; }
        }
        else { error = "pass_page_set_encoding_unsupported"; return false; }
    }
    uint64_t recordCount = 0; if( !pod( recordCount ) || recordCount != header.recordCount ) { error = "pass_page_record_count_mismatch"; return false; }
    out.clear(); out.reserve( size_t( recordCount ) );
    for( uint64_t recordIndex = 0; recordIndex < recordCount; ++recordIndex )
    {
        PassPageRecord record; if( !pod( record ) || record.directSetId >= decodedSets.size() || record.inclusiveSetId >= decodedSets.size() ||
            record.nameBytes > uint64_t( end - cursor ) || record.evidenceCount > uint64_t( end - cursor - record.nameBytes ) / sizeof( GpuDetailedEvidenceAnalysisRecord ) )
        { error = "pass_page_record_corrupt"; return false; }
        GpuPassWorkingSet value; value.passId = record.passId; value.parentPassId = record.parentPassId; value.frameId = record.frameId;
        value.commandListId = record.commandListId; value.startNs = record.startNs; value.endNs = record.endNs;
        value.directRangeBytes = record.directRangeBytes; value.directPhysicalBytes = record.directPhysicalBytes;
        value.inclusivePhysicalBytes = record.inclusivePhysicalBytes; value.unknownRangeResourceCount = record.unknownRangeResourceCount;
        value.complete = record.complete != 0; value.truncated = record.truncated != 0;
        value.name.assign( reinterpret_cast<const char*>( cursor ), record.nameBytes ); cursor += record.nameBytes;
        value.detailedEvidence.resize( record.evidenceCount );
        if( !take( value.detailedEvidence.data(), value.detailedEvidence.size() * sizeof( GpuDetailedEvidenceAnalysisRecord ) ) )
        { error = "pass_page_evidence_corrupt"; return false; }
        value.directResources = decodedSets[record.directSetId]; value.inclusiveResources = decodedSets[record.inclusiveSetId]; out.push_back( std::move( value ) );
    }
    if( cursor != end ) { error = "pass_page_trailing_data"; return false; }
    return true;
}

bool GpuAnalysisStoreReader::RangesForResource( uint64_t resourceId, size_t offset,
    size_t limit, std::vector<GpuRangeAnalysisRecord>& out, bool& hasMore,
    std::string& error ) const
{
    out.clear(); hasMore = false; size_t skipped = 0;
    for( const auto& page : m_manifest.pages )
    {
        if( page.kind != GpuAnalysisStorePageKind::Range ||
            resourceId < page.firstKey || resourceId > page.lastKey ) continue;
        std::vector<GpuAnalysisRangeStoreEntry> values;
        if( !LoadRelationPage( m_root / page.relativePath, page, values, error ) ) return false;
        const auto first = std::lower_bound( values.begin(), values.end(), resourceId,
            []( const auto& value, uint64_t id ) { return value.resourceId < id; } );
        for( auto it = first; it != values.end() && it->resourceId == resourceId; ++it )
        {
            if( skipped++ < offset ) continue;
            if( out.size() == limit ) { hasMore = true; return true; }
            out.push_back( { it->generation, it->record } );
        }
    }
    return true;
}

std::optional<GpuResourceAnalysisRecord> GpuAnalysisStoreReader::FindResource( uint64_t id, std::string& error ) const
{
    size_t pageIndex = 0; for( const auto& page : m_manifest.pages ) if( page.kind == GpuAnalysisStorePageKind::Resource )
    {
        if( id >= page.firstKey && id <= page.lastKey ) { std::vector<GpuResourceAnalysisRecord> values; if( !LoadResourcePage( pageIndex, values, error ) ) return std::nullopt;
            const auto found = std::lower_bound( values.begin(), values.end(), id, []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
            if( found != values.end() && found->resourceId == id ) return *found; }
        ++pageIndex;
    }
    error = "gpu_resource_not_found"; return std::nullopt;
}

bool GpuAnalysisStoreReader::FindResources( std::vector<uint64_t> ids, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const
{
    std::sort( ids.begin(), ids.end() ); ids.erase( std::unique( ids.begin(), ids.end() ), ids.end() ); out.clear();
    size_t matched = 0, pageIndex = 0;
    for( const auto& page : m_manifest.pages ) if( page.kind == GpuAnalysisStorePageKind::Resource )
    {
        const auto begin = std::lower_bound( ids.begin(), ids.end(), page.firstKey );
        const auto end = std::upper_bound( begin, ids.end(), page.lastKey );
        if( begin != end )
        {
            std::vector<GpuResourceAnalysisRecord> values; if( !LoadResourcePage( pageIndex, values, error ) ) return false;
            for( auto id = begin; id != end; ++id )
            {
                const auto found = std::lower_bound( values.begin(), values.end(), *id,
                    []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
                if( found == values.end() || found->resourceId != *id ) { error = "gpu_resource_index_target_missing"; return false; }
                out.push_back( *found ); ++matched;
            }
        }
        ++pageIndex;
    }
    if( matched != ids.size() ) { error = "gpu_resource_index_unresolved"; return false; }
    return true;
}

std::optional<GpuAllocationAnalysisRecord> GpuAnalysisStoreReader::FindAllocation( uint64_t id, std::string& error ) const
{
    size_t pageIndex = 0; for( const auto& page : m_manifest.pages ) if( page.kind == GpuAnalysisStorePageKind::Allocation )
    {
        if( id >= page.firstKey && id <= page.lastKey ) { std::vector<GpuAllocationAnalysisRecord> values; if( !LoadAllocationPage( pageIndex, values, error ) ) return std::nullopt;
            const auto found = std::lower_bound( values.begin(), values.end(), id, []( const auto& value, uint64_t key ) { return value.allocationId < key; } );
            if( found != values.end() && found->allocationId == id ) return *found; }
        ++pageIndex;
    }
    error = "gpu_allocation_not_found"; return std::nullopt;
}

std::optional<GpuPassWorkingSet> GpuAnalysisStoreReader::FindPass( uint64_t id, std::string& error ) const
{
    size_t pageIndex = 0; for( const auto& page : m_manifest.pages ) if( page.kind == GpuAnalysisStorePageKind::Pass )
    {
        if( id >= page.firstKey && id <= page.lastKey ) { std::vector<GpuPassWorkingSet> values; if( !LoadPassPage( pageIndex, values, error ) ) return std::nullopt;
            const auto found = std::lower_bound( values.begin(), values.end(), id, []( const auto& value, uint64_t key ) { return value.passId < key; } );
            if( found != values.end() && found->passId == id ) return *found; }
        ++pageIndex;
    }
    error = "gpu_pass_not_found"; return std::nullopt;
}

std::optional<GpuAnalysisPassSummary> GpuAnalysisStoreReader::FindPassSummary(
    uint64_t id, std::string& error ) const
{
    for( const auto& page : m_manifest.pages )
    {
        if( page.kind != GpuAnalysisStorePageKind::PassSummary ||
            id < page.firstKey || id > page.lastKey ) continue;
        std::vector<GpuAnalysisPassSummary> values;
        if( !LoadRelationPage( m_root / page.relativePath, page, values, error ) )
            return std::nullopt;
        const auto found = std::lower_bound( values.begin(), values.end(), id,
            []( const auto& value, uint64_t key ) { return value.passId < key; } );
        if( found != values.end() && found->passId == id ) return *found;
    }
    error = "gpu_pass_summary_not_found";
    return std::nullopt;
}

std::optional<GpuAnalysisStablePassSummary>
GpuAnalysisStoreReader::FindStablePassSummary( uint64_t frameId,
    uint32_t taxonomyId, std::string& error ) const
{
    for( const auto& page : m_manifest.pages )
    {
        if( page.kind != GpuAnalysisStorePageKind::StablePassSummary ||
            frameId < page.firstKey || frameId > page.lastKey ) continue;
        std::vector<GpuAnalysisStablePassSummary> values;
        if( !LoadRelationPage( m_root / page.relativePath, page, values, error ) )
            return std::nullopt;
        const auto key = std::pair<uint64_t, uint32_t> { frameId, taxonomyId };
        const auto found = std::lower_bound( values.begin(), values.end(), key,
            []( const auto& value, const auto& candidate ) {
                return value.frameId != candidate.first ?
                    value.frameId < candidate.first : value.taxonomyId < candidate.second;
            } );
        if( found != values.end() && found->frameId == frameId &&
            found->taxonomyId == taxonomyId ) return *found;
    }
    error = "gpu_stable_pass_summary_not_found";
    return std::nullopt;
}

bool GpuAnalysisStoreReader::PassResources( uint64_t passId, bool inclusive,
    size_t offset, size_t limit, std::vector<uint64_t>& out, bool& hasMore,
    std::string& error ) const
{
    out.clear(); hasMore = false;
    const auto rootPass = FindPass( passId, error );
    if( !rootPass ) return false;
    std::vector<uint64_t> reconstructed;
    std::shared_ptr<const std::vector<uint64_t>> cached;
    const std::vector<uint64_t>* resourcesPtr = &rootPass->directResources;
    if( inclusive )
    {
        if( m_inclusiveCache )
        {
            std::lock_guard lock( m_inclusiveCache->mutex );
            const auto found = m_inclusiveCache->entries.find( passId );
            if( found != m_inclusiveCache->entries.end() )
            {
                found->second.lastUse = ++m_inclusiveCache->clock;
                cached = found->second.resources;
            }
        }
        if( cached ) resourcesPtr = cached.get();
        else
        {
        std::vector<uint64_t> passIds { passId };
        std::unordered_set<uint64_t> seen { passId };
        for( size_t cursor = 0; cursor < passIds.size(); ++cursor )
        {
            const auto parentId = passIds[cursor];
            for( const auto& page : m_manifest.pages )
            {
                if( page.kind != GpuAnalysisStorePageKind::PassChildIndex ||
                    parentId < page.firstKey || parentId > page.lastKey ) continue;
                std::vector<GpuAnalysisPassChildEntry> children;
                if( !LoadRelationPage( m_root / page.relativePath, page, children, error ) ) return false;
                const auto first = std::lower_bound( children.begin(), children.end(), parentId,
                    []( const auto& value, uint64_t key ) { return value.parentPassId < key; } );
                for( auto it = first; it != children.end() && it->parentPassId == parentId; ++it )
                {
                    if( it->childPassId == 0 || !seen.emplace( it->childPassId ).second )
                    { error = "gpu_pass_child_cycle_or_duplicate"; return false; }
                    passIds.push_back( it->childPassId );
                }
            }
        }
        std::sort( passIds.begin(), passIds.end() );
        std::vector<GpuPassWorkingSet> passes;
        if( !LoadPassesByIds( std::move( passIds ), passes, error ) ) return false;
        size_t memberCount = 0;
        for( const auto& pass : passes )
        {
            if( memberCount > std::numeric_limits<size_t>::max() - pass.directResources.size() )
            { error = "gpu_pass_inclusive_member_count_overflow"; return false; }
            memberCount += pass.directResources.size();
        }
        reconstructed.reserve( memberCount );
        for( const auto& pass : passes ) reconstructed.insert( reconstructed.end(),
            pass.directResources.begin(), pass.directResources.end() );
        std::sort( reconstructed.begin(), reconstructed.end() );
        reconstructed.erase( std::unique( reconstructed.begin(), reconstructed.end() ),
            reconstructed.end() );
        const auto summary = FindPassSummary( passId, error );
        if( !summary || summary->inclusiveResourceCount != reconstructed.size() ||
            summary->inclusiveResourceHash != GpuAnalysisResourceSetHash( reconstructed ) )
        { error = "gpu_pass_inclusive_summary_mismatch"; return false; }
            const auto bytes = uint64_t( reconstructed.size() ) * sizeof( uint64_t );
            if( m_inclusiveCache && bytes <= InclusiveCache::MaximumBytes )
            {
                auto value = std::make_shared<const std::vector<uint64_t>>(
                    std::move( reconstructed ) );
                std::lock_guard lock( m_inclusiveCache->mutex );
                while( m_inclusiveCache->bytes > InclusiveCache::MaximumBytes - bytes &&
                    !m_inclusiveCache->entries.empty() )
                {
                    const auto victim = std::min_element( m_inclusiveCache->entries.begin(),
                        m_inclusiveCache->entries.end(), []( const auto& lhs, const auto& rhs ) {
                            return lhs.second.lastUse < rhs.second.lastUse;
                        } );
                    m_inclusiveCache->bytes -= uint64_t( victim->second.resources->size() ) *
                        sizeof( uint64_t );
                    m_inclusiveCache->entries.erase( victim );
                }
                const auto [stored, inserted] = m_inclusiveCache->entries.emplace(
                    passId, InclusiveCache::Entry { value, ++m_inclusiveCache->clock } );
                if( inserted ) m_inclusiveCache->bytes += bytes;
                cached = inserted ? value : stored->second.resources;
                resourcesPtr = cached.get();
            }
            else resourcesPtr = &reconstructed;
        }
    }
    const auto& resources = *resourcesPtr;
    if( offset >= resources.size() ) return true;
    const auto count = std::min( limit, resources.size() - offset );
    out.assign( resources.begin() + offset, resources.begin() + offset + count );
    hasMore = offset + count < resources.size();
    return true;
}

bool GpuAnalysisStoreReader::PassRelationsForResource( uint64_t resourceId,
    size_t offset, size_t limit, std::vector<GpuAnalysisResourcePassEntry>& out,
    bool& hasMore, std::string& error ) const
{
    out.clear(); hasMore = false; size_t skipped = 0;
    for( const auto& page : m_manifest.pages )
    {
        if( page.kind != GpuAnalysisStorePageKind::ResourcePassIndex ||
            resourceId < page.firstKey || resourceId > page.lastKey ) continue;
        std::vector<GpuAnalysisResourcePassEntry> values;
        if( !LoadRelationPage( m_root / page.relativePath, page, values, error ) ) return false;
        const auto first = std::lower_bound( values.begin(), values.end(), resourceId,
            []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
        for( auto it = first; it != values.end() && it->resourceId == resourceId; ++it )
        {
            if( skipped++ < offset ) continue;
            if( out.size() == limit ) { hasMore = true; return true; }
            out.push_back( *it );
        }
    }
    return true;
}

bool GpuAnalysisStoreReader::PassesForFrame( uint64_t frameId, size_t offset, size_t limit,
    std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const
{
    std::vector<uint64_t> ids; if( !RelationIds<GpuAnalysisFramePassEntry>( m_root, m_manifest, GpuAnalysisStorePageKind::FramePassIndex,
        frameId, offset, limit, ids, hasMore, []( const auto& value ) { return value.frameId; }, error ) ) return false;
    return LoadPassesByIds( std::move( ids ), out, error );
}

bool GpuAnalysisStoreReader::PassesForResource( uint64_t resourceId, size_t offset, size_t limit,
    std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const
{
    std::vector<uint64_t> ids; if( !RelationIds<GpuAnalysisResourcePassEntry>( m_root, m_manifest, GpuAnalysisStorePageKind::ResourcePassIndex,
        resourceId, offset, limit, ids, hasMore, []( const auto& value ) { return value.resourceId; }, error ) ) return false;
    return LoadPassesByIds( std::move( ids ), out, error );
}

bool GpuAnalysisStoreReader::LoadPassesByIds( std::vector<uint64_t> ids, std::vector<GpuPassWorkingSet>& out, std::string& error ) const
{
    std::sort( ids.begin(), ids.end() ); ids.erase( std::unique( ids.begin(), ids.end() ), ids.end() ); out.clear();
    size_t matched = 0, pageIndex = 0;
    for( const auto& page : m_manifest.pages ) if( page.kind == GpuAnalysisStorePageKind::Pass )
    {
        const auto begin = std::lower_bound( ids.begin(), ids.end(), page.firstKey );
        const auto end = std::upper_bound( begin, ids.end(), page.lastKey );
        if( begin != end )
        {
            std::vector<GpuPassWorkingSet> values; if( !LoadPassPage( pageIndex, values, error ) ) return false;
            for( auto id = begin; id != end; ++id )
            {
                const auto found = std::lower_bound( values.begin(), values.end(), *id,
                    []( const auto& value, uint64_t key ) { return value.passId < key; } );
                if( found == values.end() || found->passId != *id ) { error = "gpu_pass_index_target_missing"; return false; }
                out.push_back( *found ); ++matched;
            }
        }
        ++pageIndex;
    }
    if( matched != ids.size() ) { error = "gpu_pass_index_unresolved"; return false; }
    return true;
}

const char* GpuAnalysisStorePageKindName( GpuAnalysisStorePageKind kind )
{
    switch( kind )
    {
    case GpuAnalysisStorePageKind::Metadata: return "metadata"; case GpuAnalysisStorePageKind::Resource: return "resource";
    case GpuAnalysisStorePageKind::Allocation: return "allocation"; case GpuAnalysisStorePageKind::Pass: return "pass";
    case GpuAnalysisStorePageKind::Residency: return "residency"; case GpuAnalysisStorePageKind::Churn: return "churn";
    case GpuAnalysisStorePageKind::ResourcePassIndex: return "resource_pass_index"; case GpuAnalysisStorePageKind::FramePassIndex: return "frame_pass_index";
    case GpuAnalysisStorePageKind::Range: return "range";
    case GpuAnalysisStorePageKind::ResourceSummary: return "resource_summary";
    case GpuAnalysisStorePageKind::PassSummary: return "pass_summary";
    case GpuAnalysisStorePageKind::StablePassSummary: return "stable_pass_summary";
    case GpuAnalysisStorePageKind::PassChildIndex: return "pass_child_index";
    }
    return "unknown";
}

}
