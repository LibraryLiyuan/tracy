#include <type_traits>
#include "TracyQueryIndex.hpp"
#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleStore.hpp"
#include "TracyQuerySampleView.hpp"
#include "TracyQueryStringStore.hpp"
#include "TracyQueryValidationStore.hpp"
#include "TracyFileHeader.hpp"
#include "TracyFileReadHooks.hpp"
#include "TracyWorker.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisIoPath.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace tracy::query
{
namespace
{

class QueryIndexOwnerLease
{
public:
    explicit QueryIndexOwnerLease( std::filesystem::path path ) : m_path( std::move( path ) )
    {
#ifdef _WIN32
        m_handle = CreateFileW( analysis::AnalysisIoPath( m_path ).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( m_handle == INVALID_HANDLE_VALUE )
            throw std::runtime_error( "Query index owner lock is held: " + std::to_string( GetLastError() ) );
#else
        m_fd = ::open( m_path.c_str(), O_CREAT | O_RDWR, 0600 );
        if( m_fd < 0 || flock( m_fd, LOCK_EX | LOCK_NB ) != 0 )
        {
            if( m_fd >= 0 ) ::close( m_fd );
            m_fd = -1;
            throw std::runtime_error( "Query index owner lock is held" );
        }
#endif
    }
    ~QueryIndexOwnerLease()
    {
#ifdef _WIN32
        if( m_handle != INVALID_HANDLE_VALUE ) CloseHandle( m_handle );
        std::error_code ignored;
        std::filesystem::remove( m_path, ignored );
#else
        if( m_fd >= 0 ) { flock( m_fd, LOCK_UN ); ::close( m_fd ); }
#endif
    }
    QueryIndexOwnerLease( const QueryIndexOwnerLease& ) = delete;
    QueryIndexOwnerLease& operator=( const QueryIndexOwnerLease& ) = delete;

private:
    std::filesystem::path m_path;
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

class QueryIndexReaderLease
{
public:
    static std::shared_ptr<void> Acquire( const std::filesystem::path& path, std::string& error,
        std::function<void()> cleanup = {} )
    {
        error.clear();
#ifdef _WIN32
        const auto handle = CreateFileW( analysis::AnalysisIoPath( path ).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            error = "Query generation reader lease failed: " + std::to_string( GetLastError() );
            return {};
        }
        return std::shared_ptr<void>( new QueryIndexReaderLease( path, handle, std::move( cleanup ) ), []( void* value ) {
            delete static_cast<QueryIndexReaderLease*>( value );
        } );
#else
        const auto fd = ::open( path.c_str(), O_CREAT | O_RDONLY, 0600 );
        if( fd < 0 || flock( fd, LOCK_SH | LOCK_NB ) != 0 )
        {
            if( fd >= 0 ) ::close( fd );
            error = "Query generation reader lease failed";
            return {};
        }
        return std::shared_ptr<void>( new QueryIndexReaderLease( path, fd, std::move( cleanup ) ), []( void* value ) {
            delete static_cast<QueryIndexReaderLease*>( value );
        } );
#endif
    }

private:
#ifdef _WIN32
    QueryIndexReaderLease( std::filesystem::path path, HANDLE handle, std::function<void()> cleanup )
        : m_path( std::move( path ) ), m_cleanup( std::move( cleanup ) ), m_handle( handle ) {}
#else
    QueryIndexReaderLease( std::filesystem::path path, int fd, std::function<void()> cleanup )
        : m_path( std::move( path ) ), m_cleanup( std::move( cleanup ) ), m_fd( fd ) {}
#endif
    ~QueryIndexReaderLease()
    {
#ifdef _WIN32
        if( m_handle != INVALID_HANDLE_VALUE ) CloseHandle( m_handle );
#else
        if( m_fd >= 0 ) { flock( m_fd, LOCK_UN ); ::close( m_fd ); }
#endif
        if( m_cleanup ) try { m_cleanup(); } catch( ... ) {}
    }

    std::filesystem::path m_path;
    std::function<void()> m_cleanup;
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

class QueryIndexGenerationCleanupLease
{
public:
    static std::unique_ptr<QueryIndexGenerationCleanupLease> TryAcquire(
        const std::filesystem::path& path, bool& busy, std::string& error )
    {
        busy = false;
        error.clear();
#ifdef _WIN32
        const auto handle = CreateFileW( analysis::AnalysisIoPath( path ).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            const auto code = GetLastError();
            if( code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ) busy = true;
            else error = "Query generation cleanup lease failed: " + std::to_string( code );
            return {};
        }
        return std::unique_ptr<QueryIndexGenerationCleanupLease>(
            new QueryIndexGenerationCleanupLease( path, handle ) );
#else
        const auto fd = ::open( path.c_str(), O_CREAT | O_RDWR, 0600 );
        if( fd < 0 )
        {
            error = "Query generation cleanup lease failed";
            return {};
        }
        if( flock( fd, LOCK_EX | LOCK_NB ) != 0 )
        {
            busy = errno == EWOULDBLOCK || errno == EAGAIN;
            if( !busy ) error = "Query generation cleanup lease failed";
            ::close( fd );
            return {};
        }
        return std::unique_ptr<QueryIndexGenerationCleanupLease>(
            new QueryIndexGenerationCleanupLease( path, fd ) );
#endif
    }

    ~QueryIndexGenerationCleanupLease()
    {
#ifdef _WIN32
        if( m_handle != INVALID_HANDLE_VALUE ) CloseHandle( m_handle );
#else
        if( m_fd >= 0 ) { flock( m_fd, LOCK_UN ); ::close( m_fd ); }
#endif
    }

private:
#ifdef _WIN32
    QueryIndexGenerationCleanupLease( std::filesystem::path path, HANDLE handle )
        : m_path( std::move( path ) ), m_handle( handle ) {}
#else
    QueryIndexGenerationCleanupLease( std::filesystem::path path, int fd )
        : m_path( std::move( path ) ), m_fd( fd ) {}
#endif
    std::filesystem::path m_path;
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

using nlohmann::json;
constexpr const char* IndexMagic = "JNTRACY-QUERY-INDEX";
constexpr uint64_t MaximumManifestBytes = 1024 * 1024;
constexpr uint64_t MaximumGpuMemorySummaryBytes = 256 * 1024 * 1024;

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
    GpuMemorySummaryCpuZone = 12,
    Sample = 13
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

    void Open( const std::filesystem::path& path, ZoneSectionKind kind, uint64_t count, uint32_t recordBytes,
        std::string_view sourceFingerprint, std::shared_ptr<analysis::AnalysisDiskBudget> disk = {} )
    {
        if( m_data ) throw std::runtime_error( "zone index section is already open" );
        if( count > ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / recordBytes ) throw std::runtime_error( "zone index section size overflow" );
        m_path = path;
        m_count = count;
        m_recordBytes = recordBytes;
        m_bytes = sizeof( ZoneSectionHeader ) + count * recordBytes;
        m_disk = std::move( disk );
#ifdef _WIN32
        m_file = CreateFileW( analysis::AnalysisIoPath( path ).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr );
        if( m_file == INVALID_HANDLE_VALUE ) throw std::runtime_error( "cannot create zone index section" );
        try { analysis::AnalysisDiskGrow( m_disk, m_bytes ); }
        catch( ... ) { CloseHandle( m_file ); m_file = INVALID_HANDLE_VALUE; std::error_code ignored; std::filesystem::remove( path, ignored ); throw; }
        LARGE_INTEGER size; size.QuadPart = m_bytes;
        if( !SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) || !SetEndOfFile( m_file ) ) throw std::runtime_error( "cannot size zone index section" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr );
        if( !m_mapping ) throw std::runtime_error( "cannot create zone index mapping" );
        m_data = static_cast<uint8_t*>( MapViewOfFile( m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t( m_bytes ) ) );
        if( !m_data ) throw std::runtime_error( "cannot map zone index section" );
#else
        m_file = ::open( path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600 );
        if( m_file < 0 ) throw std::runtime_error( "cannot create zone index section" );
        try { analysis::AnalysisDiskGrow( m_disk, m_bytes ); }
        catch( ... ) { close( m_file ); m_file = -1; std::error_code ignored; std::filesystem::remove( path, ignored ); throw; }
        if( ftruncate( m_file, off_t( m_bytes ) ) != 0 ) throw std::runtime_error( "cannot size zone index section" );
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
        if( !m_data )
        {
            std::ostringstream message;
            message << "zone index record is outside declared bounds: path=" << m_path.string() << ", index=" << index << ", count=" << m_count;
            throw std::runtime_error( message.str() );
        }
        if( index >= m_count ) Grow( index + 1 );
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
        if( m_disk && m_disk->usage && finalBytes < m_bytes ) m_disk->usage->Release( m_bytes - finalBytes );
        m_bytes = finalBytes;
    }

private:
    void Grow( uint64_t requiredCount )
    {
        if( requiredCount <= m_count ) return;
        const auto increment = std::max<uint64_t>( 1024, m_count / 2 );
        const auto grownCount = m_count <= std::numeric_limits<uint64_t>::max() - increment ? m_count + increment : std::numeric_limits<uint64_t>::max();
        const auto newCount = std::max( requiredCount, grownCount );
        if( newCount > ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / m_recordBytes )
            throw std::runtime_error( "zone index section growth overflow" );
        const auto newBytes = sizeof( ZoneSectionHeader ) + newCount * m_recordBytes;
        if( newBytes > std::numeric_limits<size_t>::max() ) throw std::runtime_error( "zone index section growth exceeds address space" );
        analysis::AnalysisDiskGrow( m_disk, newBytes - m_bytes );
#ifdef _WIN32
        FlushViewOfFile( m_data, size_t( m_bytes ) );
        UnmapViewOfFile( m_data );
        m_data = nullptr;
        CloseHandle( m_mapping );
        m_mapping = nullptr;
        LARGE_INTEGER size; size.QuadPart = newBytes;
        if( !SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) || !SetEndOfFile( m_file ) )
            throw std::runtime_error( "cannot grow zone index section" );
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr );
        if( !m_mapping ) throw std::runtime_error( "cannot remap grown zone index section" );
        m_data = static_cast<uint8_t*>( MapViewOfFile( m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t( newBytes ) ) );
        if( !m_data ) throw std::runtime_error( "cannot map grown zone index section" );
#else
        msync( m_data, size_t( m_bytes ), MS_SYNC );
        munmap( m_data, size_t( m_bytes ) );
        m_data = nullptr;
        if( ftruncate( m_file, off_t( newBytes ) ) != 0 ) throw std::runtime_error( "cannot grow zone index section" );
        m_data = static_cast<uint8_t*>( mmap( nullptr, size_t( newBytes ), PROT_READ | PROT_WRITE, MAP_SHARED, m_file, 0 ) );
        if( m_data == MAP_FAILED ) { m_data = nullptr; throw std::runtime_error( "cannot map grown zone index section" ); }
#endif
        m_count = newCount;
        m_bytes = newBytes;
        reinterpret_cast<ZoneSectionHeader*>( m_data )->count = newCount;
    }

    std::filesystem::path m_path;
    uint8_t* m_data = nullptr;
    uint64_t m_count = 0;
    uint64_t m_bytes = 0;
    uint32_t m_recordBytes = 0;
    std::shared_ptr<analysis::AnalysisDiskBudget> m_disk;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    int m_file = -1;
#endif
};

bool AtomicReplace( const std::filesystem::path& source, const std::filesystem::path& target, std::string& error,
    const std::function<bool()>& cancelled = {} );
std::string ReadManifest( const std::filesystem::path& path, uint64_t maximumBytes = MaximumManifestBytes );

class QuerySerializedStringSource final : public SerializedStringSource
{
public:
    explicit QuerySerializedStringSource( std::shared_ptr<QueryStringStoreReader> store ) : m_store( std::move( store ) ) {}
    uint64_t StringCount() const override { return m_store->StringCount(); }
    uint64_t PointerCount( uint8_t kind ) const override { return m_store->PointerCount( QueryStringPointerKind( kind ) ); }
    const char* StringAt( uint32_t ordinal ) const override { return m_store->StableOrdinal( ordinal ).data; }
    const char* PointerString( uint8_t kind, uint64_t id ) const override
    {
        if( kind > 1 ) return nullptr;
        const auto value = m_store->StablePointer( QueryStringPointerKind( kind ), id );
        return value ? value->data : nullptr;
    }
    std::pair<const char*, const char*> ExternalName( uint64_t id ) const override
    {
        const auto value = m_store->StableExternal( id );
        return value ? std::make_pair( value->first.data, value->second.data ) : std::make_pair( nullptr, nullptr );
    }
    bool HasPointer( uint8_t kind, uint64_t id ) const override
    {
        return kind < 3 && m_store->HasPointer( QueryStringPointerKind( kind ), id );
    }
    void VisitPointerIds( uint8_t kind, const std::function<void( uint64_t )>& visitor ) const override
    {
        if( kind >= 3 ) throw std::runtime_error( "query string pointer kind is invalid" );
        m_store->VisitPointerIds( QueryStringPointerKind( kind ), visitor );
    }
private:
    std::shared_ptr<QueryStringStoreReader> m_store;
};

class QuerySerializedCallstackSource final : public SerializedCallstackSource
{
public:
    explicit QuerySerializedCallstackSource( std::shared_ptr<QueryCallstackStoreReader> store ) : m_store( std::move( store ) ) {}
    void SetFrames( std::shared_ptr<QueryCallstackFrameStoreReader> frames ) { m_frames=std::move(frames); }
    uint64_t CallstackCount() const override { return m_store->Count(); }
    const CallstackFrameId* Frames( uint32_t id, uint32_t& count ) const override
    {
        if( id == 0 || id > m_store->Count() ) { count = 0; return nullptr; }
        const auto value=m_store->Frames(id);count=uint32_t(value.size());return value.data();
    }
    uint64_t FrameCount() const override { return m_frames?m_frames->Count():0; }
    bool Frame( CallstackFrameId id, FrameView& output ) const override
    {
        if(!m_frames)return false;const auto value=m_frames->Find(id);if(!value)return false;
        output.imageName=value->imageName;output.frames=value->frames.data();output.count=uint8_t(value->frames.size());return true;
    }
private:
    std::shared_ptr<QueryCallstackStoreReader> m_store;
    std::shared_ptr<QueryCallstackFrameStoreReader> m_frames;
};

class ZoneIndexWriter final : public SerializedZoneSink
{
public:
    static constexpr size_t JnDomainCount = 7;

    ZoneIndexWriter( std::filesystem::path extras, std::filesystem::path cpu, std::filesystem::path gpu,
        std::filesystem::path samples, std::filesystem::path catalog, std::filesystem::path strings,
        std::filesystem::path callstacks, std::filesystem::path callstackFrames,
        std::array<std::filesystem::path, JnDomainCount> jnPaths,
        std::string sourceFingerprint, analysis::PagedRecordOptions storageOptions = {} )
        : m_extraPath( std::move( extras ) ), m_cpuPath( std::move( cpu ) ), m_gpuPath( std::move( gpu ) )
        , m_samplePath( std::move( samples ) ), m_stringPath( std::move( strings ) ), m_callstackPath( std::move( callstacks ) ), m_callstackFramePath( std::move( callstackFrames ) ), m_jnPaths( std::move( jnPaths ) )
        , m_sourceFingerprint( std::move( sourceFingerprint ) )
        , m_storageOptions( std::move( storageOptions ) )
        , m_samples( m_samplePath, m_sourceFingerprint, m_storageOptions )
        , m_catalog( std::move( catalog ), m_sourceFingerprint, m_storageOptions )
        , m_stringWriter( std::make_unique<QueryStringStoreWriter>( m_stringPath, m_sourceFingerprint, m_storageOptions ) )
        , m_callstackWriter( std::make_unique<QueryCallstackStoreWriter>( m_callstackPath, m_sourceFingerprint, m_storageOptions ) )
        , m_callstackFrameWriter( std::make_unique<QueryCallstackFrameStoreWriter>( m_callstackFramePath, m_sourceFingerprint, m_storageOptions ) )
    {}

    bool WantsGlobalStrings() const override { return true; }
    void GlobalStringsBegin( uint64_t count ) override { m_stringWriter->StringsBegin( count ); }
    void GlobalStringBegin( uint64_t index, uint64_t token, uint64_t bytes ) override { m_stringWriter->StringBegin( index, token, bytes ); }
    void GlobalStringBytes( uint64_t index, uint64_t offset, const void* bytes, uint64_t count ) override { m_stringWriter->StringBytes( index, offset, bytes, count ); }
    void GlobalStringEnd( uint64_t index ) override { m_stringWriter->StringEnd( index ); }
    void GlobalStringsEnd() override { m_stringWriter->StringsEnd(); }
    void GlobalPointerTableBegin( uint8_t kind, uint64_t count ) override { m_stringWriter->PointerTableBegin( QueryStringPointerKind( kind ), count ); }
    void GlobalPointerRecord( uint8_t kind, uint64_t index, uint64_t id, uint64_t first, uint64_t second ) override
    { m_stringWriter->PointerRecord( QueryStringPointerKind( kind ), index, id, first, second ); }
    void GlobalPointerTableEnd( uint8_t kind ) override
    {
        m_stringWriter->PointerTableEnd( QueryStringPointerKind( kind ) );
        if( kind == 2 )
        {
            m_stringWriter->Commit();
            m_stringWriter.reset();
            m_stringReader = std::make_shared<QueryStringStoreReader>( m_stringPath, m_sourceFingerprint, m_storageOptions );
            m_stringSource = std::make_unique<QuerySerializedStringSource>( m_stringReader );
        }
    }
    SerializedStringSource* GlobalStringSource() override { return m_stringSource.get(); }
    uint64_t StringCount() const { return m_stringReader ? m_stringReader->StringCount() : 0; }
    bool WantsCallstacks() const override { return true; }
    void CallstacksBegin( uint64_t count ) override { m_callstackWriter->Begin( count ); }
    void CallstackBegin( uint32_t id, uint32_t count ) override { m_callstackWriter->CallstackBegin( id, count ); }
    void CallstackFrames( uint32_t id, uint32_t offset, const CallstackFrameId* frames, uint32_t count ) override { m_callstackWriter->CallstackFrames( id, offset, frames, count ); }
    void CallstackEnd( uint32_t id ) override { m_callstackWriter->CallstackEnd( id ); }
    void CallstacksEnd() override
    {
        m_callstackWriter->Commit();m_callstackWriter.reset();
        m_callstackReader=std::make_shared<QueryCallstackStoreReader>(m_callstackPath,m_sourceFingerprint,m_storageOptions);
        m_callstackSource=std::make_unique<QuerySerializedCallstackSource>(m_callstackReader);
    }
    SerializedCallstackSource* GlobalCallstackSource() override { return m_callstackSource.get(); }
    uint64_t CallstackCount() const { return m_callstackReader?m_callstackReader->Count():0; }
    bool WantsCallstackFrames() const override { return true; }
    void CallstackFrameDefinitionsBegin( uint64_t count ) override { m_callstackFrameWriter->Begin(count); }
    void CallstackFrameDefinitionBegin( uint64_t ordinal, CallstackFrameId id, uint8_t count, StringIdx imageName ) override { m_callstackFrameWriter->FrameBegin(ordinal,id,count,imageName); }
    void CallstackFrameDefinitionRecords( uint64_t ordinal, uint32_t offset, const CallstackFrame* frames, uint32_t count ) override { m_callstackFrameWriter->FrameRecords(ordinal,offset,frames,count); }
    void CallstackFrameDefinitionEnd( uint64_t ordinal ) override { m_callstackFrameWriter->FrameEnd(ordinal); }
    void CallstackFrameDefinitionsEnd() override
    {
        m_callstackFrameWriter->Commit();m_callstackFrameWriter.reset();m_callstackFrameReader=std::make_shared<QueryCallstackFrameStoreReader>(m_callstackFramePath,m_sourceFingerprint,m_storageOptions);m_callstackSource->SetFrames(m_callstackFrameReader);
    }
    uint64_t CallstackFrameCount() const { return m_callstackFrameReader?m_callstackFrameReader->Count():0; }
    void ReleaseExternalSources() { m_stringSource.reset(); m_stringReader.reset(); m_callstackSource.reset(); m_callstackReader.reset(); m_callstackFrameReader.reset(); }

    void ZoneExtrasBegin( uint64_t count ) override
    {
        if( count > std::numeric_limits<size_t>::max() ) throw std::runtime_error( "zone extra state exceeds address space" );
        m_expectedExtras = count;
        const auto stateBytes = count / 8 + ( count % 8 != 0 );
        if( stateBytes > ( std::numeric_limits<uint64_t>::max() - 65536 ) / 3 ) throw std::runtime_error( "zone extra state size overflow" );
        m_extraStateWorkspace = analysis::AnalysisWorkspaceReservation( m_storageOptions.workspace, 65536 + stateBytes * 3 );
        m_extraHasText.assign( size_t( count ), false );
        m_extraNameActive.assign( size_t( count ), false );
        m_extraNameResolved.assign( size_t( count ), false );
        m_extras.Open( m_extraPath, ZoneSectionKind::Extra, count, sizeof( ZoneExtraIndexRecord ), m_sourceFingerprint, m_storageOptions.disk );
    }
    void ZoneExtraRecord( uint64_t index, uint32_t callstack, bool textActive, uint32_t text, bool nameActive, uint32_t name, uint32_t color ) override
    {
        auto* record = reinterpret_cast<ZoneExtraIndexRecord*>( m_extras.Record( index ) );
        uint8_t flags=uint8_t((textActive?QueryIndexZoneExtraTextActive:0)|(nameActive?QueryIndexZoneExtraNameActive:0));
        if(nameActive&&name!=0&&m_stringReader&&name<m_stringReader->StringCount())flags|=QueryIndexZoneExtraNameResolvedFlag;
        *record = { callstack, text, name, color, flags, {} };
        if( index >= m_extraHasText.size() ) throw std::runtime_error( "zone extra index exceeds declared count" );
        m_extraHasText[size_t( index )] = textActive;
        m_extraNameActive[size_t( index )] = nameActive;
        m_extraNameResolved[size_t( index )] = QueryIndexZoneExtraNameResolved( flags );
        m_seenExtras++;
    }
    void ZoneExtrasEnd() override { if( m_seenExtras != m_expectedExtras ) throw std::runtime_error( "zone extra index count mismatch" ); m_extras.Close(); }

    void CpuZonesBegin( uint64_t count ) override
    {
        m_expectedCpu = count;
        const auto slack = std::max<uint64_t>( 1024, count / 1000 );
        m_cpu.Open( m_cpuPath, ZoneSectionKind::Cpu, count + slack, sizeof( CpuZoneIndexRecord ), m_sourceFingerprint, m_storageOptions.disk );
    }
    void CpuZoneBegin( uint64_t index, uint64_t parent, uint64_t thread, int64_t start, int16_t sourceLocation, uint32_t extra, uint32_t childCount ) override
    {
        if( m_cpuSpans.empty() || m_cpuSpans.back().thread != thread ) m_cpuSpans.push_back( { thread, index, 0 } );
        m_cpuSpans.back().count++;
        m_cpuZonesByThread[thread]++;
        if( extra < m_extraHasText.size() && m_extraHasText[extra] ) m_cpuSourcesWithText.emplace( sourceLocation );
        uint16_t flags = 0;
        if( extra < m_extraNameActive.size() )
        {
            if( m_extraNameActive[extra] ) flags |= QueryIndexCpuZoneNameActive;
            if( m_extraNameResolved[extra] ) flags |= QueryIndexCpuZoneNameResolvedFlag;
        }
        else if( extra != 0 ) flags |= QueryIndexCpuZoneExtraMissingFlag;
        auto* record = reinterpret_cast<CpuZoneIndexRecord*>( m_cpu.Record( index ) );
        *record = { start, 0, 0, thread, parent, extra, childCount, sourceLocation, flags };
        m_seenCpu++;
    }
    void CpuZoneEnd( uint64_t index, int64_t end ) override
    {
        auto* record = reinterpret_cast<CpuZoneIndexRecord*>( m_cpu.Record( index ) );
        if( !QueryIndexCpuZoneTimingComplete( end ) ) return;
        record->end = end;
        record->flags |= QueryIndexCpuZoneTimingCompleteFlag;
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
            const auto sortedBytes=sizeof(ZoneSectionHeader)+m_seenCpu*sizeof(CpuZoneIndexRecord);
            analysis::AnalysisDiskGrow(m_storageOptions.disk,sortedBytes);
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
            const auto replacedBytes=std::filesystem::file_size(m_cpuPath);
            std::string error;
            if( !AtomicReplace( sortedPath, m_cpuPath, error, m_storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit sorted CPU-zone index: " + error );
            if(m_storageOptions.disk && m_storageOptions.disk->usage) m_storageOptions.disk->usage->Release(replacedBytes);
            std::vector<bool>().swap( m_extraHasText );
            std::vector<bool>().swap( m_extraNameActive );
            std::vector<bool>().swap( m_extraNameResolved );
            m_extraStateWorkspace.Resize( 0 );
        }
        catch( ... )
        {
            std::error_code ignored;
            analysis::AnalysisDiskRemove(sortedPath,m_storageOptions.disk,ignored);
            throw;
        }
    }

    void GpuZonesBegin( uint64_t count ) override
    {
        m_expectedGpu = count;
        const auto slack = std::max<uint64_t>( 1024, count / 1000 );
        m_gpu.Open( m_gpuPath, ZoneSectionKind::Gpu, count + slack, sizeof( GpuZoneIndexRecord ), m_sourceFingerprint, m_storageOptions.disk );
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

    bool WantsSamples() const override { return true; }
    void SamplesBegin( uint64_t thread, uint8_t kind, uint64_t count ) override
    {
        m_samples.Begin( thread, kind == 0 ? QuerySampleKind::Main : QuerySampleKind::ContextSwitch, count );
    }
    void SampleRecord( uint64_t thread, uint8_t kind, uint64_t index, int64_t time, uint32_t callstack ) override
    {
        (void)thread; (void)kind;
        m_samples.Append( index, time, callstack );
    }
    void SamplesEnd( uint64_t thread, uint8_t kind ) override
    {
        (void)thread; (void)kind;
        m_samples.End();
    }
    void CommitSamples() { m_samples.Commit(); }
    uint64_t SampleCount() const { return m_samples.Count(); }
    uint64_t MainSampleCount() const { return m_samples.MainCount(); }
    uint64_t ContextSwitchSampleCount() const { return m_samples.ContextSwitchCount(); }

    void CatalogMetadata( OfflineCatalogQuality metadata ) override
    {
        m_catalog.SetMetadata( metadata );
        m_catalogMetadata = true;
    }
    bool WantsCatalogStrings() const override { return true; }
    bool AllowsLargeCatalogStrings( uint64_t ) const override { return true; }
    void CatalogStringsBegin( uint64_t count ) override { m_catalog.BeginStrings( count ); }
    void CatalogStringRecord( uint64_t index, uint64_t generation,
        const JnGpuCatalogStringRecordHeaderV1& header, const void* bytes ) override
    {
        m_catalog.CatalogString( index, generation, header, bytes );
    }
    void CatalogStringsEnd() override { m_catalog.EndStrings(); }
    void FinishCatalog()
    {
        if( !m_catalogMetadata ) m_catalog.SetMetadata( {} );
        m_catalog.Finish();
    }

    bool WantsJnRecords( JnDomain domain, uint32_t recordBytes ) const override
    {
        if( const auto catalog = CatalogDomain( domain ) ) return recordBytes == OfflineCatalogInfo( *catalog ).width;
        return size_t(domain)<JnDomainCount && recordBytes == JnRecordBytes( domain );
    }
    bool AllowsLargeJnRecords( JnDomain domain, uint64_t count, uint32_t recordBytes ) const override
    {
        return WantsJnRecords( domain, recordBytes ) &&
            count <= ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / recordBytes;
    }
    void JnRecordsBegin( JnDomain domain, uint64_t count, uint32_t recordBytes ) override
    {
        if( const auto catalog = CatalogDomain( domain ) )
        {
            m_catalog.Begin( *catalog, count, recordBytes );
            return;
        }
        const auto index = size_t( domain );
        if( index >= JnDomainCount || recordBytes != JnRecordBytes( domain ) ) throw std::runtime_error( "JN index domain layout mismatch" );
        m_jnStarted[index] = true;
        m_jnExpected[index] = count;
        m_jn[index].Open( m_jnPaths[index], JnSectionKind( domain ), count, recordBytes, m_sourceFingerprint, m_storageOptions.disk );
    }
    void JnRecordsBlock( JnDomain domain, uint64_t offset, const void* records, uint64_t count, uint32_t recordBytes ) override
    {
        if( const auto catalog = CatalogDomain( domain ) )
        {
            m_catalog.Append( *catalog, offset, records, count, recordBytes );
            return;
        }
        const auto index = size_t( domain );
        if( index >= JnDomainCount || recordBytes != JnRecordBytes( domain ) || offset != m_jnSeen[index] || count > m_jnExpected[index] - offset )
            throw std::runtime_error( "JN index block is out of sequence" );
        if( count != 0 ) std::memcpy( m_jn[index].Record( offset ), records, size_t( count * recordBytes ) );
        m_jnSeen[index] += count;
    }
    void JnRecordsEnd( JnDomain domain ) override
    {
        if( const auto catalog = CatalogDomain( domain ) )
        {
            m_catalog.End( *catalog );
            return;
        }
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
            m_jn[index].Open( m_jnPaths[index], JnSectionKind( domain ), 0, JnRecordBytes( domain ), m_sourceFingerprint, m_storageOptions.disk );
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

    static std::optional<OfflineCatalogDomain> CatalogDomain( JnDomain domain )
    {
        switch( domain )
        {
#define X(name,field,type,mut) case JnDomain::name: return OfflineCatalogDomain::name;
        TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
        default: return std::nullopt;
        }
    }

private:
    struct CpuSpan { uint64_t thread; uint64_t start; uint64_t count; };
    std::filesystem::path m_extraPath, m_cpuPath, m_gpuPath, m_samplePath, m_stringPath, m_callstackPath, m_callstackFramePath;
    std::array<std::filesystem::path, JnDomainCount> m_jnPaths;
    std::string m_sourceFingerprint;
    analysis::PagedRecordOptions m_storageOptions;
    MappedSection m_extras, m_cpu, m_gpu;
    std::array<MappedSection, JnDomainCount> m_jn;
    std::array<uint64_t, JnDomainCount> m_jnExpected {};
    std::array<uint64_t, JnDomainCount> m_jnSeen {};
    std::array<bool, JnDomainCount> m_jnStarted {};
    uint64_t m_expectedExtras = 0, m_expectedCpu = 0, m_expectedGpu = 0;
    uint64_t m_seenExtras = 0, m_seenCpu = 0, m_seenGpu = 0;
    std::vector<CpuSpan> m_cpuSpans;
    std::vector<bool> m_extraHasText;
    std::vector<bool> m_extraNameActive;
    std::vector<bool> m_extraNameResolved;
    analysis::AnalysisWorkspaceReservation m_extraStateWorkspace;
    std::unordered_set<int16_t> m_cpuSourcesWithText;
    std::unordered_map<uint64_t, uint64_t> m_cpuZonesByThread;
    std::unordered_map<uint32_t, uint64_t> m_gpuZonesByContext;
    QuerySampleStoreWriter m_samples;
    QueryCatalogStoreWriter m_catalog;
    std::unique_ptr<QueryStringStoreWriter> m_stringWriter;
    std::shared_ptr<QueryStringStoreReader> m_stringReader;
    std::unique_ptr<QuerySerializedStringSource> m_stringSource;
    std::unique_ptr<QueryCallstackStoreWriter> m_callstackWriter;
    std::shared_ptr<QueryCallstackStoreReader> m_callstackReader;
    std::unique_ptr<QuerySerializedCallstackSource> m_callstackSource;
    std::unique_ptr<QueryCallstackFrameStoreWriter> m_callstackFrameWriter;
    std::shared_ptr<QueryCallstackFrameStoreReader> m_callstackFrameReader;
    bool m_catalogMetadata = false;
};

std::string PagedSectionIdentity(std::string_view source,ZoneSectionKind kind)
{
    const auto value=std::string(source)+":"+ExtendedTraceDecoderIdentity+":jnidx:"+std::to_string(QueryIndexSchemaVersion)+":"+std::to_string(uint32_t(kind));
    analysis::Sha256Builder hash; hash.Update(value.data(),value.size()); return hash.FinalHex();
}

bool CleanupStaleCandidates( const std::filesystem::path& root, std::string_view identityPrefix,
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk, std::string& error )
{
    error.clear();
    std::error_code ec;
    if( !std::filesystem::exists( root, ec ) ) return !ec;
    std::vector<std::filesystem::path> candidates;
    for( std::filesystem::directory_iterator entry( root, ec ), end; entry != end && !ec; entry.increment( ec ) )
    {
        const auto name = entry->path().filename().string();
        if( !name.starts_with( identityPrefix ) || name.find( ".tmp." ) == std::string::npos ) continue;
        const auto status = std::filesystem::symlink_status( entry->path(), ec );
        if( ec ) break;
        if( std::filesystem::is_symlink( status ) ) continue;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW( analysis::AnalysisIoPath( entry->path() ).c_str() );
        if( attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_REPARSE_POINT ) ) continue;
#endif
        candidates.emplace_back( entry->path() );
    }
    if( ec ) { error = "Query stale candidate scan failed: " + ec.message(); return false; }
    try
    {
        if( disk && disk->usage ) disk->usage->AddRoots( candidates );
    }
    catch( const std::exception& failure )
    {
        error = std::string( "Query stale candidate accounting failed: " ) + failure.what();
        return false;
    }
    for( const auto& candidate : candidates )
    {
        ec.clear();
        analysis::AnalysisDiskRemove( candidate, disk, ec, true );
        if( ec ) { error = "Query stale candidate cleanup failed: " + candidate.string() + ": " + ec.message(); return false; }
    }
    return true;
}

std::optional<std::string> PublishedGenerationBaseName( std::string_view name, std::string_view identityPrefix )
{
    if( !name.starts_with( identityPrefix ) || name.find( ".tmp." ) != std::string_view::npos ) return std::nullopt;
    auto cursor = identityPrefix.size();
    const auto digits = cursor;
    while( cursor < name.size() && name[cursor] >= '0' && name[cursor] <= '9' ) ++cursor;
    if( cursor == digits ) return std::nullopt;
    const auto suffix = name.substr( cursor );
    static constexpr std::array<std::string_view, 20> OwnedSuffixes = {
        ".jnidx.data", ".jnidx.zextra", ".jnidx.cpu", ".jnidx.gpu", ".jnidx.sample",
        ".jnidx.string", ".jnidx.callstack", ".jnidx.callstack-frame", ".jnidx.gpu-catalog",
        ".jnidx.job-stage", ".jnidx.gfx-entity", ".jnidx.gfx-link", ".jnidx.relation",
        ".jnidx.gpu-ref-pass", ".jnidx.gpu-ref-use", ".jnidx.gpu-ref-end",
        ".jnidx.gpu-memory-cpu-zone", ".jnidx.gpu-memory-summary-cpu-zone",
        ".jnidx.gpu-memory-summary.json", ".reader.lock"
    };
    if( std::find( OwnedSuffixes.begin(), OwnedSuffixes.end(), suffix ) == OwnedSuffixes.end() )
        return std::nullopt;
    return std::string( name.substr( 0, cursor ) );
}

std::string ManifestGenerationBaseName( const QueryIndexManifest& manifest )
{
    constexpr std::string_view suffix = ".jnidx.data";
    const auto name = manifest.dataPath.filename().string();
    if( !std::string_view( name ).ends_with( suffix ) || name.size() == suffix.size() )
        throw std::runtime_error( "invalid Query generation data path" );
    return name.substr( 0, name.size() - suffix.size() );
}

std::filesystem::path GenerationReaderLeasePath( const QueryIndexManifest& manifest )
{
    return manifest.dataPath.parent_path() / ( ManifestGenerationBaseName( manifest ) + ".reader.lock" );
}

bool CleanupUnreferencedGenerations( const std::filesystem::path& root, std::string_view identityPrefix,
    std::string_view currentBaseName, const std::shared_ptr<analysis::AnalysisDiskBudget>& disk,
    std::vector<std::filesystem::path>& retained, std::string& error )
{
    retained.clear();
    error.clear();
    std::map<std::string, std::vector<std::filesystem::path>> generations;
    std::unordered_set<std::string> unsafe;
    std::error_code ec;
    for( std::filesystem::directory_iterator entry( root, ec ), end; entry != end && !ec; entry.increment( ec ) )
    {
        const auto baseName = PublishedGenerationBaseName( entry->path().filename().string(), identityPrefix );
        if( !baseName || *baseName == currentBaseName ) continue;
        const auto status = std::filesystem::symlink_status( entry->path(), ec );
        if( ec ) break;
        if( std::filesystem::is_symlink( status ) ) unsafe.emplace( *baseName );
#ifdef _WIN32
        const auto attributes = GetFileAttributesW( analysis::AnalysisIoPath( entry->path() ).c_str() );
        if( attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_REPARSE_POINT ) )
            unsafe.emplace( *baseName );
#endif
        generations[*baseName].emplace_back( entry->path() );
    }
    if( ec )
    {
        error = "Query generation scan failed: " + ec.message();
        return false;
    }
    for( auto& [baseName, paths] : generations )
    {
        const auto leasePath = root / ( baseName + ".reader.lock" );
        if( unsafe.contains( baseName ) )
        {
            retained.insert( retained.end(), paths.begin(), paths.end() );
            continue;
        }
        try
        {
            if( disk && disk->usage ) disk->usage->AddRoots( paths );
        }
        catch( const std::exception& failure )
        {
            retained.insert( retained.end(), paths.begin(), paths.end() );
            error = std::string( "Query generation accounting failed: " ) + failure.what();
            return false;
        }
        bool busy = false;
        std::string leaseError;
        auto lease = QueryIndexGenerationCleanupLease::TryAcquire( leasePath, busy, leaseError );
        if( !lease )
        {
            retained.insert( retained.end(), paths.begin(), paths.end() );
            if( busy ) continue;
            error = std::move( leaseError );
            return false;
        }
        for( const auto& path : paths )
        {
            if( path == leasePath ) continue;
            ec.clear();
            analysis::AnalysisDiskRemove( path, disk, ec, true );
            if( ec )
            {
                retained.insert( retained.end(), paths.begin(), paths.end() );
                error = "Query generation cleanup failed: " + path.string() + ": " + ec.message();
                return false;
            }
        }
        lease.reset();
        ec.clear();
        analysis::AnalysisDiskRemove( leasePath, disk, ec );
        if( ec )
        {
            retained.emplace_back( leasePath );
            error = "Query generation lease cleanup failed: " + leasePath.string() + ": " + ec.message();
            return false;
        }
    }
    std::sort( retained.begin(), retained.end() );
    retained.erase( std::unique( retained.begin(), retained.end() ), retained.end() );
    return true;
}
uint64_t SectionBytes(const std::filesystem::path& path)
{
    return analysis::AnalysisDiskUsage::Bytes(path);
}
std::string SectionFingerprint(const std::filesystem::path& path)
{
    if(!std::filesystem::is_directory(path)) return analysis::WorkerTraceSource::ComputeFingerprint(path);
    analysis::Sha256Builder hash;
    std::vector<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(path))
        if(entry.is_regular_file()) files.emplace_back(std::filesystem::relative(entry.path(),path));
    std::sort(files.begin(),files.end());
    for(const auto& name:files) {
        const auto value=name.generic_string()+":"+analysis::Sha256File(path/name)+"\n";
        hash.Update(value.data(),value.size());
    }
    return hash.FinalHex();
}

analysis::PagedRecordOptions ResourceStorageOptions(const std::shared_ptr<QueryResourceContext>& resources)
{
    analysis::PagedRecordOptions options;
    if(resources)
    {
        options.workspace=resources->Workspace();
        options.disk=resources->Disk();
    }
    return options;
}

std::shared_ptr<tracy::FileReadHooks> ResourceReadHooks(
    const std::shared_ptr<QueryResourceContext>& resources,std::string prefix)
{
    if(!resources) return {};
    auto hooks=resources->CreateFileReadHooks();
    auto check=hooks->check;
    hooks->check=[resources,prefix=std::move(prefix),check=std::move(check)] {
        const auto progress=analysis::WorkerTraceSource::GetLoadProgress();
        resources->SetPhase(prefix+progress.stage);
        resources->SetProgress(progress.subTotal?progress.subCompleted:progress.completed,
            progress.subTotal?progress.subTotal:progress.total);
        if(check) check();
    };
    return hooks;
}

class ReadOnlyZoneSection
{
public:
    ReadOnlyZoneSection() = default;
    ReadOnlyZoneSection( const QueryIndexSection& section, ZoneSectionKind kind, uint32_t recordBytes,
        std::string_view sourceFingerprint, analysis::PagedRecordOptions options = {} )
    {
        Open( section, kind, recordBytes, sourceFingerprint, std::move( options ) );
    }
    ~ReadOnlyZoneSection() { Close(); }
    ReadOnlyZoneSection( const ReadOnlyZoneSection& ) = delete;
    ReadOnlyZoneSection& operator=( const ReadOnlyZoneSection& ) = delete;

    template<typename T>
    T At( uint64_t index ) const
    {
        if( index >= m_count || sizeof( T ) != m_recordBytes ) throw std::out_of_range( "zone index record is outside the mapped section" );
        if(m_paged) {
            std::lock_guard lock(m_readMutex);
            T value{}; m_paged->Read(index,&value,1); return value;
        }
        return *reinterpret_cast<const T*>( m_data + sizeof( ZoneSectionHeader ) + index * m_recordBytes );
    }
    uint64_t Count() const { return m_count; }

private:
    void Open( const QueryIndexSection& section, ZoneSectionKind kind, uint32_t recordBytes,
        std::string_view sourceFingerprint, analysis::PagedRecordOptions options )
    {
        if( section.recordBytes != recordBytes || section.count > ( std::numeric_limits<uint64_t>::max() - sizeof( ZoneSectionHeader ) ) / recordBytes )
            throw std::runtime_error( "zone index section layout mismatch" );
        if(std::filesystem::is_directory(section.path)) {
            if(SectionBytes(section.path)!=section.bytes) throw std::runtime_error("paged index section size mismatch");
            m_paged=std::make_unique<analysis::PagedRecordReader>(section.path,PagedSectionIdentity(sourceFingerprint,kind),recordBytes,std::move(options));
            if(m_paged->Count()!=section.count) throw std::runtime_error("paged index section count mismatch");
            m_count=section.count; m_recordBytes=recordBytes; return;
        }
        const auto expectedBytes = sizeof( ZoneSectionHeader ) + section.count * recordBytes;
        if( section.bytes != expectedBytes || std::filesystem::file_size( section.path ) != expectedBytes )
            throw std::runtime_error( "zone index section size mismatch" );
        if( expectedBytes > std::numeric_limits<size_t>::max() ) throw std::runtime_error( "zone index section exceeds address space" );
#ifdef _WIN32
        m_file = CreateFileW( analysis::AnalysisIoPath( section.path ).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
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
        m_paged.reset();
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

    std::unique_ptr<analysis::PagedRecordReader> m_paged;
    mutable std::mutex m_readMutex;
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

struct MainPlayerLoopInterval
{
    uint64_t index = 0;
    int64_t start = 0;
    int64_t end = 0;
};

inline constexpr uint8_t MainPlayerLoopSource = 1;
inline constexpr uint8_t UnityPlayerLoopSource = 2;
using PlayerLoopSourceTable = std::array<uint8_t, 1u << 16>;
using MainPlayerLoopRoots = std::unordered_map<uint64_t, std::vector<MainPlayerLoopInterval>>;

template<typename RefFn, typename NoteFn>
void ValidateMainPlayerLoopRootOverlaps( MainPlayerLoopRoots& rootsByThread,
    RefFn&& makeRef, NoteFn&& note )
{
    for( auto& [thread, roots] : rootsByThread )
    {
        std::sort( roots.begin(), roots.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.start != rhs.start ? lhs.start < rhs.start : lhs.index < rhs.index;
        } );
        if( roots.empty() ) continue;
        auto longestOpen = roots.front();
        for( size_t index = 1; index < roots.size(); index++ )
        {
            const auto& current = roots[index];
            if( current.start < longestOpen.end )
            {
                note( "error", "CPU_FRAME_ROOT_CROSSING",
                    "Main.PlayerLoop overlaps the next authoritative frame root",
                    [&] { return makeRef( current.index ); } );
            }
            if( current.end > longestOpen.end ) longestOpen = current;
        }
    }
}

json GpuMemorySummaryJson( const analysis::GpuMemoryAttribution& value )
{
    json logicalResources = json::array();
    for( const auto& item : value.logicalResources ) logicalResources.push_back( {
        { "id", item.logicalResourceId }, { "physical", item.physicalAllocationId }, { "size", item.size },
        { "offset", item.physicalOffset }, { "owner", item.primaryOwnerId }, { "physical_owner", item.physicalOwnerId },
        { "flags", item.flags }, { "kind", uint8_t( item.kind ) }, { "segment", uint8_t( item.segment ) }, { "name", item.name } } );
    json logicalGenerations = json::array();
    std::vector<std::pair<analysis::MemoryEventKey, size_t>> generationKeys(
        value.logicalGenerationByKey.begin(), value.logicalGenerationByKey.end() );
    std::sort( generationKeys.begin(), generationKeys.end(), []( const auto& lhs, const auto& rhs ) { return lhs.first < rhs.first; } );
    for( const auto& [key, generationIndex] : generationKeys )
    {
        if( generationIndex >= value.logicalGenerations.size() ) continue;
        const auto& item = value.logicalGenerations[generationIndex];
        logicalGenerations.push_back( {
            { "pool", key.pool }, { "index", key.index }, { "id", item.logicalResourceId },
            { "physical", item.physicalAllocationId }, { "size", item.size }, { "offset", item.physicalOffset },
            { "owner", item.primaryOwnerId }, { "physical_owner", item.physicalOwnerId }, { "flags", item.flags },
            { "kind", uint8_t( item.kind ) }, { "segment", uint8_t( item.segment ) }, { "name", item.name } } );
    }
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
        { "schema", 3 }, { "protocol", value.protocolPresent }, { "protocol2", value.protocol2Present },
        { "structured", value.structuredReferencePresent }, { "complete", value.complete },
        { "capture_boundary", value.captureBoundaryPasses }, { "capture_boundary_reference_uses", value.captureBoundaryReferenceUses },
        { "submission_unobserved", value.submissionUnobservedPasses },
        { "gpu_unavailable", value.gpuResultUnavailablePasses }, { "warnings", value.warnings },
        { "logical_count", value.logicalResources.size() }, { "logical_resources", std::move( logicalResources ) },
        { "logical_generations", std::move( logicalGenerations ) },
        { "origins", std::move( origins ) }, { "owner_rollups", std::move( ownerRollups ) },
        { "working_count", value.aggregatedWorkingSetCount },
        { "unknown_use_quality_available", value.unknownUseQualityAvailable },
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
    const auto schema = value.at( "schema" ).get<uint32_t>();
    if( schema != 2 && schema != 3 ) throw std::runtime_error( "GPU-memory summary schema mismatch" );
    analysis::GpuMemoryAttribution result;
    result.protocolPresent = value.at( "protocol" ).get<bool>(); result.protocol2Present = value.at( "protocol2" ).get<bool>();
    result.structuredReferencePresent = value.at( "structured" ).get<bool>(); result.complete = value.at( "complete" ).get<bool>();
    result.captureBoundaryPasses = value.at( "capture_boundary" ).get<uint64_t>();
    result.captureBoundaryReferenceUses = schema >= 3 ? value.value( "capture_boundary_reference_uses", uint64_t( 0 ) ) : 0;
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
    if( schema >= 3 ) for( const auto& item : value.at( "logical_generations" ) )
    {
        analysis::GpuMemoryLogicalResource resource;
        resource.logicalResourceId = item.at( "id" ).get<uint64_t>(); resource.physicalAllocationId = item.at( "physical" ).get<uint64_t>();
        resource.size = item.at( "size" ).get<uint64_t>(); resource.physicalOffset = item.at( "offset" ).get<uint64_t>();
        resource.primaryOwnerId = item.at( "owner" ).get<uint32_t>(); resource.physicalOwnerId = item.at( "physical_owner" ).get<uint32_t>();
        resource.flags = item.at( "flags" ).get<uint32_t>(); resource.kind = char( item.at( "kind" ).get<uint8_t>() );
        resource.segment = char( item.at( "segment" ).get<uint8_t>() ); resource.name = item.at( "name" ).get<std::string>();
        const analysis::MemoryEventKey key { item.at( "pool" ).get<uint64_t>(), item.at( "index" ).get<size_t>() };
        result.logicalGenerationByKey.emplace( key, result.logicalGenerations.size() );
        result.logicalGenerations.emplace_back( std::move( resource ) );
    }
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
    result.unknownUseQualityAvailable = schema >= 3 ? value.value( "unknown_use_quality_available", false ) : false;
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

class IndexedTraceSource final : public analysis::TraceSource,
    public analysis::NativeBoundedTraceSource,
    public analysis::GpuCatalogBoundedScanSource,
    public QueryCatalogStatusSource,
    public QueryCatalogResourceSource,
    public QueryResourceContextSource
{
public:
    IndexedTraceSource( QueryIndexManifest manifest, std::unique_ptr<QuerySerializedStringSource> stringSource,
        std::unique_ptr<QuerySerializedCallstackSource> callstackSource,
        std::unique_ptr<analysis::WorkerTraceSource> source,
        std::shared_ptr<QueryResourceContext> resources = {} )
        : m_manifest( std::move( manifest ) )
        , m_stringSource( std::move( stringSource ) )
        , m_callstackSource( std::move( callstackSource ) )
        , m_source( std::move( source ) )
        , m_resources( std::move( resources ) )
        , m_storageOptions( ResourceStorageOptions( m_resources ) )
        , m_extras( m_manifest.zoneExtras, ZoneSectionKind::Extra, sizeof( ZoneExtraIndexRecord ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_cpu( m_manifest.cpuZones, ZoneSectionKind::Cpu, sizeof( CpuZoneIndexRecord ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpu( m_manifest.gpuZones, ZoneSectionKind::Gpu, sizeof( GpuZoneIndexRecord ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_samples( m_manifest.samples.path, m_manifest.sourceFingerprint, m_storageOptions )
        , m_catalog( m_manifest.gpuCatalog.path, m_manifest.sourceFingerprint, m_storageOptions )
        , m_jobStages( m_manifest.jobStages, ZoneSectionKind::JobStage, sizeof( JnJobStageData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gfxEntities( m_manifest.gfxEntities, ZoneSectionKind::GfxEntity, sizeof( JnGfxEntityData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gfxLinks( m_manifest.gfxLinks, ZoneSectionKind::GfxLink, sizeof( JnGfxLinkData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_relations( m_manifest.relations, ZoneSectionKind::Relation, sizeof( JnRelationData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpuReferencePasses( m_manifest.gpuReferencePasses, ZoneSectionKind::GpuReferencePass, sizeof( JnGpuReferencePassData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpuReferenceUses( m_manifest.gpuReferenceUses, ZoneSectionKind::GpuReferenceUse, sizeof( JnGpuReferenceUseData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpuReferenceEnds( m_manifest.gpuReferenceEnds, ZoneSectionKind::GpuReferenceEnd, sizeof( JnGpuReferenceEndData ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpuMemoryCpuZones( m_manifest.gpuMemoryCpuZones, ZoneSectionKind::GpuMemoryCpuZone, sizeof( uint64_t ), m_manifest.sourceFingerprint, m_storageOptions )
        , m_gpuMemorySummaryCpuZones( m_manifest.gpuMemorySummaryCpuZones, ZoneSectionKind::GpuMemorySummaryCpuZone, sizeof( uint64_t ), m_manifest.sourceFingerprint, m_storageOptions )
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
            if( item.domain == "zone.cpu" || item.domain == "zone.gpu" || item.domain == "timeline" )
            {
                item.present = item.domain == "zone.gpu" ? m_gpu.Count() != 0 : item.domain == "zone.cpu" ? m_cpu.Count() != 0 : item.present;
                item.queryable = true;
                item.indexed = true;
                item.reason = "exact N16.10 memory-mapped zone sidecar";
            }
            else if( item.domain == "sample" && m_samples.Count() != 0 )
            {
                item.present = true;
                item.queryable = true;
                item.indexed = true;
                item.reason = "bounded Query sample sidecar; ghost-zone and symbol-statistics indices are pending";
            }
            else if( item.domain == "job" || item.domain == "job.gfx" || item.domain == "evidence" )
            {
                item.queryable = false;
                item.indexed = true;
                item.reason = "JobStage sidecar is present; bounded Job aggregation is pending";
            }
            else if( item.domain == "gpu.catalog" || item.domain == "gpu.resource" || item.domain == "gpu.memory" )
            {
                item.present = m_manifest.catalogValidation.present;
                item.indexed = true;
                if( !m_manifest.catalogValidation.healthy )
                {
                    item.queryable = item.domain == "gpu.catalog";
                    item.reason = m_manifest.catalogValidation.present ?
                        "GPU Catalog sidecar failed completed semantic validation" : "GPU Catalog data is absent";
                }
                else if( item.domain == "gpu.catalog" )
                {
                    item.queryable = true;
                    item.reason = "bounded GPU Catalog status is ready; full validation diagnostics are pending";
                }
                else if( item.domain == "gpu.resource" )
                {
                    item.queryable = m_catalog.ResourceIndexReady();
                    item.reason = item.queryable ?
                        "bounded GPU resource get and lifetime indices are ready; search and associations are pending" :
                        "bounded GPU resource indices are pending";
                }
                else
                {
                    item.queryable = false;
                    item.reason = "bounded GPU memory analysis indices are pending";
                }
            }
        }
        return result;
    }

    uint32_t NativeBoundedScanVersion() const override
    {
        return analysis::NativeBoundedScanSchemaVersion;
    }

    analysis::TraceInfoDto GetTraceInfo() const override
    {
        auto result = m_source->GetTraceInfo();
        result.counts.cpuZones = m_cpu.Count();
        result.counts.gpuZones = m_gpu.Count();
        result.counts.samples = m_samples.MainCount();
        result.counts.contextSwitchSamples = m_samples.ContextSwitchCount();
        result.counts.jobStages = m_jobStages.Count();
        result.counts.gfxEntities = m_gfxEntities.Count();
        result.counts.gfxLinks = m_gfxLinks.Count();
        result.counts.relations = m_relations.Count();
        result.counts.gpuReferencePasses = m_gpuReferencePasses.Count();
        result.counts.gpuReferenceUses = m_gpuReferenceUses.Count();
        result.counts.gpuReferenceEnds = m_gpuReferenceEnds.Count();
        return result;
    }

    std::shared_ptr<const tracy::JnTraceData> GetGpuCatalogData() const override
    {
        // Catalog section 12 remains immutable in the authoritative Worker
        // snapshot.  The sidecar accelerates existing timeline/reference
        // domains while forwarding Catalog evidence verbatim, so an indexed
        // open cannot silently report N27 as absent.
        return m_source->GetGpuCatalogData();
    }

    QueryCatalogStatusData GetQueryCatalogStatus( size_t generationLimit, size_t evidenceLimit ) const override
    {
        return m_catalog.Status( generationLimit, evidenceLimit );
    }
    const QueryCatalogValidationResult& GetQueryCatalogValidation() const override
    {
        return m_manifest.catalogValidation;
    }

    std::optional<QueryCatalogResourceStateData> GetQueryCatalogResourceState( uint64_t resourceId ) const override
    {
        return m_catalog.ResourceState( resourceId );
    }

    std::optional<std::string> GetQueryCatalogResourceName( uint64_t generation, uint32_t stringId ) const override
    {
        return m_catalog.ResourceName( generation, stringId );
    }

    uint64_t GetGpuCatalogResourceCountBounded() const override { return m_source->GetGpuCatalogResourceCountBounded(); }
    uint64_t GetGpuCatalogAllocationCountBounded() const override { return m_source->GetGpuCatalogAllocationCountBounded(); }
    uint64_t GetGpuCatalogPassCountBounded() const override { return m_source->GetGpuCatalogPassCountBounded(); }
    uint64_t GetGpuCatalogRangeCountBounded() const override { return m_source->GetGpuCatalogRangeCountBounded(); }
    std::vector<analysis::GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded(
        size_t offset, size_t limit ) const override
    {
        return m_source->ScanGpuCatalogResourcesBounded( offset, limit );
    }
    std::vector<analysis::GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded(
        size_t offset, size_t limit ) const override
    {
        return m_source->ScanGpuCatalogAllocationsBounded( offset, limit );
    }
    std::vector<analysis::GpuPassWorkingSet> ScanGpuCatalogPassesBounded(
        size_t offset, size_t limit ) const override
    {
        return m_source->ScanGpuCatalogPassesBounded( offset, limit );
    }
    std::vector<analysis::GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded(
        size_t offset, size_t limit ) const override
    {
        return m_source->ScanGpuCatalogRangesBounded( offset, limit );
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
        if( !m_manifest.zoneValidationPrecomputed ) return std::nullopt;
        auto result = m_manifest.zoneValidation;
        if( allowance( 1 ) == 0 ) result.complete = false;
        return result;
    }
    std::optional<analysis::ZoneValidationSummaryDto> ValidateSystemTrace( const std::function<size_t( size_t )>& allowance ) const override
    {
        return m_source->ValidateSystemTrace( allowance );
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
    std::vector<std::string> ScanSamples( const analysis::ScanRange& range ) const override
    {
        const auto rows = m_samples.Scan( range.startNs, range.endNs, range.offset, range.limit, QuerySampleKind::Main );
        std::vector<std::string> result;
        result.reserve( rows.size() );
        for( const auto& row : rows ) result.emplace_back( m_source->MakeEntityRef( "sample", row.ordinal ) );
        return result;
    }

    std::shared_ptr<QueryResourceContext> GetQueryResourceContext() const override { return m_resources; }
    bool SampleDerivedDataReady() const override { return false; }
    bool JobDerivedDataReady() const override { return false; }
    bool CatalogDerivedDataReady() const override { return false; }
    TRACY_INDEX_FORWARD0( std::vector<analysis::JobDto>, GetJobs )
    TRACY_INDEX_FORWARD0( uint64_t, GetJobCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::JobDto>, ScanJobs, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::vector<analysis::JobDto>, GetEvidenceJobs, uint64_t, frameId )
    std::vector<analysis::JobDto> GetEvidenceJobs( uint64_t frameId, const std::function<void()>& check ) const override
    {
        return m_source->GetEvidenceJobs( frameId, check );
    }
    std::vector<analysis::JobDto> GetDirectedJobs( uint64_t jobId, bool includeNeighbors,
        const std::function<void()>& check ) const override
    {
        return m_source->GetDirectedJobs( jobId, includeNeighbors, check );
    }
    TRACY_INDEX_FORWARD0( std::vector<analysis::IoRequestDto>, GetIoRequests )
    TRACY_INDEX_FORWARD0( uint64_t, GetIoRequestCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::IoRequestDto>, ScanIoRequests, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD0( std::vector<analysis::GfxDispatchDto>, GetGfxDispatches )
    TRACY_INDEX_FORWARD0( uint64_t, GetGfxDispatchCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::GfxDispatchDto>, ScanGfxDispatches, size_t, offset, size_t, limit )
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
    uint64_t GetGfxEntityCount() const override { return m_gfxEntities.Count(); }
    std::vector<analysis::GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        const auto begin = std::min<uint64_t>( offset, m_gfxEntities.Count() );
        const auto end = begin + std::min<uint64_t>( limit, m_gfxEntities.Count() - begin );
        std::vector<analysis::GfxEntityDto> result;
        result.reserve( size_t( end - begin ) );
        for( uint64_t index = begin; index < end; ++index )
        {
            const auto& value = m_gfxEntities.At<JnGfxEntityData>( index );
            result.push_back( { MakeEntityRef( "gfx-entity", value.entityId ), value.entityId,
                value.parentId, value.time, MakeEntityRef( "thread", value.thread ),
                value.gpuQueryId, value.gpuContext, value.kind, value.flags } );
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
    uint64_t GetGfxLinkCount() const override { return m_gfxLinks.Count(); }
    std::vector<analysis::GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        const auto begin = std::min<uint64_t>( offset, m_gfxLinks.Count() );
        const auto end = begin + std::min<uint64_t>( limit, m_gfxLinks.Count() - begin );
        std::vector<analysis::GfxLinkDto> result;
        result.reserve( size_t( end - begin ) );
        for( uint64_t index = begin; index < end; ++index )
        {
            const auto& value = m_gfxLinks.At<JnGfxLinkData>( index );
            result.push_back( { MakeEntityRef( "gfx-link", index ), value.sourceId, value.targetId,
                value.time, MakeEntityRef( "thread", value.thread ), value.relation, value.flags } );
        }
        return result;
    }
    analysis::GfxEvidenceSlice GetEvidenceGfx( uint64_t frameId,
        const std::vector<uint64_t>& seedIds ) const override
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
    TRACY_INDEX_FORWARD0( uint64_t, GetRuntimeDomainStateCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::RuntimeDomainStateDto>, ScanRuntimeDomainStates, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD0( std::vector<analysis::ScriptFrameDto>, GetScriptFrames )
    TRACY_INDEX_FORWARD0( uint64_t, GetScriptFrameCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::ScriptFrameDto>, ScanScriptFrames, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD0( std::vector<analysis::ScriptStackEventDto>, GetScriptStackEvents )
    TRACY_INDEX_FORWARD0( uint64_t, GetScriptStackEventCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::ScriptStackEventDto>, ScanScriptStackEvents, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD0( analysis::CrashDto, GetCrash )
    TRACY_INDEX_FORWARD0( std::vector<analysis::CpuTopologyDto>, GetCpuTopology )
    TRACY_INDEX_FORWARD0( std::vector<analysis::CpuUsagePointDto>, GetCpuUsage )
    TRACY_INDEX_FORWARD1( std::vector<analysis::ContextSwitchDto>, ScanContextSwitchEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD1( std::vector<analysis::CpuContextSwitchDto>, ScanCpuContextSwitchEvents, const analysis::ScanRange&, range )
    std::vector<analysis::SampleDto> ScanSampleEvents( const analysis::ScanRange& range ) const override
    {
        const auto rows = m_samples.Scan( range.startNs, range.endNs, range.offset, range.limit );
        std::vector<analysis::SampleDto> result;
        result.reserve( rows.size() );
        for( const auto& row : rows )
        {
            analysis::SampleDto value;
            value.ref = m_source->MakeEntityRef( row.kind == QuerySampleKind::Main ? "sample" : "context-switch-sample", row.ordinal );
            value.threadRef = m_source->MakeEntityRef( "thread", row.thread );
            value.timeNs = row.timeNs;
            value.callstack = row.callstack;
            if( value.callstack != 0 ) value.callstackRef = m_source->MakeEntityRef( "callstack", value.callstack );
            value.kind = row.kind == QuerySampleKind::Main ? "sample" : "context_switch";
            result.emplace_back( std::move( value ) );
        }
        return result;
    }
    TRACY_INDEX_FORWARD1( std::vector<analysis::GhostZoneDto>, ScanGhostZones, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD0( std::vector<analysis::HardwareSampleDto>, GetHardwareSamples )
    TRACY_INDEX_FORWARD4( std::vector<analysis::HardwareSampleEventDto>, GetHardwareSampleEvents, uint64_t, address, std::string_view, kind, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::vector<analysis::LockEventDto>, ScanLockEvents, const analysis::ScanRange&, range )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SymbolDto>, GetSymbols )
    TRACY_INDEX_FORWARD2( std::vector<analysis::SymbolAddressMappingDto>, GetSymbolAddressMappings, size_t, offset, size_t, limit )
    TRACY_INDEX_FORWARD1( std::optional<analysis::SymbolAddressMappingDto>, ResolveSymbolAddress, uint64_t, address )
    TRACY_INDEX_FORWARD0( std::vector<analysis::SourceLocationDto>, GetSourceLocations )
    TRACY_INDEX_FORWARD0( std::vector<analysis::CallsiteDto>, GetCallsites )
    TRACY_INDEX_FORWARD0( uint64_t, GetCallsiteCount )
    TRACY_INDEX_FORWARD2( std::vector<analysis::CallsiteDto>, ScanCallsites, size_t, offset, size_t, limit )
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
                pass.uses.push_back( { value.resourceId, value.usageMask, kind, value.resourceSetId, value.encoding } );
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
                pass.uses.push_back( { value.resourceId, value.usageMask, kind, value.resourceSetId, value.encoding } );
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
    std::optional<analysis::GpuMemoryUseReferencePage> ScanGpuMemoryUsesByResource( uint64_t resourceId,
        size_t offset, size_t limit ) const override
    {
        EnsureGpuResourcePassRefs();
        analysis::GpuMemoryUseReferencePage page;
        const auto found = m_gpuResourcePassRefs.find( resourceId );
        if( found == m_gpuResourcePassRefs.end() ) return page;
        page.totalUses = found->second.references.size();
        const auto begin = std::min( offset, found->second.references.size() );
        const auto end = begin + std::min( limit, found->second.references.size() - begin );
        page.references.insert( page.references.end(), found->second.references.begin() + begin,
            found->second.references.begin() + end );
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
                const auto generation = m_precomputedGpuMemorySummary->logicalGenerationByKey.find( allocation.key );
                if( generation != m_precomputedGpuMemorySummary->logicalGenerationByKey.end() &&
                    generation->second < m_precomputedGpuMemorySummary->logicalGenerations.size() )
                    logical = &m_precomputedGpuMemorySummary->logicalGenerations[generation->second];
                else
                {
                    const auto found = m_precomputedGpuMemorySummary->logicalById.find( allocation.allocationId );
                    if( found != m_precomputedGpuMemorySummary->logicalById.end() ) logical = &m_precomputedGpuMemorySummary->logicalResources[found->second];
                }
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
            const bool hasPassRefs = passRefs != m_gpuResourcePassRefs.end() && !passRefs->second.references.empty();
            const std::string currentState = requestLabel && hasPassRefs ? "request_and_uses" : requestLabel ? "request_only" : hasPassRefs ? "uses_only" : "unattributed";
            if( !relationState.empty() && currentState != relationState ) continue;
            if( matched++ < offset ) continue;
            if( page.allocations.size() >= limit ) { page.hasMore = true; break; }

            analysis::GpuMemoryAllocationPageItem item;
            item.attribution.allocation = allocation;
            item.attribution.requestLabelId = requestLabel;
            if( passRefs != m_gpuResourcePassRefs.end() )
            {
                item.passRefCount = passRefs->second.references.size();
                const auto previewCount = std::min<size_t>( 100, passRefs->second.references.size() );
                item.passIds.reserve( previewCount );
                for( size_t index = 0; index < previewCount; ++index )
                    item.passIds.emplace_back( passRefs->second.references[index].passId );
            }
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
        // The compact sidecar deliberately aggregates structured pass/use
        // records without materializing the full attribution graph. Never
        // represent this omitted deep lifetime check as a clean zero.
        result.unknownUseQualityAvailable = false;
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
        std::vector<analysis::GpuMemoryUseReference> references;
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
            std::unordered_map<uint64_t, std::pair<int64_t, int64_t>> passTimes;
            passTimes.reserve( size_t( m_gpuReferencePasses.Count() ) );
            for( uint64_t index = 0; index < m_gpuReferencePasses.Count(); index++ )
            {
                const auto& value = m_gpuReferencePasses.At<JnGpuReferencePassData>( index );
                passTimes.emplace( value.passId, std::pair<int64_t, int64_t> { value.time, value.time } );
            }
            for( uint64_t index = 0; index < m_gpuReferenceEnds.Count(); index++ )
            {
                const auto& value = m_gpuReferenceEnds.At<JnGpuReferenceEndData>( index );
                const auto pass = passTimes.find( value.passId );
                if( pass != passTimes.end() ) pass->second.second = value.time;
            }
            for( uint64_t index = 0; index < m_gpuReferenceUses.Count(); index++ )
            {
                const auto& value = m_gpuReferenceUses.At<JnGpuReferenceUseData>( index );
                const auto pass = passTimes.find( value.passId );
                if( value.resourceId == 0 || pass == passTimes.end() ) continue;
                auto& refs = m_gpuResourcePassRefs[value.resourceId].references;
                refs.push_back( { value.passId, pass->second.first, pass->second.second,
                    { value.resourceId, value.usageMask, 'U', value.resourceSetId, value.encoding } } );
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
        std::optional<ZoneExtraIndexRecord> extra;
        if(zone.extra<m_extras.Count()) extra=m_extras.At<ZoneExtraIndexRecord>(zone.extra);
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
    std::unique_ptr<QuerySerializedStringSource> m_stringSource;
    std::unique_ptr<QuerySerializedCallstackSource> m_callstackSource;
    std::unique_ptr<analysis::WorkerTraceSource> m_source;
    std::shared_ptr<QueryResourceContext> m_resources;
    analysis::PagedRecordOptions m_storageOptions;
    ReadOnlyZoneSection m_extras, m_cpu, m_gpu;
    QuerySampleViewReader m_samples;
    QueryCatalogStoreReader m_catalog;
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

// An inherited default silently materializes all Jobs and drops in-read controls.
// Keep the production index wrapper on the directed Worker path.
static_assert( std::is_same_v<decltype(&IndexedTraceSource::GetDirectedJobs),
    std::vector<analysis::JobDto> (IndexedTraceSource::*)(uint64_t, bool, const std::function<void()>&) const>,
    "IndexedTraceSource must override controlled directed Job reads" );
template<class Owner>
constexpr bool IndexedOwnsControlledFrameJobs(std::vector<analysis::JobDto> (Owner::*)(uint64_t, const std::function<void()>&) const)
{ return std::is_same_v<Owner, IndexedTraceSource>; }
static_assert( IndexedOwnsControlledFrameJobs(&IndexedTraceSource::GetEvidenceJobs),
    "IndexedTraceSource must override controlled frame Job reads" );


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

bool AtomicReplace( const std::filesystem::path& source, const std::filesystem::path& target, std::string& error,
    const std::function<bool()>& cancelled )
{
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    auto delay = std::chrono::milliseconds( 5 );
    while( true )
    {
        if( MoveFileExW( analysis::AnalysisIoPath( source ).c_str(), analysis::AnalysisIoPath( target ).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
        const auto code = GetLastError();
        if( cancelled && cancelled() ) { error = "Query index publication cancelled"; return false; }
        if( code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION )
        { error = "MoveFileExW failed with error " + std::to_string( code ); return false; }
        if( std::chrono::steady_clock::now() >= deadline )
        { error = "MoveFileExW failed with error " + std::to_string( code ); return false; }
        std::this_thread::sleep_for( delay );
        delay = std::min( delay * 2, std::chrono::milliseconds( 100 ) );
    }
#else
    if( cancelled && cancelled() ) { error = "Query index publication cancelled"; return false; }
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = ec.message();
    return false;
#endif
}

std::string ReadManifest( const std::filesystem::path& path, uint64_t maximumBytes )
{
    const auto native = analysis::AnalysisIoPath( path );
    const auto bytes = std::filesystem::file_size( native );
    if( bytes > maximumBytes ) throw std::runtime_error( "index JSON exceeds configured size limit" );
    std::ifstream input( native, std::ios::binary );
    if( !input ) throw std::runtime_error( "cannot open index manifest" );
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

bool ClearGenerationCleanupReport( const std::filesystem::path& path,
    const std::shared_ptr<QueryResourceContext>& resources, std::string& error )
{
    error.clear();
    std::error_code ec;
    analysis::AnalysisDiskRemove( path, resources ? resources->Disk() : nullptr, ec );
    if( ec )
    {
        error = "Query generation cleanup report removal failed: " + ec.message();
        return false;
    }
    return true;
}

bool WriteGenerationCleanupReport( const std::filesystem::path& path, std::string_view state,
    std::string_view reason, const std::vector<std::filesystem::path>& retained,
    const std::shared_ptr<QueryResourceContext>& resources, std::string& error )
{
    error.clear();
    try
    {
        constexpr size_t MaximumPaths = 256;
        json paths = json::array();
        uint64_t retainedCount = 0;
        for( const auto& value : retained )
        {
            std::error_code statusError;
            const auto status = std::filesystem::symlink_status( value, statusError );
            if( !statusError && !std::filesystem::exists( status ) ) continue;
            retainedCount++;
            if( paths.size() < MaximumPaths ) paths.push_back( value.filename().string() );
        }
        const auto boundedReason = std::string( reason.substr( 0, std::min<size_t>( reason.size(), 4096 ) ) );
        const auto payload = json {
            { "schema", 1 }, { "state", state }, { "reason", boundedReason },
            { "retained_count", std::to_string( retainedCount ) },
            { "retained_paths", std::move( paths ) },
            { "retained_paths_truncated", retainedCount > MaximumPaths }
        }.dump( 2 );
        if( payload.size() > 512 * 1024 ) throw std::runtime_error( "cleanup report exceeds bounded size" );
        auto temporary = path;
        temporary += ".tmp." + std::to_string( ProcessId() ) + "." +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
        const auto previous = resources && resources->Disk() && resources->Disk()->usage ?
            analysis::AnalysisDiskUsage::Bytes( path ) : 0;
        if( resources ) resources->BeforeFileGrowth( temporary, payload.size() );
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output ) throw std::runtime_error( "cleanup report cannot be opened" );
        output.write( payload.data(), std::streamsize( payload.size() ) );
        output.flush();
        if( !output ) throw std::runtime_error( "cleanup report cannot be written" );
        output.close();
        std::string publishError;
        if( !AtomicReplace( temporary, path, publishError, {} ) )
        {
            std::error_code ignored;
            analysis::AnalysisDiskRemove( temporary, resources ? resources->Disk() : nullptr, ignored );
            throw std::runtime_error( publishError );
        }
        if( previous != 0 && resources && resources->Disk() && resources->Disk()->usage )
            resources->Disk()->usage->Release( previous );
        return true;
    }
    catch( const std::exception& failure )
    {
        error = std::string( "Query generation cleanup report failed: " ) + failure.what();
        return false;
    }
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
    json catalogValidationFindings = json::array();
    for( const auto& finding : value.catalogValidation.findings ) catalogValidationFindings.push_back( {
        { "code", finding.code }, { "count", std::to_string( finding.count ) }
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
            { "gpu_zones_by_context", std::move( gpuZonesByContext ) },
            { "samples", std::to_string( value.sampleMainCount ) },
            { "context_switch_samples", std::to_string( value.sampleContextSwitchCount ) }
        } },
        { "zone_validation", {
            { "precomputed", value.zoneValidationPrecomputed },
            { "complete", value.zoneValidation.complete },
            { "scanned", std::to_string( value.zoneValidation.scanned ) },
            { "findings", std::move( zoneValidationFindings ) }
        } },
        { "catalog_validation", {
            { "complete", value.catalogValidation.complete },
            { "present", value.catalogValidation.present },
            { "original_valid", value.catalogValidation.originalValid },
            { "healthy", value.catalogValidation.healthy },
            { "findings", std::move( catalogValidationFindings ) }
        } },
        { "sections", {
            { "zone_extra", section( value.zoneExtras ) },
            { "cpu_zone", section( value.cpuZones ) },
            { "gpu_zone", section( value.gpuZones ) },
            { "sample", section( value.samples ) },
            { "global_string", section( value.globalStrings ) },
            { "global_callstack", section( value.globalCallstacks ) },
            { "global_callstack_frame", section( value.globalCallstackFrames ) },
            { "gpu_catalog", section( value.gpuCatalog ) },
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

std::vector<std::filesystem::path> ExistingDerivedPaths(const std::filesystem::path& tracePath)
{
    auto manifestPath=tracePath; manifestPath+=".jnidx";
    auto failurePath=manifestPath; failurePath+=".failure.json";
    auto cleanupPath=manifestPath; cleanupPath+=".cleanup.json";
    std::vector<std::filesystem::path> result{manifestPath,failurePath,cleanupPath};
    try
    {
        if(!std::filesystem::is_regular_file(manifestPath)) return result;
        const auto parsed=json::parse(ReadManifest(manifestPath));
        if(parsed.value("magic","")!=IndexMagic || parsed.value("schema_version",0u)!=QueryIndexSchemaVersion) return result;
        const auto append=[&](const json& value,const char* field) {
            const auto name=std::filesystem::path(value.at(field).get<std::string>());
            if(name.empty() || name!=name.filename()) throw std::runtime_error("invalid index resource path");
            result.emplace_back(tracePath.parent_path()/name);
        };
        append(parsed.at("data"),"file");
        for(const auto& [_,section]:parsed.at("sections").items()) append(section,"file");
    }
    catch(...) {}
    return result;
}

std::optional<std::string> CurrentPublishedGenerationBase( const std::filesystem::path& manifestPath,
    std::string_view identityPrefix, std::string_view expectedSourceFingerprint )
{
    std::error_code ec;
    if( !std::filesystem::exists( manifestPath, ec ) )
        return ec ? std::nullopt : std::optional<std::string>( std::string() );
    if( ec || !std::filesystem::is_regular_file( manifestPath, ec ) || ec ) return std::nullopt;
    try
    {
        const auto parsed = json::parse( ReadManifest( manifestPath ) );
        if( parsed.value( "magic", "" ) != IndexMagic ||
            parsed.value( "schema_version", 0u ) != QueryIndexSchemaVersion ||
            parsed.at( "source" ).at( "sha256" ).get<std::string>() != expectedSourceFingerprint )
            return std::nullopt;
        const auto file = std::filesystem::path( parsed.at( "data" ).at( "file" ).get<std::string>() );
        if( file.empty() || file != file.filename() ) return std::nullopt;
        const auto baseName = PublishedGenerationBaseName( file.string(), identityPrefix );
        if( !baseName || file.string() != *baseName + ".jnidx.data" ) return std::nullopt;
        return *baseName;
    }
    catch( ... )
    {
        return std::nullopt;
    }
}

std::shared_ptr<void> AcquireManifestReaderLease( const QueryIndexManifest& manifest,
    const std::shared_ptr<QueryResourceContext>& resources, std::string& error )
{
    const auto root = manifest.dataPath.parent_path();
    const auto identityPrefix = std::string( "jnidx." ) + manifest.sourceFingerprint.substr( 0, 16 ) +
        ".s" + std::to_string( QueryIndexSchemaVersion ) + ".g";
    const auto manifestPath = manifest.manifestPath;
    auto cleanupDiagnostic = manifestPath;
    cleanupDiagnostic += ".cleanup.json";
    auto ownerLockPath = manifestPath;
    ownerLockPath += ".owner.lock";
    const auto sourceFingerprint = manifest.sourceFingerprint;
    const auto cleanup = [root, identityPrefix, manifestPath, cleanupDiagnostic, ownerLockPath,
            sourceFingerprint, resources] {
        try
        {
            QueryIndexOwnerLease ownerLease( ownerLockPath );
            const auto currentGeneration = CurrentPublishedGenerationBase(
                manifestPath, identityPrefix, sourceFingerprint );
            if( !currentGeneration ) return;
            std::vector<std::filesystem::path> retained;
            std::string cleanupError;
            const auto disk = resources ? resources->Disk() : nullptr;
            const bool complete = CleanupUnreferencedGenerations( root, identityPrefix,
                *currentGeneration, disk, retained, cleanupError );
            std::string reportError;
            if( !complete )
                WriteGenerationCleanupReport( cleanupDiagnostic, "failed", cleanupError,
                    retained, resources, reportError );
            else if( !retained.empty() )
                WriteGenerationCleanupReport( cleanupDiagnostic, "deferred",
                    "active reader or protected generation", retained, resources, reportError );
            else ClearGenerationCleanupReport( cleanupDiagnostic, resources, reportError );
        }
        catch( ... )
        {
            // A builder or another final-reader callback owns the lock.  The
            // durable deferred report and next Build remain the recovery path.
        }
    };
    return QueryIndexReaderLease::Acquire( GenerationReaderLeasePath( manifest ), error, cleanup );
}

}

bool QueryIndex::PublishPathForTesting( const std::filesystem::path& source, const std::filesystem::path& target,
    std::string& error, const std::function<bool()>& cancelled )
{
    return AtomicReplace( source, target, error, cancelled );
}

bool QueryIndex::ProbeOwnerLockForTesting( const std::filesystem::path& tracePath, std::string& error )
{
    auto lockPath = ManifestPath( tracePath );
    lockPath += ".owner.lock";
    try
    {
        QueryIndexOwnerLease lease( lockPath );
        return true;
    }
    catch( const std::exception& failure )
    {
        error = failure.what();
        return false;
    }
}

bool QueryIndex::CleanupStaleCandidatesForTesting( const std::filesystem::path& root,
    std::string_view identityPrefix, std::string& error )
{
    return CleanupStaleCandidates( analysis::AnalysisIoPath( root ), identityPrefix, {}, error );
}

std::shared_ptr<void> QueryIndex::AcquireReaderLeaseForTesting( const std::filesystem::path& path,
    std::string& error )
{
    return QueryIndexReaderLease::Acquire( path, error );
}

bool QueryIndex::CleanupUnreferencedGenerationsForTesting( const std::filesystem::path& root,
    std::string_view identityPrefix, std::string_view currentBaseName,
    std::vector<std::filesystem::path>& retained, std::string& error )
{
    return CleanupUnreferencedGenerations( analysis::AnalysisIoPath( root ), identityPrefix,
        currentBaseName, {}, retained, error );
}

std::filesystem::path QueryIndex::ManifestPath( const std::filesystem::path& tracePath )
{
    auto result = analysis::AnalysisIoPath( tracePath );
    result += ".jnidx";
    return result;
}

QueryIndexManifest QueryIndex::Build( const std::filesystem::path& inputTracePath,
    analysis::WorkerTraceSource::StateCallback stateCallback, std::shared_ptr<QueryResourceContext> resources )
{
    const auto tracePath = analysis::AnalysisIoPath( inputTracePath );
    QueryIndexManifest result;
    result.manifestPath = ManifestPath( tracePath );
    result.sourceBytes = std::filesystem::file_size( tracePath );
    result.sourceWriteTime = WriteTime( tracePath );
    result.sourceFingerprint = analysis::WorkerTraceSource::ComputeFingerprint( tracePath );
    if(resources) resources->ValidateTraceIdentity(result.sourceBytes,result.sourceFingerprint);
    // The manifest remains adjacent to and named after the source trace, but
    // section files must not repeat an arbitrarily long trace filename.  The
    // old form could exceed the legacy Win32 MAX_PATH limit only after the
    // longest section suffix and the atomic-write .tmp.<pid> suffix were
    // appended.  The manifest already validates the complete 256-bit source
    // fingerprint; this bounded basename is only a collision-resistant local
    // section identity within the trace directory.
    const auto identityPrefix = std::string( "jnidx." ) + result.sourceFingerprint.substr( 0, 16 ) +
        ".s" + std::to_string( QueryIndexSchemaVersion ) + ".g";
    const auto baseName = identityPrefix +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dataName = baseName + ".jnidx.data";
    result.dataPath = tracePath.parent_path() / dataName;
    result.zoneExtras.path = tracePath.parent_path() / ( baseName + ".jnidx.zextra" );
    result.cpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.cpu" );
    result.gpuZones.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu" );
    result.samples.path = tracePath.parent_path() / ( baseName + ".jnidx.sample" );
    result.globalStrings.path = tracePath.parent_path() / ( baseName + ".jnidx.string" );
    result.globalCallstacks.path = tracePath.parent_path() / ( baseName + ".jnidx.callstack" );
    result.globalCallstackFrames.path = tracePath.parent_path() / ( baseName + ".jnidx.callstack-frame" );
    result.gpuCatalog.path = tracePath.parent_path() / ( baseName + ".jnidx.gpu-catalog" );
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
    result.samples.recordBytes = QuerySampleStoreRecordBytes;
    result.globalStrings.recordBytes = 0;
    result.globalCallstacks.recordBytes = 0;
    result.globalCallstackFrames.recordBytes = 0;
    result.gpuCatalog.recordBytes = 0;
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
    auto temporarySamples = result.samples.path; temporarySamples += suffix;
    auto temporaryStrings = result.globalStrings.path; temporaryStrings += suffix;
    auto temporaryCallstacks = result.globalCallstacks.path; temporaryCallstacks += suffix;
    auto temporaryCallstackFrames = result.globalCallstackFrames.path; temporaryCallstackFrames += suffix;
    auto temporaryCatalog = result.gpuCatalog.path; temporaryCatalog += suffix;
    std::array<std::filesystem::path, ZoneIndexWriter::JnDomainCount> temporaryJn;
    for( size_t index = 0; index < temporaryJn.size(); index++ ) { temporaryJn[index] = jnSections[index]->path; temporaryJn[index] += suffix; }
    auto temporaryGpuMemoryCpuZones = result.gpuMemoryCpuZones.path; temporaryGpuMemoryCpuZones += suffix;
    auto temporaryGpuMemorySummaryCpuZones = result.gpuMemorySummaryCpuZones.path; temporaryGpuMemorySummaryCpuZones += suffix;
    auto temporaryGpuMemorySummary = result.gpuMemorySummary.path; temporaryGpuMemorySummary += suffix;
    auto temporaryValidation = tracePath.parent_path() / ( baseName + ".jnidx.validation" + suffix );
    auto failureDiagnostic = result.manifestPath; failureDiagnostic += ".failure.json";
    auto cleanupDiagnostic = result.manifestPath; cleanupDiagnostic += ".cleanup.json";
    auto ownerLockPath = result.manifestPath; ownerLockPath += ".owner.lock";
    std::vector<std::filesystem::path> packedScratch;
    std::vector<std::filesystem::path> publishedCandidates;

    if( !resources ) resources=CreateResourceContext(tracePath);
    resources->ValidateTraceIdentity(result.sourceBytes,result.sourceFingerprint);
    resources->IncludeDiskRoots({
        result.manifestPath,result.dataPath,result.zoneExtras.path,result.cpuZones.path,result.gpuZones.path,
        result.samples.path,result.globalStrings.path,result.globalCallstacks.path,result.globalCallstackFrames.path,result.gpuCatalog.path,result.jobStages.path,result.gfxEntities.path,result.gfxLinks.path,
        result.relations.path,result.gpuReferencePasses.path,result.gpuReferenceUses.path,result.gpuReferenceEnds.path,
        result.gpuMemoryCpuZones.path,result.gpuMemorySummaryCpuZones.path,result.gpuMemorySummary.path,
        failureDiagnostic,cleanupDiagnostic,temporaryValidation,ownerLockPath
    });
    const auto storageOptions = ResourceStorageOptions( resources );

    try
    {
        QueryIndexOwnerLease ownerLease( ownerLockPath );
        resources->SetPhase( "cleanup-stale-candidates" );
        std::string cleanupError;
        if( !CleanupStaleCandidates( tracePath.parent_path(), identityPrefix, storageOptions.disk, cleanupError ) )
            throw std::runtime_error( cleanupError );
        const auto currentGeneration = CurrentPublishedGenerationBase(
            result.manifestPath, identityPrefix, result.sourceFingerprint );
        if( currentGeneration )
        {
            resources->SetPhase( "cleanup-unreferenced-generations" );
            std::vector<std::filesystem::path> retained;
            if( !CleanupUnreferencedGenerations( tracePath.parent_path(), identityPrefix,
                    *currentGeneration, storageOptions.disk, retained, cleanupError ) )
            {
                std::string reportError;
                WriteGenerationCleanupReport( cleanupDiagnostic, "failed", cleanupError,
                    retained, resources, reportError );
                throw std::runtime_error( cleanupError );
            }
            std::string reportError;
            if( retained.empty() )
            {
                if( !ClearGenerationCleanupReport( cleanupDiagnostic, resources, reportError ) )
                    throw std::runtime_error( reportError );
            }
            else if( !WriteGenerationCleanupReport( cleanupDiagnostic, "deferred",
                    "active reader or protected generation", retained, resources, reportError ) )
                throw std::runtime_error( reportError );
            for( const auto& path : retained )
                fprintf( stderr, "Query generation retained before build: %s\n", path.string().c_str() );
        }
        resources->SetPhase( "worker-load" );
        resources->SetLiveWorkers( 1 );
        resources->CheckMemory();
        ZoneIndexWriter writer( temporaryExtras, temporaryCpu, temporaryGpu, temporarySamples, temporaryCatalog, temporaryStrings, temporaryCallstacks, temporaryCallstackFrames,
            temporaryJn, result.sourceFingerprint, storageOptions );
        auto source = analysis::WorkerTraceSource::Open( tracePath, std::move( stateCallback ), result.sourceFingerprint,
            analysis::WorkerTraceLoadMode::CompactIndex, &writer, ResourceReadHooks(resources,"worker-load:" ) );
        writer.FinishMissingJnDomains();
        writer.CommitSamples();
        writer.FinishCatalog();
        resources->SetPhase( "sample-view" );
        resources->CheckMemory();
        BuildQuerySampleView( temporarySamples, result.sourceFingerprint,
            [&]( uint64_t thread ) { return source->GetContextSwitchStartTimes( thread ); }, storageOptions );
        result.zoneExtras.count = writer.ExtraCount();
        result.cpuZones.count = writer.CpuCount();
        result.gpuZones.count = writer.GpuCount();
        result.zoneExtras.declaredCount = writer.DeclaredExtraCount();
        result.cpuZones.declaredCount = writer.DeclaredCpuCount();
        result.gpuZones.declaredCount = writer.DeclaredGpuCount();
        result.samples.count = writer.SampleCount();
        result.samples.declaredCount = result.samples.count;
        {
            QuerySampleViewReader sampleView( temporarySamples, result.sourceFingerprint, storageOptions );
            result.sampleMainCount = sampleView.MainCount();
            result.sampleContextSwitchCount = sampleView.ContextSwitchCount();
            if( writer.MainSampleCount() != result.sampleMainCount )
                throw std::runtime_error( "sample logical view main count mismatch" );
        }
        result.samples.bytes = SectionBytes( temporarySamples );
        result.samples.fingerprint = SectionFingerprint( temporarySamples );
        result.globalStrings.count = writer.StringCount();
        result.globalStrings.declaredCount = result.globalStrings.count;
        result.globalStrings.bytes = SectionBytes( temporaryStrings );
        result.globalStrings.fingerprint = SectionFingerprint( temporaryStrings );
        result.globalCallstacks.count = writer.CallstackCount();
        result.globalCallstacks.declaredCount = result.globalCallstacks.count;
        result.globalCallstacks.bytes = SectionBytes( temporaryCallstacks );
        result.globalCallstacks.fingerprint = SectionFingerprint( temporaryCallstacks );
        result.globalCallstackFrames.count = writer.CallstackFrameCount();
        result.globalCallstackFrames.declaredCount = result.globalCallstackFrames.count;
        result.globalCallstackFrames.bytes = SectionBytes( temporaryCallstackFrames );
        result.globalCallstackFrames.fingerprint = SectionFingerprint( temporaryCallstackFrames );
        {
            QueryCatalogStoreReader catalog( temporaryCatalog, result.sourceFingerprint, storageOptions );
            resources->SetPhase( "catalog-validation" );
            result.catalogValidation = catalog.ValidateSemantics( temporaryCatalog / "semantic-validation" );
            resources->SetPhase( "catalog-resource-index" );
            catalog.BuildResourceIndex( temporaryCatalog / "resource-index-scratch" );
            if( !catalog.ResourceIndexReady() ) throw std::runtime_error( "Catalog resource index is not ready" );
            uint64_t count = 0;
            for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
            {
                const auto domainCount = catalog.Count( OfflineCatalogDomain( index ) );
                if( domainCount > std::numeric_limits<uint64_t>::max() - count )
                    throw std::runtime_error( "Catalog sidecar count overflow" );
                count += domainCount;
            }
            result.gpuCatalog.count = count;
            result.gpuCatalog.declaredCount = count;
        }
        result.gpuCatalog.bytes = SectionBytes( temporaryCatalog );
        result.gpuCatalog.fingerprint = SectionFingerprint( temporaryCatalog );
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
            resources->SetPhase("zone-validation:cpu");
            QueryIndexSection temporaryCpuSection = result.cpuZones; temporaryCpuSection.path = temporaryCpu;
            QueryIndexSection temporaryExtraSection = result.zoneExtras; temporaryExtraSection.path = temporaryExtras;
            QueryIndexSection temporaryGpuSection = result.gpuZones; temporaryGpuSection.path = temporaryGpu;
            ReadOnlyZoneSection cpuSection( temporaryCpuSection, ZoneSectionKind::Cpu, sizeof( CpuZoneIndexRecord ), result.sourceFingerprint );
            ReadOnlyZoneSection extraSection( temporaryExtraSection, ZoneSectionKind::Extra, sizeof( ZoneExtraIndexRecord ), result.sourceFingerprint );
            ReadOnlyZoneSection gpuSection( temporaryGpuSection, ZoneSectionKind::Gpu, sizeof( GpuZoneIndexRecord ), result.sourceFingerprint );
            analysis::ZoneValidationSummaryDto zoneValidation;
            std::unordered_map<std::string_view, size_t> validationIssueByCode;
            const auto noteValidation = [&]( const char* severity, const char* code, const char* message, const auto& makeRef ) {
                auto found = validationIssueByCode.find( code );
                if( found == validationIssueByCode.end() )
                {
                    found = validationIssueByCode.emplace( code, zoneValidation.findings.size() ).first;
                    zoneValidation.findings.push_back( { severity, code, message, 0, {} } );
                }
                auto& issue = zoneValidation.findings[found->second];
                issue.count++;
                if( issue.refs.size() < 20 ) issue.refs.emplace_back( makeRef() );
            };
            std::unordered_set<uint64_t> validThreads;
            for( const auto& thread : source->GetThreads() ) validThreads.emplace( thread.nativeId );
            std::unordered_set<uint64_t> validContexts;
            for( const auto& context : source->GetGpuContexts() ) validContexts.emplace( context.index );
            PlayerLoopSourceTable playerLoopSources {};
            bool hasMainPlayerLoopSource = false;
            for( const auto& location : source->GetSourceLocations() )
            {
                const auto& name = location.name.empty() ? location.function : location.name;
                auto& sourceState = playerLoopSources[uint16_t( int16_t( location.nativeId ) )];
                if( name == "Main.PlayerLoop" ) { sourceState |= MainPlayerLoopSource; hasMainPlayerLoopSource = true; }
                else if( name == "PlayerLoop" ) sourceState |= UnityPlayerLoopSource;
            }
            std::array<uint8_t, 1u << 16> validationSourceState {};
            for( const auto& location : source->GetSourceLocations() )
                validationSourceState[uint16_t( int16_t( location.nativeId ) )] = location.name.empty() && location.function.empty() ? 1 : 2;
            enum : uint8_t { GpuMemoryMarkerNone, GpuMemoryMarkerRequest, GpuMemoryMarkerPass, GpuMemoryMarkerOrigin, GpuMemoryMarkerResidency };
            std::array<uint8_t, 1u << 16> gpuMemoryMarkers {};
            for( const auto& location : source->GetSourceLocations() )
            {
                const auto& name = location.name.empty() ? location.function : location.name;
                auto& marker = gpuMemoryMarkers[uint16_t( int16_t( location.nativeId ) )];
                if( name == analysis::GpuMemoryRequestMarker ) marker = GpuMemoryMarkerRequest;
                else if( name == analysis::GpuMemoryPassMarker ) marker = GpuMemoryMarkerPass;
                else if( name == analysis::GpuMemoryOriginMarker ) marker = GpuMemoryMarkerOrigin;
                else if( name == analysis::GpuMemoryResidencyMarker ) marker = GpuMemoryMarkerResidency;
            }
            analysis::PagedRecordWriter gpuMemoryCpuZoneWriter(temporaryGpuMemoryCpuZones,
                PagedSectionIdentity(result.sourceFingerprint,ZoneSectionKind::GpuMemoryCpuZone),sizeof(uint64_t),storageOptions);
            analysis::PagedRecordWriter gpuMemorySummaryCpuZoneWriter(temporaryGpuMemorySummaryCpuZones,
                PagedSectionIdentity(result.sourceFingerprint,ZoneSectionKind::GpuMemorySummaryCpuZone),sizeof(uint64_t),storageOptions);
            uint64_t gpuMemoryCpuZoneCount=0,gpuMemorySummaryCpuZoneCount=0;
            auto validationWriter=std::make_unique<QueryValidationStoreWriter>(temporaryValidation,result.sourceFingerprint,storageOptions);
            resources->SetPhase("zone-validation:extra-callstack-index");
            for(uint64_t index=0;index<extraSection.Count();++index)
            {
                if((index&0xffff)==0) { resources->SetProgress(index,extraSection.Count()); resources->CheckMemory(false); }
                const auto extra=extraSection.At<ZoneExtraIndexRecord>(index);
                validationWriter->AppendCallstack(extra.callstack);
            }
            resources->SetPhase("zone-validation:gpu-callstack-index");
            for(uint64_t index=0;index<gpuSection.Count();++index)
            {
                if((index&0xffff)==0) { resources->SetProgress(index,gpuSection.Count()); resources->CheckMemory(false); }
                validationWriter->AppendCallstack(gpuSection.At<GpuZoneIndexRecord>(index).callstack);
            }
            validationWriter->Commit();
            validationWriter.reset();
            auto validationStore=std::make_unique<QueryValidationStoreReader>(temporaryValidation,result.sourceFingerprint,storageOptions);
            MainPlayerLoopRoots playerLoopRoots;
            analysis::AnalysisWorkspaceReservation playerLoopWorkspace( resources->Workspace(), 65536 );
            uint64_t playerLoopRootCount = 0;
            const auto reportValidationContainers=[&] {
                resources->SetContainer("gpu_memory_cpu_zone_ordinals",gpuMemoryCpuZoneCount,0);
                resources->SetContainer("gpu_memory_summary_cpu_zone_ordinals",gpuMemorySummaryCpuZoneCount,0);
                resources->SetContainer("validation_playerloop_roots",playerLoopRootCount,playerLoopRootCount*64);
            };
            reportValidationContainers();
            uint64_t previousValidationThread = 0;
            bool havePreviousValidationThread = false;
            bool previousValidationThreadIsValid = false;
            for( uint64_t index = 0; index < cpuSection.Count(); index++ )
            {
                if((index&0xffff)==0)
                {
                    resources->SetProgress(index,cpuSection.Count());
                    reportValidationContainers();
                    resources->CheckMemory(false);
                }
                const auto& zone = cpuSection.At<CpuZoneIndexRecord>( index );
                const auto validationRef = [&] { return source->MakeEntityRef( "cpu-zone", index ); };
                if( !( zone.flags & QueryIndexCpuZoneTimingCompleteFlag ) ) noteValidation( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", validationRef );
                else if( zone.end < zone.start ) noteValidation( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", validationRef );
                if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= cpuSection.Count() )
                    noteValidation( "warning", "UNRESOLVED_CPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                if( !havePreviousValidationThread || previousValidationThread != zone.thread )
                {
                    previousValidationThread = zone.thread;
                    havePreviousValidationThread = true;
                    previousValidationThreadIsValid = validThreads.find( zone.thread ) != validThreads.end();
                }
                if( !previousValidationThreadIsValid )
                    noteValidation( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                const auto locationState = validationSourceState[uint16_t( zone.sourceLocation )];
                if( locationState == 0 )
                    noteValidation( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                bool nameResolved = locationState == 2;
                if( zone.flags & QueryIndexCpuZoneNameActive ) nameResolved = QueryIndexCpuZoneNameResolved( zone.flags );
                else if( QueryIndexCpuZoneExtraMissing( zone.flags ) ) nameResolved = false;
                if( !nameResolved )
                    noteValidation( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", validationRef );
                if( hasMainPlayerLoopSource )
                {
                    const auto sourceState = playerLoopSources[uint16_t( zone.sourceLocation )];
                    if( sourceState & UnityPlayerLoopSource )
                        noteValidation( "error", "DUPLICATE_AUTHORITATIVE_PLAYERLOOP_ROOT",
                            "Unity's cross-frame PlayerLoop marker was mirrored beside the authoritative Main.PlayerLoop root",
                            validationRef );
                    if( sourceState & MainPlayerLoopSource )
                    {
                        auto parent = zone.parent;
                        size_t parentDepth = 0;
                        while( parent != std::numeric_limits<uint64_t>::max() &&
                            parent < cpuSection.Count() && parentDepth++ < 4096 )
                        {
                            const auto& parentZone = cpuSection.At<CpuZoneIndexRecord>( parent );
                            if( playerLoopSources[uint16_t( parentZone.sourceLocation )] & MainPlayerLoopSource )
                            {
                                noteValidation( "error", "CPU_FRAME_ROOT_NESTED_SAME_SOURCE",
                                    "Main.PlayerLoop is nested below another Main.PlayerLoop root", validationRef );
                                break;
                            }
                            parent = parentZone.parent;
                        }
                        if( zone.flags & QueryIndexCpuZoneTimingCompleteFlag )
                        {
                            playerLoopRoots[zone.thread].push_back( { index, zone.start, zone.end } );
                            playerLoopRootCount++;
                            if( ( playerLoopRootCount & 0x3ff ) == 0 )
                                playerLoopWorkspace.Resize( 65536 + playerLoopRootCount * 64 +
                                    uint64_t( playerLoopRoots.size() ) * 128 );
                        }
                    }
                }
                const auto marker = gpuMemoryMarkers[uint16_t( zone.sourceLocation )];
                if( marker == GpuMemoryMarkerNone || ( zone.flags & QueryIndexCpuZoneTimingCompleteFlag ) == 0 || zone.extra >= extraSection.Count() ) continue;
                const auto& extra = extraSection.At<ZoneExtraIndexRecord>( zone.extra );
                if( ( extra.flags & QueryIndexZoneExtraTextActive ) == 0 ) continue;
                const auto text = source->ResolveStringIndex( extra.text );
                if( !text ) continue;
                gpuMemoryCpuZoneWriter.Append(&index,1); ++gpuMemoryCpuZoneCount;
                if( marker == GpuMemoryMarkerOrigin || marker == GpuMemoryMarkerResidency )
                { gpuMemorySummaryCpuZoneWriter.Append(&index,1); ++gpuMemorySummaryCpuZoneCount; }
                else if( marker == GpuMemoryMarkerRequest )
                {
                    constexpr std::string_view prefix = "GTMEM1|RESOURCE|allocation=";
                    // Pointer values are reused across resource generations.
                    // Keep every resource metadata record in the compact
                    // summary so an allocation event can resolve the backing
                    // that was active in its own lifetime, not the final
                    // last-writer for that address.
                    if( text->find( prefix ) != std::string::npos )
                    { gpuMemorySummaryCpuZoneWriter.Append(&index,1); ++gpuMemorySummaryCpuZoneCount; }
                }
                if( marker == GpuMemoryMarkerOrigin || marker == GpuMemoryMarkerResidency )
                    result.gpuMemoryProtocol2 = true;
            }
            playerLoopWorkspace.Resize( 65536 + playerLoopRootCount * 64 +
                uint64_t( playerLoopRoots.size() ) * 128 );
            ValidateMainPlayerLoopRootOverlaps( playerLoopRoots,
                [&]( uint64_t index ) { return source->MakeEntityRef( "cpu-zone", index ); }, noteValidation );
            MainPlayerLoopRoots().swap( playerLoopRoots );
            playerLoopRootCount = 0;
            playerLoopWorkspace.Resize( 0 );
            reportValidationContainers();

            resources->SetPhase("zone-validation:gpu");
            for( uint64_t index = 0; index < gpuSection.Count(); index++ )
            {
                if((index&0xffff)==0)
                {
                    resources->SetProgress(index,gpuSection.Count());
                    reportValidationContainers();
                    resources->CheckMemory(false);
                }
                const auto& zone = gpuSection.At<GpuZoneIndexRecord>( index );
                const auto validationRef = [&] { return source->MakeEntityRef( "gpu-zone", index ); };
                if( !( zone.flags & 1 ) ) noteValidation( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", validationRef );
                else if( zone.gpuEnd < zone.gpuStart || zone.cpuEnd < zone.cpuStart )
                    noteValidation( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", validationRef );
                if( zone.parent != std::numeric_limits<uint64_t>::max() && zone.parent >= gpuSection.Count() )
                    noteValidation( "warning", "UNRESOLVED_GPU_ZONE_PARENT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                if( validThreads.find( zone.thread ) == validThreads.end() )
                    noteValidation( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                if( validContexts.find( zone.context ) == validContexts.end() )
                    noteValidation( "warning", "UNRESOLVED_GPU_CONTEXT_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
                if( validationSourceState[uint16_t( zone.sourceLocation )] == 0 )
                    noteValidation( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", validationRef );
            }
            resources->SetPhase("zone-validation:callstacks");
            validationStore->VisitUniqueCallstacks(4096,[&](const std::vector<uint32_t>& callstacks) {
                analysis::AnalysisWorkspaceReservation resolvedWorkspace(resources->Workspace(),65536+uint64_t(callstacks.size())*64);
                std::unordered_set<uint32_t> resolvedCallstacks;
                for(const auto& frame:source->ResolveCallstacks(callstacks,1)) resolvedCallstacks.emplace(frame.callstack);
                for(const auto callstack:callstacks) if(!resolvedCallstacks.contains(callstack))
                    noteValidation("warning","UNRESOLVED_CALLSTACK","persisted zone references a callstack that cannot be resolved",[&] { return source->MakeEntityRef("callstack",callstack); });
                resources->SetContainer("validation_callstack_batch",callstacks.size(),callstacks.capacity());
                resources->SetContainer("validation_resolved_callstacks",resolvedCallstacks.size(),resolvedCallstacks.bucket_count());
                resources->CheckMemory(false);
            });
            zoneValidation.complete = true;
            zoneValidation.scanned = cpuSection.Count() + gpuSection.Count();
            result.zoneValidationPrecomputed = true;
            result.zoneValidation = std::move( zoneValidation );
            reportValidationContainers();
            gpuMemoryCpuZoneWriter.Commit(); gpuMemorySummaryCpuZoneWriter.Commit();
            result.gpuMemoryCpuZones.count = gpuMemoryCpuZoneCount;
            result.gpuMemoryCpuZones.declaredCount = gpuMemoryCpuZoneCount;
            result.gpuMemoryCpuZones.bytes = SectionBytes( temporaryGpuMemoryCpuZones );
            result.gpuMemoryCpuZones.fingerprint = SectionFingerprint( temporaryGpuMemoryCpuZones );
            result.gpuMemorySummaryCpuZones.count = gpuMemorySummaryCpuZoneCount;
            result.gpuMemorySummaryCpuZones.declaredCount = gpuMemorySummaryCpuZoneCount;
            result.gpuMemorySummaryCpuZones.bytes = SectionBytes( temporaryGpuMemorySummaryCpuZones );
            result.gpuMemorySummaryCpuZones.fingerprint = SectionFingerprint( temporaryGpuMemorySummaryCpuZones );
            validationStore.reset();
            std::error_code validationCleanup;
            analysis::AnalysisDiskRemove(temporaryValidation,storageOptions.disk,validationCleanup,true);
            if(validationCleanup) throw std::runtime_error("cannot clean validation scratch: "+validationCleanup.message());
        }
        result.zoneExtras.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryExtras );
        result.cpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryCpu );
        result.gpuZones.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpu );
        result.cpuZoneIndex = true;
        result.gpuZoneIndex = true;

        resources->SetPhase( "compact-write" );
        resources->CheckMemory();
        source->WriteCompactSnapshot( temporaryData, resources->CreateFileWriteHooks( temporaryData ) );
        result.dataBytes = std::filesystem::file_size( temporaryData );
        result.dataFingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryData );
        source.reset();
        writer.ReleaseExternalSources();
        resources->SetLiveWorkers( 0 );
        {
            resources->SetPhase( "gpu-memory-summary-deferred" );
            resources->BeforeFileGrowth( temporaryGpuMemorySummary, 0 );
            std::ofstream output( temporaryGpuMemorySummary, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "cannot create GPU-memory summary index" );
            output.flush();
            if( !output ) throw std::runtime_error( "cannot flush GPU-memory summary index" );
            result.gpuMemorySummary.count = 0; result.gpuMemorySummary.declaredCount = 0;
            result.gpuMemorySummary.bytes = std::filesystem::file_size( temporaryGpuMemorySummary );
            result.gpuMemorySummary.fingerprint = analysis::WorkerTraceSource::ComputeFingerprint( temporaryGpuMemorySummary );
        }

        // Mutable build extents are closed before compression. Publication
        // names are generation-specific, so existing readers retain their
        // immutable files until their own analysis is released.
        const auto packSection=[&](std::filesystem::path& temporary,QueryIndexSection& section,ZoneSectionKind kind) {
            const auto raw=temporary;
            auto packed=temporary; packed+=".pages";
            if(std::filesystem::exists(packed)) throw std::runtime_error("index packed scratch already exists");
            packedScratch.push_back(packed);
            std::ifstream input(raw,std::ios::binary);
            ZoneSectionHeader header{}; input.read(reinterpret_cast<char*>(&header),sizeof(header));
            if(!input || std::memcmp(header.magic,"JNIDXZ1",7) || header.count!=section.count ||
                header.recordBytes!=section.recordBytes || header.kind!=uint32_t(kind))
                throw std::runtime_error("index compression input mismatch");
            auto options=storageOptions;
            const auto identity=PagedSectionIdentity(result.sourceFingerprint,kind);
            analysis::PagedRecordWriter output(packed,identity,section.recordBytes,options);
            const uint64_t perBlock=options.blockBytes/section.recordBytes;
            std::vector<char> buffer(size_t(perBlock*section.recordBytes));
            for(uint64_t ordinal=0;ordinal<section.count;) {
                const auto count=std::min(perBlock,section.count-ordinal),bytes=count*section.recordBytes;
                input.read(buffer.data(),std::streamsize(bytes));
                if(!input) throw std::runtime_error("index compression input truncated");
                output.Append(buffer.data(),count); ordinal+=count;
            }
            output.Commit();
            analysis::PagedRecordReader check(packed,identity,section.recordBytes,options); check.ValidateAll();
            input.close();
            // Only this invocation's known raw scratch file is reclaimed.
            std::error_code removeError;
            analysis::AnalysisDiskRemove(raw,storageOptions.disk,removeError);
            if(removeError) throw std::runtime_error("cannot remove packed raw index section: "+removeError.message());
            temporary=packed; section.bytes=SectionBytes(packed); section.fingerprint=SectionFingerprint(packed);
        };
        packSection(temporaryExtras,result.zoneExtras,ZoneSectionKind::Extra);
        packSection(temporaryCpu,result.cpuZones,ZoneSectionKind::Cpu);
        packSection(temporaryGpu,result.gpuZones,ZoneSectionKind::Gpu);
        for(size_t index=0;index<temporaryJn.size();++index)
            packSection(temporaryJn[index],*jnSections[index],ZoneIndexWriter::JnSectionKind(SerializedZoneSink::JnDomain(index)));

        {
            const auto payload = ManifestJson( result ).dump( 2 );
            resources->BeforeFileGrowth( temporaryManifest, payload.size() );
            std::ofstream output( temporaryManifest, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "cannot create index manifest" );
            output.write( payload.data(), std::streamsize( payload.size() ) );
            output.flush();
            if( !output ) throw std::runtime_error( "cannot flush index manifest" );
        }

        resources->SetPhase( "publish" );
        resources->CheckMemory();
        resources->CheckFinalDisk();

        std::string error;
        if( !AtomicReplace( temporaryExtras, result.zoneExtras.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit zone-extra index: " + error );
        publishedCandidates.emplace_back( result.zoneExtras.path );
        if( !AtomicReplace( temporaryCpu, result.cpuZones.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit CPU-zone index: " + error );
        publishedCandidates.emplace_back( result.cpuZones.path );
        if( !AtomicReplace( temporaryGpu, result.gpuZones.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit GPU-zone index: " + error );
        publishedCandidates.emplace_back( result.gpuZones.path );
        if( !AtomicReplace( temporarySamples, result.samples.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit sample index: " + error );
        publishedCandidates.emplace_back( result.samples.path );
        if( !AtomicReplace( temporaryStrings, result.globalStrings.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit global string index: " + error );
        publishedCandidates.emplace_back( result.globalStrings.path );
        if( !AtomicReplace( temporaryCallstacks, result.globalCallstacks.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit global callstack index: " + error );
        publishedCandidates.emplace_back( result.globalCallstacks.path );
        if( !AtomicReplace( temporaryCallstackFrames, result.globalCallstackFrames.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit global callstack frame index: " + error );
        publishedCandidates.emplace_back( result.globalCallstackFrames.path );
        if( !AtomicReplace( temporaryCatalog, result.gpuCatalog.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit GPU Catalog index: " + error );
        publishedCandidates.emplace_back( result.gpuCatalog.path );
        for( size_t index = 0; index < temporaryJn.size(); index++ )
        {
            if( !AtomicReplace( temporaryJn[index], jnSections[index]->path, error, storageOptions.cancelled ) )
                throw std::runtime_error( "cannot commit JN domain index " + temporaryJn[index].string() +
                    " -> " + jnSections[index]->path.string() + ": " + error );
            publishedCandidates.emplace_back( jnSections[index]->path );
        }
        if( !AtomicReplace( temporaryGpuMemoryCpuZones, result.gpuMemoryCpuZones.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit GPU-memory CPU-zone index: " + error );
        publishedCandidates.emplace_back( result.gpuMemoryCpuZones.path );
        if( !AtomicReplace( temporaryGpuMemorySummaryCpuZones, result.gpuMemorySummaryCpuZones.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit GPU-memory summary CPU-zone index: " + error );
        publishedCandidates.emplace_back( result.gpuMemorySummaryCpuZones.path );
        if( !AtomicReplace( temporaryGpuMemorySummary, result.gpuMemorySummary.path, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit GPU-memory summary index: " + error );
        publishedCandidates.emplace_back( result.gpuMemorySummary.path );
        if( !AtomicReplace( temporaryData, result.dataPath, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit index data: " + error );
        publishedCandidates.emplace_back( result.dataPath );
        if( !AtomicReplace( temporaryManifest, result.manifestPath, error, storageOptions.cancelled ) ) throw std::runtime_error( "cannot commit index manifest: " + error );
        publishedCandidates.emplace_back( result.manifestPath );
        // Publication is already durable.  Cleanup is best-effort from this
        // point so a deferred Windows handle or a diagnostic allocation cannot
        // roll back files referenced by the newly visible manifest.
        try
        {
            std::vector<std::filesystem::path> retained;
            std::string cleanupError;
            const bool cleanupComplete = CleanupUnreferencedGenerations( tracePath.parent_path(),
                identityPrefix, baseName, storageOptions.disk, retained, cleanupError );
            std::string reportError;
            if( !cleanupComplete )
            {
                if( !WriteGenerationCleanupReport( cleanupDiagnostic, "failed", cleanupError,
                        retained, resources, reportError ) )
                    fprintf( stderr, "%s\n", reportError.c_str() );
                fprintf( stderr, "Query generation cleanup deferred: %s\n", cleanupError.c_str() );
            }
            else if( !retained.empty() )
            {
                if( !WriteGenerationCleanupReport( cleanupDiagnostic, "deferred",
                        "active reader or protected generation", retained, resources, reportError ) )
                    fprintf( stderr, "%s\n", reportError.c_str() );
            }
            else if( !ClearGenerationCleanupReport( cleanupDiagnostic, resources, reportError ) )
                fprintf( stderr, "%s\n", reportError.c_str() );
            for( const auto& path : retained )
                fprintf( stderr, "Query generation retained: %s\n", path.string().c_str() );
        }
        catch( const std::exception& cleanupFailure )
        {
            std::vector<std::filesystem::path> retained;
            std::string reportError;
            WriteGenerationCleanupReport( cleanupDiagnostic, "failed", cleanupFailure.what(),
                retained, resources, reportError );
            fprintf( stderr, "Query generation cleanup deferred: %s\n", cleanupFailure.what() );
        }
        return result;
    }
    catch( ... )
    {
        const auto failure=std::current_exception();
        resources->SetLiveWorkers( 0 );
        std::error_code ignored;
        for( auto candidate = publishedCandidates.rbegin(); candidate != publishedCandidates.rend(); ++candidate )
        {
            ignored.clear();
            const auto parent = std::filesystem::absolute( tracePath ).parent_path().lexically_normal();
            const auto target = std::filesystem::absolute( *candidate ).lexically_normal();
            if( target.parent_path() != parent || !target.filename().string().starts_with( baseName ) ) continue;
            if( std::filesystem::is_symlink( std::filesystem::symlink_status( target, ignored ) ) ) continue;
#ifdef _WIN32
            const auto attributes = GetFileAttributesW( analysis::AnalysisIoPath( target ).c_str() );
            if( attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_REPARSE_POINT ) ) continue;
#endif
            analysis::AnalysisDiskRemove( target, storageOptions.disk, ignored, true );
        }
        // Directory cleanup is limited to this build's explicitly recorded
        // direct children; never recurse through a link or another generation.
        for(const auto& packed:packedScratch) {
            const auto parent=std::filesystem::absolute(tracePath).parent_path().lexically_normal();
            const auto target=std::filesystem::absolute(packed).lexically_normal();
            if(target.parent_path()!=parent || !target.filename().string().starts_with(baseName)) continue;
            if(std::filesystem::is_symlink(std::filesystem::symlink_status(target,ignored))) continue;
#ifdef _WIN32
            const auto attributes=GetFileAttributesW(analysis::AnalysisIoPath(target).c_str());
            if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
            analysis::AnalysisDiskRemove(target,storageOptions.disk,ignored,true);
        }
        analysis::AnalysisDiskRemove( temporaryData, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryManifest, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryExtras, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryCpu, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryGpu, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporarySamples, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryStrings, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryCallstacks, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryCallstackFrames, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryCatalog, storageOptions.disk, ignored, true );
        for( const auto& path : temporaryJn ) analysis::AnalysisDiskRemove( path, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryGpuMemoryCpuZones, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryGpuMemorySummaryCpuZones, storageOptions.disk, ignored, true );
        analysis::AnalysisDiskRemove( temporaryGpuMemorySummary, storageOptions.disk, ignored );
        analysis::AnalysisDiskRemove( temporaryValidation, storageOptions.disk, ignored, true );
        try { resources->RunResourceBoundary([&] { std::rethrow_exception(failure); }); }
        catch(...)
        {
            try { resources->WriteDiagnostic(failureDiagnostic); } catch(...) {}
            throw;
        }
        std::rethrow_exception(failure);
    }
}

QueryIndexValidation QueryIndex::Validate( const std::filesystem::path& inputTracePath, bool deep,
    std::shared_ptr<QueryResourceContext> resources )
{
    const auto tracePath = analysis::AnalysisIoPath( inputTracePath );
    QueryIndexValidation result;
    try
    {
        if(!resources) resources=CreateResourceContext(tracePath);
        resources->SetPhase(deep?"index-validate:deep":"index-validate:identity");
        resources->CheckMemory();
        const auto storageOptions=ResourceStorageOptions(resources);
        QueryIndexManifest manifest;
        manifest.manifestPath = ManifestPath( tracePath );
        if( !std::filesystem::is_regular_file( manifest.manifestPath ) )
        {
            result.reason = "missing";
            return result;
        }

        const auto manifestBytes=std::filesystem::file_size(manifest.manifestPath);
        analysis::AnalysisWorkspaceReservation manifestWorkspace(resources->Workspace(),65536+16*manifestBytes);
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
        resources->ValidateTraceIdentity(manifest.sourceBytes,manifest.sourceFingerprint);
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
        std::string leaseError;
        manifest.readerLease = AcquireManifestReaderLease( manifest, resources, leaseError );
        if( !manifest.readerLease ) throw std::runtime_error( leaseError );
        manifest.cpuZoneIndex = parsed.at( "domains" ).value( "cpu_zone_index", false );
        manifest.gpuZoneIndex = parsed.at( "domains" ).value( "gpu_zone_index", false );
        manifest.gpuMemoryProtocol2 = parsed.at( "domains" ).at( "gpu_memory_protocol2" ).get<bool>();
        const auto& counts = parsed.at( "counts" );
        for( const auto& [thread, count] : counts.at( "cpu_zones_by_thread" ).items() )
            manifest.cpuZonesByThread.emplace( std::stoull( thread ), std::stoull( count.get<std::string>() ) );
        for( const auto& [context, count] : counts.at( "gpu_zones_by_context" ).items() )
            manifest.gpuZonesByContext.emplace( uint32_t( std::stoul( context ) ), std::stoull( count.get<std::string>() ) );
        manifest.sampleMainCount = ParseUnsigned( counts, "samples" );
        manifest.sampleContextSwitchCount = ParseUnsigned( counts, "context_switch_samples" );
        const auto& zoneValidation = parsed.at( "zone_validation" );
        manifest.zoneValidationPrecomputed = zoneValidation.value( "precomputed", false );
        manifest.zoneValidation.complete = zoneValidation.value( "complete", false );
        if( !manifest.zoneValidationPrecomputed || !manifest.zoneValidation.complete )
        {
            result.reason = "zone_validation_not_ready";
            return result;
        }
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
        const auto& catalogValidation = parsed.at( "catalog_validation" );
        manifest.catalogValidation.complete = catalogValidation.value( "complete", false );
        manifest.catalogValidation.present = catalogValidation.value( "present", false );
        manifest.catalogValidation.originalValid = catalogValidation.value( "original_valid", false );
        manifest.catalogValidation.healthy = catalogValidation.value( "healthy", false );
        if( !manifest.catalogValidation.complete )
        {
            result.reason = "catalog_validation_not_ready";
            return result;
        }
        for( const auto& finding : catalogValidation.at( "findings" ) )
            manifest.catalogValidation.findings.push_back( {
                finding.at( "code" ).get<std::string>(), ParseUnsigned( finding, "count" ) } );
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
        parseSection( "sample", manifest.samples );
        parseSection( "global_string", manifest.globalStrings );
        parseSection( "global_callstack", manifest.globalCallstacks );
        parseSection( "global_callstack_frame", manifest.globalCallstackFrames );
        parseSection( "gpu_catalog", manifest.gpuCatalog );
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
        const auto validateSection = [&]( const QueryIndexSection& section, uint32_t expectedRecordBytes, const char* name,
            ZoneSectionKind kind=ZoneSectionKind::Extra ) {
            if(std::filesystem::is_directory(section.path) && expectedRecordBytes) {
                if(section.recordBytes!=expectedRecordBytes || SectionBytes(section.path)!=section.bytes)
                    throw std::runtime_error(std::string(name)+" paged section identity mismatch");
                if(deep && SectionFingerprint(section.path)!=section.fingerprint)
                    throw std::runtime_error(std::string(name)+" paged section hash mismatch");
                analysis::PagedRecordReader records(section.path,PagedSectionIdentity(manifest.sourceFingerprint,kind),expectedRecordBytes,storageOptions);
                if(records.Count()!=section.count) throw std::runtime_error(std::string(name)+" paged count mismatch");
                if(deep) records.ValidateAll();
                return;
            }
            if( section.recordBytes != expectedRecordBytes || !std::filesystem::is_regular_file( section.path ) || std::filesystem::file_size( section.path ) != section.bytes )
                throw std::runtime_error( std::string( name ) + " section identity mismatch" );
            if( deep && analysis::WorkerTraceSource::ComputeFingerprint( section.path ) != section.fingerprint )
                throw std::runtime_error( std::string( name ) + " section hash mismatch" );
        };
        validateSection( manifest.zoneExtras, sizeof( ZoneExtraIndexRecord ), "zone-extra" );
        validateSection( manifest.cpuZones, sizeof( CpuZoneIndexRecord ), "CPU-zone",ZoneSectionKind::Cpu );
        validateSection( manifest.gpuZones, sizeof( GpuZoneIndexRecord ), "GPU-zone",ZoneSectionKind::Gpu );
        if( manifest.samples.recordBytes != QuerySampleStoreRecordBytes ||
            !std::filesystem::is_directory( manifest.samples.path ) ||
            SectionBytes( manifest.samples.path ) != manifest.samples.bytes ||
            manifest.samples.count < manifest.sampleMainCount )
            throw std::runtime_error( "sample section identity mismatch" );
        if( deep && SectionFingerprint( manifest.samples.path ) != manifest.samples.fingerprint )
            throw std::runtime_error( "sample section hash mismatch" );
        {
            QuerySampleStoreReader rawSamples( manifest.samples.path, manifest.sourceFingerprint, storageOptions );
            QuerySampleViewReader samples( manifest.samples.path, manifest.sourceFingerprint, storageOptions );
            if( rawSamples.Count() != manifest.samples.count || samples.MainCount() != manifest.sampleMainCount ||
                samples.ContextSwitchCount() != manifest.sampleContextSwitchCount )
                throw std::runtime_error( "sample section count mismatch" );
            if( deep ) samples.ValidateAll();
        }
        if( manifest.globalStrings.recordBytes != 0 || !std::filesystem::is_directory( manifest.globalStrings.path ) ||
            SectionBytes( manifest.globalStrings.path ) != manifest.globalStrings.bytes )
            throw std::runtime_error( "global string section identity mismatch" );
        if( deep && SectionFingerprint( manifest.globalStrings.path ) != manifest.globalStrings.fingerprint )
            throw std::runtime_error( "global string section hash mismatch" );
        {
            QueryStringStoreReader strings( manifest.globalStrings.path, manifest.sourceFingerprint, storageOptions );
            if( strings.StringCount() != manifest.globalStrings.count ||
                manifest.globalStrings.declaredCount != manifest.globalStrings.count )
                throw std::runtime_error( "global string section count mismatch" );
            if( deep ) strings.ValidateAll();
        }
        if( manifest.globalCallstacks.recordBytes != 0 || !std::filesystem::is_directory( manifest.globalCallstacks.path ) ||
            SectionBytes( manifest.globalCallstacks.path ) != manifest.globalCallstacks.bytes )
            throw std::runtime_error( "global callstack section identity mismatch" );
        if( deep && SectionFingerprint( manifest.globalCallstacks.path ) != manifest.globalCallstacks.fingerprint )
            throw std::runtime_error( "global callstack section hash mismatch" );
        {
            QueryCallstackStoreReader callstacks( manifest.globalCallstacks.path, manifest.sourceFingerprint, storageOptions );
            if( callstacks.Count() != manifest.globalCallstacks.count ||
                manifest.globalCallstacks.declaredCount != manifest.globalCallstacks.count )
                throw std::runtime_error( "global callstack section count mismatch" );
            if( deep ) callstacks.ValidateAll();
        }
        if( manifest.globalCallstackFrames.recordBytes != 0 || !std::filesystem::is_directory( manifest.globalCallstackFrames.path ) ||
            SectionBytes( manifest.globalCallstackFrames.path ) != manifest.globalCallstackFrames.bytes )
            throw std::runtime_error( "global callstack frame section identity mismatch" );
        if( deep && SectionFingerprint( manifest.globalCallstackFrames.path ) != manifest.globalCallstackFrames.fingerprint )
            throw std::runtime_error( "global callstack frame section hash mismatch" );
        {
            QueryCallstackFrameStoreReader frames( manifest.globalCallstackFrames.path, manifest.sourceFingerprint, storageOptions );
            if( frames.Count() != manifest.globalCallstackFrames.count ||
                manifest.globalCallstackFrames.declaredCount != manifest.globalCallstackFrames.count )
                throw std::runtime_error( "global callstack frame section count mismatch" );
            if( deep ) frames.ValidateAll();
        }
        if( manifest.gpuCatalog.recordBytes != 0 || !std::filesystem::is_directory( manifest.gpuCatalog.path ) ||
            SectionBytes( manifest.gpuCatalog.path ) != manifest.gpuCatalog.bytes )
            throw std::runtime_error( "GPU Catalog section identity mismatch" );
        if( deep && SectionFingerprint( manifest.gpuCatalog.path ) != manifest.gpuCatalog.fingerprint )
            throw std::runtime_error( "GPU Catalog section hash mismatch" );
        {
            QueryCatalogStoreReader catalog( manifest.gpuCatalog.path, manifest.sourceFingerprint, storageOptions );
            if( !catalog.ResourceIndexReady() ) throw std::runtime_error( "GPU Catalog resource index is not ready" );
            uint64_t count = 0;
            for( size_t index = 0; index < OfflineCatalogDomainCount; index++ )
            {
                const auto domainCount = catalog.Count( OfflineCatalogDomain( index ) );
                if( domainCount > std::numeric_limits<uint64_t>::max() - count )
                    throw std::runtime_error( "GPU Catalog section count overflow" );
                count += domainCount;
            }
            if( count != manifest.gpuCatalog.count || manifest.gpuCatalog.declaredCount != count )
                throw std::runtime_error( "GPU Catalog section count mismatch" );
            if( deep )
            {
                catalog.ValidateAll();
                auto semanticScratch = manifest.gpuCatalog.path;
                semanticScratch += ".semantic-validation." + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count() );
                resources->IncludeDiskRoots( { semanticScratch } );
                const auto semantic = catalog.ValidateSemantics( semanticScratch );
                bool same = semantic.complete == manifest.catalogValidation.complete &&
                    semantic.present == manifest.catalogValidation.present &&
                    semantic.originalValid == manifest.catalogValidation.originalValid &&
                    semantic.healthy == manifest.catalogValidation.healthy &&
                    semantic.findings.size() == manifest.catalogValidation.findings.size();
                if( same )
                {
                    for( size_t index = 0; index < semantic.findings.size(); index++ )
                    {
                        if( semantic.findings[index].code != manifest.catalogValidation.findings[index].code ||
                            semantic.findings[index].count != manifest.catalogValidation.findings[index].count )
                        { same = false; break; }
                    }
                }
                if( !same ) throw std::runtime_error( "GPU Catalog semantic validation mismatch" );
            }
        }
        validateSection( manifest.jobStages, sizeof( JnJobStageData ), "job-stage",ZoneSectionKind::JobStage );
        validateSection( manifest.gfxEntities, sizeof( JnGfxEntityData ), "gfx-entity",ZoneSectionKind::GfxEntity );
        validateSection( manifest.gfxLinks, sizeof( JnGfxLinkData ), "gfx-link",ZoneSectionKind::GfxLink );
        validateSection( manifest.relations, sizeof( JnRelationData ), "relation",ZoneSectionKind::Relation );
        validateSection( manifest.gpuReferencePasses, sizeof( JnGpuReferencePassData ), "GPU-reference-pass",ZoneSectionKind::GpuReferencePass );
        validateSection( manifest.gpuReferenceUses, sizeof( JnGpuReferenceUseData ), "GPU-reference-use",ZoneSectionKind::GpuReferenceUse );
        validateSection( manifest.gpuReferenceEnds, sizeof( JnGpuReferenceEndData ), "GPU-reference-end",ZoneSectionKind::GpuReferenceEnd );
        validateSection( manifest.gpuMemoryCpuZones, sizeof( uint64_t ), "GPU-memory-CPU-zone",ZoneSectionKind::GpuMemoryCpuZone );
        validateSection( manifest.gpuMemorySummaryCpuZones, sizeof( uint64_t ), "GPU-memory-summary-CPU-zone",ZoneSectionKind::GpuMemorySummaryCpuZone );
        validateSection( manifest.gpuMemorySummary, 0, "GPU-memory-summary" );
        resources->CheckMemory();
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
    analysis::WorkerTraceSource::StateCallback stateCallback, std::string fingerprintOverride,
    std::shared_ptr<QueryResourceContext> resources )
{
    auto leasedManifest = manifest;
    if(!resources)
    {
        auto tracePath=manifest.manifestPath;
        tracePath.replace_extension();
        resources=CreateResourceContext(tracePath);
    }
    if( !leasedManifest.readerLease )
    {
        std::string leaseError;
        leasedManifest.readerLease = AcquireManifestReaderLease( leasedManifest, resources, leaseError );
        if( !leasedManifest.readerLease ) throw std::runtime_error( leaseError );
    }
    resources->ValidateTraceIdentity(leasedManifest.sourceBytes,leasedManifest.sourceFingerprint);
    resources->SetPhase("compact-open");
    resources->SetLiveWorkers(1);
    resources->CheckMemory();
    const auto storageOptions=ResourceStorageOptions(resources);
    auto stringStore=std::make_shared<QueryStringStoreReader>(leasedManifest.globalStrings.path,
        leasedManifest.sourceFingerprint,storageOptions);
    auto stringSource=std::make_unique<QuerySerializedStringSource>(std::move(stringStore));
    auto callstackStore=std::make_shared<QueryCallstackStoreReader>(leasedManifest.globalCallstacks.path,
        leasedManifest.sourceFingerprint,storageOptions);
    auto callstackSource=std::make_unique<QuerySerializedCallstackSource>(std::move(callstackStore));
    auto callstackFrames=std::make_shared<QueryCallstackFrameStoreReader>(leasedManifest.globalCallstackFrames.path,
        leasedManifest.sourceFingerprint,storageOptions);
    callstackSource->SetFrames(std::move(callstackFrames));
    auto source = analysis::WorkerTraceSource::Open( leasedManifest.dataPath, std::move( stateCallback ),
        fingerprintOverride.empty() ? leasedManifest.sourceFingerprint : std::move( fingerprintOverride ),
        analysis::WorkerTraceLoadMode::IndexedSidecar, nullptr, ResourceReadHooks(resources,"compact-load:" ),
        stringSource.get(),callstackSource.get() );
    auto result=std::make_unique<IndexedTraceSource>(std::move(leasedManifest),std::move(stringSource),std::move(callstackSource),std::move(source),resources);
    resources->SetPhase("ready");
    resources->CheckMemory();
    return result;
}

std::shared_ptr<QueryResourceContext> QueryIndex::CreateResourceContext(const std::filesystem::path& inputTracePath,
    QueryResourceContextOptions options)
{
    const auto tracePath=analysis::AnalysisIoPath(inputTracePath);
    const auto actualBytes=std::filesystem::file_size(tracePath);
    const auto limits=analysis::AnalysisDiskBudget::ForTrace(actualBytes);
    if((options.expectedTraceBytes && options.expectedTraceBytes!=actualBytes) ||
        (options.expectedFinalDiskLimit && options.expectedFinalDiskLimit!=limits.finalBytes) ||
        (options.expectedPeakDiskLimit && options.expectedPeakDiskLimit!=limits.peakBytes))
        throw std::runtime_error("query_resource_profile_identity_mismatch");
    options.traceBytes=actualBytes;
    options.derivedRoots=ExistingDerivedPaths(tracePath);
    if(!options.diagnosticMemoryReserveBytes) options.diagnosticMemoryReserveBytes=64*1024;
    options.diagnosticDiskReserveBytes=std::min<uint64_t>(options.diagnosticDiskReserveBytes,options.traceBytes);
    return std::make_shared<QueryResourceContext>(std::move(options));
}

}
