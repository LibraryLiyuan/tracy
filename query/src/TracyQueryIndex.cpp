#include "TracyQueryIndex.hpp"
#include "TracyFileHeader.hpp"
#include "TracyWorker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace tracy::query
{
namespace
{

using nlohmann::json;
constexpr const char* IndexMagic = "JNTRACY-QUERY-INDEX";
constexpr uint64_t MaximumManifestBytes = 1024 * 1024;
constexpr uint64_t MaximumGpuMemorySummaryBytes = 64 * 1024 * 1024;

uint64_t UnsignedProtocolField( std::string_view line, std::string_view key )
{
    const std::string pattern = "|" + std::string( key ) + '=';
    const auto begin = line.find( pattern );
    if( begin == std::string_view::npos ) return 0;
    const auto valueBegin = begin + pattern.size();
    const auto valueEnd = line.find( '|', valueBegin );
    const auto value = line.substr( valueBegin, valueEnd == std::string_view::npos ? line.size() - valueBegin : valueEnd - valueBegin );
    uint64_t result = 0;
    const auto parsed = std::from_chars( value.data(), value.data() + value.size(), result );
    return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() ? result : 0;
}

enum class ZoneSectionKind : uint32_t
{
    Extra = 1,
    Cpu = 2,
    Gpu = 3,
    JobStage = 4,
    GfxEntity = 5,
    GfxLink = 6,
    Relation = 7,
    GpuReferencePass = 8,
    GpuReferenceUse = 9,
    GpuReferenceEnd = 10,
    GpuMemoryCpuZone = 11,
    GpuMemorySummaryCpuZone = 12
};

#pragma pack( push, 1 )
struct ZoneSectionHeader
{
    char magic[8];
    uint32_t schemaVersion;
    uint32_t kind;
    uint32_t recordBytes;
    uint32_t reserved;
    uint64_t count;
    char sourceFingerprint[64];
};

struct ZoneExtraIndexRecord
{
    uint32_t callstack;
    uint32_t text;
    uint32_t name;
    uint32_t color;
    uint8_t flags;
    uint8_t reserved[3];
};

struct CpuZoneIndexRecord
{
    int64_t start;
    int64_t end;
    int64_t childTime;
    uint64_t thread;
    uint64_t parent;
    uint32_t extra;
    uint32_t childCount;
    int16_t sourceLocation;
    uint16_t flags;
};

struct GpuZoneIndexRecord
{
    int64_t cpuStart;
    int64_t cpuEnd;
    int64_t gpuStart;
    int64_t gpuEnd;
    int64_t childGpuTime;
    uint64_t parent;
    uint64_t thread;
    uint64_t childCount;
    uint32_t context;
    uint32_t callstack;
    uint16_t queryId;
    int16_t sourceLocation;
    uint16_t flags;
};
#pragma pack( pop )

static_assert( sizeof( ZoneSectionHeader ) == 96 );
static_assert( sizeof( ZoneExtraIndexRecord ) == 20 );
static_assert( sizeof( CpuZoneIndexRecord ) == 52 );
static_assert( sizeof( GpuZoneIndexRecord ) == 78 );

class MappedSection
{
public:
    ~MappedSection() { try { Close(); } catch( ... ) {} }

    void Open( const std::filesystem::path& path, ZoneSectionKind kind, uint64_t count, uint32_t recordBytes, std::string_view sourceFingerprint )
    {
        if( m_data ) throw std::runtime_error( "zone index section is already open" );
        if( count > ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / recordBytes ) throw std::runtime_error( "zone index section size overflow" );
        m_path = path;
        m_count = count;
        m_recordBytes = recordBytes;
        m_bytes = sizeof( ZoneSectionHeader ) + count * recordBytes;
#ifdef _WIN32
        m_file = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr );
        if( m_file == INVALID_HANDLE_VALUE ) throw std::runtime_error( "cannot create zone index section" );
        LARGE_INTEGER size; size.QuadPart = m_bytes;
        if( !SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) || !SetEndOfFile( m_file ) ) throw std::runtime_error( "cannot size zone index section" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr );
        if( !m_mapping ) throw std::runtime_error( "cannot create zone index mapping" );
        m_data = static_cast<uint8_t*>( MapViewOfFile( m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t( m_bytes ) ) );
        if( !m_data ) throw std::runtime_error( "cannot map zone index section" );
#else
        m_file = ::open( path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600 );
        if( m_file < 0 || ftruncate( m_file, off_t( m_bytes ) ) != 0 ) throw std::runtime_error( "cannot size zone index section" );
        m_data = static_cast<uint8_t*>( mmap( nullptr, size_t( m_bytes ), PROT_READ | PROT_WRITE, MAP_SHARED, m_file, 0 ) );
        if( m_data == MAP_FAILED ) { m_data = nullptr; throw std::runtime_error( "cannot map zone index section" ); }
#endif
        auto* header = reinterpret_cast<ZoneSectionHeader*>( m_data );
        std::memset( header, 0, sizeof( *header ) );
        std::memcpy( header->magic, "JNIDXZ1", 7 );
        header->schemaVersion = QueryIndexSchemaVersion;
        header->kind = uint32_t( kind );
        header->recordBytes = recordBytes;
        header->count = count;
        if( sourceFingerprint.size() != 64 ) throw std::runtime_error( "source fingerprint must contain 64 hexadecimal characters" );
        std::memcpy( header->sourceFingerprint, sourceFingerprint.data(), 64 );
    }

    uint8_t* Record( uint64_t index )
    {
        if( !m_data || index >= m_count )
        {
            std::ostringstream message;
            message << "zone index record is outside declared bounds: path=" << m_path.string() << ", index=" << index << ", count=" << m_count;
            throw std::runtime_error( message.str() );
        }
        return m_data + sizeof( ZoneSectionHeader ) + index * m_recordBytes;
    }

    void Close( uint64_t finalCount = std::numeric_limits<uint64_t>::max() )
    {
        if( !m_data ) return;
        if( finalCount == std::numeric_limits<uint64_t>::max() ) finalCount = m_count;
        if( finalCount > m_count ) throw std::runtime_error( "zone index final count exceeds mapped capacity" );
        reinterpret_cast<ZoneSectionHeader*>( m_data )->count = finalCount;
        const auto finalBytes = sizeof( ZoneSectionHeader ) + finalCount * m_recordBytes;
#ifdef _WIN32
        FlushViewOfFile( m_data, size_t( m_bytes ) );
        UnmapViewOfFile( m_data );
        m_data = nullptr;
        CloseHandle( m_mapping ); m_mapping = nullptr;
        LARGE_INTEGER size; size.QuadPart = finalBytes;
        if( !SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) || !SetEndOfFile( m_file ) ) throw std::runtime_error( "cannot truncate zone index section" );
        FlushFileBuffers( m_file );
        CloseHandle( m_file ); m_file = INVALID_HANDLE_VALUE;
#else
        msync( m_data, size_t( m_bytes ), MS_SYNC );
        munmap( m_data, size_t( m_bytes ) );
        m_data = nullptr;
        if( ftruncate( m_file, off_t( finalBytes ) ) != 0 ) throw std::runtime_error( "cannot truncate zone index section" );
        fsync( m_file );
        close( m_file ); m_file = -1;
#endif
        m_count = finalCount;
        m_bytes = finalBytes;
    }

private:
    std::filesystem::path m_path;
    uint8_t* m_data = nullptr;
    uint64_t m_count = 0;
    uint64_t m_bytes = 0;
    uint32_t m_recordBytes = 0;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    int m_file = -1;
#endif
};

bool AtomicReplace( const std::filesystem::path& source, const std::filesystem::path& target, std::string& error );
std::string ReadManifest( const std::filesystem::path& path, uint64_t maximumBytes = MaximumManifestBytes );

class ZoneIndexWriter final : public SerializedZoneSink
{
public:
    static constexpr size_t JnDomainCount = 7;

    ZoneIndexWriter( std::filesystem::path extras, std::filesystem::path cpu, std::filesystem::path gpu,
        std::array<std::filesystem::path, JnDomainCount> jnPaths, std::string sourceFingerprint )
        : m_extraPath( std::move( extras ) ), m_cpuPath( std::move( cpu ) ), m_gpuPath( std::move( gpu ) )
        , m_jnPaths( std::move( jnPaths ) ), m_sourceFingerprint( std::move( sourceFingerprint ) )
    {}

    void ZoneExtrasBegin( uint64_t count ) override { m_expectedExtras = count; m_extras.Open( m_extraPath, ZoneSectionKind::Extra, count, sizeof( ZoneExtraIndexRecord ), m_sourceFingerprint ); }
    void ZoneExtraRecord( uint64_t index, uint32_t callstack, bool textActive, uint32_t text, bool nameActive, uint32_t name, uint32_t color ) override
    {
        auto* record = reinterpret_cast<ZoneExtraIndexRecord*>( m_extras.Record( index ) );
        *record = { callstack, text, name, color, uint8_t( ( textActive ? 1 : 0 ) | ( nameActive ? 2 : 0 ) ), {} };
        if( m_extraHasText.size() <= index ) m_extraHasText.resize( size_t( index + 1 ) );
        m_extraHasText[size_t( index )] = textActive;
        m_seenExtras++;
    }
    void ZoneExtrasEnd() override { if( m_seenExtras != m_expectedExtras ) throw std::runtime_error( "zone extra index count mismatch" ); m_extras.Close(); }

    void CpuZonesBegin( uint64_t count ) override
    {
        m_expectedCpu = count;
        const auto slack = std::max<uint64_t>( 1024, count / 1000 );
        m_cpu.Open( m_cpuPath, ZoneSectionKind::Cpu, count + slack, sizeof( CpuZoneIndexRecord ), m_sourceFingerprint );
    }
    void CpuZoneBegin( uint64_t index, uint64_t parent, uint64_t thread, int64_t start, int16_t sourceLocation, uint32_t extra, uint32_t childCount ) override
    {
        if( m_cpuSpans.empty() || m_cpuSpans.back().thread != thread ) m_cpuSpans.push_back( { thread, index, 0 } );
        m_cpuSpans.back().count++;
        m_cpuZonesByThread[thread]++;
        if( extra < m_extraHasText.size() && m_extraHasText[extra] ) m_cpuSourcesWithText.emplace( sourceLocation );
        auto* record = reinterpret_cast<CpuZoneIndexRecord*>( m_cpu.Record( index ) );
        *record = { start, 0, 0, thread, parent, extra, childCount, sourceLocation, 0 };
        m_seenCpu++;
    }
    void CpuZoneEnd( uint64_t index, int64_t end ) override
    {
        auto* record = reinterpret_cast<CpuZoneIndexRecord*>( m_cpu.Record( index ) );
        if( !QueryIndexCpuZoneTimingComplete( end ) ) return;
        record->end = end;
        record->flags |= 1;
        if( record->parent != std::numeric_limits<uint64_t>::max() && end >= record->start )
            reinterpret_cast<CpuZoneIndexRecord*>( m_cpu.Record( record->parent ) )->childTime += end - record->start;
    }
    void CpuZonesEnd() override
    {
        auto spans = m_cpuSpans;
        std::sort( spans.begin(), spans.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.thread != rhs.thread ? lhs.thread < rhs.thread : lhs.start < rhs.start;
        } );
        auto sortedPath = m_cpuPath;
        sortedPath += ".sorted";
        try
        {
            m_cpu.Close( m_seenCpu );
            std::ifstream input( m_cpuPath, std::ios::binary );
            std::ofstream output( sortedPath, std::ios::binary | std::ios::trunc );
            if( !input || !output ) throw std::runtime_error( "cannot open CPU-zone sorting streams" );
            ZoneSectionHeader header {};
            input.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
            output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
            if( !input || !output ) throw std::runtime_error( "cannot copy CPU-zone section header" );
            constexpr size_t RecordsPerBlock = 64 * 1024;
            std::vector<CpuZoneIndexRecord> records( RecordsPerBlock );
            uint64_t destination = 0;
            for( const auto& span : spans )
            {
                input.seekg( std::streamoff( sizeof( ZoneSectionHeader ) + span.start * sizeof( CpuZoneIndexRecord ) ), std::ios::beg );
                for( uint64_t offset = 0; offset < span.count; )
                {
                    const auto count = size_t( std::min<uint64_t>( RecordsPerBlock, span.count - offset ) );
                    input.read( reinterpret_cast<char*>( records.data() ), std::streamsize( count * sizeof( CpuZoneIndexRecord ) ) );
                    if( !input ) throw std::runtime_error( "cannot read CPU-zone sorting block" );
                    for( size_t recordIndex = 0; recordIndex < count; recordIndex++ )
                    {
                        auto& record = records[recordIndex];
                        if( record.parent != std::numeric_limits<uint64_t>::max() )
                        {
                            if( record.parent < span.start || record.parent >= span.start + span.count )
                                throw std::runtime_error( "CPU-zone parent crosses a serialized thread span" );
                            record.parent = destination + ( record.parent - span.start );
                        }
                    }
                    output.write( reinterpret_cast<const char*>( records.data() ), std::streamsize( count * sizeof( CpuZoneIndexRecord ) ) );
                    if( !output ) throw std::runtime_error( "cannot write CPU-zone sorting block" );
                    offset += count;
                }
                destination += span.count;
            }
            if( destination != m_seenCpu ) throw std::runtime_error( "CPU-zone sorted span count mismatch" );
            output.flush();
            if( !output ) throw std::runtime_error( "cannot flush sorted CPU-zone section" );
            input.close(); output.close();
            std::string error;
            if( !AtomicReplace( sortedPath, m_cpuPath, error ) ) throw std::runtime_error( "cannot commit sorted CPU-zone index: " + error );
        }
        catch( ... )
        {
            std::error_code ignored;
            std::filesystem::remove( sortedPath, ignored );
            throw;
        }
    }

    void GpuZonesBegin( uint64_t count ) override
    {
        m_expectedGpu = count;
        const auto slack = std::max<uint64_t>( 1024, count / 1000 );
        m_gpu.Open( m_gpuPath, ZoneSectionKind::Gpu, count + slack, sizeof( GpuZoneIndexRecord ), m_sourceFingerprint );
    }
    void GpuZoneBegin( uint64_t index, uint64_t parent, uint32_t context, int64_t cpuStart, int64_t gpuStart,
        int16_t sourceLocation, uint32_t callstack, uint64_t thread, uint64_t childCount ) override
    {
        auto* record = reinterpret_cast<GpuZoneIndexRecord*>( m_gpu.Record( index ) );
        *record = { cpuStart, 0, gpuStart, 0, 0, parent, thread, childCount, context, callstack, 0, sourceLocation, 0 };
        m_gpuZonesByContext[context]++;
        m_seenGpu++;
    }
    void GpuZoneEnd( uint64_t index, int64_t cpuEnd, int64_t gpuEnd, uint16_t queryId ) override
    {
        auto* record = reinterpret_cast<GpuZoneIndexRecord*>( m_gpu.Record( index ) );
        record->queryId = queryId;
        if( !QueryIndexGpuZoneTimingComplete( cpuEnd, gpuEnd ) ) return;
        record->cpuEnd = cpuEnd; record->gpuEnd = gpuEnd;
        record->flags |= 1;
        if( record->parent != std::numeric_limits<uint64_t>::max() && gpuEnd >= record->gpuStart )
            reinterpret_cast<GpuZoneIndexRecord*>( m_gpu.Record( record->parent ) )->childGpuTime += gpuEnd - record->gpuStart;
    }
    void GpuZonesEnd() override { m_gpu.Close( m_seenGpu ); }

    bool WantsJnRecords( JnDomain domain, uint32_t recordBytes ) const override
    {
        return recordBytes == JnRecordBytes( domain );
    }
    void JnRecordsBegin( JnDomain domain, uint64_t count, uint32_t recordBytes ) override
    {
        const auto index = size_t( domain );
        if( index >= JnDomainCount || recordBytes != JnRecordBytes( domain ) ) throw std::runtime_error( "JN index domain layout mismatch" );
        m_jnStarted[index] = true;
        m_jnExpected[index] = count;
        m_jn[index].Open( m_jnPaths[index], JnSectionKind( domain ), count, recordBytes, m_sourceFingerprint );
    }
    void JnRecordsBlock( JnDomain domain, uint64_t offset, const void* records, uint64_t count, uint32_t recordBytes ) override
    {
        const auto index = size_t( domain );
        if( index >= JnDomainCount || recordBytes != JnRecordBytes( domain ) || offset != m_jnSeen[index] || count > m_jnExpected[index] - offset )
            throw std::runtime_error( "JN index block is out of sequence" );
        if( count != 0 ) std::memcpy( m_jn[index].Record( offset ), records, size_t( count * recordBytes ) );
        m_jnSeen[index] += count;
    }
    void JnRecordsEnd( JnDomain domain ) override
    {
        const auto index = size_t( domain );
        if( m_jnSeen[index] != m_jnExpected[index] ) throw std::runtime_error( "JN index count mismatch" );
        m_jn[index].Close( m_jnSeen[index] );
    }

    uint64_t ExtraCount() const { return m_seenExtras; }
    uint64_t CpuCount() const { return m_seenCpu; }
    uint64_t GpuCount() const { return m_seenGpu; }
    uint64_t DeclaredExtraCount() const { return m_expectedExtras; }
    uint64_t DeclaredCpuCount() const { return m_expectedCpu; }
    uint64_t DeclaredGpuCount() const { return m_expectedGpu; }
    int64_t CpuCountDelta() const { return int64_t( m_seenCpu ) - int64_t( m_expectedCpu ); }
    int64_t GpuCountDelta() const { return int64_t( m_seenGpu ) - int64_t( m_expectedGpu ); }
    bool CpuSourceHasText( int16_t sourceLocation ) const { return m_cpuSourcesWithText.find( sourceLocation ) != m_cpuSourcesWithText.end(); }
    const std::unordered_map<uint64_t, uint64_t>& CpuZonesByThread() const { return m_cpuZonesByThread; }
    const std::unordered_map<uint32_t, uint64_t>& GpuZonesByContext() const { return m_gpuZonesByContext; }
    uint64_t JnCount( JnDomain domain ) const { return m_jnSeen[size_t( domain )]; }
    void FinishMissingJnDomains()
    {
        for( size_t index = 0; index < JnDomainCount; index++ )
        {
            if( m_jnStarted[index] ) continue;
            const auto domain = JnDomain( index );
            m_jn[index].Open( m_jnPaths[index], JnSectionKind( domain ), 0, JnRecordBytes( domain ), m_sourceFingerprint );
            m_jn[index].Close( 0 );
        }
    }

    static uint32_t JnRecordBytes( JnDomain domain )
    {
        switch( domain )
        {
        case JnDomain::JobStage: return sizeof( JnJobStageData );
        case JnDomain::GfxEntity: return sizeof( JnGfxEntityData );
        case JnDomain::GfxLink: return sizeof( JnGfxLinkData );
        case JnDomain::Relation: return sizeof( JnRelationData );
        case JnDomain::GpuReferencePass: return sizeof( JnGpuReferencePassData );
        case JnDomain::GpuReferenceUse: return sizeof( JnGpuReferenceUseData );
        case JnDomain::GpuReferenceEnd: return sizeof( JnGpuReferenceEndData );
        }
        throw std::runtime_error( "unknown JN index domain" );
    }
    static ZoneSectionKind JnSectionKind( JnDomain domain ) { return ZoneSectionKind( uint32_t( ZoneSectionKind::JobStage ) + uint32_t( domain ) ); }

private:
    struct CpuSpan { uint64_t thread; uint64_t start; uint64_t count; };
    std::filesystem::path m_extraPath, m_cpuPath, m_gpuPath;
    std::array<std::filesystem::path, JnDomainCount> m_jnPaths;
    std::string m_sourceFingerprint;
    MappedSection m_extras, m_cpu, m_gpu;
    std::array<MappedSection, JnDomainCount> m_jn;
    std::array<uint64_t, JnDomainCount> m_jnExpected {};
    std::array<uint64_t, JnDomainCount> m_jnSeen {};
    std::array<bool, JnDomainCount> m_jnStarted {};
    uint64_t m_expectedExtras = 0, m_expectedCpu = 0, m_expectedGpu = 0;
    uint64_t m_seenExtras = 0, m_seenCpu = 0, m_seenGpu = 0;
    std::vector<CpuSpan> m_cpuSpans;
    std::vector<bool> m_extraHasText;
    std::unordered_set<int16_t> m_cpuSourcesWithText;
    std::unordered_map<uint64_t, uint64_t> m_cpuZonesByThread;
    std::unordered_map<uint32_t, uint64_t> m_gpuZonesByContext;
};

class ReadOnlyZoneSection
{
public:
    ReadOnlyZoneSection() = default;
    ReadOnlyZoneSection( const QueryIndexSection& section, ZoneSectionKind kind, uint32_t recordBytes, std::string_view sourceFingerprint )
    {
        Open( section, kind, recordBytes, sourceFingerprint );
    }
    ~ReadOnlyZoneSection() { Close(); }
    ReadOnlyZoneSection( const ReadOnlyZoneSection& ) = delete;
    ReadOnlyZoneSection& operator=( const ReadOnlyZoneSection& ) = delete;

    template<typename T>
    const T& At( uint64_t index ) const
    {
        if( index >= m_count || sizeof( T ) != m_recordBytes ) throw std::out_of_range( "zone index record is outside the mapped section" );
        return *reinterpret_cast<const T*>( m_data + sizeof( ZoneSectionHeader ) + index * m_recordBytes );
    }
    uint64_t Count() const { return m_count; }

private:
    void Open( const QueryIndexSection& section, ZoneSectionKind kind, uint32_t recordBytes, std::string_view sourceFingerprint )
    {
        if( section.recordBytes != recordBytes || section.count > ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / recordBytes )
            throw std::runtime_error( "zone index section layout mismatch" );
        const auto expectedBytes = sizeof( ZoneSectionHeader ) + section.count * recordBytes;
        if( section.bytes != expectedBytes || std::filesystem::file_size( section.path ) != expectedBytes )
            throw std::runtime_error( "zone index section size mismatch" );
        if( expectedBytes > std::numeric_limits<size_t>::max() ) throw std::runtime_error( "zone index section exceeds address space" );
#ifdef _WIN32
        m_file = CreateFileW( section.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( m_file == INVALID_HANDLE_VALUE ) throw std::runtime_error( "cannot open zone index section" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READONLY, 0, 0, nullptr );
        if( !m_mapping ) { Close(); throw std::runtime_error( "cannot create read-only zone index mapping" ); }
        m_data = static_cast<const uint8_t*>( MapViewOfFile( m_mapping, FILE_MAP_READ, 0, 0, size_t( expectedBytes ) ) );
        if( !m_data ) { Close(); throw std::runtime_error( "cannot map read-only zone index section" ); }
#else
        m_file = ::open( section.path.c_str(), O_RDONLY );
        if( m_file < 0 ) throw std::runtime_error( "cannot open zone index section" );
        m_data = static_cast<const uint8_t*>( mmap( nullptr, size_t( expectedBytes ), PROT_READ, MAP_SHARED, m_file, 0 ) );
        if( m_data == MAP_FAILED ) { m_data = nullptr; Close(); throw std::runtime_error( "cannot map read-only zone index section" ); }
#endif
        m_bytes = expectedBytes;
        const auto& header = *reinterpret_cast<const ZoneSectionHeader*>( m_data );
        if( std::memcmp( header.magic, "JNIDXZ1", 7 ) != 0 || header.schemaVersion != QueryIndexSchemaVersion ||
            header.kind != uint32_t( kind ) || header.recordBytes != recordBytes || header.count != section.count ||
            sourceFingerprint.size() != 64 || std::memcmp( header.sourceFingerprint, sourceFingerprint.data(), 64 ) != 0 )
        {
            Close();
            throw std::runtime_error( "zone index section header mismatch" );
        }
        m_count = section.count;
        m_recordBytes = recordBytes;
    }

    void Close()
    {
#ifdef _WIN32
        if( m_data ) UnmapViewOfFile( m_data );
        if( m_mapping ) CloseHandle( m_mapping );
        if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
        m_mapping = nullptr; m_file = INVALID_HANDLE_VALUE;
#else
        if( m_data ) munmap( const_cast<uint8_t*>( m_data ), size_t( m_bytes ) );
        if( m_file >= 0 ) close( m_file );
        m_file = -1;
#endif
        m_data = nullptr; m_bytes = 0; m_count = 0; m_recordBytes = 0;
    }

    const uint8_t* m_data = nullptr;
    uint64_t m_bytes = 0;
    uint64_t m_count = 0;
    uint32_t m_recordBytes = 0;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    int m_file = -1;
#endif
};

json GpuMemorySummaryJson( const analysis::GpuMemoryAttribution& value )
{
    json logicalResources = json::array();
    for( const auto& item : value.logicalResources ) logicalResources.push_back( {
        { "id", item.logicalResourceId }, { "physical", item.physicalAllocationId }, { "size", item.size },
        { "offset", item.physicalOffset }, { "owner", item.primaryOwnerId }, { "physical_owner", item.physicalOwnerId },
        { "flags", item.flags }, { "kind", uint8_t( item.kind ) }, { "segment", uint8_t( item.segment ) }, { "name", item.name } } );
    json origins = json::array();
    for( const auto& item : value.origins ) origins.push_back( {
        { "allocation", item.allocationId }, { "connection", item.connectionId }, { "cpu_zone", item.cpuZoneIndex },
        { "callstack_requested", item.callstackRequested }, { "layer", uint8_t( item.layer ) }, { "residency", uint8_t( item.residency ) },
        { "replayed", item.replayed }, { "pre_capture", item.preCapture }, { "callstack_emitted", item.callstackEmitted },
        { "residency_managed", item.residencyManaged } } );
    json ownerRollups = json::array();
    for( const auto& item : value.ownerRollups ) ownerRollups.push_back( {
        { "taxonomy", item.taxonomyId }, { "physical_bytes", item.physicalBytes },
        { "physical_count", item.physicalAllocationCount }, { "logical_count", item.logicalResourceCount } } );
    json fragmentation = json::array();
    for( const auto& item : value.fragmentation ) fragmentation.push_back( {
        { "heap", item.heapId }, { "capacity", item.capacityBytes }, { "requested", item.requestedBytes },
        { "covered", item.coveredBytes }, { "aliased", item.aliasedBytes }, { "free", item.freeBytes },
        { "largest_free", item.largestFreeBlockBytes }, { "logical_count", item.logicalResourceCount },
        { "external_ratio", item.externalFragmentationRatio } } );
    json residencyEvents = json::array();
    for( const auto& item : value.residencyEvents ) residencyEvents.push_back( {
        { "allocation", item.allocationId }, { "frame", item.frame }, { "fence", item.fence }, { "size", item.size },
        { "connection", item.connectionId }, { "cpu_zone", item.cpuZoneIndex }, { "flags", item.flags },
        { "reason", item.reason }, { "state", uint8_t( item.state ) }, { "replayed", item.replayed }, { "time", item.timeNs } } );
    json preview = json::array();
    for( const auto& item : value.aggregatedIncompleteReferencePreview ) preview.push_back( {
        { "pass", item.passId }, { "parent", item.parentPassId }, { "label", item.labelId }, { "frame", item.frame },
        { "ordinal", item.ordinal }, { "command_list", item.commandListId }, { "thread", item.thread },
        { "start", item.start }, { "end", item.end }, { "level", item.level }, { "command_count", item.commandCount },
        { "emitted", item.emittedUseCount }, { "total", item.totalUseCount }, { "dropped", item.droppedUses },
        { "truncated", item.truncated }, { "complete", item.complete }, { "flags", item.flags }, { "pairing", uint8_t( item.gpuPairing ) } } );
    return {
        { "schema", 2 }, { "protocol", value.protocolPresent }, { "protocol2", value.protocol2Present },
        { "structured", value.structuredReferencePresent }, { "complete", value.complete },
        { "capture_boundary", value.captureBoundaryPasses }, { "submission_unobserved", value.submissionUnobservedPasses },
        { "gpu_unavailable", value.gpuResultUnavailablePasses }, { "warnings", value.warnings },
        { "logical_count", value.logicalResources.size() }, { "logical_resources", std::move( logicalResources ) },
        { "origins", std::move( origins ) }, { "owner_rollups", std::move( ownerRollups ) },
        { "working_count", value.aggregatedWorkingSetCount },
        { "quality", {
            { "incomplete", value.aggregatedIncompleteReferencePasses },
            { "structured_incomplete", value.aggregatedStructuredIncompleteReferencePasses },
            { "legacy_incomplete", value.aggregatedLegacyIncompleteReferencePasses },
            { "truncated", value.aggregatedTruncatedReferencePasses }, { "failure", value.aggregatedFailureFlagReferencePasses },
            { "command_boundary", value.aggregatedCommandListBoundaryPasses }, { "dropped", value.aggregatedDroppedReferenceUses },
            { "preview", std::move( preview ) } } },
        { "churn", {
            { "peak", value.churn.peakPhysicalBytes }, { "active", value.churn.activePhysicalBytes },
            { "created_bytes", value.churn.createdBytes }, { "created_count", value.churn.createdCount },
            { "freed_bytes", value.churn.freedBytes }, { "freed_count", value.churn.freedCount },
            { "baseline_freed_bytes", value.churn.freedFromBaselineBytes }, { "baseline_freed_count", value.churn.freedFromBaselineCount } } },
        { "residency", {
            { "resident_bytes", value.residency.residentBytes }, { "resident_count", value.residency.residentCount },
            { "evicted_bytes", value.residency.evictedBytes }, { "evicted_count", value.residency.evictedCount },
            { "unknown_bytes", value.residency.unknownBytes }, { "unknown_count", value.residency.unknownCount } } },
        { "fragmentation", std::move( fragmentation ) }, { "residency_events", std::move( residencyEvents ) }
    };
}

analysis::GpuMemoryAttribution ParseGpuMemorySummary( const json& value )
{
    if( value.at( "schema" ).get<uint32_t>() != 2 ) throw std::runtime_error( "GPU-memory summary schema mismatch" );
    analysis::GpuMemoryAttribution result;
    result.protocolPresent = value.at( "protocol" ).get<bool>(); result.protocol2Present = value.at( "protocol2" ).get<bool>();
    result.structuredReferencePresent = value.at( "structured" ).get<bool>(); result.complete = value.at( "complete" ).get<bool>();
    result.captureBoundaryPasses = value.at( "capture_boundary" ).get<uint64_t>();
    result.submissionUnobservedPasses = value.at( "submission_unobserved" ).get<uint64_t>();
    result.gpuResultUnavailablePasses = value.at( "gpu_unavailable" ).get<uint64_t>(); result.warnings = value.at( "warnings" ).get<std::vector<std::string>>();
    for( const auto& item : value.at( "logical_resources" ) )
    {
        analysis::GpuMemoryLogicalResource resource;
        resource.logicalResourceId = item.at( "id" ).get<uint64_t>(); resource.physicalAllocationId = item.at( "physical" ).get<uint64_t>();
        resource.size = item.at( "size" ).get<uint64_t>(); resource.physicalOffset = item.at( "offset" ).get<uint64_t>();
        resource.primaryOwnerId = item.at( "owner" ).get<uint32_t>(); resource.physicalOwnerId = item.at( "physical_owner" ).get<uint32_t>();
        resource.flags = item.at( "flags" ).get<uint32_t>(); resource.kind = char( item.at( "kind" ).get<uint8_t>() );
        resource.segment = char( item.at( "segment" ).get<uint8_t>() ); resource.name = item.at( "name" ).get<std::string>();
        result.logicalById.emplace( resource.logicalResourceId, result.logicalResources.size() ); result.logicalResources.emplace_back( resource );
    }
    if( result.logicalResources.size() != value.at( "logical_count" ).get<size_t>() ) throw std::runtime_error( "GPU-memory logical summary count mismatch" );
    for( const auto& item : value.at( "origins" ) )
    {
        analysis::GpuMemoryAllocationOrigin origin;
        origin.allocationId = item.at( "allocation" ).get<uint64_t>(); origin.connectionId = item.at( "connection" ).get<uint64_t>();
        origin.cpuZoneIndex = item.at( "cpu_zone" ).get<uint64_t>(); origin.callstackRequested = item.at( "callstack_requested" ).get<uint32_t>();
        origin.layer = char( item.at( "layer" ).get<uint8_t>() ); origin.residency = char( item.at( "residency" ).get<uint8_t>() );
        origin.replayed = item.at( "replayed" ).get<bool>(); origin.preCapture = item.at( "pre_capture" ).get<bool>();
        origin.callstackEmitted = item.at( "callstack_emitted" ).get<bool>(); origin.residencyManaged = item.at( "residency_managed" ).get<bool>();
        const auto index = result.origins.size(); result.origins.emplace_back( origin );
        if( origin.layer == 'P' ) result.physicalOriginById[origin.allocationId] = index;
        else if( origin.layer == 'L' ) result.logicalOriginById[origin.allocationId] = index;
    }
    for( const auto& item : value.at( "owner_rollups" ) ) result.ownerRollups.push_back( {
        item.at( "taxonomy" ).get<uint32_t>(), item.at( "physical_bytes" ).get<uint64_t>(),
        item.at( "physical_count" ).get<uint64_t>(), item.at( "logical_count" ).get<uint64_t>() } );
    result.passQualityAggregated = true; result.aggregatedWorkingSetCount = value.at( "working_count" ).get<uint64_t>();
    const auto& quality = value.at( "quality" );
    result.aggregatedIncompleteReferencePasses = quality.at( "incomplete" ).get<uint64_t>();
    result.aggregatedStructuredIncompleteReferencePasses = quality.at( "structured_incomplete" ).get<uint64_t>();
    result.aggregatedLegacyIncompleteReferencePasses = quality.at( "legacy_incomplete" ).get<uint64_t>();
    result.aggregatedTruncatedReferencePasses = quality.at( "truncated" ).get<uint64_t>();
    result.aggregatedFailureFlagReferencePasses = quality.at( "failure" ).get<uint64_t>();
    result.aggregatedCommandListBoundaryPasses = quality.at( "command_boundary" ).get<uint64_t>();
    result.aggregatedDroppedReferenceUses = quality.at( "dropped" ).get<uint64_t>();
    for( const auto& item : quality.at( "preview" ) )
    {
        analysis::GpuMemoryPass pass;
        pass.passId = item.at( "pass" ).get<uint64_t>(); pass.parentPassId = item.at( "parent" ).get<uint64_t>();
        pass.labelId = item.at( "label" ).get<uint64_t>(); pass.frame = item.at( "frame" ).get<uint64_t>(); pass.ordinal = item.at( "ordinal" ).get<uint64_t>();
        pass.commandListId = item.at( "command_list" ).get<uint64_t>(); pass.thread = item.at( "thread" ).get<uint64_t>();
        pass.start = item.at( "start" ).get<int64_t>(); pass.end = item.at( "end" ).get<int64_t>(); pass.level = item.at( "level" ).get<int>();
        pass.commandCount = item.at( "command_count" ).get<uint32_t>(); pass.emittedUseCount = item.at( "emitted" ).get<uint32_t>();
        pass.totalUseCount = item.at( "total" ).get<uint32_t>(); pass.droppedUses = item.at( "dropped" ).get<uint32_t>();
        pass.truncated = item.at( "truncated" ).get<bool>(); pass.complete = item.at( "complete" ).get<bool>();
        pass.structuredBinary = true; pass.flags = item.at( "flags" ).get<uint8_t>(); pass.gpuPairing = analysis::GpuZonePairing( item.at( "pairing" ).get<uint8_t>() );
        result.aggregatedIncompleteReferencePreview.emplace_back( std::move( pass ) );
    }
    const auto& churn = value.at( "churn" );
    result.churn = { churn.at( "peak" ).get<uint64_t>(), churn.at( "active" ).get<uint64_t>(), churn.at( "created_bytes" ).get<uint64_t>(),
        churn.at( "created_count" ).get<uint64_t>(), churn.at( "freed_bytes" ).get<uint64_t>(), churn.at( "freed_count" ).get<uint64_t>(),
        churn.at( "baseline_freed_bytes" ).get<uint64_t>(), churn.at( "baseline_freed_count" ).get<uint64_t>() };
    const auto& residency = value.at( "residency" );
    result.residency = { residency.at( "resident_bytes" ).get<uint64_t>(), residency.at( "resident_count" ).get<uint64_t>(),
        residency.at( "evicted_bytes" ).get<uint64_t>(), residency.at( "evicted_count" ).get<uint64_t>(),
        residency.at( "unknown_bytes" ).get<uint64_t>(), residency.at( "unknown_count" ).get<uint64_t>() };
    for( const auto& item : value.at( "fragmentation" ) ) result.fragmentation.push_back( {
        item.at( "heap" ).get<uint64_t>(), item.at( "capacity" ).get<uint64_t>(), item.at( "requested" ).get<uint64_t>(),
        item.at( "covered" ).get<uint64_t>(), item.at( "aliased" ).get<uint64_t>(), item.at( "free" ).get<uint64_t>(),
        item.at( "largest_free" ).get<uint64_t>(), item.at( "logical_count" ).get<uint64_t>(), item.at( "external_ratio" ).get<double>() } );
    for( const auto& item : value.at( "residency_events" ) ) result.residencyEvents.push_back( {
        item.at( "allocation" ).get<uint64_t>(), item.at( "frame" ).get<uint64_t>(), item.at( "fence" ).get<uint64_t>(), item.at( "size" ).get<uint64_t>(),
        item.at( "connection" ).get<uint64_t>(), item.at( "cpu_zone" ).get<uint64_t>(), item.at( "flags" ).get<uint32_t>(),
        item.at( "reason" ).get<uint8_t>(), char( item.at( "state" ).get<uint8_t>() ), item.at( "replayed" ).get<bool>(), item.at( "time" ).get<int64_t>() } );
    return result;
}

class IndexedTraceSource final : public analysis::TraceSource
{
public:
    IndexedTraceSource( QueryIndexManifest manifest, std::unique_ptr<analysis::WorkerTraceSource> source )
        : m_manifest( std::move( manifest ) )
        , m_source( std::move( source ) )
        , m_extras( m_manifest.zoneExtras, ZoneSectionKind::Extra, sizeof( ZoneExtraIndexRecord ), m_manifest.sourceFingerprint )
        , m_cpu( m_manifest.cpuZones, ZoneSectionKind::Cpu, sizeof( CpuZoneIndexRecord ), m_manifest.sourceFingerprint )
        , m_gpu( m_manifest.gpuZones, ZoneSectionKind::Gpu, sizeof( GpuZoneIndexRecord ), m_manifest.sourceFingerprint )
        , m_jobStages( m_manifest.jobStages, ZoneSectionKind::JobStage, sizeof( JnJobStageData ), m_manifest.sourceFingerprint )
        , m_gfxEntities( m_manifest.gfxEntities, ZoneSectionKind::GfxEntity, sizeof( JnGfxEntityData ), m_manifest.sourceFingerprint )
        , m_gfxLinks( m_manifest.gfxLinks, ZoneSectionKind::GfxLink, sizeof( JnGfxLinkData ), m_manifest.sourceFingerprint )
        , m_relations( m_manifest.relations, ZoneSectionKind::Relation, sizeof( JnRelationData ), m_manifest.sourceFingerprint )
        , m_gpuReferencePasses( m_manifest.gpuReferencePasses, ZoneSectionKind::GpuReferencePass, sizeof( JnGpuReferencePassData ), m_manifest.sourceFingerprint )
        , m_gpuReferenceUses( m_manifest.gpuReferenceUses, ZoneSectionKind::GpuReferenceUse, sizeof( JnGpuReferenceUseData ), m_manifest.sourceFingerprint )
        , m_gpuReferenceEnds( m_manifest.gpuReferenceEnds, ZoneSectionKind::GpuReferenceEnd, sizeof( JnGpuReferenceEndData ), m_manifest.sourceFingerprint )
        , m_gpuMemoryCpuZones( m_manifest.gpuMemoryCpuZones, ZoneSectionKind::GpuMemoryCpuZone, sizeof( uint64_t ), m_manifest.sourceFingerprint )
        , m_gpuMemorySummaryCpuZones( m_manifest.gpuMemorySummaryCpuZones, ZoneSectionKind::GpuMemorySummaryCpuZone, sizeof( uint64_t ), m_manifest.sourceFingerprint )
    {
        m_gpuQueryIdAvailable = m_source->GetTraceInfo().traceVersion >= FileVersion( 0, 12, 4 );
        const auto locations = m_source->GetSourceLocations();
        for( const auto& location : locations ) m_locations.emplace( location.nativeId, location );
        if( m_manifest.gpuMemorySummary.count == 1 )
            m_precomputedGpuMemorySummary = ParseGpuMemorySummary( json::parse( ReadManifest( m_manifest.gpuMemorySummary.path, MaximumGpuMemorySummaryBytes ) ) );
    }

    std::vector<analysis::Capability> GetCapabilities() const override
    {
        auto result = m_source->GetCapabilities();
        for( auto& item : result )
        {
            if( item.domain == "zone.cpu" || item.domain == "zone.gpu" || item.domain == "timeline" || item.domain == "evidence" )
            {
                item.present = item.domain == "zone.gpu" ? m_gpu.Count() != 0 : item.domain == "zone.cpu" ? m_cpu.Count() != 0 : item.present;
                item.queryable = true;
                item.indexed = true;
                item.reason = "exact N16.10 memory-mapped zone sidecar";
            }
        }
        return result;
    }

    analysis::TraceInfoDto GetTraceInfo() const override
    {
        auto result = m_source->GetTraceInfo();
        result.counts.cpuZones = m_cpu.Count();
        result.counts.gpuZones = m_gpu.Count();
        result.counts.gfxEntities = m_gfxEntities.Count();
        result.counts.gfxLinks = m_gfxLinks.Count();
        result.counts.relations = m_relations.Count();
        result.counts.gpuReferencePasses = m_gpuReferencePasses.Count();
        result.counts.gpuReferenceUses = m_gpuReferenceUses.Count();
        result.counts.gpuReferenceEnds = m_gpuReferenceEnds.Count();
        return result;
    }

    std::vector<analysis::CpuZoneDto> ScanCpuZones( const analysis::ScanRange& range ) const override
    {
        std::vector<analysis::CpuZoneDto> result;
        if( range.limit == 0 ) return result;
        if( range.startNs == std::numeric_limits<int64_t>::min() && range.endNs == std::numeric_limits<int64_t>::max() )
        {
            const auto begin = std::min<uint64_t>( range.offset, m_cpu.Count() );
            const auto end = std::min<uint64_t>( begin + range.limit, m_cpu.Count() );
            result.reserve( size_t( end - begin ) );
            for( uint64_t index = begin; index < end; index++ ) result.emplace_back( CpuDto( index ) );
            return result;
        }
        size_t skipped = 0;
        for( uint64_t index = 0; index < m_cpu.Count(); index++ )
        {
            const auto& zone = m_cpu.At<CpuZoneIndexRecord>( index );
            const auto end = zone.flags & 1 ? zone.end : zone.start;
            if( !Intersects( zone.start, end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( CpuDto( index ) );
            if( result.size() >= range.limit ) break;
        }
        return result;
    }

    std::vector<analysis::GpuZoneDto> ScanGpuZones( const analysis::ScanRange& range ) const override
    {
        std::vector<analysis::GpuZoneDto> result;
        if( range.limit == 0 ) return result;
        if( range.startNs == std::numeric_limits<int64_t>::min() && range.endNs == std::numeric_limits<int64_t>::max() )
        {
            const auto begin = std::min<uint64_t>( range.offset, m_gpu.Count() );
            const auto end = std::min<uint64_t>( begin + range.limit, m_gpu.Count() );
            result.reserve( size_t( end - begin ) );
            for( uint64_t index = begin; index < end; index++ ) result.emplace_back( GpuDto( index ) );
            return result;
        }
        size_t skipped = 0;
        for( uint64_t index = 0; index < m_gpu.Count(); index++ )
        {
            const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index );
            const auto end = zone.flags & 1 ? zone.gpuEnd : zone.gpuStart;
            if( !Intersects( zone.gpuStart, end, range ) ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( GpuDto( index ) );
            if( result.size() >= range.limit ) break;
        }
        return result;
    }

    std::optional<analysis::CpuZoneDto> GetCpuZone( std::string_view ref ) const override
    {
        const auto index = m_source->ParseEntityRef( ref, "cpu-zone" );
        if( !index || *index >= m_cpu.Count() ) return std::nullopt;
        return CpuDto( *index );
    }
    std::optional<analysis::GpuZoneDto> GetGpuZone( std::string_view ref ) const override
    {
        const auto index = m_source->ParseEntityRef( ref, "gpu-zone" );
        if( !index || *index >= m_gpu.Count() ) return std::nullopt;
        return GpuDto( *index );
    }
    std::vector<analysis::CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override
    {
        std::vector<analysis::CpuZoneDto> result;
        const auto parent = m_source->ParseEntityRef( ref, "cpu-zone" );
        if( !parent || *parent >= m_cpu.Count() || limit == 0 ) return result;
        const auto wanted = m_cpu.At<CpuZoneIndexRecord>( *parent ).childCount;
        size_t position = 0;
        for( uint64_t index = *parent + 1; index < m_cpu.Count() && position < wanted; index++ )
        {
            if( m_cpu.At<CpuZoneIndexRecord>( index ).parent != *parent ) continue;
            if( position++ >= offset && result.size() < limit ) result.emplace_back( CpuDto( index ) );
        }
        return result;
    }
    std::vector<analysis::GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override
    {
        std::vector<analysis::GpuZoneDto> result;
        const auto parent = m_source->ParseEntityRef( ref, "gpu-zone" );
        if( !parent || *parent >= m_gpu.Count() || limit == 0 ) return result;
        const auto wanted = m_gpu.At<GpuZoneIndexRecord>( *parent ).childCount;
        size_t position = 0;
        for( uint64_t index = *parent + 1; index < m_gpu.Count() && position < wanted; index++ )
        {
            if( m_gpu.At<GpuZoneIndexRecord>( index ).parent != *parent ) continue;
            if( position++ >= offset && result.size() < limit ) result.emplace_back( GpuDto( index ) );
        }
        return result;
    }
    std::optional<std::string> GetCpuZoneRef( uint64_t index ) const override { return index < m_cpu.Count() ? std::optional( m_source->MakeEntityRef( "cpu-zone", index ) ) : std::nullopt; }
    std::optional<std::string> GetGpuZoneRef( uint64_t index ) const override { return index < m_gpu.Count() ? std::optional( m_source->MakeEntityRef( "gpu-zone", index ) ) : std::nullopt; }

    std::optional<analysis::ZoneValidationSummaryDto> ValidateZoneIndex( const std::function<size_t( size_t )>& allowance ) const override
    {
        if( m_manifest.zoneValidationPrecomputed )
        {
            auto result = m_manifest.zoneValidation;
            if( allowance( 1 ) == 0 ) result.complete = false;
            return result;
        }
        analysis::ZoneValidationSummaryDto result;
        std::unordered_map<std::string, size_t> issueByCode;
        const auto note = [&]( const char* severity, const char* code, const char* message, const std::string& ref ) {
            auto found = issueByCode.find( code );
            if( found == issueByCode.end() )
            {
                found = issueByCode.emplace( code, result.findings.size() ).first;
                result.findings.push_back( { severity, code, message, 0, {} } );
            }
            auto& issue = result.findings[found->second];
            issue.count++;
            if( issue.refs.size() < 20 ) issue.refs.emplace_back( ref );
        };
        std::unordered_set<uint64_t> threads;
        const auto sourceThreads = m_source->GetThreads();
        threads.reserve( sourceThreads.size() );
        for( const auto& thread : sourceThreads ) threads.emplace( thread.nativeId );
        std::unordered_set<uint64_t> contexts;
        const auto sourceContexts = m_source->GetGpuContexts();
        contexts.reserve( sourceContexts.size() );
        for( const auto& context : sourceContexts ) contexts.emplace( context.index );
        // Source-location ids are persisted as int16_t. Validation touches every
        // zone, so replacing tens of millions of unordered-map lookups with a
        // fixed 64 KiB state table materially reduces indexed validation time.
        // 0 = missing, 1 = present without a stable name, 2 = present and named.
        std::array<uint8_t, 1u << 16> sourceState {};
        for( const auto& [sourceId, source] : m_locations )
            sourceState[uint16_t( sourceId )] = source.name.empty() && source.function.empty() ? 1 : 2;
        std::vector<int8_t> extraNameState( size_t( m_extras.Count() ), -1 );
        std::vector<uint32_t> callstacks;
        callstacks.reserve( size_t( std::min<uint64_t>( m_extras.Count(), 10000000 ) ) );
        constexpr size_t Chunk = 4096;
        for( uint64_t base = 0; base < m_cpu.Count(); )
        {
            const auto requested = size_t( std::min<uint64_t>( Chunk, m_cpu.Count() - base ) );
            const auto allowed = std::min( requested, allowance( requested ) );
            if( allowed == 0 ) { result.complete = false; break; }
            for( size_t offset = 0; offset < allowed; offset++ )
            {
                const auto index = base + offset;
                const auto& zone = m_cpu.At<CpuZoneIndexRecord>( index );
                const auto ref = [&] { return m_source->MakeEntityRef( "cpu-zone", index ); };
                if( !( zone.flags & 1 ) ) note( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", ref() );
                else if( zone.end < zone.start ) note( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", ref() );
                if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= m_cpu.Count() ) note( "warning", "UNRESOLVED_CPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                if( threads.find( zone.thread ) == threads.end() ) note( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                const auto locationState = sourceState[uint16_t( zone.sourceLocation )];
                if( locationState == 0 ) note( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                bool nameResolved = locationState == 2;
                if( zone.extra < m_extras.Count() )
                {
                    const auto& extra = m_extras.At<ZoneExtraIndexRecord>( zone.extra );
                    if( extra.callstack != 0 ) callstacks.emplace_back( extra.callstack );
                    if( extra.flags & 2 )
                    {
                        auto& state = extraNameState[zone.extra];
                        if( state < 0 ) state = m_source->ResolveStringIndex( extra.name ).has_value() ? 1 : 0;
                        nameResolved = state != 0;
                    }
                }
                else if( zone.extra != 0 ) nameResolved = false;
                if( !nameResolved ) note( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", ref() );
            }
            result.scanned += allowed; base += allowed;
            if( allowed < requested ) { result.complete = false; break; }
        }
        if( result.complete )
        {
            for( uint64_t base = 0; base < m_gpu.Count(); )
            {
                const auto requested = size_t( std::min<uint64_t>( Chunk, m_gpu.Count() - base ) );
                const auto allowed = std::min( requested, allowance( requested ) );
                if( allowed == 0 ) { result.complete = false; break; }
                for( size_t offset = 0; offset < allowed; offset++ )
                {
                    const auto index = base + offset;
                    const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index );
                    const auto ref = [&] { return m_source->MakeEntityRef( "gpu-zone", index ); };
                    if( !( zone.flags & 1 ) ) note( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", ref() );
                    else if( zone.gpuEnd < zone.gpuStart || zone.cpuEnd < zone.cpuStart ) note( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", ref() );
                    if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= m_gpu.Count() ) note( "warning", "UNRESOLVED_GPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                    if( threads.find( zone.thread ) == threads.end() ) note( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                    if( contexts.find( zone.context ) == contexts.end() ) note( "warning", "UNRESOLVED_GPU_CONTEXT_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                    if( sourceState[uint16_t( zone.sourceLocation )] == 0 ) note( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                    if( zone.callstack != 0 ) callstacks.emplace_back( zone.callstack );
                }
                result.scanned += allowed; base += allowed;
                if( allowed < requested ) { result.complete = false; break; }
            }
        }
        std::sort( callstacks.begin(), callstacks.end() );
        callstacks.erase( std::unique( callstacks.begin(), callstacks.end() ), callstacks.end() );
        result.referencedCallstacks = std::move( callstacks );
        return result;
    }

    std::optional<bool> HasGpuMemoryProtocol2() const override { return m_manifest.gpuMemoryProtocol2; }

#define TRACY_INDEX_FORWARD0( Return, Name ) Return Name() const override { return m_source->Name(); }
#define TRACY_INDEX_FORWARD1( Return, Name, T1, A1 ) Return Name( T1 A1 ) const override { return m_source->Name( A1 ); }
#define TRACY_INDEX_FORWARD2( Return, Name, T1, A1, T2, A2 ) Return Name( T1 A1, T2 A2 ) const override { return m_source->Name( A1, A2 ); }
#define TRACY_INDEX_FORWARD3( Return, Name, T1, A1, T2, A2, T3, A3 ) Return Name( T1 A1, T2 A2, T3 A3 ) const override { return m_source->Name( A1, A2, A3 ); }
#define TRACY_INDEX_FORWARD4( Return, Name, T1, A1, T2, A2, T3, A3, T4, A4 ) Return Name( T1 A1, T2 A2, T3 A3, T4 A4 ) const override { return m_source->Name( A1, A2, A3, A4 ); }
    TRACY_INDEX_FORWARD0( analysis::TraceReadView, AcquireReadView )
    std::vector<analysis::ThreadDto> GetThreads() const override
    {
        auto result = m_source->GetThreads();
        for( auto& thread : result )
        {
            const auto found = m_manifest.cpuZonesByThread.find( thread.nativeId );
            thread.zoneCount = found == m_manifest.cpuZonesByThread.end() ? 0 : found->second;
        }
        return result;
    }
    TRACY_INDEX_FORWARD0( std::vector<analysis::FrameSetDto>, GetFrameSets )
    std::vector<analysis::GpuContextDto> GetGpuContexts() const override
    {
        auto result = m_source->GetGpuContexts();
        for( auto& context : result )
        {
            const auto found = m_manifest.gpuZonesByContext.find( uint32_t( context.index ) );
            context.zoneCount = found == m_manifest.gpuZonesByContext.end() ? 0 : found->second;
        }
        return result;
    }
    TRACY_INDEX_FORWARD0( std::vector<analysis::MemoryPoolDto>, GetMemoryPools )
    TRACY_INDEX_FORWARD0( std::vector<analysis::PlotDto>, GetPlotList )
    TRACY_INDEX_FORWARD0( std::vector<analysis::LockDto>, GetLocks )
    TRACY_INDEX_FORWARD1( std::vector<analysis::FrameDto>, ScanFrames, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::MemoryEventDto>, ScanMemoryEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::MessageDto>, ScanMessages, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::PlotPointDto>, ScanPlots, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<std::string>, ScanLocks, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<std::string>, ScanContextSwitches, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<std::string>, ScanSamples, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD0( std::vector<analysis::JobDto>, GetJobs )
    TRACY_INDEX_FORWARD1( std::vector<analysis::JobDto>, GetEvidenceJobs, uint64_t, frameId )
    TRACY_INDEX_FORWARD0( std::vector<analysis::IoRequestDto>, GetIoRequests )
    TRACY_INDEX_FORWARD0( std::vector<analysis::GfxDispatchDto>, GetGfxDispatches )
    std::vector<analysis::GfxEntityDto> GetGfxEntities() const override
    {
        std::vector<analysis::GfxEntityDto> result;
        result.reserve( size_t( m_gfxEntities.Count() ) );
        for( uint64_t index = 0; index < m_gfxEntities.Count(); index++ )
        {
            const auto& value = m_gfxEntities.At<JnGfxEntityData>( index );
            result.push_back( { MakeEntityRef( "gfx-entity", value.entityId ), value.entityId, value.parentId, value.time,
                MakeEntityRef( "thread", value.thread ), value.gpuQueryId, value.gpuContext, value.kind, value.flags } );
        }
        return result;
    }
    std::vector<analysis::GfxLinkDto> GetGfxLinks() const override
    {
        std::vector<analysis::GfxLinkDto> result;
        result.reserve( size_t( m_gfxLinks.Count() ) );
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            result.push_back( { MakeEntityRef( "gfx-link", index ), value.sourceId, value.targetId, value.time,
                MakeEntityRef( "thread", value.thread ), value.relation, value.flags } );
        }
        return result;
    }
    analysis::GfxEvidenceSlice GetEvidenceGfx( uint64_t frameId, const std::vector<uint64_t>& seedIds ) const override
    {
        analysis::GfxEvidenceSlice result;
        std::unordered_set<uint64_t> reachable( seedIds.begin(), seedIds.end() );
        for( const auto& dispatch : m_source->GetGfxDispatches() ) if( dispatch.frameIndex == frameId )
        {
            result.dispatches.emplace_back( dispatch );
            reachable.emplace( dispatch.dispatchId );
        }
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            if( value.relation == 8 && value.targetId == frameId ) reachable.emplace( value.sourceId );
        }
        bool changed = true;
        while( changed )
        {
            changed = false;
            for( uint64_t index = 0; index < m_gfxEntities.Count(); index++ )
            {
                const auto& value = m_gfxEntities.At<JnGfxEntityData>( index );
                if( value.parentId != 0 && reachable.contains( value.parentId ) ) changed |= reachable.emplace( value.entityId ).second;
            }
            for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
            {
                const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
                if( reachable.contains( value.sourceId ) ) changed |= reachable.emplace( value.targetId ).second;
            }
        }
        for( uint64_t index = 0; index < m_gfxEntities.Count(); index++ )
        {
            const auto& value = m_gfxEntities.At<JnGfxEntityData>( index );
            if( !reachable.contains( value.entityId ) ) continue;
            result.entities.push_back( { MakeEntityRef( "gfx-entity", value.entityId ), value.entityId, value.parentId, value.time,
                MakeEntityRef( "thread", value.thread ), value.gpuQueryId, value.gpuContext, value.kind, value.flags } );
        }
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            if( !reachable.contains( value.sourceId ) ) continue;
            result.links.push_back( { MakeEntityRef( "gfx-link", index ), value.sourceId, value.targetId, value.time,
                MakeEntityRef( "thread", value.thread ), value.relation, value.flags } );
        }
        return result;
    }
    TRACY_INDEX_FORWARD0( std::vector<analysis::CorrelatedFrameEventDto>, GetCorrelatedFrameEvents )
    std::vector<analysis::RelationDto> GetRelations() const override { return ScanRelations( 0, size_t( m_relations.Count() ) ); }
    uint64_t GetRelationCount() const override { return m_relations.Count(); }
    std::vector<analysis::RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        const auto begin = std::min<uint64_t>( offset, m_relations.Count() );
        const auto end = begin + std::min<uint64_t>( limit, m_relations.Count() - begin );
        std::vector<analysis::RelationDto> result;
        result.reserve( size_t( end - begin ) );
        for( uint64_t index = begin; index < end; index++ )
        {
            const auto& value = m_relations.At<JnRelationData>( index );
            result.push_back( { MakeEntityRef( "relation", index ), value.sourceId, value.targetId, value.time,
                MakeEntityRef( "thread", value.thread ), value.sourceKind, value.targetKind,
                value.relationNamespace, value.relation, value.flags } );
        }
        return result;
    }
    TRACY_INDEX_FORWARD0( std::vector<analysis::RuntimeDomainStateDto>, GetRuntimeDomainStates )
    TRACY_INDEX_FORWARD0( std::vector<analysis::ScriptFrameDto>, GetScriptFrames )
    TRACY_INDEX_FORWARD0( std::vector<analysis::ScriptStackEventDto>, GetScriptStackEvents )
    TRACY_INDEX_FORWARD0( analysis::CrashDto, GetCrash )
    TRACY_INDEX_FORWARD0( std::vector<analysis::CpuTopologyDto>, GetCpuTopology )
    TRACY_INDEX_FORWARD0( std::vector<analysis::CpuUsagePointDto>, GetCpuUsage )
    TRACY_INDEX_FORWARD1( std::vector<analysis::ContextSwitchDto>, ScanContextSwitchEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::CpuContextSwitchDto>, ScanCpuContextSwitchEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::SampleDto>, ScanSampleEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::GhostZoneDto>, ScanGhostZones, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD0( std::vector<analysis::HardwareSampleDto>, GetHardwareSamples )
    TRACY_INDEX_FORWARD4( std::vector<analysis::HardwareSampleEventDto>, GetHardwareSampleEvents, uint64_t, address, std::string_view, kind, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::vector<analysis::LockEventDto>, ScanLockEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SymbolDto>, GetSymbols )
    TRACY_INDEX_FORWARD2( std::vector<analysis::SymbolAddressMappingDto>, GetSymbolAddressMappings, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::optional<analysis::SymbolAddressMappingDto>, ResolveSymbolAddress, uint64_t, address )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SourceLocationDto>, GetSourceLocations )
    TRACY_INDEX_FORWARD2( std::vector<analysis::CallstackFrameDto>, ResolveCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
    TRACY_INDEX_FORWARD2( std::vector<analysis::CallstackFrameDto>, ResolveParentCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
    TRACY_INDEX_FORWARD2( std::vector<analysis::SourceTextDto>, ResolveSources, const std::vector<std::string>&, sourceRefs, size_t, maxBytes )
    TRACY_INDEX_FORWARD2( std::vector<analysis::SymbolCodeDto>, ResolveSymbols, const std::vector<std::string>&, symbolRefs, size_t, maxBytes )
    TRACY_INDEX_FORWARD2( std::vector<analysis::FrameImageDto>, ResolveFrameImages, const std::vector<std::string>&, imageRefs, size_t, maxBytes )
    TRACY_INDEX_FORWARD3( std::vector<analysis::FrameDto>, GetFramesForSet, size_t, frameSetIndex, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::vector<int64_t>, GetFrameDurations, size_t, frameSetIndex )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SourceResourceDto>, GetSourceResources )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SymbolResourceDto>, GetSymbolResources )
    TRACY_INDEX_FORWARD0( std::vector<analysis::FrameImageMetadataDto>, GetFrameImageResources )
    TRACY_INDEX_FORWARD4( analysis::MemoryFrameSnapshot, GetMemoryFrameSnapshot, size_t, frameSetIndex, size_t, frameIndex, const std::vector<std::string>&, poolRefs, bool, allGpuD3D12Pools )
    TRACY_INDEX_FORWARD1( std::optional<analysis::MemoryEventDto>, GetMemoryEvent, const analysis::MemoryEventKey&, key )
    TRACY_INDEX_FORWARD1( std::optional<std::string>, GetMemoryPoolRef, uint64_t, key )
    TRACY_INDEX_FORWARD2( std::string, MakeEntityRef, std::string_view, kind, uint64_t, id )
    TRACY_INDEX_FORWARD2( std::optional<uint64_t>, ParseEntityRef, std::string_view, ref, std::string_view, kind )
    analysis::GpuMemoryAttribution GetGpuMemoryAttribution() const override { return BuildIndexedGpuMemoryAttribution( false ); }
    analysis::GpuMemoryAttribution GetGpuMemorySummaryAttribution() const override
    {
        return m_precomputedGpuMemorySummary ? *m_precomputedGpuMemorySummary : BuildIndexedGpuMemoryAttribution( true );
    }
    analysis::GpuMemoryEvidenceSlice GetGpuMemoryEvidence( const std::vector<uint64_t>& passIds, size_t maxUses ) const override
    {
        if( !m_precomputedGpuMemorySummary ) return analysis::TraceSource::GetGpuMemoryEvidence( passIds, maxUses );
        analysis::GpuMemoryEvidenceSlice result;
        result.protocolPresent = m_precomputedGpuMemorySummary->protocolPresent;
        result.complete = m_precomputedGpuMemorySummary->complete;
        result.warnings = m_precomputedGpuMemorySummary->warnings;
        std::unordered_map<uint64_t, size_t> selected;
        selected.reserve( passIds.size() );
        for( const auto passId : passIds ) selected.emplace( passId, selected.size() );
        result.passes.resize( selected.size() );
        std::vector<bool> present( selected.size(), false );
        for( uint64_t index = 0; index < m_gpuReferencePasses.Count(); index++ )
        {
            const auto& value = m_gpuReferencePasses.At<JnGpuReferencePassData>( index );
            const auto found = selected.find( value.passId );
            if( found == selected.end() ) continue;
            auto& pass = result.passes[found->second];
            pass.passId = value.passId; pass.labelId = value.taxonomyId; pass.frame = value.frameIndex;
            pass.ordinal = value.passId; pass.thread = value.thread; pass.start = value.time; pass.end = value.time;
            pass.level = value.taxonomyLevel; pass.commandCount = 1; pass.structuredBinary = true; pass.flags = value.flags;
            pass.name = "Taxonomy " + std::to_string( value.taxonomyId ); pass.operations = "resource";
            present[found->second] = true;
        }
        for( uint64_t index = 0; index < m_gpuReferenceEnds.Count(); index++ )
        {
            const auto& value = m_gpuReferenceEnds.At<JnGpuReferenceEndData>( index );
            const auto found = selected.find( value.passId );
            if( found == selected.end() || !present[found->second] ) continue;
            auto& pass = result.passes[found->second];
            pass.end = value.time; pass.commandListId = value.commandListId;
            pass.totalUseCount = value.totalReferenceCount; pass.droppedUses = value.droppedReferenceCount; pass.flags |= value.flags;
            pass.truncated = ( pass.flags & 0x3 ) != 0; pass.complete = !pass.truncated && pass.droppedUses == 0;
        }
        for( uint64_t index = 0; index < m_relations.Count(); index++ )
        {
            const auto& value = m_relations.At<JnRelationData>( index );
            if( value.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) || value.relation != 1 ) continue;
            const auto found = selected.find( value.sourceId );
            if( found != selected.end() && present[found->second] ) result.passes[found->second].parentPassId = value.targetId;
        }
        size_t keptUses = 0;
        std::unordered_set<uint64_t> resourceIds;
        for( uint64_t index = 0; index < m_gpuReferenceUses.Count(); index++ )
        {
            const auto& value = m_gpuReferenceUses.At<JnGpuReferenceUseData>( index );
            const auto found = selected.find( value.passId );
            if( found == selected.end() || !present[found->second] || value.resourceId == 0 ) continue;
            auto& pass = result.passes[found->second];
            pass.emittedUseCount++;
            if( keptUses < maxUses )
            {
                char kind = 'U';
                if( const auto logical = m_precomputedGpuMemorySummary->logicalById.find( value.resourceId ); logical != m_precomputedGpuMemorySummary->logicalById.end() )
                    kind = m_precomputedGpuMemorySummary->logicalResources[logical->second].kind;
                pass.uses.push_back( { value.resourceId, value.usageMask, kind } );
                resourceIds.emplace( value.resourceId );
                keptUses++;
            }
            else
            {
                result.truncated = true;
                result.omittedUses++;
            }
        }
        result.passes.erase( std::remove_if( result.passes.begin(), result.passes.end(), [&]( const auto& pass ) {
            return pass.passId == 0;
        } ), result.passes.end() );
        const auto allocations = m_source->GetGpuMemoryAllocationInputs();
        std::unordered_map<uint64_t, const analysis::GpuMemoryAllocationInput*> allocationById;
        for( const auto& allocation : allocations ) if( resourceIds.contains( allocation.allocationId ) ) allocationById.emplace( allocation.allocationId, &allocation );
        result.resources.reserve( resourceIds.size() );
        for( const auto resourceId : resourceIds )
        {
            analysis::GpuMemoryEvidenceResource resource;
            resource.resourceId = resourceId;
            if( const auto logical = m_precomputedGpuMemorySummary->logicalById.find( resourceId ); logical != m_precomputedGpuMemorySummary->logicalById.end() )
            {
                const auto& value = m_precomputedGpuMemorySummary->logicalResources[logical->second];
                resource.physicalAllocationId = value.physicalAllocationId; resource.size = value.size;
                resource.primaryOwnerId = value.primaryOwnerId; resource.kind = value.kind; resource.name = value.name;
            }
            if( const auto allocation = allocationById.find( resourceId ); allocation != allocationById.end() )
            {
                if( resource.physicalAllocationId == 0 ) resource.physicalAllocationId = resourceId;
                if( resource.size == 0 ) resource.size = allocation->second->size;
                if( resource.name.empty() ) resource.name = allocation->second->poolName;
            }
            result.resources.emplace_back( std::move( resource ) );
        }
        return result;
    }
    std::optional<analysis::GpuMemoryPassPage> ScanGpuMemoryPasses( size_t offset, size_t limit,
        std::optional<uint64_t> requestedPassId, size_t useOffset, size_t useLimit ) const override
    {
        analysis::GpuMemoryPassPage page;
        page.totalPasses = m_gpuReferencePasses.Count();
        if( m_precomputedGpuMemorySummary ) { page.complete = m_precomputedGpuMemorySummary->complete; page.warnings = m_precomputedGpuMemorySummary->warnings; }
        struct Selected { uint64_t rawIndex; analysis::GpuMemoryPass pass; bool ended = false; bool hasChild = false; bool hasSegment = false; bool submitted = false; };
        std::vector<Selected> selected;
        const auto add = [&]( uint64_t rawIndex ) {
            const auto& value = m_gpuReferencePasses.At<JnGpuReferencePassData>( rawIndex );
            analysis::GpuMemoryPass pass;
            pass.passId = value.passId; pass.labelId = value.taxonomyId; pass.frame = value.frameIndex; pass.ordinal = value.passId;
            pass.thread = value.thread; pass.start = value.time; pass.end = value.time; pass.level = value.taxonomyLevel;
            pass.commandCount = 1; pass.structuredBinary = true; pass.flags = value.flags;
            pass.name = "Taxonomy " + std::to_string( value.taxonomyId ); pass.operations = "resource";
            selected.push_back( { rawIndex, std::move( pass ) } );
        };
        uint64_t earliestFrame = std::numeric_limits<uint64_t>::max(), latestFrame = 0;
        for( uint64_t index = 0; index < m_gpuReferencePasses.Count(); index++ )
        {
            const auto& value = m_gpuReferencePasses.At<JnGpuReferencePassData>( index );
            earliestFrame = std::min( earliestFrame, value.frameIndex ); latestFrame = std::max( latestFrame, value.frameIndex );
            if( requestedPassId ? value.passId == *requestedPassId : index >= offset && selected.size() < limit ) add( index );
        }
        if( requestedPassId && selected.empty() ) return page;
        std::unordered_map<uint64_t, size_t> selectedById;
        for( size_t index = 0; index < selected.size(); index++ ) selectedById.emplace( selected[index].pass.passId, index );
        for( uint64_t index = 0; index < m_gpuReferenceEnds.Count(); index++ )
        {
            const auto& value = m_gpuReferenceEnds.At<JnGpuReferenceEndData>( index ); const auto found = selectedById.find( value.passId );
            if( found == selectedById.end() ) continue;
            auto& pass = selected[found->second].pass; pass.end = value.time; pass.commandListId = value.commandListId;
            pass.totalUseCount = value.totalReferenceCount; pass.droppedUses = value.droppedReferenceCount; pass.flags |= value.flags;
            pass.truncated = ( pass.flags & 0x3 ) != 0; pass.complete = !pass.truncated && pass.droppedUses == 0;
            selected[found->second].ended = true;
        }
        for( uint64_t index = 0; index < m_relations.Count(); index++ )
        {
            const auto& value = m_relations.At<JnRelationData>( index );
            if( value.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) || value.relation != 1 ) continue;
            const auto child = selectedById.find( value.sourceId ); if( child != selectedById.end() ) selected[child->second].pass.parentPassId = value.targetId;
            const auto parent = selectedById.find( value.targetId ); if( parent != selectedById.end() ) selected[parent->second].hasChild = true;
        }
        for( uint64_t index = 0; index < m_gpuReferenceUses.Count(); index++ )
        {
            const auto& value = m_gpuReferenceUses.At<JnGpuReferenceUseData>( index ); const auto found = selectedById.find( value.passId );
            if( found == selectedById.end() || value.resourceId == 0 ) continue;
            auto& pass = selected[found->second].pass; const auto useIndex = pass.emittedUseCount++;
            if( requestedPassId && useIndex >= useOffset && pass.uses.size() < useLimit )
            {
                char kind = 'U';
                if( m_precomputedGpuMemorySummary )
                {
                    const auto logical = m_precomputedGpuMemorySummary->logicalById.find( value.resourceId );
                    if( logical != m_precomputedGpuMemorySummary->logicalById.end() ) kind = m_precomputedGpuMemorySummary->logicalResources[logical->second].kind;
                }
                pass.uses.push_back( { value.resourceId, value.usageMask, kind } );
            }
            else if( !requestedPassId && pass.uses.empty() ) pass.uses.push_back( {} );
        }
        if( requestedPassId && !selected.empty() ) page.totalUses = selected.front().pass.emittedUseCount;

        std::unordered_map<uint64_t, size_t> passEntityToSelected, segmentToSelected;
        std::unordered_set<uint64_t> selectedCommandLists;
        for( size_t index = 0; index < selected.size(); index++ ) if( selected[index].pass.commandListId != 0 ) selectedCommandLists.emplace( selected[index].pass.commandListId );
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            if( value.relation == 11 ) { const auto found = selectedById.find( value.targetId ); if( found != selectedById.end() ) passEntityToSelected[value.sourceId] = found->second; }
            else if( value.relation == 13 ) { const auto found = selectedById.find( value.targetId ); if( found != selectedById.end() ) segmentToSelected[value.sourceId] = found->second; }
            else if( value.relation == 4 && selectedCommandLists.find( value.sourceId ) != selectedCommandLists.end() )
                for( auto& item : selected ) if( item.pass.commandListId == value.sourceId ) item.submitted = true;
        }
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index ); if( value.relation != 5 ) continue;
            const auto found = passEntityToSelected.find( value.sourceId ); if( found != passEntityToSelected.end() ) segmentToSelected[value.targetId] = found->second;
        }
        std::unordered_map<uint64_t, std::vector<size_t>> selectedByQuery;
        for( uint64_t index = 0; index < m_gfxEntities.Count(); index++ )
        {
            const auto& value = m_gfxEntities.At<JnGfxEntityData>( index ); if( value.kind != 4 ) continue;
            const auto found = segmentToSelected.find( value.entityId ); if( found == segmentToSelected.end() ) continue;
            selected[found->second].hasSegment = true;
            selectedByQuery[( uint64_t( value.gpuContext ) << 32 ) | value.gpuQueryId].push_back( found->second );
        }
        std::unordered_set<size_t> exact;
        for( uint64_t index = 0; index < m_gpu.Count(); index++ )
        {
            const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index ); if( ( zone.flags & 1 ) == 0 || zone.gpuEnd < 0 ) continue;
            const auto found = selectedByQuery.find( ( uint64_t( zone.context ) << 32 ) | zone.queryId ); if( found == selectedByQuery.end() ) continue;
            const auto position = std::find_if( found->second.begin(), found->second.end(), [&]( size_t value ) { return exact.find( value ) == exact.end(); } );
            if( position == found->second.end() ) continue;
            exact.emplace( *position ); selected[*position].pass.gpuZoneIndex = index; selected[*position].pass.gpuThread = zone.thread;
            const auto* source = Source( zone.sourceLocation ); if( source ) selected[*position].pass.name = source->name.empty() ? source->function : source->name;
        }
        const auto captureEnd = m_source->GetTraceInfo().lastTimeNs;
        for( size_t index = 0; index < selected.size(); index++ )
        {
            auto& item = selected[index]; auto& pass = item.pass;
            const bool nearHead = pass.frame <= earliestFrame + 1, nearTail = latestFrame <= pass.frame + 1;
            const bool failure = ( pass.flags & 0xA ) != 0;
            const bool forcedBoundaryClose = item.ended && pass.truncated && !failure && pass.droppedUses == 0 && ( nearHead || nearTail );
            const bool timestampBoundary = item.hasSegment && exact.find( index ) == exact.end() && nearTail;
            const bool boundary = ( !item.ended && pass.frame == latestFrame ) || forcedBoundaryClose || timestampBoundary;
            if( boundary )
            {
                pass.gpuPairing = analysis::GpuZonePairing::CaptureBoundary;
                if( !item.ended || forcedBoundaryClose ) pass.end = captureEnd;
            }
            else if( exact.find( index ) != exact.end() ) pass.gpuPairing = analysis::GpuZonePairing::Exact;
            else if( item.hasSegment ) pass.gpuPairing = analysis::GpuZonePairing::GpuResultUnavailable;
            else if( pass.commandListId != 0 && !item.submitted ) pass.gpuPairing = analysis::GpuZonePairing::SubmissionUnobserved;
            else if( pass.emittedUseCount == 0 && item.hasChild ) pass.gpuPairing = analysis::GpuZonePairing::DerivedLogicalRollup;
            page.passes.emplace_back( std::move( pass ) );
        }
        return page;
    }
    std::optional<analysis::GpuMemoryRequestScopePage> ScanGpuMemoryRequestScopes( size_t offset, size_t limit ) const override
    {
        EnsureGpuRequestScopes();
        analysis::GpuMemoryRequestScopePage page;
        page.totalScopes = m_gpuRequestScopeZones.size();
        const auto begin = std::min<uint64_t>( offset, m_gpuRequestScopeZones.size() );
        const auto end = begin + std::min<uint64_t>( limit, m_gpuRequestScopeZones.size() - begin );
        page.scopes.reserve( size_t( end - begin ) );
        for( auto index = begin; index < end; index++ )
        {
            const auto scope = RequestScope( m_gpuRequestScopeZones[size_t( index )] );
            if( scope ) page.scopes.emplace_back( *scope );
        }
        return page;
    }
    std::optional<analysis::GpuMemoryAllocationPage> ScanGpuMemoryAllocations( size_t offset, size_t limit,
        std::optional<uint64_t> allocationId, const std::string& poolRef, const std::string& relationState ) const override
    {
        EnsureGpuRequestScopes();
        EnsureGpuResourcePassRefs();
        analysis::GpuMemoryAllocationPage page;
        if( m_precomputedGpuMemorySummary )
        {
            page.protocolPresent = m_precomputedGpuMemorySummary->protocolPresent;
            page.complete = m_precomputedGpuMemorySummary->complete;
            page.warnings = m_precomputedGpuMemorySummary->warnings;
        }
        const auto allocations = m_source->GetGpuMemoryAllocationInputs();
        std::unordered_map<uint64_t, std::string> poolRefs;
        size_t matched = 0;
        for( const auto& allocation : allocations )
        {
            if( allocationId && allocation.allocationId != *allocationId ) continue;
            if( !poolRef.empty() )
            {
                auto found = poolRefs.find( allocation.key.pool );
                if( found == poolRefs.end() ) found = poolRefs.emplace( allocation.key.pool, m_source->GetMemoryPoolRef( allocation.key.pool ).value_or( "" ) ).first;
                if( found->second != poolRef ) continue;
            }
            const bool logicalPool = allocation.poolName.rfind( "GPU D3D12 Logical ", 0 ) == 0;
            const analysis::GpuMemoryLogicalResource* logical = nullptr;
            if( logicalPool && m_precomputedGpuMemorySummary )
            {
                const auto found = m_precomputedGpuMemorySummary->logicalById.find( allocation.allocationId );
                if( found != m_precomputedGpuMemorySummary->logicalById.end() ) logical = &m_precomputedGpuMemorySummary->logicalResources[found->second];
            }
            std::optional<uint64_t> requestLabel;
            std::optional<uint64_t> requestScopeZone;
            if( logical && logical->primaryOwnerId != 0 ) requestLabel = logical->primaryOwnerId;
            else
            {
                requestScopeZone = FindRequestScope( allocation.thread, allocation.allocationNs );
                if( requestScopeZone )
                {
                    const auto scope = RequestScope( *requestScopeZone );
                    if( scope ) requestLabel = scope->labelId;
                }
            }
            const auto passRefs = m_gpuResourcePassRefs.find( allocation.allocationId );
            const bool hasPassRefs = passRefs != m_gpuResourcePassRefs.end() && passRefs->second.count != 0;
            const std::string currentState = requestLabel && hasPassRefs ? "request_and_uses" : requestLabel ? "request_only" : hasPassRefs ? "uses_only" : "unattributed";
            if( !relationState.empty() && currentState != relationState ) continue;
            if( matched++ < offset ) continue;
            if( page.allocations.size() >= limit ) { page.hasMore = true; break; }

            analysis::GpuMemoryAllocationPageItem item;
            item.attribution.allocation = allocation;
            item.attribution.requestLabelId = requestLabel;
            if( passRefs != m_gpuResourcePassRefs.end() ) { item.passRefCount = passRefs->second.count; item.passIds = passRefs->second.preview; }
            if( logical ) item.logicalResource = *logical;
            if( m_precomputedGpuMemorySummary )
            {
                const auto& origins = logicalPool ? m_precomputedGpuMemorySummary->logicalOriginById : m_precomputedGpuMemorySummary->physicalOriginById;
                const auto origin = origins.find( allocation.allocationId );
                if( origin != origins.end() && origin->second < m_precomputedGpuMemorySummary->origins.size() )
                    item.origin = m_precomputedGpuMemorySummary->origins[origin->second];
            }
            page.allocations.emplace_back( std::move( item ) );
        }
        return page;
    }
    analysis::GpuMemoryAttribution BuildIndexedGpuMemoryAttribution( bool summaryOnly ) const
    {
        const auto& filteredCpuZones = summaryOnly ? m_gpuMemorySummaryCpuZones : m_gpuMemoryCpuZones;
        std::vector<analysis::GpuMemoryCpuZoneInput> cpuInputs;
        cpuInputs.reserve( size_t( filteredCpuZones.Count() ) );
        bool hasLegacyPass = false;
        for( uint64_t filteredIndex = 0; filteredIndex < filteredCpuZones.Count(); filteredIndex++ )
        {
            const auto zoneIndex = filteredCpuZones.At<uint64_t>( filteredIndex );
            if( zoneIndex >= m_cpu.Count() ) continue;
            const auto& zone = m_cpu.At<CpuZoneIndexRecord>( zoneIndex );
            const auto* source = Source( zone.sourceLocation );
            if( !source || zone.extra >= m_extras.Count() ) continue;
            const auto& extra = m_extras.At<ZoneExtraIndexRecord>( zone.extra );
            if( ( extra.flags & 1 ) == 0 ) continue;
            const auto marker = source->name.empty() ? source->function : source->name;
            if( summaryOnly && marker != analysis::GpuMemoryRequestMarker &&
                marker != analysis::GpuMemoryOriginMarker && marker != analysis::GpuMemoryResidencyMarker ) continue;
            const auto text = m_source->ResolveStringIndex( extra.text );
            if( !text ) continue;
            hasLegacyPass |= marker == analysis::GpuMemoryPassMarker;
            auto displayName = marker;
            if( ( extra.flags & 2 ) != 0 )
            {
                const auto value = m_source->ResolveStringIndex( extra.name );
                if( value ) displayName = *value;
            }
            cpuInputs.push_back( { zoneIndex, marker, std::move( displayName ), *text, zone.thread, zone.start, zone.end } );
        }

        std::vector<analysis::GpuMemoryGpuZoneInput> gpuInputs;
        if( hasLegacyPass )
        {
            gpuInputs.reserve( size_t( m_gpu.Count() ) );
            for( uint64_t index = 0; index < m_gpu.Count(); index++ )
            {
                const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index );
                if( ( zone.flags & 1 ) == 0 || zone.gpuEnd < 0 ) continue;
                const auto* source = Source( zone.sourceLocation );
                gpuInputs.push_back( { index, source ? ( source->name.empty() ? source->function : source->name ) : std::string(),
                    zone.thread, zone.cpuStart, zone.gpuStart, zone.gpuEnd, 0 } );
            }
        }
        auto result = m_source->GetGpuMemoryAttributionFromExternalZones( cpuInputs, gpuInputs );
        if( m_gpuReferencePasses.Count() == 0 ) return result;

        struct PassAggregate
        {
            JnGpuReferencePassData begin {};
            int64_t end = 0;
            uint64_t commandListId = 0;
            uint64_t parentPassId = 0;
            uint32_t emittedUses = 0;
            uint32_t totalUses = 0;
            uint16_t droppedUses = 0;
            uint8_t flags = 0;
            bool ended = false;
            bool hasChild = false;
        };
        std::vector<PassAggregate> passes;
        passes.reserve( size_t( m_gpuReferencePasses.Count() ) );
        std::unordered_map<uint64_t, size_t> passById;
        passById.reserve( size_t( m_gpuReferencePasses.Count() ) );
        uint64_t earliestFrame = std::numeric_limits<uint64_t>::max(), latestFrame = 0;
        for( uint64_t index = 0; index < m_gpuReferencePasses.Count(); index++ )
        {
            const auto& value = m_gpuReferencePasses.At<JnGpuReferencePassData>( index );
            if( value.passId == 0 || passById.find( value.passId ) != passById.end() ) continue;
            passById.emplace( value.passId, passes.size() );
            PassAggregate pass; pass.begin = value; pass.end = value.time; pass.flags = value.flags;
            passes.emplace_back( pass );
            earliestFrame = std::min( earliestFrame, value.frameIndex ); latestFrame = std::max( latestFrame, value.frameIndex );
        }
        for( uint64_t index = 0; index < m_gpuReferenceEnds.Count(); index++ )
        {
            const auto& value = m_gpuReferenceEnds.At<JnGpuReferenceEndData>( index );
            const auto found = passById.find( value.passId ); if( found == passById.end() ) continue;
            auto& pass = passes[found->second]; pass.end = value.time; pass.commandListId = value.commandListId;
            pass.totalUses = value.totalReferenceCount; pass.droppedUses = value.droppedReferenceCount;
            pass.flags |= value.flags; pass.ended = true;
        }
        for( uint64_t index = 0; index < m_relations.Count(); index++ )
        {
            const auto& value = m_relations.At<JnRelationData>( index );
            if( value.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) || value.relation != 1 ) continue;
            const auto child = passById.find( value.sourceId ); if( child != passById.end() ) passes[child->second].parentPassId = value.targetId;
            const auto parent = passById.find( value.targetId ); if( parent != passById.end() ) passes[parent->second].hasChild = true;
        }
        struct WorkingKey { uint64_t frame; uint32_t taxonomy; bool operator==( const WorkingKey& rhs ) const { return frame == rhs.frame && taxonomy == rhs.taxonomy; } };
        struct WorkingHash { size_t operator()( const WorkingKey& value ) const { return std::hash<uint64_t>()( value.frame ^ ( uint64_t( value.taxonomy ) << 32 ) ); } };
        std::unordered_set<WorkingKey, WorkingHash> workingSets;
        workingSets.reserve( passes.size() / 8 );
        for( uint64_t index = 0; index < m_gpuReferenceUses.Count(); index++ )
        {
            const auto& value = m_gpuReferenceUses.At<JnGpuReferenceUseData>( index );
            const auto found = passById.find( value.passId ); if( found == passById.end() || value.resourceId == 0 ) continue;
            auto& pass = passes[found->second]; pass.emittedUses++;
            workingSets.emplace( WorkingKey { pass.begin.frameIndex, pass.begin.taxonomyId } );
        }

        std::unordered_map<uint64_t, uint64_t> segmentByPass, referenceByPass, referenceBySegment;
        std::unordered_set<uint64_t> submittedCommandLists;
        segmentByPass.reserve( passes.size() ); referenceByPass.reserve( passes.size() ); referenceBySegment.reserve( passes.size() );
        for( uint64_t index = 0; index < m_gfxLinks.Count(); index++ )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            if( value.relation == 5 ) segmentByPass[value.sourceId] = value.targetId;
            else if( value.relation == 11 ) referenceByPass[value.sourceId] = value.targetId;
            else if( value.relation == 4 ) submittedCommandLists.emplace( value.sourceId );
            else if( value.relation == 13 ) referenceBySegment[value.sourceId] = value.targetId;
        }
        std::unordered_map<uint64_t, uint64_t> tokenBySegment = referenceBySegment;
        for( const auto& [passId, token] : referenceByPass )
        {
            const auto segment = segmentByPass.find( passId ); if( segment != segmentByPass.end() ) tokenBySegment[segment->second] = token;
        }
        std::unordered_set<uint64_t> segmentTokens;
        segmentTokens.reserve( tokenBySegment.size() );
        std::unordered_map<uint64_t, std::vector<uint64_t>> tokensByQuery;
        for( uint64_t index = 0; index < m_gfxEntities.Count(); index++ )
        {
            const auto& value = m_gfxEntities.At<JnGfxEntityData>( index );
            if( value.kind != 4 ) continue;
            const auto token = tokenBySegment.find( value.entityId ); if( token == tokenBySegment.end() ) continue;
            segmentTokens.emplace( token->second );
            tokensByQuery[( uint64_t( value.gpuContext ) << 32 ) | value.gpuQueryId].emplace_back( token->second );
        }
        std::unordered_map<uint64_t, size_t> matchedByQuery;
        std::unordered_set<uint64_t> exactTokens;
        exactTokens.reserve( segmentTokens.size() );
        for( uint64_t index = 0; index < m_gpu.Count(); index++ )
        {
            const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index );
            if( ( zone.flags & 1 ) == 0 || zone.gpuEnd < 0 ) continue;
            const auto key = ( uint64_t( zone.context ) << 32 ) | zone.queryId;
            const auto found = tokensByQuery.find( key );
            if( found == tokensByQuery.end() ) continue;
            auto& cursor = matchedByQuery[key];
            while( cursor < found->second.size() && exactTokens.find( found->second[cursor] ) != exactTokens.end() ) cursor++;
            if( cursor < found->second.size() ) exactTokens.emplace( found->second[cursor++] );
        }

        result.protocolPresent = true;
        result.structuredReferencePresent = true;
        result.warnings.erase( std::remove( result.warnings.begin(), result.warnings.end(),
            "GTMEM1 markers are present but no valid PASS record was parsed" ), result.warnings.end() );
        result.passQualityAggregated = true;
        result.aggregatedWorkingSetCount = workingSets.size();
        uint64_t missingPairings = 0;
        const auto captureEnd = m_source->GetTraceInfo().lastTimeNs;
        for( const auto& input : passes )
        {
            const bool nearHead = input.begin.frameIndex <= earliestFrame + 1;
            const bool nearTail = latestFrame <= input.begin.frameIndex + 1;
            const bool explicitlyTruncated = ( input.flags & 0x1 ) != 0;
            const bool hasFailureFlag = ( input.flags & 0xA ) != 0;
            const bool forcedBoundaryClose = input.ended && explicitlyTruncated && !hasFailureFlag && input.droppedUses == 0 && ( nearHead || nearTail );
            const bool hasSegment = segmentTokens.find( input.begin.passId ) != segmentTokens.end();
            const bool hasExactTimestamp = exactTokens.find( input.begin.passId ) != exactTokens.end();
            const bool timestampBoundary = hasSegment && !hasExactTimestamp && nearTail;
            const bool captureBoundary = ( !input.ended && captureEnd >= input.begin.time && input.begin.frameIndex == latestFrame ) || forcedBoundaryClose || timestampBoundary;
            const bool complete = input.ended && !explicitlyTruncated && input.droppedUses == 0;
            analysis::GpuZonePairing pairing = analysis::GpuZonePairing::Missing;
            if( captureBoundary ) pairing = analysis::GpuZonePairing::CaptureBoundary;
            else if( hasExactTimestamp ) pairing = analysis::GpuZonePairing::Exact;
            else if( hasSegment ) pairing = analysis::GpuZonePairing::GpuResultUnavailable;
            else if( input.commandListId != 0 && !submittedCommandLists.empty() && submittedCommandLists.find( input.commandListId ) == submittedCommandLists.end() )
                pairing = analysis::GpuZonePairing::SubmissionUnobserved;
            else if( input.emittedUses == 0 && input.hasChild ) pairing = analysis::GpuZonePairing::DerivedLogicalRollup;
            else missingPairings++;
            if( captureBoundary ) result.captureBoundaryPasses++;
            if( pairing == analysis::GpuZonePairing::GpuResultUnavailable ) result.gpuResultUnavailablePasses++;
            if( pairing == analysis::GpuZonePairing::SubmissionUnobserved ) result.submissionUnobservedPasses++;
            if( ( input.flags & uint8_t( JnGpuReferenceFlags::CommandListBoundary ) ) != 0 ) result.aggregatedCommandListBoundaryPasses++;
            if( complete || captureBoundary ) continue;
            result.aggregatedIncompleteReferencePasses++;
            result.aggregatedStructuredIncompleteReferencePasses++;
            if( explicitlyTruncated ) result.aggregatedTruncatedReferencePasses++;
            if( hasFailureFlag ) result.aggregatedFailureFlagReferencePasses++;
            result.aggregatedDroppedReferenceUses += input.droppedUses;
            if( result.aggregatedIncompleteReferencePreview.size() < 16 )
            {
                analysis::GpuMemoryPass preview;
                preview.passId = input.begin.passId; preview.parentPassId = input.parentPassId; preview.labelId = input.begin.taxonomyId;
                preview.frame = input.begin.frameIndex; preview.ordinal = input.begin.passId; preview.commandListId = input.commandListId;
                preview.thread = input.begin.thread; preview.start = input.begin.time; preview.end = captureBoundary ? captureEnd : input.end;
                preview.level = input.begin.taxonomyLevel; preview.commandCount = 1; preview.emittedUseCount = input.emittedUses;
                preview.totalUseCount = input.totalUses; preview.droppedUses = input.droppedUses; preview.truncated = explicitlyTruncated;
                preview.complete = complete; preview.structuredBinary = true; preview.flags = input.flags; preview.gpuPairing = pairing;
                result.aggregatedIncompleteReferencePreview.emplace_back( std::move( preview ) );
            }
        }
        if( result.aggregatedIncompleteReferencePasses != 0 ) result.complete = false;
        if( missingPairings != 0 )
        {
            result.complete = false;
            result.warnings.emplace_back( std::to_string( missingPairings ) + " pass(es) have no matching GPU zone" );
        }
        return result;
    }
    TRACY_INDEX_FORWARD2( analysis::SourceTextDto, ReadEmbeddedSource, size_t, sourceId, size_t, maxBytes )
    TRACY_INDEX_FORWARD3( analysis::BinaryResourceChunkDto, ReadEmbeddedSourceBytes, size_t, sourceId, size_t, offset, size_t, maxBytes )
    TRACY_INDEX_FORWARD2( analysis::SymbolCodeDto, ReadSymbolCode, uint64_t, symbolId, size_t, maxBytes )
    TRACY_INDEX_FORWARD3( analysis::BinaryResourceChunkDto, ReadSymbolCodeBytes, uint64_t, symbolId, size_t, offset, size_t, maxBytes )
    TRACY_INDEX_FORWARD3( std::vector<analysis::DisassemblyInstructionDto>, DisassembleSymbol, std::string_view, symbolRef, size_t, maxBytes, size_t, maxInstructions )
    TRACY_INDEX_FORWARD2( analysis::FrameImageDto, ReadFrameImage, size_t, imageId, size_t, maxBytes )
    TRACY_INDEX_FORWARD3( analysis::BinaryResourceChunkDto, ReadFrameImageBc1, size_t, imageId, size_t, offset, size_t, maxBytes )
#undef TRACY_INDEX_FORWARD0
#undef TRACY_INDEX_FORWARD1
#undef TRACY_INDEX_FORWARD2
#undef TRACY_INDEX_FORWARD3
#undef TRACY_INDEX_FORWARD4

private:
    static bool Intersects( int64_t begin, int64_t end, const analysis::ScanRange& range ) { return begin < range.endNs && end > range.startNs; }
    struct ResourcePassRefs
    {
        uint64_t count = 0;
        std::vector<uint64_t> preview;
    };
    void EnsureGpuRequestScopes() const
    {
        std::call_once( m_gpuRequestScopeOnce, [&] {
            for( uint64_t filteredIndex = 0; filteredIndex < m_gpuMemoryCpuZones.Count(); filteredIndex++ )
            {
                const auto zoneIndex = m_gpuMemoryCpuZones.At<uint64_t>( filteredIndex );
                if( zoneIndex >= m_cpu.Count() ) continue;
                const auto& zone = m_cpu.At<CpuZoneIndexRecord>( zoneIndex );
                const auto* source = Source( zone.sourceLocation );
                if( !source ) continue;
                const auto marker = source->name.empty() ? source->function : source->name;
                if( marker != analysis::GpuMemoryRequestMarker ) continue;
                m_gpuRequestScopeZones.emplace_back( zoneIndex );
                m_gpuRequestScopeZonesByThread[zone.thread].emplace_back( zoneIndex );
            }
            const auto less = [&]( uint64_t lhs, uint64_t rhs ) {
                const auto& a = m_cpu.At<CpuZoneIndexRecord>( lhs ); const auto& b = m_cpu.At<CpuZoneIndexRecord>( rhs );
                return a.thread != b.thread ? a.thread < b.thread : a.start != b.start ? a.start < b.start : lhs < rhs;
            };
            std::sort( m_gpuRequestScopeZones.begin(), m_gpuRequestScopeZones.end(), less );
            for( auto& [thread, zones] : m_gpuRequestScopeZonesByThread ) std::sort( zones.begin(), zones.end(), [&]( uint64_t lhs, uint64_t rhs ) {
                const auto& a = m_cpu.At<CpuZoneIndexRecord>( lhs ); const auto& b = m_cpu.At<CpuZoneIndexRecord>( rhs );
                return a.start != b.start ? a.start < b.start : lhs < rhs;
            } );
        } );
    }
    std::optional<analysis::GpuMemoryRequestScope> RequestScope( uint64_t zoneIndex ) const
    {
        if( zoneIndex >= m_cpu.Count() ) return std::nullopt;
        const auto& zone = m_cpu.At<CpuZoneIndexRecord>( zoneIndex );
        if( zone.extra >= m_extras.Count() ) return std::nullopt;
        const auto& extra = m_extras.At<ZoneExtraIndexRecord>( zone.extra );
        if( ( extra.flags & 1 ) == 0 ) return std::nullopt;
        const auto text = m_source->ResolveStringIndex( extra.text );
        if( !text ) return std::nullopt;
        const auto marker = text->find( "GTMEM1|SCOPE|" );
        if( marker == std::string::npos ) return std::nullopt;
        const auto lineEnd = text->find( '\n', marker );
        const std::string_view line( text->data() + marker, ( lineEnd == std::string::npos ? text->size() : lineEnd ) - marker );
        std::string name = analysis::GpuMemoryRequestMarker;
        if( ( extra.flags & 2 ) != 0 )
        {
            const auto value = m_source->ResolveStringIndex( extra.name );
            if( value ) name = *value;
        }
        return analysis::GpuMemoryRequestScope { UnsignedProtocolField( line, "label" ), UnsignedProtocolField( line, "frame" ),
            zone.thread, zone.start, zone.end, std::move( name ), zoneIndex };
    }
    std::optional<uint64_t> FindRequestScope( uint64_t thread, int64_t time ) const
    {
        const auto found = m_gpuRequestScopeZonesByThread.find( thread );
        if( found == m_gpuRequestScopeZonesByThread.end() ) return std::nullopt;
        const auto& zones = found->second;
        auto it = std::upper_bound( zones.begin(), zones.end(), time, [&]( int64_t value, uint64_t index ) { return value < m_cpu.At<CpuZoneIndexRecord>( index ).start; } );
        while( it != zones.begin() )
        {
            --it;
            const auto& zone = m_cpu.At<CpuZoneIndexRecord>( *it );
            if( zone.start <= time && time <= zone.end ) return *it;
            if( zone.end < time ) break;
        }
        return std::nullopt;
    }
    void EnsureGpuResourcePassRefs() const
    {
        std::call_once( m_gpuResourcePassRefsOnce, [&] {
            std::unordered_set<uint64_t> validPasses;
            validPasses.reserve( size_t( m_gpuReferencePasses.Count() ) );
            for( uint64_t index = 0; index < m_gpuReferencePasses.Count(); index++ ) validPasses.emplace( m_gpuReferencePasses.At<JnGpuReferencePassData>( index ).passId );
            for( uint64_t index = 0; index < m_gpuReferenceUses.Count(); index++ )
            {
                const auto& value = m_gpuReferenceUses.At<JnGpuReferenceUseData>( index );
                if( value.resourceId == 0 || validPasses.find( value.passId ) == validPasses.end() ) continue;
                auto& refs = m_gpuResourcePassRefs[value.resourceId]; refs.count++;
                if( refs.preview.size() < 100 ) refs.preview.emplace_back( value.passId );
            }
        } );
    }
    const analysis::SourceLocationDto* Source( int16_t id ) const
    {
        const auto found = m_locations.find( id );
        return found == m_locations.end() ? nullptr : &found->second;
    }
    analysis::CpuZoneDto CpuDto( uint64_t index ) const
    {
        const auto& zone = m_cpu.At<CpuZoneIndexRecord>( index );
        const auto* source = Source( zone.sourceLocation );
        analysis::CpuZoneDto dto;
        dto.ref = m_source->MakeEntityRef( "cpu-zone", index );
        dto.threadRef = m_source->MakeEntityRef( "thread", zone.thread );
        dto.sourceLocationRef = source ? source->ref : m_source->MakeEntityRef( "source", uint16_t( zone.sourceLocation ) );
        dto.extraIndex = zone.extra;
        const ZoneExtraIndexRecord* extra = zone.extra < m_extras.Count() ? &m_extras.At<ZoneExtraIndexRecord>( zone.extra ) : nullptr;
        if( extra )
        {
            dto.extraColor = extra->color;
            dto.callstack = extra->callstack;
            if( extra->flags & 2 ) dto.extraName = m_source->ResolveStringIndex( extra->name );
            if( extra->flags & 1 ) dto.extraText = m_source->ResolveStringIndex( extra->text );
        }
        else if( zone.extra != 0 ) { dto.extraValid = false; dto.nameResolved = false; }
        dto.name = dto.extraName.value_or( source ? ( source->name.empty() ? source->function : source->name ) : std::string() );
        dto.nameResolved = dto.nameResolved && !dto.name.empty();
        if( source ) { dto.function = source->function; dto.file = source->file; dto.line = source->line; }
        if( zone.parent != std::numeric_limits<uint64_t>::max() ) dto.parentRef = m_source->MakeEntityRef( "cpu-zone", zone.parent );
        dto.startNs = zone.start; dto.childCount = zone.childCount; dto.complete = ( zone.flags & 1 ) != 0;
        if( dto.complete ) { dto.endNs = zone.end; dto.selfTimeNs = std::max<int64_t>( 0, zone.end - zone.start - zone.childTime ); }
        if( dto.callstack != 0 ) dto.callstackRef = m_source->MakeEntityRef( "callstack", dto.callstack );
        return dto;
    }
    analysis::GpuZoneDto GpuDto( uint64_t index ) const
    {
        const auto& zone = m_gpu.At<GpuZoneIndexRecord>( index );
        const auto* source = Source( zone.sourceLocation );
        analysis::GpuZoneDto dto;
        dto.ref = m_source->MakeEntityRef( "gpu-zone", index );
        dto.contextRef = m_source->MakeEntityRef( "gpu-context", zone.context );
        dto.threadRef = m_source->MakeEntityRef( "thread", zone.thread );
        dto.sourceLocationRef = source ? source->ref : m_source->MakeEntityRef( "source", uint16_t( zone.sourceLocation ) );
        if( source ) { dto.name = source->name.empty() ? source->function : source->name; dto.function = source->function; dto.file = source->file; dto.line = source->line; }
        if( zone.parent != std::numeric_limits<uint64_t>::max() ) dto.parentRef = m_source->MakeEntityRef( "gpu-zone", zone.parent );
        dto.gpuStartNs = zone.gpuStart; dto.cpuStartNs = zone.cpuStart; dto.childCount = uint32_t( std::min<uint64_t>( zone.childCount, std::numeric_limits<uint32_t>::max() ) );
        dto.complete = ( zone.flags & 1 ) != 0;
        if( dto.complete ) { dto.gpuEndNs = zone.gpuEnd; dto.cpuEndNs = zone.cpuEnd; dto.selfTimeNs = std::max<int64_t>( 0, zone.gpuEnd - zone.gpuStart - zone.childGpuTime ); }
        dto.callstack = zone.callstack; if( dto.callstack != 0 ) dto.callstackRef = m_source->MakeEntityRef( "callstack", dto.callstack );
        dto.queryId = zone.queryId; dto.queryIdAvailability.available = m_gpuQueryIdAvailable;
        if( !dto.queryIdAvailability.available ) dto.queryIdAvailability.reason = "gpu query IDs were not persisted before Tracy 0.12.4";
        return dto;
    }

    QueryIndexManifest m_manifest;
    std::unique_ptr<analysis::WorkerTraceSource> m_source;
    ReadOnlyZoneSection m_extras, m_cpu, m_gpu;
    ReadOnlyZoneSection m_jobStages, m_gfxEntities, m_gfxLinks, m_relations;
    ReadOnlyZoneSection m_gpuReferencePasses, m_gpuReferenceUses, m_gpuReferenceEnds;
    ReadOnlyZoneSection m_gpuMemoryCpuZones;
    ReadOnlyZoneSection m_gpuMemorySummaryCpuZones;
    std::unordered_map<int32_t, analysis::SourceLocationDto> m_locations;
    std::optional<analysis::GpuMemoryAttribution> m_precomputedGpuMemorySummary;
    mutable std::once_flag m_gpuRequestScopeOnce;
    mutable std::vector<uint64_t> m_gpuRequestScopeZones;
    mutable std::unordered_map<uint64_t, std::vector<uint64_t>> m_gpuRequestScopeZonesByThread;
    mutable std::once_flag m_gpuResourcePassRefsOnce;
    mutable std::unordered_map<uint64_t, ResourcePassRefs> m_gpuResourcePassRefs;
    bool m_gpuQueryIdAvailable = false;
};

int64_t WriteTime( const std::filesystem::path& path )
{
    return std::filesystem::last_write_time( path ).time_since_epoch().count();
}

uint64_t ProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

bool AtomicReplace( const std::filesystem::path& source, const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "MoveFileExW failed with error " + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = ec.message();
    return false;
#endif
}

std::string ReadManifest( const std::filesystem::path& path, uint64_t maximumBytes )
{
    const auto bytes = std::filesystem::file_size( path );
    if( bytes > maximumBytes ) throw std::runtime_error( "index JSON exceeds configured size limit" );
    std::ifstream input( path, std::ios::binary );
    if( !input ) throw std::runtime_error( "cannot open index manifest" );
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

json ManifestJson( const QueryIndexManifest& value )
{
    const auto section = []( const QueryIndexSection& item ) {
        return json {
            { "file", item.path.filename().string() }, { "sha256", item.fingerprint },
            { "bytes", std::to_string( item.bytes ) }, { "count", std::to_string( item.count ) },
            { "declared_count", std::to_string( item.declaredCount ) },
            { "record_bytes", item.recordBytes }
        };
    };
    json cpuZonesByThread = json::object();
    for( const auto& [thread, count] : value.cpuZonesByThread ) cpuZonesByThread[std::to_string( thread )] = std::to_string( count );
    json gpuZonesByContext = json::object();
    for( const auto& [context, count] : value.gpuZonesByContext ) gpuZonesByContext[std::to_string( context )] = std::to_string( count );
    json zoneValidationFindings = json::array();
    for( const auto& finding : value.zoneValidation.findings ) zoneValidationFindings.push_back( {
        { "severity", finding.severity }, { "code", finding.code }, { "message", finding.message },
        { "count", std::to_string( finding.count ) }, { "refs", finding.refs }
    } );
    return {
        { "magic", IndexMagic },
        { "schema_version", QueryIndexSchemaVersion },
        { "source", {
            { "sha256", value.sourceFingerprint },
            { "bytes", std::to_string( value.sourceBytes ) },
            { "write_time", std::to_string( value.sourceWriteTime ) }
        } },
        { "data", {
            { "file", value.dataPath.filename().string() },
            { "sha256", value.dataFingerprint },
            { "bytes", std::to_string( value.dataBytes ) },
            { "format", "compact-tracy" }
        } },
        { "domains", {
            { "cpu_zone_index", value.cpuZoneIndex },
            { "gpu_zone_index", value.gpuZoneIndex },
            { "gpu_memory_protocol2", value.gpuMemoryProtocol2 },
            { "jn_binary_domains", true },
            { "memory", true }
        } },
        { "counts", {
            { "cpu_zones_by_thread", std::move( cpuZonesByThread ) },
            { "gpu_zones_by_context", std::move( gpuZonesByContext ) }
        } },
        { "zone_validation", {
            { "precomputed", value.zoneValidationPrecomputed },
            { "complete", value.zoneValidation.complete },
            { "scanned", std::to_string( value.zoneValidation.scanned ) },
            { "findings", std::move( zoneValidationFindings ) }
        } },
        { "sections", {
            { "zone_extra", section( value.zoneExtras ) },
            { "cpu_zone", section( value.cpuZones ) },
            { "gpu_zone", section( value.gpuZones ) },
            { "job_stage", section( value.jobStages ) },
            { "gfx_entity", section( value.gfxEntities ) },
            { "gfx_link", section( value.gfxLinks ) },
            { "relation", section( value.relations ) },
            { "gpu_reference_pass", section( value.gpuReferencePasses ) },
            { "gpu_reference_use", section( value.gpuReferenceUses ) },
            { "gpu_reference_end", section( value.gpuReferenceEnds ) },
            { "gpu_memory_cpu_zone", section( value.gpuMemoryCpuZones ) },
            { "gpu_memory_summary_cpu_zone", section( value.gpuMemorySummaryCpuZones ) },
            { "gpu_memory_summary", section( value.gpuMemorySummary ) }
        } }
    };
}

uint64_t ParseUnsigned( const json& value, const char* field )
{
    const auto text = value.at( field ).get<std::string>();
    size_t consumed = 0;
    const auto result = std::stoull( text, &consumed );
    if( consumed != text.size() ) throw std::runtime_error( std::string( "invalid unsigned field: " ) + field );
    return result;
}

int64_t ParseSigned( const json& value, const char* field )
{
    const auto text = value.at( field ).get<std::string>();
    size_t consumed = 0;
    const auto result = std::stoll( text, &consumed );
    if( consumed != text.size() ) throw std::runtime_error( std::string( "invalid signed field: " ) + field );
    return result;
}

}

std::filesystem::path QueryIndex::ManifestPath( const std::filesystem::path& tracePath )
{
    auto result = tracePath;
    result += ".jnidx";
    return result;
}

QueryIndexManifest QueryIndex::Build( const std::filesystem::path& tracePath, analysis::WorkerTraceSource::StateCallback stateCallback )
{
    QueryIndexManifest result;
    result.manifestPath = ManifestPath( tracePath );
    result.sourceBytes = std::filesystem::file_size( tracePath );
    result.sourceWriteTime = WriteTime( tracePath );
    result.sourceFingerprint = analysis::WorkerTraceSource::ComputeFingerprint( tracePath );
    // The manifest remains adjacent to and named after the source trace, but
    // section files must not repeat an arbitrarily long trace filename.  The
    // old form could exceed the legacy Win32 MAX_PATH limit only after the
    // longest section suffix and the atomic-write .tmp.<pid> suffix were
    // appended.  The manifest already validates the complete 256-bit source
    // fingerprint; this bounded basename is only a collision-resistant local
    // section identity within the trace directory.
    const auto baseName = std::string( "jnidx." ) + result.sourceFingerprint.substr( 0, 16 ) +
        ".s" + std::to_string( QueryIndexSchemaVersion );
    const auto dataName = baseName + ".jnidx.data";
    result.dataPath = tracePath.parent_path() / dataName;
    result.zoneExtras.path = tracePath.parent_path() / ( baseName + ".jnidx.zextra" );
    result.cpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.cpu" );
    result.gpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu" );
    result.jobStages.path = tracePath.parent_path() / ( baseName + ".jnidx.job-stage" );
    result.gfxEntities.path = tracePath.parent_path() / ( baseName + ".jnidx.gfx-entity" );
    result.gfxLinks.path = tracePath.parent_path() / ( baseName + ".jnidx.gfx-link" );
    result.relations.path = tracePath.parent_path() / ( baseName + ".jnidx.relation" );
    result.gpuReferencePasses.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-ref-pass" );
    result.gpuReferenceUses.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-ref-use" );
    result.gpuReferenceEnds.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-ref-end" );
    result.gpuMemoryCpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-memory-cpu-zone" );
    result.gpuMemorySummaryCpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-memory-summary-cpu-zone" );
    result.gpuMemorySummary.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-memory-summary.json" );
    result.zoneExtras.recordBytes = sizeof( ZoneExtraIndexRecord );
    result.cpuZones.recordBytes = sizeof( CpuZoneIndexRecord );
    result.gpuZones.recordBytes = sizeof( GpuZoneIndexRecord );
    result.jobStages.recordBytes = sizeof( JnJobStageData );
    result.gfxEntities.recordBytes = sizeof( JnGfxEntityData );
    result.gfxLinks.recordBytes = sizeof( JnGfxLinkData );
    result.relations.recordBytes = sizeof( JnRelationData );
    result.gpuReferencePasses.recordBytes = sizeof( JnGpuReferencePassData );
    result.gpuReferenceUses.recordBytes = sizeof( JnGpuReferenceUseData );
    result.gpuReferenceEnds.recordBytes = sizeof( JnGpuReferenceEndData );
    result.gpuMemoryCpuZones.recordBytes = sizeof( uint64_t );
    result.gpuMemorySummaryCpuZones.recordBytes = sizeof( uint64_t );
    result.gpuMemorySummary.recordBytes = 0;
    std::array<QueryIndexSection*, ZoneIndexWriter::JnDomainCount> jnSections = {
        &result.jobStages, &result.gfxEntities, &result.gfxLinks, &result.relations,
        &result.gpuReferencePasses, &result.gpuReferenceUses, &result.gpuReferenceEnds
    };

    const auto suffix = ".tmp." + std::to_string( ProcessId() );
    auto temporaryData = result.dataPath;
    temporaryData += suffix;
    auto temporaryManifest = result.manifestPath;
    temporaryManifest += suffix;
    auto temporaryExtras = result.zoneExtras.path; temporaryExtras += suffix;
    auto temporaryCpu = result.cpuZones.path; temporaryCpu += suffix;
    auto temporaryGpu = result.gpuZones.path; temporaryGpu += suffix;
    std::array<std::filesystem::path, ZoneIndexWriter::JnDomainCount> temporaryJn;
    for( size_t index = 0; index < temporaryJn.size(); index++ ) { temporaryJn[index] = jnSections[index]->path; temporaryJn[index] += suffix; }
    auto temporaryGpuMemoryCpuZones = result.gpuMemoryCpuZones.path; temporaryGpuMemoryCpuZones += suffix;
    auto temporaryGpuMemorySummaryCpuZones = result.gpuMemorySummaryCpuZones.path; temporaryGpuMemorySummaryCpuZones += suffix;
    auto temporaryGpuMemorySummary = result.gpuMemorySummary.path; temporaryGpuMemorySummary += suffix;

    try
    {
        ZoneIndexWriter writer( temporaryExtras, temporaryCpu, temporaryGpu, temporaryJn, result.sourceFingerprint );
        auto source = analysis::WorkerTraceSource::Open( tracePath, std::move( stateCallback ), result.sourceFingerprint,
            analysis::WorkerTraceLoadMode::CompactIndex, &writer );
        writer.FinishMissingJnDomains();
        result.zoneExtras.count = writer.ExtraCount();
        result.cpuZones.count = writer.CpuCount();
        result.gpuZones.count = writer.GpuCount();
        result.zoneExtras.declaredCount = writer.DeclaredExtraCount();
        result.cpuZones.declaredCount = writer.DeclaredCpuCount();
        result.gpuZones.declaredCount = writer.DeclaredGpuCount();
        for( size_t index = 0; index < jnSections.size(); index++ )
        {
            jnSections[index]->count = writer.JnCount( SerializedZoneSink::JnDomain( index ) );
            jnSections[index]->declaredCount = jnSections[index]->count;
            jnSections[index]->bytes = std::filesystem::file_size( temporaryJn[index] );
            jnSections[index]->fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryJn[index] );
        }
        result.cpuZonesByThread = writer.CpuZonesByThread();
        result.gpuZonesByContext = writer.GpuZonesByContext();
        result.zoneExtras.bytes = std::filesystem::file_size( temporaryExtras );
        result.cpuZones.bytes = std::filesystem::file_size( temporaryCpu );
        result.gpuZones.bytes = std::filesystem::file_size( temporaryGpu );
        {
            QueryIndexSection temporaryCpuSection = result.cpuZones; temporaryCpuSection.path = temporaryCpu;
            QueryIndexSection temporaryExtraSection = result.zoneExtras; temporaryExtraSection.path = temporaryExtras;
            QueryIndexSection temporaryGpuSection = result.gpuZones; temporaryGpuSection.path = temporaryGpu;
            ReadOnlyZoneSection cpuSection( temporaryCpuSection, ZoneSectionKind::Cpu, sizeof( CpuZoneIndexRecord ), result.sourceFingerprint );
            ReadOnlyZoneSection extraSection( temporaryExtraSection, ZoneSectionKind::Extra, sizeof( ZoneExtraIndexRecord ), result.sourceFingerprint );
            ReadOnlyZoneSection gpuSection( temporaryGpuSection, ZoneSectionKind::Gpu, sizeof( GpuZoneIndexRecord ), result.sourceFingerprint );
            analysis::ZoneValidationSummaryDto zoneValidation;
            std::unordered_map<std::string, size_t> validationIssueByCode;
            const auto noteValidation = [&]( const char* severity, const char* code, const char* message, const std::string& ref ) {
                auto found = validationIssueByCode.find( code );
                if( found == validationIssueByCode.end() )
                {
                    found = validationIssueByCode.emplace( code, zoneValidation.findings.size() ).first;
                    zoneValidation.findings.push_back( { severity, code, message, 0, {} } );
                }
                auto& issue = zoneValidation.findings[found->second];
                issue.count++;
                if( issue.refs.size() < 20 ) issue.refs.emplace_back( ref );
            };
            std::unordered_set<uint64_t> validThreads;
            for( const auto& thread : source->GetThreads() ) validThreads.emplace( thread.nativeId );
            std::unordered_set<uint64_t> validContexts;
            for( const auto& context : source->GetGpuContexts() ) validContexts.emplace( context.index );
            std::array<uint8_t, 1u << 16> validationSourceState {};
            for( const auto& location : source->GetSourceLocations() )
                validationSourceState[uint16_t( int16_t( location.nativeId ) )] = location.name.empty() && location.function.empty() ? 1 : 2;
            std::vector<int8_t> validationExtraNameState( size_t( extraSection.Count() ), -1 );
            std::vector<uint8_t> validationExtraSeen( size_t( extraSection.Count() ), 0 );
            std::vector<uint32_t> validationCallstacks;
            validationCallstacks.reserve( size_t( std::min<uint64_t>( extraSection.Count() + gpuSection.Count(), 10000000 ) ) );
            std::unordered_map<int16_t, std::string> gpuMemoryMarkers;
            for( const auto& location : source->GetSourceLocations() )
            {
                const auto& name = location.name.empty() ? location.function : location.name;
                if( name == analysis::GpuMemoryRequestMarker || name == analysis::GpuMemoryPassMarker ||
                    name == analysis::GpuMemoryOriginMarker || name == analysis::GpuMemoryResidencyMarker )
                    gpuMemoryMarkers.emplace( int16_t( location.nativeId ), name );
            }
            std::vector<uint64_t> gpuMemoryCpuZones;
            std::vector<uint64_t> gpuMemorySummaryCpuZones;
            std::unordered_map<uint64_t, uint64_t> latestResourceZoneById;
            for( uint64_t index = 0; index < cpuSection.Count(); index++ )
            {
                const auto& zone = cpuSection.At<CpuZoneIndexRecord>( index );
                const auto validationRef = [&] { return source->MakeEntityRef( "cpu-zone", index ); };
                if( !( zone.flags & 1 ) ) noteValidation( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", validationRef() );
                else if( zone.end < zone.start ) noteValidation( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", validationRef() );
                if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= cpuSection.Count() )
                    noteValidation( "warning", "UNRESOLVED_CPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                if( validThreads.find( zone.thread ) == validThreads.end() )
                    noteValidation( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                const auto locationState = validationSourceState[uint16_t( zone.sourceLocation )];
                if( locationState == 0 )
                    noteValidation( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                bool nameResolved = locationState == 2;
                if( zone.extra < extraSection.Count() )
                {
                    const auto& extra = extraSection.At<ZoneExtraIndexRecord>( zone.extra );
                    if( !validationExtraSeen[zone.extra] )
                    {
                        validationExtraSeen[zone.extra] = 1;
                        if( extra.callstack != 0 ) validationCallstacks.emplace_back( extra.callstack );
                    }
                    if( extra.flags & 2 )
                    {
                        auto& state = validationExtraNameState[zone.extra];
                        if( state < 0 ) state = source->ResolveStringIndex( extra.name ).has_value() ? 1 : 0;
                        nameResolved = state != 0;
                    }
                }
                else if( zone.extra != 0 ) nameResolved = false;
                if( !nameResolved )
                    noteValidation( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", validationRef() );
                const auto marker = gpuMemoryMarkers.find( zone.sourceLocation );
                if( marker == gpuMemoryMarkers.end() || ( zone.flags & 1 ) == 0 || zone.extra >= extraSection.Count() ) continue;
                const auto& extra = extraSection.At<ZoneExtraIndexRecord>( zone.extra );
                if( ( extra.flags & 1 ) == 0 ) continue;
                const auto text = source->ResolveStringIndex( extra.text );
                if( !text ) continue;
                gpuMemoryCpuZones.emplace_back( index );
                if( marker->second == analysis::GpuMemoryOriginMarker || marker->second == analysis::GpuMemoryResidencyMarker )
                    gpuMemorySummaryCpuZones.emplace_back( index );
                else if( marker->second == analysis::GpuMemoryRequestMarker )
                {
                    constexpr std::string_view prefix = "GTMEM1|RESOURCE|allocation=";
                    const auto begin = text->find( prefix );
                    if( begin != std::string::npos )
                    {
                        const auto valueBegin = text->data() + begin + prefix.size();
                        const auto valueEnd = std::find( valueBegin, text->data() + text->size(), '|' );
                        uint64_t allocationId = 0;
                        const auto parsed = std::from_chars( valueBegin, valueEnd, allocationId );
                        if( parsed.ec == std::errc() && parsed.ptr == valueEnd && allocationId != 0 ) latestResourceZoneById[allocationId] = index;
                    }
                }
                if( marker->second == analysis::GpuMemoryOriginMarker || marker->second == analysis::GpuMemoryResidencyMarker )
                    result.gpuMemoryProtocol2 = true;
            }
            for( uint64_t index = 0; index < gpuSection.Count(); index++ )
            {
                const auto& zone = gpuSection.At<GpuZoneIndexRecord>( index );
                const auto validationRef = [&] { return source->MakeEntityRef( "gpu-zone", index ); };
                if( !( zone.flags & 1 ) ) noteValidation( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", validationRef() );
                else if( zone.gpuEnd < zone.gpuStart || zone.cpuEnd < zone.cpuStart )
                    noteValidation( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", validationRef() );
                if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= gpuSection.Count() )
                    noteValidation( "warning", "UNRESOLVED_GPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                if( validThreads.find( zone.thread ) == validThreads.end() )
                    noteValidation( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                if( validContexts.find( zone.context ) == validContexts.end() )
                    noteValidation( "warning", "UNRESOLVED_GPU_CONTEXT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                if( validationSourceState[uint16_t( zone.sourceLocation )] == 0 )
                    noteValidation( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef() );
                if( zone.callstack != 0 ) validationCallstacks.emplace_back( zone.callstack );
            }
            std::sort( validationCallstacks.begin(), validationCallstacks.end() );
            validationCallstacks.erase( std::unique( validationCallstacks.begin(), validationCallstacks.end() ), validationCallstacks.end() );
            if( !validationCallstacks.empty() )
            {
                std::unordered_set<uint32_t> resolvedCallstacks;
                for( const auto& frame : source->ResolveCallstacks( validationCallstacks, 1 ) ) resolvedCallstacks.emplace( frame.callstack );
                for( const auto callstack : validationCallstacks ) if( resolvedCallstacks.find( callstack ) == resolvedCallstacks.end() )
                    noteValidation( "warning", "UNRESOLVED_CALLSTACK", "persisted zone references a callstack that cannot be resolved", source->MakeEntityRef( "callstack", callstack ) );
            }
            zoneValidation.complete = true;
            zoneValidation.scanned = cpuSection.Count() + gpuSection.Count();
            result.zoneValidationPrecomputed = true;
            result.zoneValidation = std::move( zoneValidation );
            for( const auto& [allocationId, index] : latestResourceZoneById ) gpuMemorySummaryCpuZones.emplace_back( index );
            std::sort( gpuMemorySummaryCpuZones.begin(), gpuMemorySummaryCpuZones.end() );
            MappedSection filtered;
            filtered.Open( temporaryGpuMemoryCpuZones, ZoneSectionKind::GpuMemoryCpuZone, gpuMemoryCpuZones.size(), sizeof( uint64_t ), result.sourceFingerprint );
            if( !gpuMemoryCpuZones.empty() ) std::memcpy( filtered.Record( 0 ), gpuMemoryCpuZones.data(), gpuMemoryCpuZones.size() * sizeof( uint64_t ) );
            filtered.Close( gpuMemoryCpuZones.size() );
            result.gpuMemoryCpuZones.count = gpuMemoryCpuZones.size();
            result.gpuMemoryCpuZones.declaredCount = gpuMemoryCpuZones.size();
            result.gpuMemoryCpuZones.bytes = std::filesystem::file_size( temporaryGpuMemoryCpuZones );
            result.gpuMemoryCpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpuMemoryCpuZones );
            MappedSection summaryFiltered;
            summaryFiltered.Open( temporaryGpuMemorySummaryCpuZones, ZoneSectionKind::GpuMemorySummaryCpuZone,
                gpuMemorySummaryCpuZones.size(), sizeof( uint64_t ), result.sourceFingerprint );
            if( !gpuMemorySummaryCpuZones.empty() ) std::memcpy( summaryFiltered.Record( 0 ), gpuMemorySummaryCpuZones.data(), gpuMemorySummaryCpuZones.size() * sizeof( uint64_t ) );
            summaryFiltered.Close( gpuMemorySummaryCpuZones.size() );
            result.gpuMemorySummaryCpuZones.count = gpuMemorySummaryCpuZones.size();
            result.gpuMemorySummaryCpuZones.declaredCount = gpuMemorySummaryCpuZones.size();
            result.gpuMemorySummaryCpuZones.bytes = std::filesystem::file_size( temporaryGpuMemorySummaryCpuZones );
            result.gpuMemorySummaryCpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpuMemorySummaryCpuZones );
        }
        result.zoneExtras.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryExtras );
        result.cpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryCpu );
        result.gpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpu );
        result.cpuZoneIndex = true;
        result.gpuZoneIndex = true;

        source->WriteCompactSnapshot( temporaryData );
        result.dataBytes = std::filesystem::file_size( temporaryData );
        result.dataFingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryData );
        {
            auto buildManifest = result;
            buildManifest.dataPath = temporaryData;
            buildManifest.zoneExtras.path = temporaryExtras; buildManifest.cpuZones.path = temporaryCpu; buildManifest.gpuZones.path = temporaryGpu;
            for( size_t index = 0; index < jnSections.size(); index++ )
            {
                QueryIndexSection* buildSections[] = { &buildManifest.jobStages, &buildManifest.gfxEntities, &buildManifest.gfxLinks,
                    &buildManifest.relations, &buildManifest.gpuReferencePasses, &buildManifest.gpuReferenceUses, &buildManifest.gpuReferenceEnds };
                buildSections[index]->path = temporaryJn[index];
            }
            buildManifest.gpuMemoryCpuZones.path = temporaryGpuMemoryCpuZones;
            buildManifest.gpuMemorySummaryCpuZones.path = temporaryGpuMemorySummaryCpuZones;
            buildManifest.gpuMemorySummary.count = 0;
            auto compact = analysis::WorkerTraceSource::Open( temporaryData, {}, result.sourceFingerprint, analysis::WorkerTraceLoadMode::IndexedSidecar );
            IndexedTraceSource indexed( std::move( buildManifest ), std::move( compact ) );
            const auto summary = indexed.GetGpuMemorySummaryAttribution();
            std::ofstream output( temporaryGpuMemorySummary, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "cannot create GPU-memory summary index" );
            const auto payload = GpuMemorySummaryJson( summary ).dump();
            output.write( payload.data(), std::streamsize( payload.size() ) ); output.flush();
            if( !output ) throw std::runtime_error( "cannot flush GPU-memory summary index" );
            result.gpuMemorySummary.count = 1; result.gpuMemorySummary.declaredCount = 1;
            result.gpuMemorySummary.bytes = std::filesystem::file_size( temporaryGpuMemorySummary );
            result.gpuMemorySummary.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpuMemorySummary );
        }

        {
            std::ofstream output( temporaryManifest, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "cannot create index manifest" );
            const auto payload = ManifestJson( result ).dump( 2 );
            output.write( payload.data(), std::streamsize( payload.size() ) );
            output.flush();
            if( !output ) throw std::runtime_error( "cannot flush index manifest" );
        }

        std::string error;
        if( !AtomicReplace( temporaryExtras, result.zoneExtras.path, error ) ) throw std::runtime_error( "cannot commit zone-extra index: " + error );
        if( !AtomicReplace( temporaryCpu, result.cpuZones.path, error ) ) throw std::runtime_error( "cannot commit CPU-zone index: " + error );
        if( !AtomicReplace( temporaryGpu, result.gpuZones.path, error ) ) throw std::runtime_error( "cannot commit GPU-zone index: " + error );
        for( size_t index = 0; index < temporaryJn.size(); index++ )
            if( !AtomicReplace( temporaryJn[index], jnSections[index]->path, error ) ) throw std::runtime_error( "cannot commit JN domain index: " + error );
        if( !AtomicReplace( temporaryGpuMemoryCpuZones, result.gpuMemoryCpuZones.path, error ) ) throw std::runtime_error( "cannot commit GPU-memory CPU-zone index: " + error );
        if( !AtomicReplace( temporaryGpuMemorySummaryCpuZones, result.gpuMemorySummaryCpuZones.path, error ) ) throw std::runtime_error( "cannot commit GPU-memory summary CPU-zone index: " + error );
        if( !AtomicReplace( temporaryGpuMemorySummary, result.gpuMemorySummary.path, error ) ) throw std::runtime_error( "cannot commit GPU-memory summary index: " + error );
        if( !AtomicReplace( temporaryData, result.dataPath, error ) ) throw std::runtime_error( "cannot commit index data: " + error );
        if( !AtomicReplace( temporaryManifest, result.manifestPath, error ) ) throw std::runtime_error( "cannot commit index manifest: " + error );
        return result;
    }
    catch( ... )
    {
        std::error_code ignored;
        std::filesystem::remove( temporaryData, ignored );
        std::filesystem::remove( temporaryManifest, ignored );
        std::filesystem::remove( temporaryExtras, ignored );
        std::filesystem::remove( temporaryCpu, ignored );
        std::filesystem::remove( temporaryGpu, ignored );
        for( const auto& path : temporaryJn ) std::filesystem::remove( path, ignored );
        std::filesystem::remove( temporaryGpuMemoryCpuZones, ignored );
        std::filesystem::remove( temporaryGpuMemorySummaryCpuZones, ignored );
        std::filesystem::remove( temporaryGpuMemorySummary, ignored );
        throw;
    }
}

QueryIndexValidation QueryIndex::Validate( const std::filesystem::path& tracePath, bool deep )
{
    QueryIndexValidation result;
    try
    {
        QueryIndexManifest manifest;
        manifest.manifestPath = ManifestPath( tracePath );
        if( !std::filesystem::is_regular_file( manifest.manifestPath ) )
        {
            result.reason = "missing";
            return result;
        }

        const auto parsed = json::parse( ReadManifest( manifest.manifestPath ) );
        if( parsed.value( "magic", "" ) != IndexMagic || parsed.value( "schema_version", 0u ) != QueryIndexSchemaVersion )
        {
            result.reason = "schema_mismatch";
            return result;
        }
        const auto& source = parsed.at( "source" );
        const auto& data = parsed.at( "data" );
        manifest.sourceFingerprint = source.at( "sha256" ).get<std::string>();
        manifest.sourceBytes = ParseUnsigned( source, "bytes" );
        manifest.sourceWriteTime = ParseSigned( source, "write_time" );
        manifest.dataFingerprint = data.at( "sha256" ).get<std::string>();
        manifest.dataBytes = ParseUnsigned( data, "bytes" );
        const auto dataFile = std::filesystem::path( data.at( "file" ).get<std::string>() );
        if( dataFile.empty() || dataFile != dataFile.filename() )
        {
            result.reason = "invalid_data_path";
            return result;
        }
        manifest.dataPath = tracePath.parent_path() / dataFile;
        manifest.cpuZoneIndex = parsed.at( "domains" ).value( "cpu_zone_index", false );
        manifest.gpuZoneIndex = parsed.at( "domains" ).value( "gpu_zone_index", false );
        manifest.gpuMemoryProtocol2 = parsed.at( "domains" ).at( "gpu_memory_protocol2" ).get<bool>();
        const auto& counts = parsed.at( "counts" );
        for( const auto& [thread, count] : counts.at( "cpu_zones_by_thread" ).items() )
            manifest.cpuZonesByThread.emplace( std::stoull( thread ), std::stoull( count.get<std::string>() ) );
        for( const auto& [context, count] : counts.at( "gpu_zones_by_context" ).items() )
            manifest.gpuZonesByContext.emplace( uint32_t( std::stoul( context ) ), std::stoull( count.get<std::string>() ) );
        const auto& zoneValidation = parsed.at( "zone_validation" );
        manifest.zoneValidationPrecomputed = zoneValidation.value( "precomputed", false );
        manifest.zoneValidation.complete = zoneValidation.value( "complete", false );
        manifest.zoneValidation.scanned = ParseUnsigned( zoneValidation, "scanned" );
        for( const auto& finding : zoneValidation.at( "findings" ) )
        {
            analysis::ZoneValidationFindingDto value;
            value.severity = finding.at( "severity" ).get<std::string>();
            value.code = finding.at( "code" ).get<std::string>();
            value.message = finding.at( "message" ).get<std::string>();
            value.count = ParseUnsigned( finding, "count" );
            value.refs = finding.at( "refs" ).get<std::vector<std::string>>();
            manifest.zoneValidation.findings.emplace_back( std::move( value ) );
        }
        const auto parseSection = [&]( const char* name, QueryIndexSection& output ) {
            const auto& section = parsed.at( "sections" ).at( name );
            const auto file = std::filesystem::path( section.at( "file" ).get<std::string>() );
            if( file.empty() || file != file.filename() ) throw std::runtime_error( std::string( "invalid section path: " ) + name );
            output.path = tracePath.parent_path() / file;
            output.fingerprint = section.at( "sha256" ).get<std::string>();
            output.bytes = ParseUnsigned( section, "bytes" );
            output.count = ParseUnsigned( section, "count" );
            output.declaredCount = ParseUnsigned( section, "declared_count" );
            output.recordBytes = section.at( "record_bytes" ).get<uint32_t>();
        };
        parseSection( "zone_extra", manifest.zoneExtras );
        parseSection( "cpu_zone", manifest.cpuZones );
        parseSection( "gpu_zone", manifest.gpuZones );
        parseSection( "job_stage", manifest.jobStages );
        parseSection( "gfx_entity", manifest.gfxEntities );
        parseSection( "gfx_link", manifest.gfxLinks );
        parseSection( "relation", manifest.relations );
        parseSection( "gpu_reference_pass", manifest.gpuReferencePasses );
        parseSection( "gpu_reference_use", manifest.gpuReferenceUses );
        parseSection( "gpu_reference_end", manifest.gpuReferenceEnds );
        parseSection( "gpu_memory_cpu_zone", manifest.gpuMemoryCpuZones );
        parseSection( "gpu_memory_summary_cpu_zone", manifest.gpuMemorySummaryCpuZones );
        parseSection( "gpu_memory_summary", manifest.gpuMemorySummary );

        if( std::filesystem::file_size( tracePath ) != manifest.sourceBytes || WriteTime( tracePath ) != manifest.sourceWriteTime )
        {
            result.reason = "source_identity_mismatch";
            return result;
        }
        if( deep && analysis::WorkerTraceSource::ComputeFingerprint( tracePath ) != manifest.sourceFingerprint )
        {
            result.reason = "source_hash_mismatch";
            return result;
        }
        if( !std::filesystem::is_regular_file( manifest.dataPath ) || std::filesystem::file_size( manifest.dataPath ) != manifest.dataBytes )
        {
            result.reason = "data_identity_mismatch";
            return result;
        }
        if( deep && analysis::WorkerTraceSource::ComputeFingerprint( manifest.dataPath ) != manifest.dataFingerprint )
        {
            result.reason = "data_hash_mismatch";
            return result;
        }
        const auto validateSection = [&]( const QueryIndexSection& section, uint32_t expectedRecordBytes, const char* name ) {
            if( section.recordBytes != expectedRecordBytes || !std::filesystem::is_regular_file( section.path ) || std::filesystem::file_size( section.path ) != section.bytes )
                throw std::runtime_error( std::string( name ) + " section identity mismatch" );
            if( deep && analysis::WorkerTraceSource::ComputeFingerprint( section.path ) != section.fingerprint )
                throw std::runtime_error( std::string( name ) + " section hash mismatch" );
        };
        validateSection( manifest.zoneExtras, sizeof( ZoneExtraIndexRecord ), "zone-extra" );
        validateSection( manifest.cpuZones, sizeof( CpuZoneIndexRecord ), "CPU-zone" );
        validateSection( manifest.gpuZones, sizeof( GpuZoneIndexRecord ), "GPU-zone" );
        validateSection( manifest.jobStages, sizeof( JnJobStageData ), "job-stage" );
        validateSection( manifest.gfxEntities, sizeof( JnGfxEntityData ), "gfx-entity" );
        validateSection( manifest.gfxLinks, sizeof( JnGfxLinkData ), "gfx-link" );
        validateSection( manifest.relations, sizeof( JnRelationData ), "relation" );
        validateSection( manifest.gpuReferencePasses, sizeof( JnGpuReferencePassData ), "GPU-reference-pass" );
        validateSection( manifest.gpuReferenceUses, sizeof( JnGpuReferenceUseData ), "GPU-reference-use" );
        validateSection( manifest.gpuReferenceEnds, sizeof( JnGpuReferenceEndData ), "GPU-reference-end" );
        validateSection( manifest.gpuMemoryCpuZones, sizeof( uint64_t ), "GPU-memory-CPU-zone" );
        validateSection( manifest.gpuMemorySummaryCpuZones, sizeof( uint64_t ), "GPU-memory-summary-CPU-zone" );
        validateSection( manifest.gpuMemorySummary, 0, "GPU-memory-summary" );
        result.manifest = std::move( manifest );
        result.reason = "valid";
    }
    catch( const std::exception& error )
    {
        result.reason = std::string( "invalid: " ) + error.what();
    }
    return result;
}

std::unique_ptr<analysis::TraceSource> QueryIndex::Open( const QueryIndexManifest& manifest,
    analysis::WorkerTraceSource::StateCallback stateCallback, std::string fingerprintOverride )
{
    auto source = analysis::WorkerTraceSource::Open( manifest.dataPath, std::move( stateCallback ),
        fingerprintOverride.empty() ? manifest.sourceFingerprint : std::move( fingerprintOverride ), analysis::WorkerTraceLoadMode::IndexedSidecar );
    return std::make_unique<IndexedTraceSource>( manifest, std::move( source ) );
}

}
