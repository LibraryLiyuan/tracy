#include "TracyGpuAnalysisSidecar.hpp"

#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisPath.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "../public/common/TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <unistd.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t ShardMagic = 0x3157415241474e4aull; // JNGARAW1
constexpr uint64_t SummaryMagic = 0x31594d4d5553474aull; // JGSUMMY1
constexpr uint64_t ManifestMagic = 0x31464e414d474e4aull; // JNGMANF1
constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

enum class RawKind : uint32_t
{
    CatalogControl = 1,
    CatalogBatch,
    CatalogGeneration,
    CatalogString,
    Resource,
    Allocation,
    View,
    Logical,
    Part,
    CatalogRelation,
    VirtualGeometry,
    Range,
    DetailedEvidence,
    ReferencePass,
    ReferenceUse,
    ReferenceEnd,
    Relation
};

#pragma pack( push, 1 )
struct ShardHeader
{
    uint64_t magic = ShardMagic;
    uint32_t schema = GpuAnalysisRawSchemaVersion;
    uint32_t kind = 0;
    uint32_t elementSize = 0;
    uint32_t flags = 0;
    uint64_t firstIndex = 0;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t checksum = 0;
};

struct SummaryFile
{
    uint64_t magic = SummaryMagic;
    uint32_t schema = GpuAnalysisDerivedSchemaVersion;
    uint32_t bytes = sizeof( GpuAnalysisExactSummary );
    GpuAnalysisExactSummary summary;
    uint64_t checksum = 0;
};
#pragma pack( pop )

static_assert( sizeof( ShardHeader ) == 56 );

uint64_t Hash64( const void* data, size_t size, uint64_t seed = FnvOffset )
{
    auto hash = seed;
    const auto* bytes = static_cast<const uint8_t*>( data );
    for( size_t i = 0; i < size; ++i ) { hash ^= bytes[i]; hash *= FnvPrime; }
    return hash;
}

int64_t WriteTime( const std::filesystem::path& path )
{
    std::error_code ec;
    const auto value = std::filesystem::last_write_time( GpuAnalysisIoPath( path ), ec );
    return ec ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>( value.time_since_epoch() ).count();
}

uint32_t ProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t( getpid() );
#endif
}

bool LeaseProcessAlive( const std::filesystem::path& heartbeat )
{
    std::ifstream in( GpuAnalysisIoPath( heartbeat ), std::ios::binary ); std::string key; uint32_t pid = 0;
    if( !in || !std::getline( in, key ) ) return false;
    if( !key.empty() && key.back() == '\r' ) key.pop_back();
    if( key.rfind( "pid=", 0 ) != 0 ) return false;
    const auto text = key.substr( 4 ); const auto [ptr, ec] = std::from_chars( text.data(), text.data() + text.size(), pid );
    if( ec != std::errc() || ptr != text.data() + text.size() || pid == 0 ) return false;
#ifdef _WIN32
    const auto process = OpenProcess( SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
    if( !process ) return false; DWORD code = 0; const bool alive = GetExitCodeProcess( process, &code ) && code == STILL_ACTIVE; CloseHandle( process ); return alive;
#else
    return kill( pid_t( pid ), 0 ) == 0 || errno == EPERM;
#endif
}

std::string GenerationName()
{
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch() ).count();
    return "g" + std::to_string( now ) + "-p" + std::to_string( ProcessId() );
}

bool ReplaceFileAtomically( const std::filesystem::path& temporary, const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    DWORD lastError = ERROR_SUCCESS;
    for( unsigned attempt = 0; attempt < 8; ++attempt )
    {
        const auto ioTarget = GpuAnalysisIoPath( target ); const auto ioTemporary = GpuAnalysisIoPath( temporary );
        const bool targetExists = GetFileAttributesW( ioTarget.c_str() ) != INVALID_FILE_ATTRIBUTES;
        const bool replaced = targetExists
            ? ReplaceFileW( ioTarget.c_str(), ioTemporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr ) != FALSE
            : MoveFileExW( ioTemporary.c_str(), ioTarget.c_str(), MOVEFILE_WRITE_THROUGH ) != FALSE;
        if( replaced ) return true;
        lastError = GetLastError();
        if( lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION &&
            lastError != ERROR_UNABLE_TO_REMOVE_REPLACED && lastError != ERROR_UNABLE_TO_MOVE_REPLACEMENT &&
            lastError != ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 ) break;
        Sleep( 1u << attempt );
    }
    error = "atomic_replace_failed:" + std::to_string( lastError );
#else
    std::error_code ec;
    std::filesystem::rename( temporary, target, ec );
    if( !ec ) return true;
    error = "atomic_replace_failed:" + ec.message();
#endif
    return false;
}

std::string SafeReason( std::string value )
{
    for( auto& ch : value ) if( ch == '\r' || ch == '\n' ) ch = ' ';
    return value;
}

bool SaveManifest( const std::filesystem::path& root, const GpuAnalysisSidecarManifest& value, std::string& error )
{
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories( GpuAnalysisIoPath( root ), ec );
    if( ec ) { error = "manifest_directory_failed:" + ec.message(); return false; }
    const auto target = root / "manifest";
    auto temporary = target; temporary += ".tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "manifest_open_failed"; return false; }
    out << "magic " << ManifestMagic << '\n';
    out << "manifest_schema " << value.manifestSchema << '\n';
    out << "raw_schema " << value.rawSchema << '\n';
    out << "derived_schema " << value.derivedSchema << '\n';
    out << "algorithm " << std::quoted( value.algorithmId ) << '\n';
    out << "raw_generation " << std::quoted( value.rawGeneration ) << '\n';
    out << "derived_generation " << std::quoted( value.derivedGeneration ) << '\n';
    out << "state " << unsigned( value.state ) << '\n';
    out << "identity_state " << unsigned( value.identityState ) << '\n';
    out << "trace_sha256 " << std::quoted( value.identity.sha256 ) << '\n';
    out << "trace_size " << value.identity.fileSize << '\n';
    out << "trace_write_time " << value.identity.writeTime << '\n';
    out << "trace_volume " << value.identity.volumeId << '\n';
    out << "trace_file_id_high " << value.identity.fileIdHigh << '\n';
    out << "trace_file_id_low " << value.identity.fileIdLow << '\n';
    out << "trace_head_hash " << value.identity.headHash << '\n';
    out << "trace_middle_hash " << value.identity.middleHash << '\n';
    out << "trace_tail_hash " << value.identity.tailHash << '\n';
#define S( field ) out << "summary_" #field " " << value.summary.field << '\n'
    S( resourceRecordCount ); S( allocationRecordCount ); S( passCount ); S( referenceUseCount ); S( referenceEndCount );
    S( rangeCount ); S( relationCount ); S( viewRecordCount ); S( logicalRecordCount ); S( partRecordCount );
    S( vgRecordCount ); S( evidenceRecordCount ); S( generationCount ); S( payloadBytes );
    S( allocationCreateCount ); S( allocationDestroyCount );
    S( engineKnownPhysicalBytes ); S( engineKnownPhysicalPeakBytes ); S( engineKnownPhysicalPeakTimeNs );
    S( directResourceSetHash ); S( catalogChecksum ); S( unresolvedCount ); S( invalidRecordCount );
#undef S
    out << "summary_catalog_present " << value.summary.catalogPresent << '\n';
    out << "summary_catalog_valid " << value.summary.catalogValid << '\n';
    out << "summary_exact " << value.summary.exact << '\n';
    out << "raw_complete " << value.rawComplete << '\n';
    out << "derived_complete " << value.derivedComplete << '\n';
    out << "reason " << std::quoted( SafeReason( value.reason ) ) << '\n';
    out << "shard_count " << value.shards.size() << '\n';
    for( const auto& shard : value.shards )
    {
        out << "shard " << shard.kind << ' ' << shard.elementSize << ' ' << shard.firstIndex << ' '
            << shard.recordCount << ' ' << shard.payloadBytes << ' ' << shard.checksum << ' '
            << std::quoted( shard.relativePath.generic_string() ) << '\n';
    }
    out.flush();
    if( !out ) { error = "manifest_write_failed"; return false; }
    out.close();
    return ReplaceFileAtomically( temporary, target, error );
}

template<typename T>
bool WritePodShards( const std::filesystem::path& root, const std::filesystem::path& generationRoot,
    RawKind kind, const char* stem, const std::vector<T>& values, std::vector<GpuAnalysisShard>& shards,
    uint64_t& totalBytes, const GpuAnalysisSidecarControl& control, std::string& error )
{
    static_assert( std::is_trivially_copyable_v<T> );
    if( values.empty() ) return true;
    const uint64_t recordsPerShard = std::max<uint64_t>( 1, GpuAnalysisTargetShardBytes / sizeof( T ) );
    uint64_t first = 0;
    uint32_t shardIndex = 0;
    while( first < values.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        const auto count = std::min<uint64_t>( recordsPerShard, values.size() - first );
        const auto payloadBytes = count * sizeof( T );
        if( totalBytes > control.maximumSidecarBytes - payloadBytes ) { error = "sidecar_size_limit"; return false; }
        std::ostringstream name; name << stem << '.' << std::setw( 6 ) << std::setfill( '0' ) << shardIndex++ << ".bin";
        const auto path = generationRoot / name.str();
        auto temporary = path; temporary += ".tmp";
        const auto* payload = reinterpret_cast<const uint8_t*>( values.data() + first );
        const auto checksum = Hash64( payload, size_t( payloadBytes ) );
        ShardHeader header;
        header.kind = uint32_t( kind ); header.elementSize = sizeof( T ); header.firstIndex = first;
        header.recordCount = count; header.payloadBytes = payloadBytes; header.checksum = checksum;
        std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
        if( !out ) { error = "shard_open_failed:" + path.string(); return false; }
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( reinterpret_cast<const char*>( payload ), std::streamsize( payloadBytes ) );
        out.flush();
        if( !out ) { error = "shard_write_failed:" + path.string(); return false; }
        out.close();
        if( !ReplaceFileAtomically( temporary, path, error ) ) return false;
        shards.push_back( { uint32_t( kind ), uint32_t( sizeof( T ) ), first, count, payloadBytes, checksum,
            std::filesystem::relative( path, root ) } );
        totalBytes += sizeof( header ) + payloadBytes;
        first += count;
    }
    return true;
}

bool WriteStringShards( const std::filesystem::path& root, const std::filesystem::path& generationRoot,
    const std::vector<JnGpuCatalogStringData>& values, std::vector<GpuAnalysisShard>& shards,
    uint64_t& totalBytes, const GpuAnalysisSidecarControl& control, std::string& error )
{
    uint64_t first = 0;
    uint32_t shardIndex = 0;
    while( first < values.size() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::vector<uint8_t> payload;
        payload.reserve( size_t( std::min<uint64_t>( GpuAnalysisTargetShardBytes, 16ull * 1024 * 1024 ) ) );
        const auto append = [&]( const void* ptr, size_t size ) {
            const auto* bytes = static_cast<const uint8_t*>( ptr ); payload.insert( payload.end(), bytes, bytes + size );
        };
        uint64_t count = 0;
        while( first + count < values.size() && ( payload.empty() || payload.size() < GpuAnalysisTargetShardBytes ) )
        {
            const auto& value = values[size_t( first + count )];
            const uint32_t length = uint32_t( std::min<size_t>( value.value.size(), std::numeric_limits<uint32_t>::max() ) );
            append( &value.generation, sizeof( value.generation ) ); append( &value.header, sizeof( value.header ) );
            append( &length, sizeof( length ) ); append( value.value.data(), length );
            ++count;
        }
        if( totalBytes > control.maximumSidecarBytes - payload.size() ) { error = "sidecar_size_limit"; return false; }
        std::ostringstream name; name << "strings." << std::setw( 6 ) << std::setfill( '0' ) << shardIndex++ << ".bin";
        const auto path = generationRoot / name.str(); auto temporary = path; temporary += ".tmp";
        const auto checksum = Hash64( payload.data(), payload.size() );
        ShardHeader header; header.kind = uint32_t( RawKind::CatalogString ); header.elementSize = 0;
        header.firstIndex = first; header.recordCount = count; header.payloadBytes = payload.size(); header.checksum = checksum;
        std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
        if( !out ) { error = "string_shard_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( reinterpret_cast<const char*>( payload.data() ), std::streamsize( payload.size() ) ); out.flush();
        if( !out ) { error = "string_shard_write_failed"; return false; }
        out.close(); if( !ReplaceFileAtomically( temporary, path, error ) ) return false;
        shards.push_back( { uint32_t( RawKind::CatalogString ), 0, first, count, payload.size(), checksum,
            std::filesystem::relative( path, root ) } );
        totalBytes += sizeof( header ) + payload.size(); first += count;
    }
    return true;
}

GpuAnalysisExactSummary ComputeSummary( const JnTraceData& data )
{
    GpuAnalysisExactSummary result;
    result.resourceRecordCount = data.gpuCatalogResources.size();
    result.allocationRecordCount = data.gpuCatalogAllocations.size();
    result.passCount = data.gpuReferencePasses.size();
    result.referenceUseCount = data.gpuReferenceUses.size();
    result.referenceEndCount = data.gpuReferenceEnds.size();
    result.rangeCount = data.gpuRangeSets.size();
    result.relationCount = data.gpuCatalogRelations.size() + data.relations.size();
    result.viewRecordCount = data.gpuCatalogViews.size();
    result.logicalRecordCount = data.gpuCatalogLogicals.size();
    result.partRecordCount = data.gpuCatalogParts.size();
    result.vgRecordCount = data.gpuCatalogVg.size();
    result.evidenceRecordCount = data.gpuDetailedEvidence.size();
    result.generationCount = data.gpuCatalogGenerations.size();
    result.catalogPresent = data.gpuCatalogPresent;
    result.catalogValid = data.gpuCatalogValid;
    for( const auto& generation : data.gpuCatalogGenerations )
    {
        result.unresolvedCount += generation.unresolvedCount;
        result.payloadBytes += generation.payloadBytes;
        result.catalogValid &= generation.valid != 0 && generation.ended != 0 &&
            generation.state == uint8_t( JnGpuCatalogGenerationState::Complete );
    }
    uint64_t setHash = FnvOffset;
    for( const auto& value : data.gpuReferenceUses ) setHash = Hash64( &value, sizeof( value ), setHash );
    result.directResourceSetHash = setHash;
    uint64_t catalogHash = FnvOffset;
    const auto hashVector = [&]( const auto& values ) {
        if( !values.empty() ) catalogHash = Hash64( values.data(), values.size() * sizeof( values.front() ), catalogHash );
    };
    hashVector( data.gpuCatalogResources ); hashVector( data.gpuCatalogAllocations ); hashVector( data.gpuCatalogViews );
    hashVector( data.gpuCatalogLogicals ); hashVector( data.gpuCatalogParts ); hashVector( data.gpuCatalogRelations );
    hashVector( data.gpuCatalogVg ); hashVector( data.gpuRangeSets ); hashVector( data.gpuDetailedEvidence );
    result.catalogChecksum = catalogHash;

    std::vector<size_t> order( data.gpuCatalogAllocations.size() );
    for( size_t i = 0; i < order.size(); ++i ) order[i] = i;
    std::stable_sort( order.begin(), order.end(), [&]( size_t lhs, size_t rhs ) {
        return data.gpuCatalogAllocations[lhs].time < data.gpuCatalogAllocations[rhs].time;
    } );
    std::unordered_map<uint64_t, uint64_t> live;
    uint64_t current = 0;
    for( const auto index : order )
    {
        const auto& value = data.gpuCatalogAllocations[index];
        if( value.allocationId == 0 ) { ++result.invalidRecordCount; continue; }
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        const bool define = operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Update ||
            operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot;
        if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot )
            ++result.allocationCreateCount;
        else if( operation == JnGpuCatalogRecordOperation::Destroy ) ++result.allocationDestroyCount;
        if( value.exactness == uint8_t( JnGpuCatalogExactness::Invalid ) ) ++result.invalidRecordCount;
        if( value.parentAllocationId != 0 ) continue;
        const auto found = live.find( value.allocationId );
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        {
            if( found != live.end() ) { current -= found->second; live.erase( found ); }
        }
        else if( define )
        {
            if( found != live.end() ) current -= found->second;
            live[value.allocationId] = value.sizeBytes; current += value.sizeBytes;
        }
        if( current > result.engineKnownPhysicalPeakBytes )
        {
            result.engineKnownPhysicalPeakBytes = current;
            result.engineKnownPhysicalPeakTimeNs = value.time;
        }
    }
    result.engineKnownPhysicalBytes = current;
    result.exact = result.catalogPresent && result.catalogValid && result.unresolvedCount == 0 && result.invalidRecordCount == 0;
    return result;
}

bool WriteSummary( const std::filesystem::path& root, const GpuAnalysisExactSummary& summary, std::string& error )
{
    const auto directory = root / "derived" / std::to_string( GpuAnalysisDerivedSchemaVersion ) / GpuAnalysisAlgorithmId;
    std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( directory ), ec );
    if( ec ) { error = "summary_directory_failed:" + ec.message(); return false; }
    SummaryFile file; file.summary = summary; file.checksum = Hash64( &file.summary, sizeof( file.summary ) );
    const auto target = directory / "exact-summary.bin"; auto temporary = target; temporary += ".tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "summary_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &file ), sizeof( file ) ); out.flush();
    if( !out ) { error = "summary_write_failed"; return false; }
    out.close(); return ReplaceFileAtomically( temporary, target, error );
}

template<typename T>
bool ReadPodShard( const std::filesystem::path& path, const GpuAnalysisShard& expected,
    std::vector<T>& output, std::string& error )
{
    static_assert( std::is_trivially_copyable_v<T> );
    std::ifstream in( GpuAnalysisIoPath( path ), std::ios::binary );
    ShardHeader header;
    if( !in || !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) )
    {
        std::error_code ec; const auto ioPath = GpuAnalysisIoPath( path ); const auto exists = std::filesystem::exists( ioPath, ec );
        error = "shard_header_read_failed:" + path.string() + ":io=" + ioPath.string() +
            ":exists=" + ( exists ? "1" : "0" ) + ":ec=" + ec.message();
        return false;
    }
    if( header.magic != ShardMagic || header.schema != GpuAnalysisRawSchemaVersion || header.kind != expected.kind ||
        header.elementSize != sizeof( T ) || header.firstIndex != expected.firstIndex || header.recordCount != expected.recordCount ||
        header.payloadBytes != expected.payloadBytes || header.checksum != expected.checksum ||
        header.payloadBytes != header.recordCount * sizeof( T ) ) { error = "shard_header_mismatch"; return false; }
    if( header.recordCount > uint64_t( std::numeric_limits<size_t>::max() - output.size() ) ) { error = "shard_record_limit"; return false; }
    const auto old = output.size(); output.resize( old + size_t( header.recordCount ) );
    if( !in.read( reinterpret_cast<char*>( output.data() + old ), std::streamsize( header.payloadBytes ) ) ) { error = "shard_payload_read_failed"; return false; }
    if( Hash64( output.data() + old, size_t( header.payloadBytes ) ) != header.checksum ) { error = "shard_checksum_mismatch"; return false; }
    char extra; if( in.read( &extra, 1 ) ) { error = "shard_trailing_data"; return false; }
    return true;
}

bool ReadStringShard( const std::filesystem::path& path, const GpuAnalysisShard& expected,
    std::vector<JnGpuCatalogStringData>& output, std::string& error )
{
    std::ifstream in( GpuAnalysisIoPath( path ), std::ios::binary ); ShardHeader header;
    if( !in || !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ) { error = "string_shard_header_read_failed"; return false; }
    if( header.magic != ShardMagic || header.kind != uint32_t( RawKind::CatalogString ) || header.elementSize != 0 ||
        header.recordCount != expected.recordCount || header.payloadBytes != expected.payloadBytes || header.checksum != expected.checksum )
    { error = "string_shard_header_mismatch"; return false; }
    std::vector<uint8_t> payload( size_t( header.payloadBytes ) );
    if( !in.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) ) ) { error = "string_shard_payload_read_failed"; return false; }
    if( Hash64( payload.data(), payload.size() ) != header.checksum ) { error = "string_shard_checksum_mismatch"; return false; }
    size_t offset = 0;
    auto take = [&]( void* target, size_t size ) {
        if( offset > payload.size() || size > payload.size() - offset ) return false;
        std::memcpy( target, payload.data() + offset, size ); offset += size; return true;
    };
    for( uint64_t i = 0; i < header.recordCount; ++i )
    {
        JnGpuCatalogStringData value; uint32_t length = 0;
        if( !take( &value.generation, sizeof( value.generation ) ) || !take( &value.header, sizeof( value.header ) ) ||
            !take( &length, sizeof( length ) ) || length > payload.size() - offset ) { error = "string_shard_record_corrupt"; return false; }
        value.value.assign( reinterpret_cast<const char*>( payload.data() + offset ), length ); offset += length;
        output.emplace_back( std::move( value ) );
    }
    if( offset != payload.size() ) { error = "string_shard_trailing_data"; return false; }
    return true;
}

bool SaveCheckpoint( const std::filesystem::path& path, std::string_view stage, std::string& error )
{
    auto temporary = path; temporary += ".tmp";
    std::ofstream out( GpuAnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "checkpoint_open_failed"; return false; }
    out << "schema=1\nstage=" << stage << "\nupdated_us=" <<
        std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::system_clock::now().time_since_epoch() ).count() << '\n';
    out.close(); return ReplaceFileAtomically( temporary, path, error );
}

class WriterLease
{
public:
    bool Acquire( const std::filesystem::path& root, std::string& error )
    {
        m_path = root / ".writer-lease";
        std::error_code ec;
        if( std::filesystem::create_directory( GpuAnalysisIoPath( m_path ), ec ) ) return Start( error );
        if( ec ) { error = "lease_create_failed:" + ec.message(); return false; }
        const auto heartbeat = m_path / "heartbeat";
        if( LeaseProcessAlive( heartbeat ) ) { error = "writer_lease_active"; return false; }
        // A lease directory without a heartbeat is an interrupted acquisition.
        // It is safe to recover because Start() publishes the first heartbeat
        // synchronously before returning ownership to the caller.
        ec.clear();
        std::filesystem::remove_all( GpuAnalysisIoPath( m_path ), ec ); ec.clear();
        if( !std::filesystem::create_directory( GpuAnalysisIoPath( m_path ), ec ) || ec ) { error = "stale_lease_recovery_failed"; return false; }
        return Start( error );
    }

    ~WriterLease()
    {
        m_stop.request_stop();
        if( m_heartbeat.joinable() ) m_heartbeat.join();
        std::error_code ec; if( !m_path.empty() ) std::filesystem::remove_all( GpuAnalysisIoPath( m_path ), ec );
    }

private:
    bool Start( std::string& error )
    {
        const auto update = [this]() {
            std::ofstream out( GpuAnalysisIoPath( m_path / "heartbeat" ), std::ios::binary | std::ios::trunc );
            out << "pid=" << ProcessId() << "\n";
        };
        update();
        if( !std::filesystem::exists( GpuAnalysisIoPath( m_path / "heartbeat" ) ) ) { error = "lease_heartbeat_failed"; return false; }
        m_heartbeat = std::jthread( [this, update]( std::stop_token token ) {
            while( !token.stop_requested() && !m_stop.stop_requested() )
            {
                for( int i = 0; i < 20 && !token.stop_requested() && !m_stop.stop_requested(); ++i )
                    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
                if( !token.stop_requested() && !m_stop.stop_requested() ) update();
            }
        } );
        return true;
    }
    std::filesystem::path m_path;
    std::stop_source m_stop;
    std::jthread m_heartbeat;
};

}

std::filesystem::path GpuAnalysisSidecarPath( const std::filesystem::path& tracePath )
{
    auto result = tracePath; result += GpuAnalysisSidecarSuffix; return result;
}

std::filesystem::path GpuAnalysisDerivedPath( const std::filesystem::path& sidecarPath )
{
    return sidecarPath / "derived" / std::to_string( GpuAnalysisDerivedSchemaVersion ) / GpuAnalysisAlgorithmId;
}

GpuAnalysisTraceIdentity ComputeGpuAnalysisQuickIdentity( const std::filesystem::path& tracePath )
{
    GpuAnalysisTraceIdentity result;
    result.fileSize = std::filesystem::file_size( GpuAnalysisIoPath( tracePath ) ); result.writeTime = WriteTime( tracePath );
#ifdef _WIN32
    const auto ioTracePath = GpuAnalysisIoPath( tracePath );
    const auto handle = CreateFileW( ioTracePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( handle != INVALID_HANDLE_VALUE )
    {
        BY_HANDLE_FILE_INFORMATION info {};
        if( GetFileInformationByHandle( handle, &info ) )
        {
            result.volumeId = info.dwVolumeSerialNumber; result.fileIdHigh = info.nFileIndexHigh; result.fileIdLow = info.nFileIndexLow;
        }
        CloseHandle( handle );
    }
#endif
    std::ifstream in( GpuAnalysisIoPath( tracePath ), std::ios::binary );
    if( !in ) throw std::runtime_error( "trace_identity_open_failed" );
    constexpr uint64_t segment = 64 * 1024;
    std::vector<uint8_t> buffer( size_t( std::min<uint64_t>( segment, result.fileSize ) ) );
    const auto readAt = [&]( uint64_t position ) {
        if( buffer.empty() ) return FnvOffset;
        in.clear(); in.seekg( std::streamoff( position ) ); in.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( buffer.size() ) );
        if( size_t( in.gcount() ) != buffer.size() ) throw std::runtime_error( "trace_identity_read_failed" );
        return Hash64( buffer.data(), buffer.size() );
    };
    result.headHash = readAt( 0 );
    result.middleHash = readAt( result.fileSize > buffer.size() ? ( result.fileSize - buffer.size() ) / 2 : 0 );
    result.tailHash = readAt( result.fileSize > buffer.size() ? result.fileSize - buffer.size() : 0 );
    return result;
}

GpuAnalysisIdentityState VerifyGpuAnalysisIdentity( const std::filesystem::path& tracePath,
    const GpuAnalysisTraceIdentity& expected, bool strong, std::string& reason )
{
    reason.clear();
    try
    {
        auto actual = ComputeGpuAnalysisQuickIdentity( tracePath );
        if( actual.fileSize != expected.fileSize || actual.headHash != expected.headHash ||
            actual.middleHash != expected.middleHash || actual.tailHash != expected.tailHash )
        { reason = "trace_quick_identity_mismatch"; return GpuAnalysisIdentityState::Mismatch; }
        if( strong || ( expected.volumeId != 0 && actual.volumeId != expected.volumeId ) ||
            ( expected.fileIdLow != 0 && actual.fileIdLow != expected.fileIdLow ) )
        {
            if( expected.sha256.empty() ) { reason = "strong_identity_pending"; return GpuAnalysisIdentityState::Pending; }
            if( Sha256File( tracePath ) != expected.sha256 ) { reason = "trace_sha256_mismatch"; return GpuAnalysisIdentityState::Mismatch; }
            return GpuAnalysisIdentityState::StrongVerified;
        }
        return GpuAnalysisIdentityState::QuickVerified;
    }
    catch( const std::exception& e ) { reason = e.what(); return GpuAnalysisIdentityState::Pending; }
}

bool WriteGpuAnalysisRawSidecar( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const JnTraceData& data,
    const GpuAnalysisSidecarControl& control, std::string& error )
{
    error.clear();
    try
    {
        const auto space = std::filesystem::space( GpuAnalysisIoPath( sidecarPath.has_parent_path() ? sidecarPath.parent_path() : std::filesystem::current_path() ) );
        if( space.available < control.minimumFreeBytes ) { error = "insufficient_free_space_for_gpu_analysis"; return false; }
        std::error_code ec;
        std::filesystem::create_directories( GpuAnalysisIoPath( sidecarPath ), ec );
        if( ec ) { error = "sidecar_directory_failed:" + ec.message(); return false; }
        GpuAnalysisSidecarManifest manifest;
        manifest.state = GpuAnalysisSidecarState::RawBuilding; manifest.identity = identity;
        manifest.identityState = identity.sha256.empty() ? GpuAnalysisIdentityState::Pending : GpuAnalysisIdentityState::StrongVerified;
        manifest.rawGeneration = GenerationName(); manifest.summary = ComputeSummary( data );
        const auto generationRoot = sidecarPath / "raw" / manifest.rawGeneration;
        std::filesystem::create_directories( GpuAnalysisIoPath( generationRoot ), ec );
        if( ec ) { error = "raw_generation_directory_failed:" + ec.message(); return false; }
        if( !SaveManifest( sidecarPath, manifest, error ) ) return false;
        uint64_t totalBytes = 0;
        const auto report = [&]( float progress, const char* stage ) { if( control.progress ) control.progress( progress, stage ); };
#define W( kind, stem, field ) if( !WritePodShards( sidecarPath, generationRoot, RawKind::kind, stem, data.field, manifest.shards, totalBytes, control, error ) ) return false
        report( .05f, "raw-control" ); W( CatalogControl, "catalog-control", gpuCatalogControls );
        W( CatalogBatch, "catalog-batch", gpuCatalogBatches ); W( CatalogGeneration, "catalog-generation", gpuCatalogGenerations );
        if( !WriteStringShards( sidecarPath, generationRoot, data.gpuCatalogStrings, manifest.shards, totalBytes, control, error ) ) return false;
        report( .20f, "raw-catalog" ); W( Resource, "resources", gpuCatalogResources ); W( Allocation, "allocations", gpuCatalogAllocations );
        W( View, "views", gpuCatalogViews ); W( Logical, "logicals", gpuCatalogLogicals ); W( Part, "parts", gpuCatalogParts );
        W( CatalogRelation, "catalog-relations", gpuCatalogRelations ); W( VirtualGeometry, "vg", gpuCatalogVg );
        report( .55f, "raw-evidence" ); W( Range, "ranges", gpuRangeSets ); W( DetailedEvidence, "detailed-evidence", gpuDetailedEvidence );
        W( ReferencePass, "reference-passes", gpuReferencePasses ); W( ReferenceUse, "reference-uses", gpuReferenceUses );
        W( ReferenceEnd, "reference-ends", gpuReferenceEnds ); W( Relation, "relations", relations );
#undef W
        report( .90f, "exact-summary" );
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        if( !WriteSummary( sidecarPath, manifest.summary, error ) ) return false;
        report( .98f, "manifest-publish" );
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        manifest.rawComplete = true; manifest.state = GpuAnalysisSidecarState::RawComplete;
        manifest.reason = manifest.summary.catalogPresent ? ( manifest.summary.catalogValid ? "raw_complete" : "invalid_catalog_generation" ) : "gpu_catalog_not_present";
        if( !SaveManifest( sidecarPath, manifest, error ) ) return false;
        report( 1.f, "raw-complete" ); return true;
    }
    catch( const std::exception& e ) { error = e.what(); return false; }
}

bool PublishGpuAnalysisSidecar( const std::filesystem::path& stagingPath,
    const std::filesystem::path& finalPath, bool overwrite, std::string& error )
{
    error.clear(); std::error_code ec;
    if( !std::filesystem::exists( GpuAnalysisIoPath( stagingPath ) ) ) { error = "sidecar_staging_missing"; return false; }
    std::filesystem::path previous;
    if( std::filesystem::exists( GpuAnalysisIoPath( finalPath ) ) )
    {
        if( !overwrite ) { error = "sidecar_output_exists"; return false; }
        previous = finalPath; previous += ".obsolete-" + GenerationName();
        std::filesystem::rename( GpuAnalysisIoPath( finalPath ), GpuAnalysisIoPath( previous ), ec );
        if( ec ) { error = "sidecar_previous_retire_failed:" + ec.message(); return false; }
    }
    std::filesystem::rename( GpuAnalysisIoPath( stagingPath ), GpuAnalysisIoPath( finalPath ), ec );
    if( ec )
    {
        if( !previous.empty() ) { std::error_code restore; std::filesystem::rename( GpuAnalysisIoPath( previous ), GpuAnalysisIoPath( finalPath ), restore ); }
        error = "sidecar_publish_failed:" + ec.message(); return false;
    }
    if( !previous.empty() ) std::filesystem::remove_all( GpuAnalysisIoPath( previous ), ec );
    return true;
}

std::optional<GpuAnalysisSidecarManifest> LoadGpuAnalysisSidecarManifest(
    const std::filesystem::path& sidecarPath, std::string& error )
{
    error.clear(); std::ifstream in( GpuAnalysisIoPath( sidecarPath / "manifest" ), std::ios::binary );
    if( !in ) { error = "gpu_analysis_sidecar_not_found"; return std::nullopt; }
    GpuAnalysisSidecarManifest result; std::string key; uint64_t magic = 0; size_t expectedShards = 0;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "manifest_schema" ) in >> result.manifestSchema;
        else if( key == "raw_schema" ) in >> result.rawSchema;
        else if( key == "derived_schema" ) in >> result.derivedSchema;
        else if( key == "algorithm" ) in >> std::quoted( result.algorithmId );
        else if( key == "raw_generation" ) in >> std::quoted( result.rawGeneration );
        else if( key == "derived_generation" ) in >> std::quoted( result.derivedGeneration );
        else if( key == "state" ) { unsigned v; in >> v; result.state = GpuAnalysisSidecarState( v ); }
        else if( key == "identity_state" ) { unsigned v; in >> v; result.identityState = GpuAnalysisIdentityState( v ); }
        else if( key == "trace_sha256" ) in >> std::quoted( result.identity.sha256 );
        else if( key == "trace_size" ) in >> result.identity.fileSize;
        else if( key == "trace_write_time" ) in >> result.identity.writeTime;
        else if( key == "trace_volume" ) in >> result.identity.volumeId;
        else if( key == "trace_file_id_high" ) in >> result.identity.fileIdHigh;
        else if( key == "trace_file_id_low" ) in >> result.identity.fileIdLow;
        else if( key == "trace_head_hash" ) in >> result.identity.headHash;
        else if( key == "trace_middle_hash" ) in >> result.identity.middleHash;
        else if( key == "trace_tail_hash" ) in >> result.identity.tailHash;
#define R( field ) else if( key == "summary_" #field ) in >> result.summary.field
        R( resourceRecordCount ); R( allocationRecordCount ); R( passCount ); R( referenceUseCount ); R( referenceEndCount );
        R( rangeCount ); R( relationCount ); R( viewRecordCount ); R( logicalRecordCount ); R( partRecordCount );
        R( vgRecordCount ); R( evidenceRecordCount ); R( generationCount ); R( payloadBytes );
        R( allocationCreateCount ); R( allocationDestroyCount ); R( engineKnownPhysicalBytes ); R( engineKnownPhysicalPeakBytes );
        R( engineKnownPhysicalPeakTimeNs ); R( directResourceSetHash ); R( catalogChecksum ); R( unresolvedCount ); R( invalidRecordCount );
#undef R
        else if( key == "summary_catalog_present" ) in >> result.summary.catalogPresent;
        else if( key == "summary_catalog_valid" ) in >> result.summary.catalogValid;
        else if( key == "summary_exact" ) in >> result.summary.exact;
        else if( key == "raw_complete" ) in >> result.rawComplete;
        else if( key == "derived_complete" ) in >> result.derivedComplete;
        else if( key == "reason" ) in >> std::quoted( result.reason );
        else if( key == "shard_count" ) in >> expectedShards;
        else if( key == "shard" )
        {
            GpuAnalysisShard shard; std::string path;
            in >> shard.kind >> shard.elementSize >> shard.firstIndex >> shard.recordCount >> shard.payloadBytes >> shard.checksum >> std::quoted( path );
            shard.relativePath = std::filesystem::u8path( path ); result.shards.emplace_back( std::move( shard ) );
        }
        else { std::string discard; std::getline( in, discard ); }
        if( !in ) { error = "gpu_analysis_manifest_parse_failed:" + key; return std::nullopt; }
    }
    if( magic != ManifestMagic || result.manifestSchema != GpuAnalysisManifestSchemaVersion || result.rawSchema != GpuAnalysisRawSchemaVersion ||
        result.derivedSchema != GpuAnalysisDerivedSchemaVersion || result.algorithmId != GpuAnalysisAlgorithmId || result.shards.size() != expectedShards )
    { error = "gpu_analysis_manifest_schema_mismatch"; return std::nullopt; }
    return result;
}

bool LoadGpuAnalysisRawData( const std::filesystem::path& sidecarPath,
    const GpuAnalysisSidecarManifest& manifest, JnTraceData& data,
    const GpuAnalysisSidecarControl& control, std::string& error )
{
    error.clear();
    if( !manifest.rawComplete ) { error = "gpu_analysis_raw_incomplete"; return false; }
    data = {}; data.present = true; data.schemaVersion = 12;
    data.gpuCatalogPresent = manifest.summary.catalogPresent; data.gpuCatalogValid = manifest.summary.catalogValid;
    data.gpuCatalogSchemaVersion = JnGpuCatalogSchemaVersion; data.gpuDetailedEvidenceSchemaVersion = JnGpuDetailedEvidenceSchemaVersion;
    for( size_t index = 0; index < manifest.shards.size(); ++index )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        const auto& shard = manifest.shards[index]; const auto path = sidecarPath / shard.relativePath;
        bool ok = false;
        switch( RawKind( shard.kind ) )
        {
#define C( kind, field ) case RawKind::kind: ok = ReadPodShard( path, shard, data.field, error ); break
        C( CatalogControl, gpuCatalogControls ); C( CatalogBatch, gpuCatalogBatches ); C( CatalogGeneration, gpuCatalogGenerations );
        case RawKind::CatalogString: ok = ReadStringShard( path, shard, data.gpuCatalogStrings, error ); break;
        C( Resource, gpuCatalogResources ); C( Allocation, gpuCatalogAllocations ); C( View, gpuCatalogViews );
        C( Logical, gpuCatalogLogicals ); C( Part, gpuCatalogParts ); C( CatalogRelation, gpuCatalogRelations );
        C( VirtualGeometry, gpuCatalogVg ); C( Range, gpuRangeSets ); C( DetailedEvidence, gpuDetailedEvidence );
        C( ReferencePass, gpuReferencePasses ); C( ReferenceUse, gpuReferenceUses ); C( ReferenceEnd, gpuReferenceEnds ); C( Relation, relations );
#undef C
        default: error = "unknown_gpu_analysis_raw_kind"; return false;
        }
        if( !ok ) return false;
        if( control.progress ) control.progress( float( index + 1 ) / float( std::max<size_t>( 1, manifest.shards.size() ) ), "load-raw" );
    }
    const auto summary = ComputeSummary( data );
    if( summary.catalogChecksum != manifest.summary.catalogChecksum || summary.directResourceSetHash != manifest.summary.directResourceSetHash ||
        summary.resourceRecordCount != manifest.summary.resourceRecordCount || summary.allocationRecordCount != manifest.summary.allocationRecordCount )
    { error = "gpu_analysis_raw_summary_mismatch"; return false; }
    return true;
}

bool BuildGpuAnalysisDerived( const std::filesystem::path& tracePath,
    const GpuAnalysisSidecarControl& control, std::string& error )
{
    error.clear(); const auto sidecar = GpuAnalysisSidecarPath( tracePath );
    auto manifest = LoadGpuAnalysisSidecarManifest( sidecar, error ); if( !manifest ) return false;
    std::string identityReason;
    const auto identity = VerifyGpuAnalysisIdentity( tracePath, manifest->identity, false, identityReason );
    if( identity == GpuAnalysisIdentityState::Mismatch ) { error = identityReason; return false; }
    WriterLease lease; if( !lease.Acquire( sidecar, error ) ) return false;
    manifest->state = GpuAnalysisSidecarState::DerivedBuilding; manifest->derivedComplete = false; manifest->reason = "derived_building";
    if( !SaveManifest( sidecar, *manifest, error ) ) return false;
    const auto derived = GpuAnalysisDerivedPath( sidecar ); std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( derived ), ec );
    if( ec ) { error = "derived_directory_failed:" + ec.message(); return false; }
    if( !SaveCheckpoint( derived / "checkpoint", "load-raw", error ) ) return false;
    JnTraceData data;
    if( !LoadGpuAnalysisRawData( sidecar, *manifest, data, control, error ) )
    {
        manifest->state = error == "cancelled" ? GpuAnalysisSidecarState::Cancelled : GpuAnalysisSidecarState::Failed;
        manifest->reason = error; SaveManifest( sidecar, *manifest, identityReason ); return false;
    }
    // Refresh the exact summary while raw shards are resident. This also
    // upgrades sidecars produced by early N29 builds without rereading the
    // original .tracy file.
    manifest->summary = ComputeSummary( data );
    if( !WriteSummary( sidecar, manifest->summary, error ) || !SaveManifest( sidecar, *manifest, error ) ) return false;
    if( !SaveCheckpoint( derived / "checkpoint", "build-indexes", error ) ) return false;
    GpuAnalysisBuildControl buildControl; buildControl.stopToken = control.stopToken;
    buildControl.progress = [&]( float value, const char* stage ) { if( control.progress ) control.progress( value, stage ); };
    auto snapshot = BuildGpuAnalysisSnapshotConsuming( data, nullptr, {}, buildControl );
    if( snapshot.manifest.state == GpuAnalysisState::Cancelled || control.stopToken.stop_requested() )
    { error = "cancelled"; manifest->state = GpuAnalysisSidecarState::Cancelled; manifest->reason = error; SaveManifest( sidecar, *manifest, identityReason ); return false; }
    if( !SaveCheckpoint( derived / "checkpoint", "publish-store", error ) ) return false;
    std::string derivedGeneration; uint64_t writtenBytes = 0;
    if( !WriteGpuAnalysisDerivedStore( sidecar, manifest->identity, snapshot, control, derivedGeneration, writtenBytes, error ) )
    {
        manifest->state = error == "cancelled" ? GpuAnalysisSidecarState::Cancelled : GpuAnalysisSidecarState::Failed;
        manifest->reason = error; SaveManifest( sidecar, *manifest, identityReason ); return false;
    }
    manifest->derivedGeneration = std::move( derivedGeneration ); manifest->derivedComplete = true; manifest->state = GpuAnalysisSidecarState::Ready;
    manifest->identityState = identity; manifest->reason = snapshot.manifest.reason;
    if( !SaveCheckpoint( derived / "checkpoint", "complete", error ) || !SaveManifest( sidecar, *manifest, error ) ) return false;
    return true;
}

std::optional<GpuAnalysisSnapshot> LoadGpuAnalysisSidecarSnapshot(
    const std::filesystem::path& tracePath, bool strongIdentity,
    GpuAnalysisSidecarManifest* manifestOut, std::string& error )
{
    auto reader = GpuAnalysisStoreReader::Open( tracePath, strongIdentity, manifestOut, error );
    if( !reader ) return std::nullopt;
    auto snapshot = reader->Overview();
    std::vector<GpuResourceAnalysisRecord> resources; std::vector<GpuAllocationAnalysisRecord> allocations; std::vector<GpuPassWorkingSet> passes;
    for( size_t page = 0; page < reader->ResourcePageCount(); ++page )
    {
        if( !reader->LoadResourcePage( page, resources, error ) ) return std::nullopt;
        for( auto& value : resources ) { snapshot.resourceById[value.resourceId] = snapshot.resources.size(); snapshot.resources.push_back( std::move( value ) ); }
    }
    for( size_t page = 0; page < reader->AllocationPageCount(); ++page )
    {
        if( !reader->LoadAllocationPage( page, allocations, error ) ) return std::nullopt;
        for( auto& value : allocations ) { snapshot.allocationById[value.allocationId] = snapshot.allocations.size(); snapshot.allocations.push_back( std::move( value ) ); }
    }
    for( size_t page = 0; page < reader->PassPageCount(); ++page )
    {
        if( !reader->LoadPassPage( page, passes, error ) ) return std::nullopt;
        for( auto& value : passes ) { snapshot.passById[value.passId] = snapshot.passes.size(); snapshot.passes.push_back( std::move( value ) ); }
    }
    return snapshot;
}

const char* GpuAnalysisIdentityStateName( GpuAnalysisIdentityState value )
{
    switch( value )
    {
    case GpuAnalysisIdentityState::QuickVerified: return "identity_quick_verified";
    case GpuAnalysisIdentityState::StrongVerified: return "identity_strong_verified";
    case GpuAnalysisIdentityState::Pending: return "identity_pending";
    case GpuAnalysisIdentityState::Mismatch: return "identity_mismatch";
    }
    return "identity_unknown";
}

const char* GpuAnalysisSidecarStateName( GpuAnalysisSidecarState value )
{
    switch( value )
    {
    case GpuAnalysisSidecarState::Missing: return "missing"; case GpuAnalysisSidecarState::RawBuilding: return "raw_building";
    case GpuAnalysisSidecarState::RawComplete: return "raw_complete"; case GpuAnalysisSidecarState::DerivedBuilding: return "derived_building";
    case GpuAnalysisSidecarState::Ready: return "ready"; case GpuAnalysisSidecarState::Cancelled: return "cancelled";
    case GpuAnalysisSidecarState::Failed: return "failed";
    }
    return "unknown";
}

}
