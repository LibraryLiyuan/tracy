#include "TracyAnalysisWriterLease.hpp"
#include "TracyAnalysisIoPath.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <thread>
#include <algorithm>
#ifdef _WIN32
# include <Windows.h>
#else
# include <csignal>
# include <unistd.h>
#endif
namespace tracy::analysis
{
namespace
{
struct WriterLeaseOwner
{
    uint64_t pid = 0;
    uint64_t processCreation = 0;
    uint64_t heartbeat = 0;
    std::string generation;
};

bool ReplaceFileAtomically( const std::filesystem::path& temporary,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    const auto ioTemporary = AnalysisIoPath( temporary );
    const auto ioTarget = AnalysisIoPath( target );
    // ReplaceFileW needs delete sharing on the old file. Antivirus, indexing,
    // status readers and Explorer may transiently omit it. In these cases the
    // documented result leaves both names unchanged, so a bounded retry is
    // safe and preserves the previously committed manifest/checkpoint.
    constexpr auto RetryLimit = std::chrono::milliseconds( 1500 );
    const auto deadline = std::chrono::steady_clock::now() + RetryLimit;
    auto delay = std::chrono::milliseconds( 5 );
    DWORD lastError = ERROR_SUCCESS;
    for( ;; )
    {
        const auto targetExists = GetFileAttributesW( ioTarget.c_str() ) != INVALID_FILE_ATTRIBUTES;
        const auto replaced = targetExists
            ? ReplaceFileW( ioTarget.c_str(), ioTemporary.c_str(), nullptr,
                REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_ACL_ERRORS,
                nullptr, nullptr ) != FALSE
            : MoveFileExW( ioTemporary.c_str(), ioTarget.c_str(), MOVEFILE_WRITE_THROUGH ) != FALSE;
        if( replaced ) return true;
        lastError = GetLastError();
        const auto retryable = lastError == ERROR_ACCESS_DENIED ||
            lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION ||
            lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS ||
            lastError == ERROR_UNABLE_TO_REMOVE_REPLACED;
        const auto now = std::chrono::steady_clock::now();
        if( !retryable || now >= deadline ) break;
        std::this_thread::sleep_for( std::min( delay,
            std::chrono::duration_cast<std::chrono::milliseconds>( deadline - now ) ) );
        delay = std::min( delay * 2, std::chrono::milliseconds( 100 ) );
    }
    error = "analysis_atomic_replace_failed:" + std::to_string( lastError ) + ":" +
        target.filename().string();
#else
    std::error_code ec;
    std::filesystem::rename( AnalysisIoPath( temporary ), AnalysisIoPath( target ), ec );
    if( !ec ) return true;
    error = "analysis_atomic_replace_failed:" + ec.message();
#endif
    return false;
}

uint64_t CurrentUnixNanoseconds()
{
    return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch() ).count() );
}

uint64_t CurrentProcessIdValue()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint64_t( getpid() );
#endif
}

uint64_t ProcessCreationValue( uint64_t pid )
{
#ifdef _WIN32
    HANDLE process = pid == GetCurrentProcessId() ? GetCurrentProcess() :
        OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD( pid ) );
    if( !process ) return 0;
    FILETIME creation {}, exit {}, kernel {}, user {};
    const auto success = GetProcessTimes( process, &creation, &exit, &kernel, &user ) != FALSE;
    if( process != GetCurrentProcess() ) CloseHandle( process );
    if( !success ) return 0;
    return uint64_t( creation.dwLowDateTime ) | ( uint64_t( creation.dwHighDateTime ) << 32 );
#else
    (void)pid;
    return 0;
#endif
}

bool ProcessMatches( const WriterLeaseOwner& owner )
{
    if( owner.pid == 0 ) return false;
#ifdef _WIN32
    const auto creation = ProcessCreationValue( owner.pid );
    return creation != 0 && creation == owner.processCreation;
#else
    return kill( pid_t( owner.pid ), 0 ) == 0;
#endif
}

bool ReadLeaseOwner( const std::filesystem::path& directory, WriterLeaseOwner& owner )
{
    std::ifstream input( AnalysisIoPath( directory / "owner" ), std::ios::binary );
    std::string key;
    while( input >> key )
    {
        if( key == "pid" ) input >> owner.pid;
        else if( key == "process_creation" ) input >> owner.processCreation;
        else if( key == "heartbeat" ) input >> owner.heartbeat;
        else if( key == "generation" ) input >> std::quoted( owner.generation );
        else { std::string ignored; std::getline( input, ignored ); }
    }
    return bool( input.eof() ) && owner.pid != 0 && !owner.generation.empty();
}

bool WriteLeaseOwner( const std::filesystem::path& directory,
    const WriterLeaseOwner& owner, std::string& error )
{
    const auto target = directory / "owner";
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output( AnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !output ) { error = "analysis_writer_lease_owner_open_failed"; return false; }
    output << "pid " << owner.pid << '\n'
        << "process_creation " << owner.processCreation << '\n'
        << "heartbeat " << owner.heartbeat << '\n'
        << "generation " << std::quoted( owner.generation ) << '\n';
    output.flush();
    if( !output ) { error = "analysis_writer_lease_owner_write_failed"; return false; }
    output.close();
    return ReplaceFileAtomically( temporary, target, error );
}


}
AnalysisWriterLease::~AnalysisWriterLease()
{
    Release();
}

AnalysisWriterLease::AnalysisWriterLease( AnalysisWriterLease&& other ) noexcept
    : m_path( std::move( other.m_path ) )
    , m_generation( std::move( other.m_generation ) )
    , m_pid( other.m_pid )
    , m_processCreation( other.m_processCreation )
{
    other.m_path.clear();
    other.m_pid = 0;
    other.m_processCreation = 0;
}

AnalysisWriterLease& AnalysisWriterLease::operator=( AnalysisWriterLease&& other ) noexcept
{
    if( this == &other ) return *this;
    Release();
    m_path = std::move( other.m_path );
    m_generation = std::move( other.m_generation );
    m_pid = other.m_pid;
    m_processCreation = other.m_processCreation;
    other.m_path.clear();
    other.m_pid = 0;
    other.m_processCreation = 0;
    return *this;
}

bool AnalysisWriterLease::Heartbeat( std::string& error )
{
    error.clear();
    if( !Active() ) { error = "analysis_writer_lease_not_active"; return false; }
    WriterLeaseOwner current;
    if( !ReadLeaseOwner( m_path, current ) || current.pid != m_pid ||
        current.processCreation != m_processCreation || current.generation != m_generation )
    {
        error = "analysis_writer_lease_lost";
        return false;
    }
    current.heartbeat = CurrentUnixNanoseconds();
    return WriteLeaseOwner( m_path, current, error );
}

void AnalysisWriterLease::Release()
{
    if( !Active() ) return;
    WriterLeaseOwner current;
    if( ReadLeaseOwner( m_path, current ) && current.pid == m_pid &&
        current.processCreation == m_processCreation && current.generation == m_generation )
    {
        std::error_code ignored;
        std::filesystem::remove_all( AnalysisIoPath( m_path ), ignored );
    }
    m_path.clear();
    m_generation.clear();
    m_pid = 0;
    m_processCreation = 0;
}

bool AcquireAnalysisWriterLease( const std::filesystem::path& storeRoot,
    AnalysisWriterLease& lease, std::string& error )
{
    error.clear();
    if( lease.Active() ) { error = "analysis_writer_lease_already_owned"; return false; }
    const auto stateDirectory = storeRoot / "build-state";
    const auto leaseDirectory = stateDirectory / "writer.lease";
    std::error_code ec;
    std::filesystem::create_directories( AnalysisIoPath( stateDirectory ), ec );
    if( ec ) { error = "analysis_writer_lease_directory_failed:" + ec.message(); return false; }

    const auto pid = CurrentProcessIdValue();
    const auto creation = ProcessCreationValue( pid );
    if( pid == 0 ) { error = "analysis_writer_lease_process_identity_failed"; return false; }
    const auto generation = std::to_string( pid ) + "-" + std::to_string( creation ) + "-" +
        std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );

    for( int attempt = 0; attempt < 3; attempt++ )
    {
        ec.clear();
        if( std::filesystem::create_directory( AnalysisIoPath( leaseDirectory ), ec ) )
        {
            WriterLeaseOwner owner { pid, creation, CurrentUnixNanoseconds(), generation };
            if( !WriteLeaseOwner( leaseDirectory, owner, error ) )
            {
                std::error_code ignored;
                std::filesystem::remove_all( AnalysisIoPath( leaseDirectory ), ignored );
                return false;
            }
            lease.m_path = leaseDirectory;
            lease.m_generation = generation;
            lease.m_pid = pid;
            lease.m_processCreation = creation;
            return true;
        }
        if( ec && ec != std::errc::file_exists )
        {
            error = "analysis_writer_lease_create_failed:" + ec.message();
            return false;
        }

        WriterLeaseOwner existing;
        if( ReadLeaseOwner( leaseDirectory, existing ) && ProcessMatches( existing ) )
        {
            error = "analysis_writer_lease_active";
            return false;
        }
        auto stale = stateDirectory / ( "writer.lease.stale." + generation + "." + std::to_string( attempt ) );
        ec.clear();
        std::filesystem::rename( AnalysisIoPath( leaseDirectory ), AnalysisIoPath( stale ), ec );
        if( ec && ec != std::errc::no_such_file_or_directory )
        {
            error = "analysis_writer_lease_stale_rename_failed:" + ec.message();
            return false;
        }
    }
    error = "analysis_writer_lease_race";
    return false;
}


}
