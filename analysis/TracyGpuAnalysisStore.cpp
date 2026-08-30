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
#include <numeric>
#include <sstream>
#include <type_traits>
#include <unordered_map>

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

namespace
{

constexpr uint64_t StoreManifestMagic = 0x314d5453474e4aull; // JNGSTM1
constexpr uint64_t RelationMagic = 0x31584449474e4aull; // JNGIDX1
constexpr uint64_t PassPageMagic = 0x31534150474e4aull; // JNGPAS1
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
    W( resourceCount ); W( allocationCount ); W( passCount ); W( residencyCount ); W( churnCount );
    W( resourcePassRelationCount ); W( framePassRelationCount ); W( totalBytes );
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

}

bool WriteGpuAnalysisDerivedStore( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error )
{
    error.clear(); writtenBytes = 0;
    const auto algorithmRoot = GpuAnalysisDerivedPath( sidecarPath );
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
        R( resourceCount ); R( allocationCount ); R( passCount ); R( residencyCount ); R( churnCount );
        R( resourcePassRelationCount ); R( framePassRelationCount ); R( totalBytes );
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
    return reader;
}

size_t GpuAnalysisStoreReader::ResourcePageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Resource ); }
size_t GpuAnalysisStoreReader::AllocationPageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Allocation ); }
size_t GpuAnalysisStoreReader::PassPageCount() const { return CountPages( m_manifest, GpuAnalysisStorePageKind::Pass ); }

bool GpuAnalysisStoreReader::LoadResourcePage( size_t index, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const
{
    const auto* page = NthPage( m_manifest, GpuAnalysisStorePageKind::Resource, index ); if( !page ) { error = "resource_page_out_of_range"; return false; }
    if( !VerifyPageFile( m_root / page->relativePath, *page, error ) ) return false;
    auto snapshot = LoadGpuAnalysisCache( m_root / page->relativePath, m_identity, error ); if( !snapshot ) return false;
    out = std::move( snapshot->resources ); return true;
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
    }
    return "unknown";
}

}
