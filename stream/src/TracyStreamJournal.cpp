#include "TracyStreamJournal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  include <Windows.h>
#  include <fcntl.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace tracy::stream
{
namespace
{

constexpr std::array<uint8_t, 8> FileMagic = { 'T', 'R', 'C', 'S', 'T', 'R', 'M', '1' };
constexpr std::array<uint8_t, 4> RecordMagic = { 'T', 'S', 'R', '1' };
constexpr std::array<uint8_t, 4> TrailerMagic = { 'T', 'C', 'M', '1' };
constexpr uint16_t TrailerVersion = 1;
constexpr size_t ScanChunkSize = 64 * 1024;

constexpr std::array<uint32_t, 256> MakeCrc32cTable()
{
    std::array<uint32_t, 256> table = {};
    for( uint32_t i = 0; i < table.size(); i++ )
    {
        uint32_t value = i;
        for( int bit = 0; bit < 8; bit++ )
        {
            value = ( value >> 1 ) ^ ( ( value & 1 ) != 0 ? 0x82F63B78u : 0u );
        }
        table[i] = value;
    }
    return table;
}

constexpr auto Crc32cTable = MakeCrc32cTable();

class Crc32cState
{
public:
    void Update( const uint8_t* data, size_t size )
    {
        for( size_t i = 0; i < size; i++ )
        {
            m_value = Crc32cTable[( m_value ^ data[i] ) & 0xFFu] ^ ( m_value >> 8 );
        }
    }

    uint32_t Finish() const { return ~m_value; }

private:
    uint32_t m_value = 0xFFFFFFFFu;
};

void Put16( uint8_t* out, size_t offset, uint16_t value )
{
    out[offset] = uint8_t( value );
    out[offset + 1] = uint8_t( value >> 8 );
}

void Put32( uint8_t* out, size_t offset, uint32_t value )
{
    out[offset] = uint8_t( value );
    out[offset + 1] = uint8_t( value >> 8 );
    out[offset + 2] = uint8_t( value >> 16 );
    out[offset + 3] = uint8_t( value >> 24 );
}

void Put64( uint8_t* out, size_t offset, uint64_t value )
{
    for( size_t i = 0; i < 8; i++ )
    {
        out[offset + i] = uint8_t( value >> ( i * 8 ) );
    }
}

uint16_t Get16( const uint8_t* in, size_t offset )
{
    return uint16_t( in[offset] ) | ( uint16_t( in[offset + 1] ) << 8 );
}

uint32_t Get32( const uint8_t* in, size_t offset )
{
    return uint32_t( in[offset] ) | ( uint32_t( in[offset + 1] ) << 8 ) | ( uint32_t( in[offset + 2] ) << 16 ) | ( uint32_t( in[offset + 3] ) << 24 );
}

uint64_t Get64( const uint8_t* in, size_t offset )
{
    uint64_t value = 0;
    for( size_t i = 0; i < 8; i++ )
    {
        value |= uint64_t( in[offset + i] ) << ( i * 8 );
    }
    return value;
}

template<size_t N>
bool HasMagic( const std::array<uint8_t, N>& expected, const uint8_t* actual )
{
    return std::equal( expected.begin(), expected.end(), actual );
}

template<size_t N>
uint32_t CrcWithZeroedField( std::array<uint8_t, N> bytes, size_t offset )
{
    std::fill_n( bytes.begin() + offset, 4, uint8_t( 0 ) );
    return Crc32c( bytes );
}

std::array<uint8_t, FileHeaderSize> EncodeFileHeader( const FileHeader& value )
{
    std::array<uint8_t, FileHeaderSize> bytes = {};
    std::copy( FileMagic.begin(), FileMagic.end(), bytes.begin() );
    Put16( bytes.data(), 8, JournalVersionMajor );
    Put16( bytes.data(), 10, uint16_t( FileHeaderSize ) );
    Put32( bytes.data(), 12, value.flags );
    Put32( bytes.data(), 16, value.protocolVersion );
    std::copy( value.sessionId.begin(), value.sessionId.end(), bytes.begin() + 24 );
    Put64( bytes.data(), 40, value.createdUnixNs );
    Put32( bytes.data(), 48, CrcWithZeroedField( bytes, 48 ) );
    return bytes;
}

bool DecodeFileHeader( const std::array<uint8_t, FileHeaderSize>& bytes, FileHeader& value, std::string& error )
{
    if( !HasMagic( FileMagic, bytes.data() ) )
    {
        error = "file magic is not TRCSTRM1";
        return false;
    }
    if( Get16( bytes.data(), 8 ) != JournalVersionMajor )
    {
        error = "unsupported journal major version";
        return false;
    }
    if( Get16( bytes.data(), 10 ) != FileHeaderSize )
    {
        error = "invalid file header size";
        return false;
    }
    if( Get32( bytes.data(), 20 ) != 0 || Get32( bytes.data(), 52 ) != 0 || Get64( bytes.data(), 56 ) != 0 )
    {
        error = "non-zero reserved file header field";
        return false;
    }
    const auto expectedCrc = Get32( bytes.data(), 48 );
    if( CrcWithZeroedField( bytes, 48 ) != expectedCrc )
    {
        error = "file header CRC32C mismatch";
        return false;
    }

    value.flags = Get32( bytes.data(), 12 );
    value.protocolVersion = Get32( bytes.data(), 16 );
    std::copy_n( bytes.begin() + 24, value.sessionId.size(), value.sessionId.begin() );
    value.createdUnixNs = Get64( bytes.data(), 40 );
    return true;
}

std::string ErrnoMessage( const char* operation )
{
    std::ostringstream out;
    out << operation;
    if( errno != 0 ) out << ": " << std::strerror( errno );
    return out.str();
}

#ifdef _WIN32
std::string WindowsErrorMessage( const char* operation )
{
    const auto code = GetLastError();
    std::ostringstream out;
    out << operation << " failed with Windows error " << code;
    return out.str();
}
#endif

bool TruncateFileDurably( const std::filesystem::path& path, uint64_t expectedSize, uint64_t newSize, std::string& error )
{
#ifdef _WIN32
    if( newSize > uint64_t( std::numeric_limits<LONGLONG>::max() ) )
    {
        error = "truncate offset exceeds the Windows signed 64-bit file API";
        return false;
    }
    HANDLE handle = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( handle == INVALID_HANDLE_VALUE )
    {
        error = WindowsErrorMessage( "CreateFileW for truncation" );
        return false;
    }
    const auto fail = [&]( const char* operation ) {
        error = WindowsErrorMessage( operation );
        CloseHandle( handle );
        return false;
    };

    LARGE_INTEGER current = {};
    if( !GetFileSizeEx( handle, &current ) ) return fail( "GetFileSizeEx before truncation" );
    if( current.QuadPart < 0 || uint64_t( current.QuadPart ) != expectedSize )
    {
        CloseHandle( handle );
        error = "journal changed after scan; refusing stale truncation";
        return false;
    }

    LARGE_INTEGER target = {};
    target.QuadPart = LONGLONG( newSize );
    if( !SetFilePointerEx( handle, target, nullptr, FILE_BEGIN ) ) return fail( "SetFilePointerEx for truncation" );
    if( !SetEndOfFile( handle ) ) return fail( "SetEndOfFile" );
    if( !FlushFileBuffers( handle ) ) return fail( "FlushFileBuffers after truncation" );
    CloseHandle( handle );
    return true;
#else
    if( newSize > uint64_t( std::numeric_limits<off_t>::max() ) )
    {
        error = "truncate offset exceeds the POSIX file API";
        return false;
    }
    const int descriptor = open( path.c_str(), O_RDWR );
    if( descriptor == -1 )
    {
        error = ErrnoMessage( "open for truncation" );
        return false;
    }
    const auto fail = [&]( const char* operation ) {
        error = ErrnoMessage( operation );
        close( descriptor );
        return false;
    };

    struct stat status = {};
    if( fstat( descriptor, &status ) != 0 ) return fail( "fstat before truncation" );
    if( status.st_size < 0 || uint64_t( status.st_size ) != expectedSize )
    {
        close( descriptor );
        error = "journal changed after scan; refusing stale truncation";
        return false;
    }
    if( ftruncate( descriptor, off_t( newSize ) ) != 0 ) return fail( "ftruncate" );
    if( fsync( descriptor ) != 0 ) return fail( "fsync after truncation" );
    close( descriptor );
    return true;
#endif
}

class FileJournalSink final : public JournalSink
{
public:
    explicit FileJournalSink( FILE* file )
        : m_file( file )
    {
    }

    ~FileJournalSink() override
    {
        if( m_file ) std::fclose( m_file );
    }

    static std::unique_ptr<FileJournalSink> Create( const std::filesystem::path& path, bool overwrite, std::string& error )
    {
#ifdef _WIN32
        const DWORD creation = overwrite ? CREATE_ALWAYS : CREATE_NEW;
        // Live readers are allowed, but a second writer must not be able to
        // mutate the append-only journal while this sink owns it.
        HANDLE handle = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            error = WindowsErrorMessage( overwrite ? "CreateFileW(CREATE_ALWAYS)" : "CreateFileW(CREATE_NEW)" );
            return {};
        }
        const int descriptor = _open_osfhandle( reinterpret_cast<intptr_t>( handle ), _O_BINARY | _O_RDWR );
        if( descriptor == -1 )
        {
            CloseHandle( handle );
            error = ErrnoMessage( "_open_osfhandle" );
            return {};
        }
        FILE* file = _fdopen( descriptor, "r+b" );
        if( !file )
        {
            _close( descriptor );
            error = ErrnoMessage( "_fdopen" );
            return {};
        }
#else
        int flags = O_CREAT | O_RDWR;
        flags |= overwrite ? O_TRUNC : O_EXCL;
        const int descriptor = open( path.c_str(), flags, 0666 );
        if( descriptor == -1 )
        {
            error = ErrnoMessage( overwrite ? "open(O_TRUNC)" : "open(O_EXCL)" );
            return {};
        }
        FILE* file = fdopen( descriptor, "r+b" );
        if( !file )
        {
            close( descriptor );
            error = ErrnoMessage( "fdopen" );
            return {};
        }
#endif
        return std::unique_ptr<FileJournalSink>( new FileJournalSink( file ) );
    }

    static std::unique_ptr<FileJournalSink> OpenExisting( const std::filesystem::path& path, std::string& error )
    {
#ifdef _WIN32
        HANDLE handle = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            error = WindowsErrorMessage( "CreateFileW(OPEN_EXISTING)" );
            return {};
        }
        const int descriptor = _open_osfhandle( reinterpret_cast<intptr_t>( handle ), _O_BINARY | _O_RDWR );
        if( descriptor == -1 )
        {
            CloseHandle( handle );
            error = ErrnoMessage( "_open_osfhandle" );
            return {};
        }
        FILE* file = _fdopen( descriptor, "r+b" );
        if( !file )
        {
            _close( descriptor );
            error = ErrnoMessage( "_fdopen" );
            return {};
        }
#else
        const int descriptor = open( path.c_str(), O_RDWR );
        if( descriptor == -1 )
        {
            error = ErrnoMessage( "open(O_RDWR)" );
            return {};
        }
        FILE* file = fdopen( descriptor, "r+b" );
        if( !file )
        {
            close( descriptor );
            error = ErrnoMessage( "fdopen" );
            return {};
        }
#endif
        return std::unique_ptr<FileJournalSink>( new FileJournalSink( file ) );
    }

    bool Seek( uint64_t offset, std::string& error )
    {
        if( offset > uint64_t( std::numeric_limits<int64_t>::max() ) )
        {
            error = "seek offset exceeds signed 64-bit file API";
            return false;
        }
#ifdef _WIN32
        if( _fseeki64( m_file, int64_t( offset ), SEEK_SET ) != 0 )
#else
        if( fseeko( m_file, off_t( offset ), SEEK_SET ) != 0 )
#endif
        {
            error = ErrnoMessage( "seek" );
            return false;
        }
        return true;
    }

    bool Write( const uint8_t* data, size_t size, size_t& written, std::string& error ) override
    {
        errno = 0;
        written = std::fwrite( data, 1, size, m_file );
        if( written < size && std::ferror( m_file ) )
        {
            error = ErrnoMessage( "fwrite" );
            return false;
        }
        return true;
    }

    bool Flush( bool durable, std::string& error ) override
    {
        errno = 0;
        if( std::fflush( m_file ) != 0 )
        {
            error = ErrnoMessage( "fflush" );
            return false;
        }
        if( !durable ) return true;

#ifdef _WIN32
        const int descriptor = _fileno( m_file );
        if( descriptor == -1 )
        {
            error = ErrnoMessage( "_fileno" );
            return false;
        }
        const auto handle = reinterpret_cast<HANDLE>( _get_osfhandle( descriptor ) );
        if( handle == INVALID_HANDLE_VALUE )
        {
            error = ErrnoMessage( "_get_osfhandle" );
            return false;
        }
        if( !FlushFileBuffers( handle ) )
        {
            error = WindowsErrorMessage( "FlushFileBuffers" );
            return false;
        }
#else
        if( fsync( fileno( m_file ) ) != 0 )
        {
            error = ErrnoMessage( "fsync" );
            return false;
        }
#endif
        return true;
    }

    uint64_t Tell() const override
    {
#ifdef _WIN32
        const auto position = _ftelli64( m_file );
#else
        const auto position = ftello( m_file );
#endif
        return position < 0 ? std::numeric_limits<uint64_t>::max() : uint64_t( position );
    }

private:
    FILE* m_file = nullptr;
};

class ScanSource
{
public:
    virtual ~ScanSource() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Read( uint64_t offset, uint8_t* output, size_t size, std::string& error ) = 0;
};

class MemoryScanSource final : public ScanSource
{
public:
    explicit MemoryScanSource( std::span<const uint8_t> bytes )
        : m_bytes( bytes )
    {
    }

    uint64_t Size() const override { return m_bytes.size(); }

    bool Read( uint64_t offset, uint8_t* output, size_t size, std::string& error ) override
    {
        if( offset > m_bytes.size() || size > m_bytes.size() - size_t( offset ) )
        {
            error = "memory scan read is out of range";
            return false;
        }
        std::copy_n( m_bytes.data() + size_t( offset ), size, output );
        return true;
    }

private:
    std::span<const uint8_t> m_bytes;
};

class FileScanSource final : public ScanSource
{
public:
    static std::unique_ptr<FileScanSource> Open( const std::filesystem::path& path, uint64_t maximumSize, std::string& error )
    {
        auto file = std::ifstream( path, std::ios::binary );
        if( !file )
        {
            error = "cannot open journal for reading";
            return {};
        }
        file.seekg( 0, std::ios::end );
        const auto end = file.tellg();
        if( end < 0 )
        {
            error = "cannot determine journal size";
            return {};
        }
        const auto fileSize = uint64_t( end );
        if( maximumSize > fileSize )
        {
            error = "journal is smaller than the requested prefix";
            return {};
        }
        file.seekg( 0, std::ios::beg );
        return std::unique_ptr<FileScanSource>( new FileScanSource( std::move( file ), maximumSize ) );
    }

    static std::unique_ptr<FileScanSource> Open( const std::filesystem::path& path, std::string& error )
    {
        auto file = std::ifstream( path, std::ios::binary );
        if( !file )
        {
            error = "cannot open journal for reading";
            return {};
        }
        file.seekg( 0, std::ios::end );
        const auto end = file.tellg();
        if( end < 0 )
        {
            error = "cannot determine journal size";
            return {};
        }
        file.seekg( 0, std::ios::beg );
        return std::unique_ptr<FileScanSource>( new FileScanSource( std::move( file ), uint64_t( end ) ) );
    }

    uint64_t Size() const override { return m_size; }

    bool Read( uint64_t offset, uint8_t* output, size_t size, std::string& error ) override
    {
        if( offset > m_size || uint64_t( size ) > m_size - offset )
        {
            error = "file scan read is out of range";
            return false;
        }
        if( offset > uint64_t( std::numeric_limits<std::streamoff>::max() ) )
        {
            error = "file scan offset exceeds stream API";
            return false;
        }
        m_file.clear();
        m_file.seekg( std::streamoff( offset ), std::ios::beg );
        if( !m_file )
        {
            error = "journal seek failed";
            return false;
        }
        m_file.read( reinterpret_cast<char*>( output ), std::streamsize( size ) );
        if( size != 0 && ( !m_file || m_file.gcount() != std::streamsize( size ) ) )
        {
            error = "journal read failed or file changed during scan";
            return false;
        }
        return true;
    }

private:
    FileScanSource( std::ifstream file, uint64_t size )
        : m_file( std::move( file ) )
        , m_size( size )
    {
    }

    std::ifstream m_file;
    uint64_t m_size = 0;
};

ScanResult Scan( ScanSource& source, const ScanOptions& options )
{
    ScanResult result;
    result.fileSize = source.Size();

    if( result.fileSize < FileHeaderSize )
    {
        result.code = ScanCode::TruncatedFileHeader;
        result.message = "journal ends before the 64-byte file header";
        return result;
    }

    std::array<uint8_t, FileHeaderSize> fileHeaderBytes = {};
    if( !source.Read( 0, fileHeaderBytes.data(), fileHeaderBytes.size(), result.message ) )
    {
        result.code = ScanCode::IoError;
        return result;
    }
    if( !DecodeFileHeader( fileHeaderBytes, result.header, result.message ) )
    {
        result.code = ScanCode::InvalidFileHeader;
        return result;
    }

    result.code = ScanCode::Ok;
    result.validSize = FileHeaderSize;
    Crc32cState prefixState;
    prefixState.Update( fileHeaderBytes.data(), fileHeaderBytes.size() );
    result.prefixCrc32c = prefixState.Finish();

    uint64_t offset = FileHeaderSize;
    uint64_t expectedSequence = 1;
    uint64_t previousTimestamp = 0;
    bool ended = false;
    std::array<uint8_t, RecordHeaderSize> headerBytes = {};
    std::array<uint8_t, RecordTrailerSize> trailerBytes = {};
    std::array<uint8_t, ScanChunkSize> payloadBuffer = {};

    const auto tailFailure = [&]( ScanCode code, const std::string& message ) {
        result.code = code;
        result.message = message;
        return result;
    };

    while( offset < result.fileSize )
    {
        if( ended )
        {
            return tailFailure( ScanCode::CorruptTail, "bytes follow the terminal SessionEnd record" );
        }
        const uint64_t remaining = result.fileSize - offset;
        if( remaining < RecordHeaderSize )
        {
            return tailFailure( ScanCode::TruncatedTail, "journal ends inside a record header" );
        }
        if( !source.Read( offset, headerBytes.data(), headerBytes.size(), result.message ) )
        {
            result.code = ScanCode::IoError;
            return result;
        }
        if( !HasMagic( RecordMagic, headerBytes.data() ) )
        {
            return tailFailure( ScanCode::CorruptTail, "record magic mismatch" );
        }
        if( Get16( headerBytes.data(), 4 ) != RecordHeaderSize )
        {
            return tailFailure( ScanCode::CorruptTail, "invalid record header size" );
        }
        if( Get32( headerBytes.data(), 12 ) != 0 )
        {
            return tailFailure( ScanCode::CorruptTail, "non-zero reserved record header field" );
        }
        if( CrcWithZeroedField( headerBytes, 44 ) != Get32( headerBytes.data(), 44 ) )
        {
            return tailFailure( ScanCode::CorruptTail, "record header CRC32C mismatch" );
        }

        const auto sequence = Get64( headerBytes.data(), 16 );
        const auto monotonicNs = Get64( headerBytes.data(), 24 );
        const auto payloadSize = Get64( headerBytes.data(), 32 );
        const auto payloadCrc = Get32( headerBytes.data(), 40 );
        if( sequence != expectedSequence )
        {
            return tailFailure( ScanCode::CorruptTail, "record sequence is not contiguous" );
        }
        if( sequence != 1 && monotonicNs < previousTimestamp )
        {
            return tailFailure( ScanCode::CorruptTail, "record monotonic timestamp moved backwards" );
        }
        if( payloadSize > options.maxPayloadSize )
        {
            return tailFailure( ScanCode::CorruptTail, "record payload exceeds configured scan limit" );
        }
        if( payloadSize > std::numeric_limits<uint64_t>::max() - RecordHeaderSize - RecordTrailerSize )
        {
            return tailFailure( ScanCode::CorruptTail, "record size overflows uint64" );
        }
        const uint64_t totalRecordSize = RecordHeaderSize + payloadSize + RecordTrailerSize;
        if( totalRecordSize > remaining )
        {
            return tailFailure( ScanCode::TruncatedTail, "journal ends inside a record payload or commit trailer" );
        }

        Crc32cState payloadState;
        Crc32cState commitState;
        auto candidatePrefixState = prefixState;
        commitState.Update( headerBytes.data(), headerBytes.size() );
        candidatePrefixState.Update( headerBytes.data(), headerBytes.size() );
        uint64_t payloadOffset = offset + RecordHeaderSize;
        uint64_t payloadRemaining = payloadSize;
        while( payloadRemaining != 0 )
        {
            const size_t chunk = size_t( std::min<uint64_t>( payloadRemaining, payloadBuffer.size() ) );
            if( !source.Read( payloadOffset, payloadBuffer.data(), chunk, result.message ) )
            {
                result.code = ScanCode::IoError;
                return result;
            }
            payloadState.Update( payloadBuffer.data(), chunk );
            commitState.Update( payloadBuffer.data(), chunk );
            candidatePrefixState.Update( payloadBuffer.data(), chunk );
            payloadOffset += chunk;
            payloadRemaining -= chunk;
        }

        if( payloadState.Finish() != payloadCrc )
        {
            return tailFailure( ScanCode::CorruptTail, "record payload CRC32C mismatch" );
        }

        const uint64_t trailerOffset = offset + RecordHeaderSize + payloadSize;
        if( !source.Read( trailerOffset, trailerBytes.data(), trailerBytes.size(), result.message ) )
        {
            result.code = ScanCode::IoError;
            return result;
        }
        if( !HasMagic( TrailerMagic, trailerBytes.data() ) )
        {
            return tailFailure( ScanCode::CorruptTail, "commit trailer magic mismatch" );
        }
        if( Get16( trailerBytes.data(), 4 ) != RecordTrailerSize || Get16( trailerBytes.data(), 6 ) != TrailerVersion )
        {
            return tailFailure( ScanCode::CorruptTail, "invalid commit trailer version or size" );
        }
        if( Get64( trailerBytes.data(), 8 ) != sequence )
        {
            return tailFailure( ScanCode::CorruptTail, "commit trailer sequence mismatch" );
        }
        if( Get64( trailerBytes.data(), 16 ) != totalRecordSize )
        {
            return tailFailure( ScanCode::CorruptTail, "commit trailer record size mismatch" );
        }
        if( CrcWithZeroedField( trailerBytes, 28 ) != Get32( trailerBytes.data(), 28 ) )
        {
            return tailFailure( ScanCode::CorruptTail, "commit trailer CRC32C mismatch" );
        }
        commitState.Update( trailerBytes.data(), 24 );
        if( commitState.Finish() != Get32( trailerBytes.data(), 24 ) )
        {
            return tailFailure( ScanCode::CorruptTail, "record commit CRC32C mismatch" );
        }
        candidatePrefixState.Update( trailerBytes.data(), trailerBytes.size() );

        const auto type = RecordType( Get16( headerBytes.data(), 6 ) );
        const RecordInfo record {
            offset,
            sequence,
            monotonicNs,
            payloadSize,
            Get32( headerBytes.data(), 8 ),
            type };
        if( options.recordVisitor ) options.recordVisitor( record, options.recordVisitorUserData );
        if( result.records.size() < options.maxCollectedRecords )
        {
            result.records.emplace_back( record );
        }
        result.recordCount++;
        result.lastSequence = sequence;
        result.lastMonotonicNs = monotonicNs;
        result.validSize = offset + totalRecordSize;
        result.complete = type == RecordType::SessionEnd;
        prefixState = candidatePrefixState;
        result.prefixCrc32c = prefixState.Finish();
        ended = result.complete;
        previousTimestamp = monotonicNs;
        expectedSequence++;
        offset = result.validSize;
        if( !ended && options.stopRequested && options.stopRequested( options.stopRequestedUserData ) )
        {
            result.code = ScanCode::Stopped;
            result.message = "journal scan stopped at a committed record boundary";
            return result;
        }
    }

    result.message = result.complete ? "journal is complete" : "journal has a valid incomplete session";
    return result;
}

}

uint32_t Crc32c( std::span<const uint8_t> bytes )
{
    Crc32cState state;
    state.Update( bytes.data(), bytes.size() );
    return state.Finish();
}

FileHeader MakeFileHeader( uint32_t protocolVersion, uint32_t flags )
{
    FileHeader header;
    header.protocolVersion = protocolVersion;
    header.flags = flags;
    header.createdUnixNs = uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::system_clock::now().time_since_epoch() ).count() );

    std::random_device randomDevice;
    std::seed_seq seed {
        randomDevice(),
        randomDevice(),
        uint32_t( header.createdUnixNs ),
        uint32_t( header.createdUnixNs >> 32 )
    };
    std::mt19937_64 generator( seed );
    for( size_t i = 0; i < header.sessionId.size(); i += 8 )
    {
        const auto value = generator();
        for( size_t j = 0; j < 8; j++ )
        {
            header.sessionId[i + j] = uint8_t( value >> ( j * 8 ) );
        }
    }
    return header;
}

JournalWriter::JournalWriter( std::unique_ptr<JournalSink> sink, const FileHeader& header, const WriterOptions& options )
    : m_sink( std::move( sink ) )
    , m_header( header )
    , m_options( options )
{
}

JournalWriter::~JournalWriter() = default;

std::unique_ptr<JournalWriter> JournalWriter::CreateFileJournal( const std::filesystem::path& path, const FileHeader& header, bool overwrite, const WriterOptions& options, std::string& error )
{
    auto sink = FileJournalSink::Create( path, overwrite, error );
    if( !sink ) return {};
    return Create( std::move( sink ), header, options, error );
}

std::unique_ptr<JournalWriter> JournalWriter::Create( std::unique_ptr<JournalSink> sink, const FileHeader& header, const WriterOptions& options, std::string& error )
{
    error.clear();
    if( !sink )
    {
        error = "journal sink is null";
        return {};
    }
    if( options.maxPayloadSize == 0 )
    {
        error = "maximum payload size must be non-zero";
        return {};
    }
    if( sink->Tell() != 0 )
    {
        error = "new journal sink is not positioned at byte zero";
        return {};
    }

    auto writer = std::unique_ptr<JournalWriter>( new JournalWriter( std::move( sink ), header, options ) );
    if( !writer->WriteInitialHeader( error ) ) return {};
    return writer;
}

std::unique_ptr<JournalWriter> JournalWriter::ResumeFile( const std::filesystem::path& path, bool truncateInvalidTail, const WriterOptions& options, std::string& error )
{
    error.clear();
    ScanOptions scanOptions;
    scanOptions.maxPayloadSize = options.maxPayloadSize;
    scanOptions.maxCollectedRecords = 0;
    auto scan = ScanJournal( path, scanOptions );
    if( !scan.HasValidHeader() )
    {
        error = std::string( "cannot resume journal: " ) + scan.message;
        return {};
    }
    if( scan.complete )
    {
        error = "cannot resume a journal after SessionEnd";
        return {};
    }
    if( scan.code != ScanCode::Ok )
    {
        if( !truncateInvalidTail )
        {
            error = "journal has an invalid tail; explicit truncation is required before resume";
            return {};
        }
        if( !TruncateToValidPrefix( path, scan, error ) ) return {};
    }

    auto sink = FileJournalSink::OpenExisting( path, error );
    if( !sink ) return {};
    if( !sink->Seek( scan.validSize, error ) ) return {};

    auto writer = std::unique_ptr<JournalWriter>( new JournalWriter( std::move( sink ), scan.header, options ) );
    writer->m_nextSequence = scan.lastSequence + 1;
    writer->m_committedSize = scan.validSize;
    writer->m_publishedSize = scan.validSize;
    writer->m_lastMonotonicNs = scan.lastMonotonicNs;
    if( !writer->Flush( FlushMode::Durable, error ) ) return {};
    return writer;
}

bool JournalWriter::WriteInitialHeader( std::string& error )
{
    const auto bytes = EncodeFileHeader( m_header );
    if( !WriteAll( bytes.data(), bytes.size(), error ) ) return false;
    m_committedSize = FileHeaderSize;
    return Flush( m_options.durableHeader ? FlushMode::Durable : FlushMode::Published, error );
}

bool JournalWriter::WriteAll( const uint8_t* data, size_t size, std::string& error )
{
    size_t offset = 0;
    while( offset < size )
    {
        size_t written = 0;
        std::string sinkError;
        const bool ok = m_sink->Write( data + offset, size - offset, written, sinkError );
        if( written > size - offset )
        {
            return FailIo( "journal sink reported an impossible write size", error );
        }
        offset += written;
        if( !ok )
        {
            return FailIo( sinkError.empty() ? "journal sink write failed" : sinkError, error );
        }
        if( written == 0 )
        {
            return FailIo( "journal sink made zero progress during write", error );
        }
    }
    return true;
}

bool JournalWriter::FailIo( const std::string& message, std::string& error )
{
    m_healthy = false;
    error = message;
    return false;
}

bool JournalWriter::Append( RecordType type, uint32_t flags, std::string_view payload, uint64_t monotonicNs, std::string& error )
{
    return Append( type, flags, std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( payload.data() ), payload.size() ), monotonicNs, error );
}

bool JournalWriter::Append( RecordType type, uint32_t flags, std::span<const uint8_t> payload, uint64_t monotonicNs, std::string& error )
{
    const PayloadSpan segment { payload.data(), payload.size() };
    return Append( type, flags, std::span<const PayloadSpan>( &segment, 1 ), monotonicNs, error );
}

bool JournalWriter::Append( RecordType type, uint32_t flags, std::span<const PayloadSpan> payload, uint64_t monotonicNs, std::string& error )
{
    error.clear();
    if( !m_healthy )
    {
        error = "journal writer is poisoned by an earlier I/O failure";
        return false;
    }
    uint64_t payloadSize = 0;
    Crc32cState payloadState;
    for( const auto& segment : payload )
    {
        if( segment.size != 0 && segment.data == nullptr )
        {
            error = "record payload segment has a null pointer";
            return false;
        }
        if( segment.size > m_options.maxPayloadSize - payloadSize )
        {
            error = "record payload exceeds configured writer limit";
            return false;
        }
        payloadSize += segment.size;
        payloadState.Update( segment.data, segment.size );
    }
    if( payloadSize > m_options.maxPayloadSize )
    {
        error = "record payload exceeds configured writer limit";
        return false;
    }
    if( m_nextSequence == 0 )
    {
        error = "record sequence overflow";
        return false;
    }
    if( m_nextSequence != 1 && monotonicNs < m_lastMonotonicNs )
    {
        error = "record monotonic timestamp moved backwards";
        return false;
    }

    std::array<uint8_t, RecordHeaderSize> headerBytes = {};
    std::copy( RecordMagic.begin(), RecordMagic.end(), headerBytes.begin() );
    Put16( headerBytes.data(), 4, uint16_t( RecordHeaderSize ) );
    Put16( headerBytes.data(), 6, uint16_t( type ) );
    Put32( headerBytes.data(), 8, flags );
    Put64( headerBytes.data(), 16, m_nextSequence );
    Put64( headerBytes.data(), 24, monotonicNs );
    Put64( headerBytes.data(), 32, payloadSize );
    Put32( headerBytes.data(), 40, payloadState.Finish() );
    Put32( headerBytes.data(), 44, CrcWithZeroedField( headerBytes, 44 ) );

    const uint64_t totalRecordSize = RecordHeaderSize + payloadSize + RecordTrailerSize;
    std::array<uint8_t, RecordTrailerSize> trailerBytes = {};
    std::copy( TrailerMagic.begin(), TrailerMagic.end(), trailerBytes.begin() );
    Put16( trailerBytes.data(), 4, uint16_t( RecordTrailerSize ) );
    Put16( trailerBytes.data(), 6, TrailerVersion );
    Put64( trailerBytes.data(), 8, m_nextSequence );
    Put64( trailerBytes.data(), 16, totalRecordSize );

    Crc32cState commitState;
    commitState.Update( headerBytes.data(), headerBytes.size() );
    for( const auto& segment : payload )
    {
        commitState.Update( segment.data, segment.size );
    }
    commitState.Update( trailerBytes.data(), 24 );
    Put32( trailerBytes.data(), 24, commitState.Finish() );
    Put32( trailerBytes.data(), 28, CrcWithZeroedField( trailerBytes, 28 ) );

    if( totalRecordSize > std::numeric_limits<uint64_t>::max() - m_committedSize )
    {
        error = "journal committed offset overflows uint64";
        return false;
    }
    const uint64_t expectedEnd = m_committedSize + totalRecordSize;
    if( !WriteAll( headerBytes.data(), headerBytes.size(), error ) ) return false;
    for( const auto& segment : payload )
    {
        if( segment.size != 0 && !WriteAll( segment.data, segment.size, error ) ) return false;
    }
    if( !WriteAll( trailerBytes.data(), trailerBytes.size(), error ) ) return false;
    if( m_sink->Tell() != expectedEnd )
    {
        return FailIo( "journal sink position does not match committed record size", error );
    }

    m_committedSize = expectedEnd;
    m_lastMonotonicNs = monotonicNs;
    m_nextSequence++;

    const bool durabilityBoundary = type == RecordType::SessionEnd || ( flags & RecordFlagDurabilityBoundary ) != 0;
    if( durabilityBoundary ) return Flush( FlushMode::Durable, error );
    if( m_options.publishEachRecord ) return Flush( FlushMode::Published, error );
    return true;
}

bool JournalWriter::Flush( FlushMode mode, std::string& error )
{
    error.clear();
    if( !m_healthy )
    {
        error = "journal writer is poisoned by an earlier I/O failure";
        return false;
    }
    std::string sinkError;
    const bool durable = mode == FlushMode::Durable;
    if( !m_sink->Flush( durable, sinkError ) )
    {
        return FailIo( sinkError.empty() ? "journal sink flush failed" : sinkError, error );
    }
    m_publishedSize = m_committedSize;
    if( durable ) m_durableSize = m_committedSize;
    return true;
}

bool ScanResult::HasValidHeader() const
{
    return ( code == ScanCode::Ok || code == ScanCode::Stopped || code == ScanCode::TruncatedTail || code == ScanCode::CorruptTail || code == ScanCode::IoError ) && validSize >= FileHeaderSize;
}

bool ScanResult::HasRecoverablePrefix() const
{
    return ( code == ScanCode::Ok || code == ScanCode::Stopped || code == ScanCode::TruncatedTail || code == ScanCode::CorruptTail ) && validSize >= FileHeaderSize;
}

ScanResult ScanJournal( const std::filesystem::path& path, const ScanOptions& options )
{
    std::string error;
    auto source = FileScanSource::Open( path, error );
    if( !source )
    {
        ScanResult result;
        result.code = ScanCode::IoError;
        result.message = std::move( error );
        return result;
    }
    return Scan( *source, options );
}

ScanResult ScanJournalPrefix( const std::filesystem::path& path, uint64_t prefixSize, const ScanOptions& options )
{
    std::string error;
    auto source = FileScanSource::Open( path, prefixSize, error );
    if( !source )
    {
        ScanResult result;
        result.code = ScanCode::IoError;
        result.message = std::move( error );
        return result;
    }
    return Scan( *source, options );
}

ScanResult ScanJournal( std::span<const uint8_t> bytes, const ScanOptions& options )
{
    MemoryScanSource source( bytes );
    return Scan( source, options );
}

bool TruncateToValidPrefix( const std::filesystem::path& path, const ScanResult& scan, std::string& error )
{
    error.clear();
    if( !scan.HasRecoverablePrefix() )
    {
        error = "scan result has no valid journal prefix";
        return false;
    }
    if( scan.validSize > scan.fileSize )
    {
        error = "scan result valid prefix exceeds the recorded file size";
        return false;
    }

    std::error_code ec;
    const auto currentSize = std::filesystem::file_size( path, ec );
    if( ec )
    {
        error = std::string( "cannot stat journal before truncation: " ) + ec.message();
        return false;
    }
    if( currentSize != scan.fileSize )
    {
        error = "journal changed after scan; refusing stale truncation";
        return false;
    }
    if( currentSize == scan.validSize ) return true;

    return TruncateFileDurably( path, scan.fileSize, scan.validSize, error );
}

const char* RecordTypeName( RecordType type )
{
    switch( type )
    {
    case RecordType::SessionBegin: return "SessionBegin";
    case RecordType::ClientToServer: return "ClientToServer";
    case RecordType::ServerToClient: return "ServerToClient";
    case RecordType::Checkpoint: return "Checkpoint";
    case RecordType::SessionEnd: return "SessionEnd";
    case RecordType::Diagnostic: return "Diagnostic";
    default: return "Unknown";
    }
}

const char* ScanCodeName( ScanCode code )
{
    switch( code )
    {
    case ScanCode::Ok: return "OK";
    case ScanCode::Stopped: return "STOPPED";
    case ScanCode::TruncatedFileHeader: return "TRUNCATED_FILE_HEADER";
    case ScanCode::InvalidFileHeader: return "INVALID_FILE_HEADER";
    case ScanCode::TruncatedTail: return "TRUNCATED_TAIL";
    case ScanCode::CorruptTail: return "CORRUPT_TAIL";
    case ScanCode::IoError: return "IO_ERROR";
    default: return "UNKNOWN";
    }
}

}
