#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"
#include "../../stream/src/TracyStreamReplay.hpp"
#include "../../stream/src/TracyStreamSnapshotMap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#  include <Psapi.h>
#  include <bcrypt.h>
#endif

namespace
{

constexpr auto ReplayProgressTimeout = std::chrono::seconds( 120 );

struct Options
{
    enum class Mode
    {
        Offline,
        Legacy
    };

    enum class Compression
    {
        Fast,
        Balanced,
        Legacy
    };

    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path progressJson;
    std::filesystem::path reportJson;
    uint16_t port = 18086;
    uint32_t threads = 0;
    uint64_t diskBudget = 0;
    bool overwrite = false;
    bool requireCleanEnd = false;
    bool noCache = false;
    bool purgeCache = false;
    bool keepFailedOutput = false;
    bool diagnostics = false;
    uint64_t testCancelAfterRecords = 0;
    bool testFailBeforeValidate = false;
    Mode mode = Mode::Offline;
    Compression compression = Compression::Fast;
};

void Usage()
{
    std::fprintf( stderr,
        "Usage: tracy-stream-convert -i input.tracy-stream -o output.tracy [-f]\n"
        "  [--compression fast|balanced|legacy] [--threads auto|N]\n"
        "  [--require-clean-end] [--progress-json path] [--report-json path]\n"
        "  [--no-cache] [--purge-cache] [--keep-failed-output] [--disk-budget bytes]\n"
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
        "  Development only: [--mode offline|legacy] [-p legacy-port]\n"
        "                    [--diagnostics] [--test-cancel-after-records N] [--test-fail-before-validate]\n"
#endif
    );
}

bool ParsePort( const char* text, uint16_t& port )
{
    char* end = nullptr;
    const auto value = std::strtol( text, &end, 10 );
    if( !end || *end != '\0' || value < 1 || value > 65535 ) return false;
    port = uint16_t( value );
    return true;
}

bool ParseUnsigned( const char* text, uint64_t& value );
uint32_t ResolveThreadCount( uint32_t requested );

bool ParseArguments( int argc, char** argv, Options& options )
{
    for( int i = 1; i < argc; i++ )
    {
        const std::string_view argument = argv[i];
        if( argument == "-i" && i + 1 < argc )
        {
            options.input = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "-o" && i + 1 < argc )
        {
            options.output = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "-p" && i + 1 < argc )
        {
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
            if( !ParsePort( argv[++i], options.port ) ) return false;
#else
            return false;
#endif
        }
        else if( argument == "-f" )
        {
            options.overwrite = true;
        }
        else if( argument == "--mode" && i + 1 < argc )
        {
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
            const std::string_view mode = argv[++i];
            if( mode == "offline" ) options.mode = Options::Mode::Offline;
            else if( mode == "legacy" ) options.mode = Options::Mode::Legacy;
            else return false;
#else
            return false;
#endif
        }
        else if( argument == "--diagnostics" )
        {
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
            options.diagnostics = true;
#else
            return false;
#endif
        }
        else if( argument == "--test-cancel-after-records" && i + 1 < argc )
        {
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
            if( !ParseUnsigned( argv[++i], options.testCancelAfterRecords ) || options.testCancelAfterRecords == 0 ) return false;
#else
            return false;
#endif
        }
        else if( argument == "--test-fail-before-validate" )
        {
#ifdef JN_STREAM_CONVERT_DEV_TOOLS
            options.testFailBeforeValidate = true;
#else
            return false;
#endif
        }
        else if( argument == "--compression" && i + 1 < argc )
        {
            const std::string_view compression = argv[++i];
            if( compression == "fast" ) options.compression = Options::Compression::Fast;
            else if( compression == "balanced" ) options.compression = Options::Compression::Balanced;
            else if( compression == "legacy" ) options.compression = Options::Compression::Legacy;
            else return false;
        }
        else if( argument == "--threads" && i + 1 < argc )
        {
            const std::string_view threads = argv[++i];
            if( threads == "auto" ) options.threads = 0;
            else
            {
                uint64_t value = 0;
                if( !ParseUnsigned( argv[i], value ) || value < 1 || value > 255 ) return false;
                options.threads = uint32_t( value );
            }
        }
        else if( argument == "--require-clean-end" )
        {
            options.requireCleanEnd = true;
        }
        else if( argument == "--progress-json" && i + 1 < argc )
        {
            options.progressJson = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "--report-json" && i + 1 < argc )
        {
            options.reportJson = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "--no-cache" )
        {
            options.noCache = true;
        }
        else if( argument == "--purge-cache" )
        {
            options.purgeCache = true;
        }
        else if( argument == "--keep-failed-output" )
        {
            options.keepFailedOutput = true;
        }
        else if( argument == "--disk-budget" && i + 1 < argc )
        {
            if( !ParseUnsigned( argv[++i], options.diskBudget ) || options.diskBudget == 0 ) return false;
        }
        else
        {
            return false;
        }
    }
    return !options.input.empty() && !options.output.empty();
}

std::atomic<uint32_t> s_cancelRequests { 0 };

#ifdef _WIN32
BOOL WINAPI ConversionControlHandler( DWORD type )
{
    if( type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT ) return FALSE;
    return s_cancelRequests.fetch_add( 1, std::memory_order_relaxed ) == 0 ? TRUE : FALSE;
}
#endif

bool CancelRequested()
{
    return s_cancelRequests.load( std::memory_order_relaxed ) != 0;
}

const char* CompressionName( Options::Compression compression )
{
    switch( compression )
    {
    case Options::Compression::Fast: return "fast";
    case Options::Compression::Balanced: return "balanced";
    case Options::Compression::Legacy: return "legacy";
    }
    return "unknown";
}

struct CompressionSettings
{
    int level;
    uint32_t streams;
};

CompressionSettings GetCompressionSettings( Options::Compression compression, uint32_t threads )
{
    switch( compression )
    {
    case Options::Compression::Fast: return { 1, threads };
    case Options::Compression::Balanced: return { 2, threads };
    case Options::Compression::Legacy: return { 3, std::min<uint32_t>( 4, threads ) };
    }
    return { 1, threads };
}

std::string JsonEscape( std::string_view text )
{
    std::string output;
    output.reserve( text.size() + 16 );
    for( const unsigned char value : text )
    {
        switch( value )
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if( value < 0x20 )
            {
                char buffer[7];
                std::snprintf( buffer, sizeof( buffer ), "\\u%04x", unsigned( value ) );
                output += buffer;
            }
            else output += char( value );
            break;
        }
    }
    return output;
}

bool AtomicWriteText( const std::filesystem::path& path, const std::string& text )
{
    if( path.empty() ) return true;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output ) return false;
        output.write( text.data(), std::streamsize( text.size() ) );
        output.flush();
        if( !output ) return false;
    }
#ifdef _WIN32
    if( MoveFileExW( temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
#else
    std::error_code error;
    std::filesystem::rename( temporary, path, error );
    if( !error ) return true;
#endif
    std::error_code ignored;
    std::filesystem::remove( temporary, ignored );
    return false;
}

struct ProcessMemoryState
{
    uint64_t workingSet = 0;
    uint64_t commit = 0;
    uint64_t physicalAvailable = 0;
    uint64_t physicalTotal = 0;
    uint32_t commitLoadPercent = 0;
};

ProcessMemoryState GetProcessMemoryState()
{
    ProcessMemoryState state;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof( counters );
    if( GetProcessMemoryInfo( GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &counters ), sizeof( counters ) ) )
    {
        state.workingSet = counters.WorkingSetSize;
        state.commit = counters.PrivateUsage;
    }
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof( memory );
    if( GlobalMemoryStatusEx( &memory ) )
    {
        state.physicalAvailable = memory.ullAvailPhys;
        state.physicalTotal = memory.ullTotalPhys;
        state.commitLoadPercent = memory.dwMemoryLoad;
    }
#endif
    return state;
}

enum class ProgressStage : uint8_t
{
    Scan,
    DecodeBuild,
    Write,
    Validate,
    Publish,
    Complete,
    Failed,
    Cancelled
};

const char* ProgressStageName( ProgressStage stage )
{
    switch( stage )
    {
    case ProgressStage::Scan: return "Scan";
    case ProgressStage::DecodeBuild: return "DecodeBuild";
    case ProgressStage::Write: return "Write";
    case ProgressStage::Validate: return "Validate";
    case ProgressStage::Publish: return "Publish";
    case ProgressStage::Complete: return "Complete";
    case ProgressStage::Failed: return "Failed";
    case ProgressStage::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

class ProgressReporter
{
public:
    ProgressReporter( std::filesystem::path jsonPath, uint32_t threads )
        : m_jsonPath( std::move( jsonPath ) )
        , m_threads( threads )
        , m_started( std::chrono::steady_clock::now() )
        , m_stageStarted( m_started )
        , m_thread( [this] { Run(); } )
    {
    }

    ~ProgressReporter()
    {
        Stop();
    }

    void SetStage( ProgressStage stage, uint64_t total = 0, const char* unit = "items" )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_stage = stage;
        m_completed = 0;
        m_total = total;
        m_unit = unit;
        m_stageStarted = std::chrono::steady_clock::now();
        m_signal.notify_all();
    }

    void Update( uint64_t completed, uint64_t total = 0 )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_completed = completed;
        if( total != 0 ) m_total = total;
    }

    void SetTemporaryPath( std::filesystem::path path )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_temporaryPath = std::move( path );
    }

    bool PressureExceeded() const { return m_pressureExceeded.load( std::memory_order_relaxed ); }
    uint64_t PeakCommitBytes() const { return m_peakCommit.load( std::memory_order_relaxed ); }
    uint64_t PeakWorkingSetBytes() const { return m_peakWorkingSet.load( std::memory_order_relaxed ); }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock( m_lock );
            if( m_stop ) return;
            m_stop = true;
            m_signal.notify_all();
        }
        if( m_thread.joinable() ) m_thread.join();
        Emit();
    }

private:
    void Run()
    {
        std::unique_lock<std::mutex> lock( m_lock );
        while( !m_stop )
        {
            m_signal.wait_for( lock, std::chrono::seconds( 2 ) );
            if( m_stop ) break;
            lock.unlock();
            Emit();
            lock.lock();
        }
    }

    void Emit()
    {
        ProgressStage stage;
        uint64_t completed;
        uint64_t total;
        std::string unit;
        std::filesystem::path temporaryPath;
        std::chrono::steady_clock::time_point stageStarted;
        {
            std::lock_guard<std::mutex> lock( m_lock );
            stage = m_stage;
            completed = m_completed;
            total = m_total;
            unit = m_unit;
            temporaryPath = m_temporaryPath;
            stageStarted = m_stageStarted;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>( now - m_started ).count();
        const auto stageElapsed = std::max( 0.001, std::chrono::duration<double>( now - stageStarted ).count() );
        const auto percent = total == 0 ? -1.0 : std::min( 100.0, 100.0 * double( completed ) / double( total ) );
        const auto rate = double( completed ) / stageElapsed;
        const auto eta = total != 0 && completed != 0 && completed < total ?
            double( total - completed ) / std::max( rate, 0.001 ) : -1.0;
        uint64_t temporarySize = 0;
        std::error_code sizeError;
        if( !temporaryPath.empty() )
        {
            const auto size = std::filesystem::file_size( temporaryPath, sizeError );
            if( !sizeError ) temporarySize = size;
        }
        const auto memory = GetProcessMemoryState();
        auto peakCommit = m_peakCommit.load( std::memory_order_relaxed );
        while( memory.commit > peakCommit && !m_peakCommit.compare_exchange_weak( peakCommit, memory.commit, std::memory_order_relaxed ) ) {}
        auto peakWorkingSet = m_peakWorkingSet.load( std::memory_order_relaxed );
        while( memory.workingSet > peakWorkingSet &&
            !m_peakWorkingSet.compare_exchange_weak( peakWorkingSet, memory.workingSet, std::memory_order_relaxed ) ) {}
        if( memory.physicalTotal != 0 )
        {
            const auto minimumAvailable = std::max<uint64_t>( 8ull * 1024 * 1024 * 1024, memory.physicalTotal * 15 / 100 );
            if( memory.commit > memory.physicalTotal * 60 / 100 || memory.physicalAvailable < minimumAvailable ||
                memory.commitLoadPercent >= 85 )
                m_pressureExceeded.store( true, std::memory_order_relaxed );
        }

        if( percent >= 0 )
            std::printf( "[%s] %.1f%% %llu/%llu %s, %.1f/s, elapsed %.1fs, ETA %.1fs, memory %.2f GiB, temp %.2f MiB\n",
                ProgressStageName( stage ), percent, static_cast<unsigned long long>( completed ),
                static_cast<unsigned long long>( total ), unit.c_str(), rate, elapsed, std::max( 0.0, eta ),
                double( memory.commit ) / double( 1ull << 30 ), double( temporarySize ) / double( 1ull << 20 ) );
        else
            std::printf( "[%s] estimating, elapsed %.1fs, memory %.2f GiB, temp %.2f MiB\n",
                ProgressStageName( stage ), elapsed, double( memory.commit ) / double( 1ull << 30 ),
                double( temporarySize ) / double( 1ull << 20 ) );
        std::fflush( stdout );

        if( !m_jsonPath.empty() )
        {
            std::ostringstream json;
            json << "{\"schema\":1,\"stage\":\"" << ProgressStageName( stage ) << "\",\"completed\":\""
                << completed << "\",\"total\":\"" << total << "\",\"unit\":\"" << JsonEscape( unit )
                << "\",\"percent\":" << percent << ",\"elapsed_seconds\":" << elapsed
                << ",\"eta_seconds\":" << eta << ",\"rate\":" << rate << ",\"threads\":" << m_threads
                << ",\"working_set_bytes\":\"" << memory.workingSet << "\",\"commit_bytes\":\"" << memory.commit
                << "\",\"system_available_bytes\":\"" << memory.physicalAvailable
                << "\",\"temporary_output_bytes\":\"" << temporarySize << "\",\"cancel_requested\":"
                << ( CancelRequested() ? "true" : "false" ) << "}";
            AtomicWriteText( m_jsonPath, json.str() );
        }
    }

    std::filesystem::path m_jsonPath;
    const uint32_t m_threads;
    const std::chrono::steady_clock::time_point m_started;
    std::mutex m_lock;
    std::condition_variable m_signal;
    ProgressStage m_stage = ProgressStage::Scan;
    uint64_t m_completed = 0;
    uint64_t m_total = 0;
    std::string m_unit = "bytes";
    std::filesystem::path m_temporaryPath;
    std::chrono::steady_clock::time_point m_stageStarted;
    bool m_stop = false;
    std::atomic<bool> m_pressureExceeded { false };
    std::atomic<uint64_t> m_peakCommit { 0 };
    std::atomic<uint64_t> m_peakWorkingSet { 0 };
    std::thread m_thread;
};

class TemporaryOutputGuard
{
public:
    TemporaryOutputGuard( std::filesystem::path path, bool keep ) : m_path( std::move( path ) ), m_keep( keep ) {}
    ~TemporaryOutputGuard()
    {
        if( m_published || m_keep ) return;
        std::error_code ignored;
        std::filesystem::remove( m_path, ignored );
    }
    void Published() { m_published = true; }

private:
    std::filesystem::path m_path;
    bool m_keep;
    bool m_published = false;
};

struct InputIdentity
{
    std::array<uint8_t, 32> sha256 {};
    std::string hex;
};

bool ComputeInputIdentity( const std::filesystem::path& path, uint64_t fileSize, InputIdentity& identity,
    ProgressReporter& progress, std::string& error )
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object;
    bool success = false;
    do
    {
        if( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
        {
            error = "BCryptOpenAlgorithmProvider(SHA-256) failed";
            break;
        }
        DWORD objectSize = 0;
        DWORD resultSize = 0;
        if( BCryptGetProperty( algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>( &objectSize ),
            sizeof( objectSize ), &resultSize, 0 ) != 0 || objectSize == 0 )
        {
            error = "BCrypt SHA-256 object-size query failed";
            break;
        }
        object.resize( objectSize );
        if( BCryptCreateHash( algorithm, &hash, object.data(), DWORD( object.size() ), nullptr, 0, 0 ) != 0 )
        {
            error = "BCryptCreateHash failed";
            break;
        }
        std::ifstream input( path, std::ios::binary );
        if( !input )
        {
            error = "cannot open input for SHA-256 identity";
            break;
        }
        std::vector<uint8_t> buffer( 4 * 1024 * 1024 );
        uint64_t readBytes = 0;
        while( input )
        {
            input.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( buffer.size() ) );
            const auto count = size_t( input.gcount() );
            if( count == 0 ) break;
            if( BCryptHashData( hash, buffer.data(), ULONG( count ), 0 ) != 0 )
            {
                error = "BCryptHashData failed";
                break;
            }
            readBytes += count;
            progress.Update( readBytes, fileSize );
            if( CancelRequested() )
            {
                error = "conversion cancelled";
                break;
            }
        }
        if( !error.empty() ) break;
        if( readBytes != fileSize )
        {
            error = "input size changed while computing SHA-256 identity";
            break;
        }
        if( BCryptFinishHash( hash, identity.sha256.data(), ULONG( identity.sha256.size() ), 0 ) != 0 )
        {
            error = "BCryptFinishHash failed";
            break;
        }
        static constexpr char Hex[] = "0123456789abcdef";
        identity.hex.resize( identity.sha256.size() * 2 );
        for( size_t index = 0; index < identity.sha256.size(); index++ )
        {
            identity.hex[index * 2] = Hex[identity.sha256[index] >> 4];
            identity.hex[index * 2 + 1] = Hex[identity.sha256[index] & 0xF];
        }
        success = true;
    }
    while( false );
    if( hash ) BCryptDestroyHash( hash );
    if( algorithm ) BCryptCloseAlgorithmProvider( algorithm, 0 );
    return success;
#else
    (void)path;
    (void)fileSize;
    (void)identity;
    (void)progress;
    error = "scan cache SHA-256 identity is currently implemented for Windows only";
    return false;
#endif
}

struct ScanCacheHeader
{
    char magic[8] = { 'J', 'N', 'S', 'C', 'A', 'N', '1', 0 };
    uint32_t schema = 1;
    uint32_t protocol = tracy::ProtocolVersion;
    uint64_t inputSize = 0;
    std::array<uint8_t, 32> inputSha256 {};
    tracy::stream::FileHeader journalHeader {};
    uint32_t scanCode = 0;
    uint64_t fileSize = 0;
    uint64_t validSize = 0;
    uint64_t recordCount = 0;
    uint64_t lastSequence = 0;
    uint64_t lastMonotonicNs = 0;
    uint32_t prefixCrc32c = 0;
    uint8_t complete = 0;
    uint64_t collectedRecordCount = 0;
    uint32_t recordsCrc32c = 0;
};

std::filesystem::path GetScanCacheDirectory( const std::filesystem::path& input, uint64_t fileSize,
    const InputIdentity& identity )
{
    return input.parent_path() / ".jnconvert-cache" /
        ( "v1-" + identity.hex.substr( 0, 32 ) + "-" + std::to_string( fileSize ) );
}

bool LoadScanCache( const std::filesystem::path& cacheFile, uint64_t inputSize, const InputIdentity& identity,
    tracy::stream::ScanResult& scan, std::string& error )
{
    std::ifstream input( cacheFile, std::ios::binary );
    if( !input ) return false;
    ScanCacheHeader header;
    input.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    if( !input || std::string_view( header.magic, 7 ) != "JNSCAN1" || header.schema != 1 ||
        header.protocol != tracy::ProtocolVersion || header.inputSize != inputSize || header.inputSha256 != identity.sha256 ||
        header.collectedRecordCount > 2'000'000 )
    {
        error = "scan cache header or input identity mismatch";
        return false;
    }
    std::vector<tracy::stream::RecordInfo> records( size_t( header.collectedRecordCount ) );
    if( !records.empty() )
        input.read( reinterpret_cast<char*>( records.data() ), std::streamsize( records.size() * sizeof( records.front() ) ) );
    if( !input )
    {
        error = "scan cache record table is truncated";
        return false;
    }
    const auto recordBytes = std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( records.data() ),
        records.size() * sizeof( tracy::stream::RecordInfo ) );
    if( tracy::stream::Crc32c( recordBytes ) != header.recordsCrc32c )
    {
        error = "scan cache record checksum mismatch";
        return false;
    }
    uint64_t previousSequence = 0;
    for( const auto& record : records )
    {
        const auto remaining = record.offset <= header.validSize ? header.validSize - record.offset : 0;
        if( record.sequence <= previousSequence || record.offset < tracy::stream::FileHeaderSize ||
            record.payloadSize > tracy::stream::DefaultMaxPayloadSize ||
            record.offset > header.validSize || remaining < tracy::stream::RecordHeaderSize + tracy::stream::RecordTrailerSize ||
            record.payloadSize > remaining - tracy::stream::RecordHeaderSize - tracy::stream::RecordTrailerSize )
        {
            error = "scan cache contains an invalid record range or sequence";
            return false;
        }
        previousSequence = record.sequence;
    }
    scan.code = tracy::stream::ScanCode( header.scanCode );
    scan.header = header.journalHeader;
    scan.fileSize = header.fileSize;
    scan.validSize = header.validSize;
    scan.recordCount = header.recordCount;
    scan.lastSequence = header.lastSequence;
    scan.lastMonotonicNs = header.lastMonotonicNs;
    scan.prefixCrc32c = header.prefixCrc32c;
    scan.complete = header.complete != 0;
    scan.message = "loaded from validated scan cache";
    scan.records = std::move( records );
    return true;
}

bool SaveScanCache( const std::filesystem::path& cacheFile, uint64_t inputSize, const InputIdentity& identity,
    const tracy::stream::ScanResult& scan, std::string& error )
{
    std::error_code filesystemError;
    std::filesystem::create_directories( cacheFile.parent_path(), filesystemError );
    if( filesystemError )
    {
        error = filesystemError.message();
        return false;
    }
    ScanCacheHeader header;
    header.inputSize = inputSize;
    header.inputSha256 = identity.sha256;
    header.journalHeader = scan.header;
    header.scanCode = uint32_t( scan.code );
    header.fileSize = scan.fileSize;
    header.validSize = scan.validSize;
    header.recordCount = scan.recordCount;
    header.lastSequence = scan.lastSequence;
    header.lastMonotonicNs = scan.lastMonotonicNs;
    header.prefixCrc32c = scan.prefixCrc32c;
    header.complete = scan.complete ? 1 : 0;
    header.collectedRecordCount = scan.records.size();
    const auto recordBytes = std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( scan.records.data() ),
        scan.records.size() * sizeof( tracy::stream::RecordInfo ) );
    header.recordsCrc32c = tracy::stream::Crc32c( recordBytes );
    auto temporary = cacheFile;
    temporary += ".tmp";
    {
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output )
        {
            error = "cannot create scan cache";
            return false;
        }
        output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        if( !scan.records.empty() )
            output.write( reinterpret_cast<const char*>( scan.records.data() ),
                std::streamsize( scan.records.size() * sizeof( scan.records.front() ) ) );
        output.flush();
        if( !output )
        {
            error = "cannot write scan cache";
            return false;
        }
    }
#ifdef _WIN32
    if( !MoveFileExW( temporary.c_str(), cacheFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
    {
        error = "cannot atomically publish scan cache";
        std::filesystem::remove( temporary, filesystemError );
        return false;
    }
#else
    std::filesystem::rename( temporary, cacheFile, filesystemError );
    if( filesystemError )
    {
        error = filesystemError.message();
        return false;
    }
#endif
    return true;
}

class PayloadReader
{
public:
    explicit PayloadReader( const std::filesystem::path& path )
        : m_file( path, std::ios::binary )
    {
    }

    bool IsOpen() const { return bool( m_file ); }

    bool Read( const tracy::stream::RecordInfo& record, std::vector<uint8_t>& payload, std::string& error )
    {
        if( record.payloadSize > uint64_t( std::numeric_limits<size_t>::max() ) )
        {
            error = "record payload does not fit in memory";
            return false;
        }
        const auto offset = record.offset + tracy::stream::RecordHeaderSize;
        if( offset > uint64_t( std::numeric_limits<std::streamoff>::max() ) )
        {
            error = "record offset exceeds stream API";
            return false;
        }
        payload.resize( size_t( record.payloadSize ) );
        m_file.clear();
        m_file.seekg( std::streamoff( offset ), std::ios::beg );
        if( !m_file )
        {
            error = "cannot seek to journal payload";
            return false;
        }
        if( !payload.empty() )
        {
            m_file.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) );
            if( !m_file || m_file.gcount() != std::streamsize( payload.size() ) )
            {
                error = "cannot read journal payload";
                return false;
            }
        }
        return true;
    }

private:
    std::ifstream m_file;
};

struct SocketDeleter
{
    void operator()( tracy::Socket* socket ) const
    {
        if( !socket ) return;
        socket->~Socket();
        tracy::tracy_free( socket );
    }
};

class ReplayError
{
public:
    void Set( std::string message )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        if( m_message.empty() ) m_message = std::move( message );
        m_failed.store( true, std::memory_order_relaxed );
    }

    bool Failed() const { return m_failed.load( std::memory_order_relaxed ); }

    std::string Message() const
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_message;
    }

private:
    mutable std::mutex m_lock;
    std::atomic<bool> m_failed { false };
    std::string m_message;
};

// A one-record bounded client mailbox plus an in-process verifier for the
// Worker's server stream.  It preserves the byte stream and causal ordering
// used by legacy socket replay, but no kernel socket, listener, port or
// verifier thread is involved.
class OfflineReplayTransport final : public tracy::WorkerOfflineTransport
{
public:
    OfflineReplayTransport(
        const std::filesystem::path& path,
        const std::vector<tracy::stream::RecordInfo>& serverRecords,
        bool complete,
        ReplayError& replayError,
        std::atomic<uint64_t>& replayedServerSequence )
        : m_reader( path )
        , m_serverRecords( serverRecords )
        , m_complete( complete )
        , m_replayError( replayError )
        , m_replayedServerSequence( replayedServerSequence )
    {
        if( !m_reader.IsOpen() ) FailLocked( "cannot open journal for in-process server verification" );
    }

    bool Connect() override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_valid;
    }

    bool Read( void* data, int size, int timeoutMs, const std::atomic<bool>& shutdown ) override
    {
        if( size < 0 ) return false;
        auto output = static_cast<uint8_t*>( data );
        size_t remaining = size_t( size );
        std::unique_lock<std::mutex> lock( m_lock );
        while( remaining != 0 )
        {
            while( m_clientOffset == m_clientPayload.size() && !m_clientClosed && m_valid &&
                !shutdown.load( std::memory_order_relaxed ) )
            {
                if( timeoutMs > 0 ) m_cv.wait_for( lock, std::chrono::milliseconds( timeoutMs ) );
                else m_cv.wait( lock );
            }
            if( !m_valid || shutdown.load( std::memory_order_relaxed ) ) return false;
            if( m_clientOffset == m_clientPayload.size() )
            {
                if( m_clientClosed ) return false;
                continue;
            }
            const auto available = m_clientPayload.size() - m_clientOffset;
            const auto amount = std::min( available, remaining );
            memcpy( output, m_clientPayload.data() + m_clientOffset, amount );
            output += amount;
            remaining -= amount;
            m_clientOffset += amount;
            if( m_clientOffset == m_clientPayload.size() )
            {
                m_clientPayload.clear();
                m_clientOffset = 0;
                m_cv.notify_all();
            }
        }
        return true;
    }

    int Send( const void* data, int size ) override
    {
        if( size < 0 ) return -1;
        std::lock_guard<std::mutex> lock( m_lock );
        if( !m_valid ) return -1;
        const auto bytes = static_cast<const uint8_t*>( data );
        m_serverPending.insert( m_serverPending.end(), bytes, bytes + size );
        if( !ConsumeServerLocked() ) return -1;
        return size;
    }

    int GetSendBufferSize() const override
    {
        return int( ( 8 * 1024 + 4 ) * tracy::ServerQueryPacketSize );
    }

    void Close() override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_valid = false;
        m_clientClosed = true;
        m_cv.notify_all();
    }

    bool IsValid() const override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_valid;
    }

    bool PublishClient( std::vector<uint8_t>& payload )
    {
        std::unique_lock<std::mutex> lock( m_lock );
        m_cv.wait( lock, [this] { return m_clientPayload.empty() || !m_valid; } );
        if( !m_valid ) return false;
        m_clientPayload = std::move( payload );
        m_clientOffset = 0;
        m_cv.notify_all();
        return true;
    }

    void CloseClient()
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_clientClosed = true;
        m_cv.notify_all();
    }

    bool Finish( std::string& error )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        if( !m_error.empty() )
        {
            error = m_error;
            return false;
        }
        if( !ConsumeServerLocked() )
        {
            error = m_error;
            return false;
        }
        if( m_serverIndex != m_serverRecords.size() )
        {
            error = "server transcript ended early (consumed=" + std::to_string( m_serverIndex ) +
                ", recorded=" + std::to_string( m_serverRecords.size() ) + ")";
            return false;
        }
        if( m_serverPendingOffset != m_serverPending.size() && m_complete )
        {
            error = "Worker emitted " + std::to_string( m_serverPending.size() - m_serverPendingOffset ) +
                " bytes beyond the complete recorded server transcript";
            return false;
        }
        if( !m_verifier.Finish( error ) ) return false;
        return true;
    }

private:
    bool ConsumeServerLocked()
    {
        std::string error;
        while( m_serverIndex < m_serverRecords.size() )
        {
            const auto& record = m_serverRecords[m_serverIndex];
            if( m_serverPending.size() - m_serverPendingOffset < record.payloadSize ) break;
            tracy::stream::ReplayServerPacket recorded { record.sequence, record.flags, {} };
            if( !m_reader.Read( record, recorded.payload, error ) )
            {
                FailLocked( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return false;
            }
            tracy::stream::ReplayServerPacket replayed { record.sequence, record.flags, {} };
            replayed.payload.assign( m_serverPending.begin() + m_serverPendingOffset,
                m_serverPending.begin() + m_serverPendingOffset + size_t( record.payloadSize ) );
            if( !m_verifier.Append( recorded, replayed, error ) )
            {
                FailLocked( error );
                return false;
            }
            m_serverPendingOffset += size_t( record.payloadSize );
            if( m_serverPendingOffset >= 1024 * 1024 && m_serverPendingOffset * 2 >= m_serverPending.size() )
            {
                m_serverPending.erase( m_serverPending.begin(), m_serverPending.begin() + m_serverPendingOffset );
                m_serverPendingOffset = 0;
            }
            m_serverIndex++;
            m_replayedServerSequence.store( record.sequence, std::memory_order_release );
            m_cv.notify_all();
        }
        if( m_serverIndex == m_serverRecords.size() && m_serverPendingOffset != m_serverPending.size() && m_complete )
        {
            FailLocked( "Worker emitted bytes beyond the complete recorded server transcript" );
            return false;
        }
        return true;
    }

    void FailLocked( std::string message )
    {
        if( m_error.empty() ) m_error = message;
        m_replayError.Set( message );
        m_valid = false;
        m_clientClosed = true;
        m_cv.notify_all();
    }

    PayloadReader m_reader;
    const std::vector<tracy::stream::RecordInfo>& m_serverRecords;
    const bool m_complete;
    ReplayError& m_replayError;
    std::atomic<uint64_t>& m_replayedServerSequence;
    mutable std::mutex m_lock;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_clientPayload;
    size_t m_clientOffset = 0;
    bool m_clientClosed = false;
    bool m_valid = true;
    std::vector<uint8_t> m_serverPending;
    size_t m_serverPendingOffset = 0;
    size_t m_serverIndex = 0;
    tracy::stream::ReplayServerTranscriptVerifier m_verifier;
    std::string m_error;
};

bool SamePath( const std::filesystem::path& left, const std::filesystem::path& right )
{
    std::error_code ec;
    if( std::filesystem::exists( left, ec ) && !ec && std::filesystem::exists( right, ec ) && !ec )
    {
        if( std::filesystem::equivalent( left, right, ec ) && !ec ) return true;
    }
    ec.clear();
    const auto absoluteLeft = std::filesystem::absolute( left, ec ).lexically_normal();
    if( ec ) return false;
    const auto absoluteRight = std::filesystem::absolute( right, ec ).lexically_normal();
    if( ec ) return false;
#ifdef _WIN32
    auto leftText = absoluteLeft.wstring();
    auto rightText = absoluteRight.wstring();
    std::transform( leftText.begin(), leftText.end(), leftText.begin(), []( wchar_t value ) { return wchar_t( std::towlower( value ) ); } );
    std::transform( rightText.begin(), rightText.end(), rightText.begin(), []( wchar_t value ) { return wchar_t( std::towlower( value ) ); } );
    return leftText == rightText;
#else
    return absoluteLeft == absoluteRight;
#endif
}

bool ParseUnsigned( const char* text, uint64_t& value )
{
    if( !text || *text == '\0' || *text == '-' ) return false;
    char* end = nullptr;
    const auto parsed = std::strtoull( text, &end, 10 );
    if( !end || *end != '\0' ) return false;
    value = parsed;
    return true;
}

uint32_t ResolveThreadCount( uint32_t requested )
{
    if( requested != 0 ) return std::clamp<uint32_t>( requested, 1, 255 );
    const auto hardware = std::thread::hardware_concurrency();
    return std::clamp<uint32_t>( hardware > 1 ? hardware - 1 : 1, 1, 255 );
}

double ElapsedSeconds( std::chrono::steady_clock::time_point begin )
{
    return std::chrono::duration<double>( std::chrono::steady_clock::now() - begin ).count();
}

bool PublishSnapshotAtomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& output,
    bool overwrite,
    std::string& error )
{
#ifdef _WIN32
    std::error_code filesystemError;
    const bool exists = std::filesystem::exists( output, filesystemError ) && !filesystemError;
    if( exists )
    {
        if( !overwrite )
        {
            error = "output already exists";
            return false;
        }
        if( ReplaceFileW( output.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr ) ) return true;
        error = "ReplaceFileW failed with error " + std::to_string( GetLastError() );
        return false;
    }
    if( MoveFileExW( temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "MoveFileExW failed with error " + std::to_string( GetLastError() );
    return false;
#else
    std::error_code filesystemError;
    if( std::filesystem::exists( output, filesystemError ) && !filesystemError )
    {
        if( !overwrite )
        {
            error = "output already exists";
            return false;
        }
        std::filesystem::remove( output, filesystemError );
        if( filesystemError )
        {
            error = "cannot remove previous output: " + filesystemError.message();
            return false;
        }
    }
    std::filesystem::rename( temporary, output, filesystemError );
    if( !filesystemError ) return true;
    error = "cannot publish output: " + filesystemError.message();
    return false;
#endif
}

const char* QueueTypeName( tracy::QueueType type )
{
    switch( type )
    {
    case tracy::QueueType::ZoneValidation: return "ZoneValidation";
    case tracy::QueueType::ContextSwitch: return "ContextSwitch";
    case tracy::QueueType::ThreadWakeup: return "ThreadWakeup";
    case tracy::QueueType::ZoneBegin: return "ZoneBegin";
    case tracy::QueueType::ZoneBeginCallstack: return "ZoneBeginCallstack";
    case tracy::QueueType::ZoneEnd: return "ZoneEnd";
    case tracy::QueueType::ZoneText: return "ZoneText";
    case tracy::QueueType::ZoneName: return "ZoneName";
    case tracy::QueueType::Callstack: return "Callstack";
    case tracy::QueueType::CallstackSerial: return "CallstackSerial";
    case tracy::QueueType::CallstackSample: return "CallstackSample";
    case tracy::QueueType::CallstackSampleRef: return "CallstackSampleRef";
    case tracy::QueueType::CallstackPayload: return "CallstackPayload";
    case tracy::QueueType::GpuTime: return "GpuTime";
    case tracy::QueueType::GpuZoneBeginSerial: return "GpuZoneBeginSerial";
    case tracy::QueueType::GpuZoneBeginCallstackSerial: return "GpuZoneBeginCallstackSerial";
    case tracy::QueueType::GpuZoneEndSerial: return "GpuZoneEndSerial";
    case tracy::QueueType::JnJobSchedule: return "JnJobSchedule";
    case tracy::QueueType::JnJobConfig: return "JnJobConfig";
    case tracy::QueueType::JnJobDependency: return "JnJobDependency";
    case tracy::QueueType::JnJobStage: return "JnJobStage";
    case tracy::QueueType::JnGfxEntity: return "JnGfxEntity";
    case tracy::QueueType::JnGfxLink: return "JnGfxLink";
    case tracy::QueueType::JnRelation: return "JnRelation";
    case tracy::QueueType::JnGpuReferencePass: return "JnGpuReferencePass";
    case tracy::QueueType::JnGpuReferenceSetUse: return "JnGpuReferenceSetUse";
    case tracy::QueueType::JnGpuReferenceEnd: return "JnGpuReferenceEnd";
    case tracy::QueueType::JnGpuCatalogControl: return "JnGpuCatalogControl";
    case tracy::QueueType::JnGpuCatalogBatch: return "JnGpuCatalogBatch";
    case tracy::QueueType::SingleStringData: return "SingleStringData";
    case tracy::QueueType::SecondStringData: return "SecondStringData";
    case tracy::QueueType::MemNamePayload: return "MemNamePayload";
    case tracy::QueueType::JnGpuReferenceSetDefinition: return "JnGpuReferenceSetDefinition";
    default: return "Other";
    }
}

void EnableLocalReplayOnly()
{
#ifdef _WIN32
    _putenv_s( "TRACY_ONLY_LOCALHOST", "1" );
    _putenv_s( "TRACY_ONLY_IPV4", "1" );
#else
    setenv( "TRACY_ONLY_LOCALHOST", "1", 1 );
    setenv( "TRACY_ONLY_IPV4", "1", 1 );
#endif
}

}

int main( int argc, char** argv )
{
    const auto totalStart = std::chrono::steady_clock::now();
    Options options;
    if( !ParseArguments( argc, argv, options ) )
    {
        Usage();
        return 1;
    }
    options.threads = ResolveThreadCount( options.threads );
#ifdef _WIN32
    SetConsoleCtrlHandler( ConversionControlHandler, TRUE );
#endif
    ProgressReporter progress( options.progressJson, options.threads );
    if( SamePath( options.input, options.output ) )
    {
        std::fprintf( stderr, "Input journal and output snapshot must use different paths.\n" );
        return 1;
    }

    if( options.purgeCache )
    {
        const auto cachePath = options.input.parent_path() / ".jnconvert-cache";
        if( cachePath.filename() != ".jnconvert-cache" )
        {
            std::fprintf( stderr, "Refusing to purge an unexpected cache path.\n" );
            return 1;
        }
        std::error_code purgeError;
        std::filesystem::remove_all( cachePath, purgeError );
        if( purgeError )
        {
            std::fprintf( stderr, "Cannot purge conversion cache: %s.\n", purgeError.message().c_str() );
            return 1;
        }
    }

    std::error_code inputSizeError;
    const auto inputFileSize = std::filesystem::file_size( options.input, inputSizeError );
    if( inputSizeError )
    {
        std::fprintf( stderr, "Cannot determine input size: %s.\n", inputSizeError.message().c_str() );
        return 1;
    }
    progress.SetStage( ProgressStage::Scan, inputFileSize, "bytes" );

    tracy::stream::ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 2'000'000;
    const auto scanStart = std::chrono::steady_clock::now();
    tracy::stream::ScanResult scan;
    InputIdentity inputIdentity;
    bool inputIdentityAvailable = false;
    bool scanCacheUsed = false;
    std::filesystem::path scanCacheDirectory;
    if( !options.noCache )
    {
        std::string identityError;
        inputIdentityAvailable = ComputeInputIdentity( options.input, inputFileSize, inputIdentity, progress, identityError );
        if( !inputIdentityAvailable )
        {
            if( CancelRequested() )
            {
                progress.SetStage( ProgressStage::Cancelled );
                return 130;
            }
            std::fprintf( stderr, "Warning: scan cache disabled because input identity failed: %s.\n", identityError.c_str() );
        }
        else
        {
            scanCacheDirectory = GetScanCacheDirectory( options.input, inputFileSize, inputIdentity );
            const auto scanCacheFile = scanCacheDirectory / "scan.cache";
            std::string cacheError;
            scanCacheUsed = LoadScanCache( scanCacheFile, inputFileSize, inputIdentity, scan, cacheError );
            if( !scanCacheUsed && std::filesystem::exists( scanCacheFile ) )
            {
                std::fprintf( stderr, "Warning: ignoring invalid scan cache: %s.\n", cacheError.c_str() );
                std::error_code ignored;
                std::filesystem::remove( scanCacheFile, ignored );
            }
        }
    }
    if( !scanCacheUsed )
    {
        progress.SetStage( ProgressStage::Scan, 0, "journal_bytes" );
        scan = tracy::stream::ScanJournal( options.input, scanOptions );
        if( inputIdentityAvailable && scan.HasRecoverablePrefix() )
        {
            std::string cacheError;
            if( !SaveScanCache( scanCacheDirectory / "scan.cache", inputFileSize, inputIdentity, scan, cacheError ) )
                std::fprintf( stderr, "Warning: scan cache could not be saved: %s.\n", cacheError.c_str() );
        }
    }
    const auto scanSeconds = ElapsedSeconds( scanStart );
    progress.Update( scan.validSize, inputFileSize );
    if( CancelRequested() )
    {
        progress.SetStage( ProgressStage::Cancelled );
        return 130;
    }
    if( !scan.HasRecoverablePrefix() )
    {
        std::fprintf( stderr, "Journal cannot be replayed: %s (%s)\n", scan.message.c_str(), tracy::stream::ScanCodeName( scan.code ) );
        return 2;
    }
    if( scan.records.size() != scan.recordCount )
    {
        std::fprintf( stderr, "Journal has too many records for this converter (%llu > %zu).\n", static_cast<unsigned long long>( scan.recordCount ), scan.records.size() );
        return 2;
    }
    if( scan.header.protocolVersion != tracy::ProtocolVersion )
    {
        std::fprintf( stderr, "Journal protocol version %u does not match this converter (%u).\n", scan.header.protocolVersion, tracy::ProtocolVersion );
        return 2;
    }
    if( scan.code != tracy::stream::ScanCode::Ok )
    {
        std::fprintf( stderr, "Warning: replaying valid prefix ending at byte %llu; ignored tail status is %s.\n",
            static_cast<unsigned long long>( scan.validSize ), tracy::stream::ScanCodeName( scan.code ) );
    }
    if( options.requireCleanEnd && !scan.complete )
    {
        std::fprintf( stderr, "Journal does not have a clean committed end and --require-clean-end was specified.\n" );
        return 2;
    }

    std::error_code filesystemError;
    if( std::filesystem::exists( options.output, filesystemError ) && !filesystemError && !options.overwrite )
    {
        std::fprintf( stderr, "Output snapshot already exists; use -f to overwrite it.\n" );
        return 3;
    }

    const auto outputDirectory = options.output.has_parent_path() ? options.output.parent_path() : std::filesystem::current_path();
    const uint64_t safetyMargin = 2ull * 1024 * 1024 * 1024;
    const uint64_t estimatedOutput = std::max<uint64_t>( scan.validSize, 512ull * 1024 * 1024 );
    const uint64_t requiredDisk = estimatedOutput > std::numeric_limits<uint64_t>::max() - safetyMargin ?
        std::numeric_limits<uint64_t>::max() : estimatedOutput + safetyMargin;
    if( options.diskBudget != 0 && requiredDisk > options.diskBudget )
    {
        std::fprintf( stderr, "Estimated conversion space %llu exceeds --disk-budget %llu.\n",
            static_cast<unsigned long long>( requiredDisk ), static_cast<unsigned long long>( options.diskBudget ) );
        return 3;
    }
    const auto diskSpace = std::filesystem::space( outputDirectory, filesystemError );
    if( filesystemError || diskSpace.available < requiredDisk )
    {
        std::fprintf( stderr, "Insufficient output disk space: need %llu bytes including safety margin, available %llu.\n",
            static_cast<unsigned long long>( requiredDisk ), static_cast<unsigned long long>( diskSpace.available ) );
        return 3;
    }

    progress.SetStage( ProgressStage::DecodeBuild, scan.recordCount, "records" );
    const auto analysisStart = std::chrono::steady_clock::now();
    std::vector<tracy::stream::RecordInfo> clientRecords;
    std::vector<tracy::stream::RecordInfo> serverRecords;
    bool hasLocalDisconnect = false;
    bool hasSessionBegin = false;
    bool deferSymbolExpansion = false;
    tracy::stream::RecordInfo sessionBeginRecord;
    uint64_t drainControlSequence = 0;
    tracy::stream::RecordInfo drainControlRecord;
    uint8_t drainControlVersion = 0;
    uint32_t serverQuerySpaceOverride = 0;
    clientRecords.reserve( scan.records.size() );
    serverRecords.reserve( 1024 );
    for( const auto& record : scan.records )
    {
        if( record.type == tracy::stream::RecordType::SessionBegin && !hasSessionBegin )
        {
            hasSessionBegin = true;
            sessionBeginRecord = record;
        }
        else if( record.type == tracy::stream::RecordType::ClientToServer )
            clientRecords.push_back( record );
        else if( record.type == tracy::stream::RecordType::ServerToClient )
            serverRecords.push_back( record );
        else if( record.type == tracy::stream::RecordType::Diagnostic &&
            ( record.flags & tracy::stream::RecordFlagLocalControl ) != 0 )
        {
            hasLocalDisconnect = true;
            if( ( record.flags & tracy::stream::RecordFlagServerQuery ) == 0 )
            {
                drainControlSequence = record.sequence;
                drainControlRecord = record;
            }
        }
    }
    if( clientRecords.empty() || serverRecords.empty() )
    {
        std::fprintf( stderr, "Journal does not contain a bidirectional Tracy handshake.\n" );
        return 2;
    }
    std::vector<bool> orderIndependentServerRecords;
    orderIndependentServerRecords.reserve( serverRecords.size() );
    {
        PayloadReader serverReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !serverReader.IsOpen() )
        {
            std::fprintf( stderr, "Cannot open journal for server dependency analysis.\n" );
            return 2;
        }
        for( const auto& record : serverRecords )
        {
            if( !serverReader.Read( record, payload, error ) )
            {
                std::fprintf( stderr, "Cannot read server dependency record: %s.\n", error.c_str() );
                return 2;
            }
            const tracy::stream::ReplayServerPacket packet { record.sequence, record.flags, payload };
            orderIndependentServerRecords.push_back( tracy::stream::IsOrderIndependentServerQuery( packet ) );
        }
    }
    bool recordedEndsWithTerminate = false;
    {
        PayloadReader tailReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !tailReader.IsOpen() || !tailReader.Read( serverRecords.back(), payload, error ) )
        {
            std::fprintf( stderr, "Cannot read final recorded server packet: %s.\n", error.c_str() );
            return 2;
        }
        recordedEndsWithTerminate = payload.size() == tracy::ServerQueryPacketSize && payload[0] == tracy::ServerQueryTerminate;
    }
    if( hasSessionBegin )
    {
        PayloadReader sessionReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !sessionReader.IsOpen() || !sessionReader.Read( sessionBeginRecord, payload, error ) )
        {
            std::fprintf( stderr, "Cannot read session metadata: %s.\n", error.c_str() );
            return 2;
        }
        if( payload.size() >= 2 )
        {
            const auto version = uint16_t( payload[0] ) | ( uint16_t( payload[1] ) << 8 );
            if( version == 2 )
            {
                if( payload.size() < 24 )
                {
                    std::fprintf( stderr, "Invalid version 2 session metadata.\n" );
                    return 2;
                }
                const auto headerSize = uint16_t( payload[2] ) | ( uint16_t( payload[3] ) << 8 );
                const auto flags =
                    uint32_t( payload[16] ) |
                    ( uint32_t( payload[17] ) << 8 ) |
                    ( uint32_t( payload[18] ) << 16 ) |
                    ( uint32_t( payload[19] ) << 24 );
                if( headerSize != 24 || ( flags & ~tracy::stream::SessionBeginSupportedFlags ) != 0 )
                {
                    std::fprintf( stderr, "Unsupported version 2 session metadata.\n" );
                    return 2;
                }
                deferSymbolExpansion = ( flags & tracy::stream::SessionBeginFlagDeferredSymbolExpansion ) != 0;
            }
            else if( version != 1 )
            {
                std::fprintf( stderr, "Unsupported session metadata version %u.\n", version );
                return 2;
            }
        }
    }
    if( drainControlSequence != 0 )
    {
        PayloadReader drainReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !drainReader.IsOpen() || !drainReader.Read( drainControlRecord, payload, error ) )
        {
            std::fprintf( stderr, "Cannot read protocol drain control record: %s.\n", error.c_str() );
            return 2;
        }
        if( payload.size() == 1 && payload[0] >= 1 && payload[0] <= 3 )
        {
            drainControlVersion = payload[0];
        }
        else if( payload.size() == 5 && payload[0] == 4 )
        {
            drainControlVersion = payload[0];
            serverQuerySpaceOverride =
                uint32_t( payload[1] ) |
                ( uint32_t( payload[2] ) << 8 ) |
                ( uint32_t( payload[3] ) << 16 ) |
                ( uint32_t( payload[4] ) << 24 );
            if( serverQuerySpaceOverride == 0 || serverQuerySpaceOverride > 8 * 1024 )
            {
                std::fprintf( stderr, "Invalid recorded server-query window.\n" );
                return 2;
            }
        }
        else
        {
            std::fprintf( stderr, "Unsupported protocol drain control payload.\n" );
            return 2;
        }
    }

    const bool replayProtocolOnly = deferSymbolExpansion || drainControlVersion >= 3;
    ReplayError replayError;
    std::atomic<uint64_t> replayedServerSequence { 0 };
    std::unique_ptr<OfflineReplayTransport> offlineTransport;
    std::unique_ptr<tracy::Socket, SocketDeleter> peer;
    std::unique_ptr<tracy::Worker> workerStorage;
    tracy::ListenSocket listener;

    if( options.mode == Options::Mode::Offline )
    {
        std::printf( "Offline replay of %zu client records with in-process validation of %zu server records...\n",
            clientRecords.size(), serverRecords.size() );
        offlineTransport = std::make_unique<OfflineReplayTransport>(
            options.input, serverRecords, scan.complete, replayError, replayedServerSequence );
        workerStorage = std::make_unique<tracy::Worker>( "offline", 0, -1, nullptr, tracy::Worker::Mode::OfflineConvert,
            tracy::Worker::DefaultRecorderDefinitionLimit, tracy::Worker::DefaultRecorderQueryQueueLimit,
            replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly, true, true, offlineTransport.get(),
            options.diagnostics );
    }
    else
    {
        EnableLocalReplayOnly();
        if( !listener.Listen( options.port, 1 ) )
        {
            std::fprintf( stderr, "Cannot bind local replay port %u; choose another port with -p.\n", options.port );
            return 4;
        }
        std::printf( "Legacy replay of %zu client records and validation of %zu server records on 127.0.0.1:%u...\n",
            clientRecords.size(), serverRecords.size(), options.port );
        workerStorage = std::make_unique<tracy::Worker>( "127.0.0.1", options.port, -1, nullptr, tracy::Worker::Mode::Full,
            tracy::Worker::DefaultRecorderDefinitionLimit, tracy::Worker::DefaultRecorderQueryQueueLimit,
            replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly, true, true );

        const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
        while( !peer && std::chrono::steady_clock::now() < acceptDeadline )
        {
            peer.reset( listener.Accept() );
        }
        if( !peer )
        {
            workerStorage->Shutdown();
            std::fprintf( stderr, "Worker did not connect to the local replay socket.\n" );
            return 4;
        }
        // Full Worker replay applies natural socket backpressure while it expands
        // dense compressed frames. A fixed send timeout can fire after partially
        // sending a frame, which cannot be retried without corrupting the stream.
        if( !peer->SetSendTimeout( 0 ) )
        {
            workerStorage->Shutdown();
            std::fprintf( stderr, "Cannot configure the local replay send timeout.\n" );
            return 4;
        }
    }

    const auto analysisSeconds = ElapsedSeconds( analysisStart );
    const auto replayStart = std::chrono::steady_clock::now();
    progress.SetStage( ProgressStage::DecodeBuild, clientRecords.size(), "client_records" );

    auto& worker = *workerStorage;
    if( hasLocalDisconnect && drainControlSequence == 0 ) worker.MarkProtocolDisconnect();

    const auto waitForWorkerProgress = [&]( auto&& complete ) {
        auto lastEventProgress = worker.GetProtocolEventCount();
        auto lastFrameProgress = worker.GetProtocolFramesProcessed();
        auto progressDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
        while( !complete() && !replayError.Failed() )
        {
            if( CancelRequested() )
            {
                replayError.Set( "conversion cancelled" );
                break;
            }
            const auto eventProgress = worker.GetProtocolEventCount();
            const auto frameProgress = worker.GetProtocolFramesProcessed();
            if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
            {
                lastEventProgress = eventProgress;
                lastFrameProgress = frameProgress;
                progressDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
            }
            else if( std::chrono::steady_clock::now() >= progressDeadline )
            {
                return false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return complete() && !replayError.Failed();
    };

    std::thread verifier;
    if( options.mode == Options::Mode::Legacy ) verifier = std::thread( [&] {
        const auto failReplay = [&]( std::string message ) {
            replayError.Set( std::move( message ) );
            peer->Close();
        };
        PayloadReader reader( options.input );
        if( !reader.IsOpen() )
        {
            failReplay( "cannot open journal for server-stream verification" );
            return;
        }
        tracy::stream::ReplayServerTranscriptVerifier transcriptVerifier;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( replayError.Failed() ) return;
            tracy::stream::ReplayServerPacket recorded { record.sequence, record.flags, {} };
            if( !reader.Read( record, recorded.payload, error ) )
            {
                failReplay( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return;
            }
            tracy::stream::ReplayServerPacket replayed { record.sequence, record.flags, {} };
            replayed.payload.resize( recorded.payload.size() );
            // Full replay may need to expand a large first batch of sampling
            // callstacks before it can reproduce the recorder's first query.
            // Treat that CPU work separately from a closed server stream.
            auto lastEventProgress = worker.GetProtocolEventCount();
            auto lastFrameProgress = worker.GetProtocolFramesProcessed();
            auto recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
            if( !replayed.payload.empty() && !peer->Read( replayed.payload.data(), int( replayed.payload.size() ), 100, [&] {
                const auto eventProgress = worker.GetProtocolEventCount();
                const auto frameProgress = worker.GetProtocolFramesProcessed();
                if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
                {
                    lastEventProgress = eventProgress;
                    lastFrameProgress = frameProgress;
                    recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                }
                return replayError.Failed() || std::chrono::steady_clock::now() >= recordDeadline;
            } ) )
            {
                const auto timedOut = std::chrono::steady_clock::now() >= recordDeadline;
                failReplay( "sequence " + std::to_string( record.sequence ) +
                    ( timedOut ? ": Worker made no protocol progress for 120 seconds while waiting for server stream; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) : ": Worker server stream ended early" ) );
                return;
            }
            if( !transcriptVerifier.Append( recorded, replayed, error ) )
            {
                failReplay( error );
                return;
            }
            // Preserve the journal's cross-direction causal ordering. Client
            // responses must not be replayed before the Worker has emitted the
            // earlier query they answer. Definition queries may still reorder
            // within a consecutive server batch; the transcript verifier
            // validates those batches by content and multiplicity.
            replayedServerSequence.store( record.sequence, std::memory_order_release );
        }
        if( !transcriptVerifier.Finish( error ) )
        {
            failReplay( error );
        }
    } );

    const auto inputCompressedFrameCount = uint64_t( std::count_if( clientRecords.begin(), clientRecords.end(), []( const auto& record ) {
        return ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
    } ) );
    uint64_t publishedClientRecordCount = 0;
    uint64_t publishedCompressedFrameCount = 0;
    uint64_t lastPublishedClientSequence = 0;
    PayloadReader clientReader( options.input );
    if( !clientReader.IsOpen() )
    {
        replayError.Set( "cannot open journal for client-stream replay" );
    }
    else
    {
        std::vector<uint8_t> payload;
        std::string error;
        size_t serverDependencyCursor = 0;
        uint64_t replayedClientRecordCount = 0;
        auto replayClientRecord = [&]( const tracy::stream::RecordInfo& record ) {
            if( CancelRequested() )
            {
                replayError.Set( "conversion cancelled" );
                return false;
            }
            if( progress.PressureExceeded() )
            {
                replayError.Set( "system memory pressure exceeded the offline conversion safety limit" );
                return false;
            }
            if( replayError.Failed() ) return false;
            if( publishedClientRecordCount != 0 && record.sequence <= lastPublishedClientSequence )
            {
                replayError.Set( "client journal record was replayed twice or out of sequence at " +
                    std::to_string( record.sequence ) );
                return false;
            }
            uint64_t requiredServerSequence = 0;
            while( serverDependencyCursor < serverRecords.size() &&
                serverRecords[serverDependencyCursor].sequence < record.sequence )
            {
                if( !orderIndependentServerRecords[serverDependencyCursor] )
                    requiredServerSequence = serverRecords[serverDependencyCursor].sequence;
                serverDependencyCursor++;
            }
            if( requiredServerSequence != 0 )
            {
                auto lastEventProgress = worker.GetProtocolEventCount();
                auto lastFrameProgress = worker.GetProtocolFramesProcessed();
                auto dependencyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                while( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence &&
                    !replayError.Failed() && std::chrono::steady_clock::now() < dependencyDeadline )
                {
                    if( CancelRequested() )
                    {
                        replayError.Set( "conversion cancelled" );
                        break;
                    }
                    const auto eventProgress = worker.GetProtocolEventCount();
                    const auto frameProgress = worker.GetProtocolFramesProcessed();
                    if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
                    {
                        lastEventProgress = eventProgress;
                        lastFrameProgress = frameProgress;
                        dependencyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                    }
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
                if( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence )
                {
                    replayError.Set( "sequence " + std::to_string( record.sequence ) +
                        ": Worker made no protocol progress for 120 seconds while waiting for preceding server sequence " +
                        std::to_string( requiredServerSequence ) + "; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) );
                    return false;
                }
            }
            if( !clientReader.Read( record, payload, error ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return false;
            }
            const bool sent = options.mode == Options::Mode::Offline ?
                offlineTransport->PublishClient( payload ) :
                ( payload.empty() || peer->Send( payload.data(), int( payload.size() ) ) == int( payload.size() ) );
            if( !sent )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) +
                    ": recorded client stream publish ended while Worker replay was active; events=" +
                    std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                    std::to_string( worker.GetProtocolFramesProcessed() ) );
                return false;
            }
            lastPublishedClientSequence = record.sequence;
            publishedClientRecordCount++;
            if( ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0 ) publishedCompressedFrameCount++;
            progress.Update( ++replayedClientRecordCount, clientRecords.size() );
            if( options.testCancelAfterRecords != 0 && replayedClientRecordCount >= options.testCancelAfterRecords )
                s_cancelRequests.store( 1, std::memory_order_relaxed );
            return true;
        };

        if( drainControlSequence == 0 )
        {
            uint64_t replayedFrames = 0;
            for( const auto& record : clientRecords )
            {
                if( !replayClientRecord( record ) ) break;
                if( ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0 ) replayedFrames++;
            }
            if( !replayError.Failed() && scan.complete && recordedEndsWithTerminate )
            {
                if( !waitForWorkerProgress( [&] { return worker.GetProtocolFramesProcessed() >= replayedFrames; } ) )
                {
                    replayError.Set( "Worker made no protocol progress for 120 seconds while processing the complete Full-capture client revision; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) + "/" + std::to_string( replayedFrames ) );
                }
                else if( worker.IsConnected() )
                {
                    // Full capture stops locally, but old/double-write journals do not contain the
                    // ProtocolOnly BeginDrain marker. Ask the Worker's protocol thread to reproduce the
                    // recorded final Terminate after every client frame and preceding server query has
                    // been processed. The transcript verifier still rejects missing or extra packets.
                    worker.RequestProtocolReplayTerminate();
                }
            }
        }
        else
        {
            const auto preDrainFrameCount = std::count_if( clientRecords.begin(), clientRecords.end(), [&]( const auto& record ) {
                return record.sequence < drainControlSequence &&
                    ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
            } );
            if( preDrainFrameCount < 2 )
            {
                replayError.Set( "drain marker does not have two preceding client frames" );
            }

            const auto prefixFrameTarget = uint64_t( preDrainFrameCount >= 2 ? preDrainFrameCount - 2 : 0 );
            uint64_t sentFrames = 0;
            size_t splitIndex = 0;
            for( ; splitIndex < clientRecords.size() && !replayError.Failed(); splitIndex++ )
            {
                const auto& record = clientRecords[splitIndex];
                if( record.sequence > drainControlSequence ) break;
                const bool compressed = ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
                if( compressed && sentFrames == prefixFrameTarget ) break;
                if( !replayClientRecord( record ) ) break;
                if( compressed ) sentFrames++;
            }

            if( !waitForWorkerProgress( [&] { return worker.GetProtocolFramesProcessed() >= prefixFrameTarget; } ) )
            {
                replayError.Set( "Worker made no protocol progress for 120 seconds while processing the pre-drain client revision; events=" +
                    std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                    std::to_string( worker.GetProtocolFramesProcessed() ) + "/" + std::to_string( prefixFrameTarget ) );
            }
            else if( !replayError.Failed() )
            {
                worker.RequestProtocolDrain( drainControlVersion >= 2 );
                for( ; splitIndex < clientRecords.size(); splitIndex++ )
                {
                    const auto& record = clientRecords[splitIndex];
                    if( record.sequence > drainControlSequence ) break;
                    if( !replayClientRecord( record ) ) break;
                }
                if( !waitForWorkerProgress( [&] { return worker.IsProtocolDrainActive(); } ) )
                {
                    replayError.Set( "Worker made no protocol progress for 120 seconds while entering protocol drain mode; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) );
                }
                else
                {
                    for( ; splitIndex < clientRecords.size(); splitIndex++ )
                    {
                        if( !replayClientRecord( clientRecords[splitIndex] ) ) break;
                    }
                }
            }
        }
    }

    if( options.mode == Options::Mode::Offline )
    {
        const auto finalServerSequence = serverRecords.back().sequence;
        auto lastEventProgress = worker.GetProtocolEventCount();
        auto lastFrameProgress = worker.GetProtocolFramesProcessed();
        auto transcriptDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
        while( replayedServerSequence.load( std::memory_order_acquire ) < finalServerSequence &&
            !replayError.Failed() && std::chrono::steady_clock::now() < transcriptDeadline )
        {
            const auto eventProgress = worker.GetProtocolEventCount();
            const auto frameProgress = worker.GetProtocolFramesProcessed();
            if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
            {
                lastEventProgress = eventProgress;
                lastFrameProgress = frameProgress;
                transcriptDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        if( replayedServerSequence.load( std::memory_order_acquire ) < finalServerSequence && !replayError.Failed() )
        {
            replayError.Set( "Worker made no protocol progress for 120 seconds before consuming the complete in-process server transcript" );
        }
        std::string offlineError;
        if( !replayError.Failed() && !offlineTransport->Finish( offlineError ) ) replayError.Set( offlineError );
        offlineTransport->CloseClient();
    }
    else
    {
        if( replayError.Failed() && peer->IsValid() ) peer->Close();
        verifier.join();
    }

    if( !replayError.Failed() && options.mode == Options::Mode::Legacy )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        uint8_t extraByte = 0;
        if( peer->HasData() && peer->ReadRaw( &extraByte, 1, 1 ) )
        {
            if( scan.complete )
            {
                replayError.Set( "Worker emitted server bytes beyond the recorded complete transcript; first byte=" +
                    std::to_string( unsigned( extraByte ) ) );
            }
            else
            {
                std::fprintf( stderr, "Warning: incomplete journal ended before a trailing Worker query; the valid client prefix remains replayable.\n" );
            }
        }
    }
    if( options.mode == Options::Mode::Legacy && peer->IsValid() ) peer->Close();

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while( !worker.HasData() && worker.GetHandshakeStatus() == tracy::HandshakePending && std::chrono::steady_clock::now() < workerDeadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    while( worker.IsConnected() && std::chrono::steady_clock::now() < workerDeadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    if( worker.IsConnected() )
    {
        worker.Disconnect();
        replayError.Set( "Worker did not finish after replay socket closed" );
    }
    if( !worker.HasData() )
    {
        replayError.Set( "Worker rejected the replay before receiving capture metadata" );
    }
    if( !replayError.Failed() &&
        ( publishedClientRecordCount != clientRecords.size() || publishedCompressedFrameCount != inputCompressedFrameCount ) )
    {
        replayError.Set( "client journal replay coverage mismatch: records=" +
            std::to_string( publishedClientRecordCount ) + "/" + std::to_string( clientRecords.size() ) +
            ", compressed_frames=" + std::to_string( publishedCompressedFrameCount ) + "/" +
            std::to_string( inputCompressedFrameCount ) );
    }
    if( replayError.Failed() )
    {
        std::fprintf( stderr, "Replay failed: %s\n", replayError.Message().c_str() );
        const auto failure = worker.GetFailureType();
        if( failure != tracy::Worker::Failure::None )
        {
            std::fprintf( stderr, "Replay Worker instrumentation failure: %s\n", tracy::Worker::GetFailureString( failure ) );
            const auto& failureData = worker.GetFailureData();
            if( !failureData.message.empty() )
                std::fprintf( stderr, "Replay Worker failure context: %s\n", failureData.message.c_str() );
        }
        if( CancelRequested() )
        {
            progress.SetStage( ProgressStage::Cancelled );
            return 130;
        }
        progress.SetStage( ProgressStage::Failed );
        return 5;
    }

    const auto protocolEventCount = worker.GetProtocolEventCount();
    const auto offlineDecodedEventCount = options.mode == Options::Mode::Offline ?
        worker.GetOfflineDecodedEventCount() : 0;
    const auto workerCompressedFrameCount = worker.GetProtocolFramesProcessed();
    const auto gpuReferenceUseCount = worker.GetJnTraceData().gpuReferenceUses.size();
    const auto gpuReferencePassCount = worker.GetJnTraceData().gpuReferencePasses.size();
    const auto jobStageCount = worker.GetJnTraceData().jobStages.size();

    if( options.mode == Options::Mode::Offline && options.diagnostics )
    {
        std::vector<std::pair<double, size_t>> estimatedCosts;
        const auto& eventStats = worker.GetOfflineEventStats();
        estimatedCosts.reserve( eventStats.size() );
        for( size_t index = 0; index < eventStats.size(); index++ )
        {
            const auto& stat = eventStats[index];
            if( stat.sampledCount == 0 ) continue;
            const auto estimatedNanoseconds = double( stat.sampledNanoseconds ) *
                double( stat.count ) / double( stat.sampledCount );
            estimatedCosts.emplace_back( estimatedNanoseconds, index );
        }
        std::sort( estimatedCosts.begin(), estimatedCosts.end(), []( const auto& left, const auto& right ) {
            return left.first > right.first;
        } );
        std::printf( "Offline sampled dispatch costs (top 20):\n" );
        const auto limit = std::min<size_t>( 20, estimatedCosts.size() );
        for( size_t rank = 0; rank < limit; rank++ )
        {
            const auto index = estimatedCosts[rank].second;
            const auto& stat = eventStats[index];
            const auto averageNanoseconds = double( stat.sampledNanoseconds ) / double( stat.sampledCount );
            std::printf( "  type=%zu name=%s count=%llu sampled=%llu avg=%.1fns estimated=%.3fs\n",
                index, QueueTypeName( tracy::QueueType( index ) ), static_cast<unsigned long long>( stat.count ),
                static_cast<unsigned long long>( stat.sampledCount ), averageNanoseconds,
                estimatedCosts[rank].first / 1'000'000'000.0 );
        }
    }

    const auto replaySeconds = ElapsedSeconds( replayStart );
    const auto writeStart = std::chrono::steady_clock::now();
    auto temporaryOutput = options.output;
    temporaryOutput += ".converting";
    TemporaryOutputGuard temporaryGuard( temporaryOutput, options.keepFailedOutput );
    progress.SetStage( ProgressStage::Write, 0, "bytes" );
    progress.SetTemporaryPath( temporaryOutput );
    tracy::Worker::JnGpuCatalogResolveStats gpuCatalogResolveStats {};
    {
        std::error_code ignored;
        std::filesystem::remove( temporaryOutput, ignored );
    }
    const auto compression = GetCompressionSettings( options.compression, options.threads );
    auto output = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open(
        temporaryOutput.string().c_str(), tracy::FileCompression::Zstd, compression.level, int( compression.streams ) ) );
    if( !output )
    {
        std::fprintf( stderr, "Cannot create output snapshot.\n" );
        return 6;
    }
    worker.Write( *output, false );
    gpuCatalogResolveStats = worker.GetJnGpuCatalogResolveStats();
    output->Finish();
    const auto statistics = output->GetCompressionStatistics();
    output.reset();
    const auto writeSeconds = ElapsedSeconds( writeStart );
    if( options.diagnostics )
    {
        std::printf( "GPU Catalog resolver: calls=%llu generation_end=%llu post_end_batch=%llu finalize_save=%llu input_units=%llu unresolved=%llu core_unresolved=%llu time=%.3fs\n",
            static_cast<unsigned long long>( gpuCatalogResolveStats.fullResolveCalls ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.generationEndCalls ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.postEndBatchCalls ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.finalizeSaveCalls ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.fullResolveInputUnits ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.totalUnresolved ),
            static_cast<unsigned long long>( gpuCatalogResolveStats.coreUnresolved ),
            double( gpuCatalogResolveStats.fullResolveNanoseconds ) / 1'000'000'000.0 );
    }
    if( options.testFailBeforeValidate )
    {
        std::fprintf( stderr, "Injected failure before validation.\n" );
        progress.SetStage( ProgressStage::Failed );
        return 99;
    }
    if( CancelRequested() )
    {
        progress.SetStage( ProgressStage::Cancelled );
        return 130;
    }

    const auto validationStart = std::chrono::steady_clock::now();
    progress.SetStage( ProgressStage::Validate, 1, "checks" );
    workerStorage.reset();
    try
    {
        auto validationFile = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( temporaryOutput.string().c_str() ) );
        if( !validationFile )
        {
            std::fprintf( stderr, "Cannot reopen temporary snapshot for validation.\n" );
            return 6;
        }
        tracy::Worker validationWorker( *validationFile, tracy::EventType::All, true );
        if( !validationWorker.HasData() )
        {
            std::fprintf( stderr, "Temporary snapshot validation produced no data.\n" );
            return 6;
        }
        progress.Update( 1, 1 );
    }
    catch( const std::exception& exception )
    {
        std::fprintf( stderr, "Temporary snapshot validation failed: %s\n", exception.what() );
        return 6;
    }
    catch( ... )
    {
        std::fprintf( stderr, "Temporary snapshot validation failed with an unknown error.\n" );
        return 6;
    }
    const auto validationSeconds = ElapsedSeconds( validationStart );

    progress.SetStage( ProgressStage::Publish, 1, "artifacts" );
    std::string publishError;
    if( !PublishSnapshotAtomically( temporaryOutput, options.output, options.overwrite, publishError ) )
    {
        std::fprintf( stderr, "Snapshot validation passed but atomic publication failed: %s\n", publishError.c_str() );
        return 6;
    }
    temporaryGuard.Published();

    const auto mapStart = std::chrono::steady_clock::now();
    std::string snapshotMapError;
    if( !tracy::stream::WriteConvertedSnapshotMap( options.input, scan, options.output, snapshotMapError ) )
    {
        std::fprintf( stderr, "Snapshot was converted but its stream mapping could not be published: %s\n", snapshotMapError.c_str() );
        return 7;
    }
    const auto mapSeconds = ElapsedSeconds( mapStart );
    if( !scanCacheDirectory.empty() )
    {
        std::error_code cacheCleanupError;
        std::filesystem::remove_all( scanCacheDirectory, cacheCleanupError );
        if( cacheCleanupError )
            std::fprintf( stderr, "Warning: successful conversion could not remove its scan cache: %s.\n",
                cacheCleanupError.message().c_str() );
        else
        {
            std::error_code emptyRootError;
            std::filesystem::remove( scanCacheDirectory.parent_path(), emptyRootError );
        }
    }
    progress.Update( 1, 1 );
    progress.SetTemporaryPath( {} );
    progress.SetStage( ProgressStage::Complete, 1, "conversion" );
    progress.Update( 1, 1 );
    progress.Stop();
    const auto totalSeconds = ElapsedSeconds( totalStart );
    std::printf( "Converted %llu valid journal bytes into %llu snapshot bytes (%.2f%%), compression=%s, threads=%u.\n",
        static_cast<unsigned long long>( scan.validSize ), static_cast<unsigned long long>( statistics.second ),
        statistics.first == 0 ? 0. : 100. * statistics.second / statistics.first,
        CompressionName( options.compression ), options.threads );
    std::printf( "Stages: scan=%.3fs analysis=%.3fs replay=%.3fs write=%.3fs validate=%.3fs map=%.3fs total=%.3fs.\n",
        scanSeconds, analysisSeconds, replaySeconds, writeSeconds, validationSeconds, mapSeconds,
        totalSeconds );
    if( !options.reportJson.empty() )
    {
        std::error_code outputSizeError;
        const auto outputSize = std::filesystem::file_size( options.output, outputSizeError );
        std::ostringstream report;
        report << "{\"schema\":1,\"success\":true,\"mode\":\""
            << ( options.mode == Options::Mode::Offline ? "offline" : "legacy" )
            << "\",\"protocol\":" << tracy::ProtocolVersion
            << ",\"compression\":\"" << CompressionName( options.compression ) << "\",\"threads\":" << options.threads
            << ",\"input_bytes\":\"" << scan.fileSize << "\",\"valid_bytes\":\"" << scan.validSize
            << "\",\"ignored_tail_bytes\":\"" << ( scan.fileSize - scan.validSize )
            << "\",\"capture_complete\":" << ( scan.complete ? "true" : "false" )
            << ",\"scan_cache_used\":" << ( scanCacheUsed ? "true" : "false" )
            << ",\"committed_revision\":\"" << scan.lastSequence << "\",\"client_records\":" << clientRecords.size()
            << ",\"published_client_records\":" << publishedClientRecordCount
            << ",\"compressed_frames\":" << inputCompressedFrameCount
            << ",\"published_compressed_frames\":" << publishedCompressedFrameCount
            << ",\"worker_compressed_frames\":" << workerCompressedFrameCount
            << ",\"server_records\":" << serverRecords.size() << ",\"protocol_events\":\"" << protocolEventCount
            << "\",\"offline_decoded_events\":\"" << offlineDecodedEventCount
            << "\",\"gpu_reference_passes\":\"" << gpuReferencePassCount << "\",\"gpu_reference_uses\":\""
            << gpuReferenceUseCount << "\",\"job_stages\":\"" << jobStageCount << "\",\"output_bytes\":\""
            << ( outputSizeError ? 0 : outputSize ) << "\",\"peak_commit_bytes\":\"" << progress.PeakCommitBytes()
            << "\",\"peak_working_set_bytes\":\"" << progress.PeakWorkingSetBytes()
            << "\",\"gpu_catalog_resolver\":{\"full_resolve_calls\":\"" << gpuCatalogResolveStats.fullResolveCalls
            << "\",\"generation_end_calls\":\"" << gpuCatalogResolveStats.generationEndCalls
            << "\",\"post_end_batch_calls\":\"" << gpuCatalogResolveStats.postEndBatchCalls
            << "\",\"finalize_save_calls\":\"" << gpuCatalogResolveStats.finalizeSaveCalls
            << "\",\"input_units\":\"" << gpuCatalogResolveStats.fullResolveInputUnits
            << "\",\"total_unresolved\":\"" << gpuCatalogResolveStats.totalUnresolved
            << "\",\"core_unresolved\":\"" << gpuCatalogResolveStats.coreUnresolved
            << "\",\"nanoseconds\":\"" << gpuCatalogResolveStats.fullResolveNanoseconds
            << "\"},\"stages_seconds\":{"
            << "\"scan\":" << scanSeconds << ",\"analysis\":" << analysisSeconds << ",\"replay\":" << replaySeconds
            << ",\"write\":" << writeSeconds << ",\"validate\":" << validationSeconds << ",\"snapshot_map\":"
            << mapSeconds << ",\"total\":" << totalSeconds << "}}";
        if( !AtomicWriteText( options.reportJson, report.str() ) )
        {
            std::fprintf( stderr, "Snapshot is valid, but --report-json could not be published.\n" );
            return 7;
        }
    }
    return 0;
}
