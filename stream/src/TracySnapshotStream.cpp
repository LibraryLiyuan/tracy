#include "TracySnapshotStream.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#  include <Windows.h>
#  include <bcrypt.h>
#endif

namespace tracy::stream
{
namespace
{
constexpr size_t BlockBytes = 64 * 1024;
constexpr uint8_t Signature[] = { 't', 'r', 253, 'P' };
void Check( bool value, const char* error )
{
    if( !value ) throw std::runtime_error( error );
}
void Cancel( const SnapshotStreamOptions& options )
{
    if( options.cancelled && options.cancelled() ) throw std::runtime_error( "Cancelled" );
}
#ifdef _WIN32
class File
{
public:
    File( const std::filesystem::path& path, bool write )
        : path( path ), discard( write )
    {
        handle = CreateFileW( path.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr, write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr );
        Check( handle != INVALID_HANDLE_VALUE, write ? "Cannot create new snapshot temporary file" : "Cannot open snapshot stream" );
    }
    ~File()
    {
        if( handle != INVALID_HANDLE_VALUE ) CloseHandle( handle );
        if( discard ) { std::error_code error; std::filesystem::remove( path, error ); }
    }
    void Write( std::span<const uint8_t> bytes )
    {
        while( !bytes.empty() )
        {
            DWORD written = 0;
            const auto size = DWORD( std::min<size_t>( bytes.size(), MAXDWORD ) );
            Check( WriteFile( handle, bytes.data(), size, &written, nullptr ) && written > 0, "Snapshot write failed" );
            bytes = bytes.subspan( written );
        }
    }
    size_t Read( std::span<uint8_t> bytes )
    {
        DWORD read = 0;
        Check( ReadFile( handle, bytes.data(), DWORD( std::min<size_t>( bytes.size(), MAXDWORD ) ), &read, nullptr ) != 0, "Snapshot read failed" );
        return read;
    }
    void Flush() { Check( FlushFileBuffers( handle ) != 0, "Snapshot flush failed" ); }
    void Keep() { discard = false; }
    void Close()
    {
        Check( CloseHandle( handle ) != 0, "Snapshot close failed" );
        handle = INVALID_HANDLE_VALUE;
    }

private:
    std::filesystem::path path;
    bool discard;
    HANDLE handle = INVALID_HANDLE_VALUE;
};
class Hash
{
public:
    Hash()
    {
        Check( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) == 0, "Snapshot SHA256 initialization failed" );
        if( BCryptCreateHash( algorithm, &hash, nullptr, 0, nullptr, 0, 0 ) != 0 )
        {
            BCryptCloseAlgorithmProvider( algorithm, 0 );
            algorithm = nullptr;
            throw std::runtime_error( "Snapshot SHA256 creation failed" );
        }
    }
    ~Hash()
    {
        if( hash ) BCryptDestroyHash( hash );
        if( algorithm ) BCryptCloseAlgorithmProvider( algorithm, 0 );
    }
    void Update( std::span<const uint8_t> bytes )
    {
        while( !bytes.empty() )
        {
            const auto n = ULONG( std::min<size_t>( bytes.size(), UINT32_MAX ) );
            Check( BCryptHashData( hash, const_cast<PUCHAR>( bytes.data() ), n, 0 ) == 0, "Snapshot SHA256 update failed" );
            bytes = bytes.subspan( n );
        }
    }
    std::array<uint8_t, 32> Finish()
    {
        std::array<uint8_t, 32> value{};
        Check( BCryptFinishHash( hash, value.data(), ULONG( value.size() ), 0 ) == 0, "Snapshot SHA256 finish failed" );
        return value;
    }

private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};
#else
#  error Snapshot stream currently targets Windows x64, as specified by the implementation plan.
#endif
void Exact( File& file, std::span<uint8_t> bytes )
{
    while( !bytes.empty() )
    {
        const auto n = file.Read( bytes );
        Check( n > 0, "Truncated snapshot stream" );
        bytes = bytes.subspan( n );
    }
}
}
struct SnapshotStreamWriter::Impl
{
    struct Slot
    {
        std::array<uint8_t, BlockBytes> input{}, dictionary{};
        std::vector<uint8_t> output = std::vector<uint8_t>( LZ4_compressBound( int( BlockBytes ) ) );
        LZ4_stream_t* codec = LZ4_createStream();
        std::thread thread;
        std::mutex mutex;
        std::condition_variable signal;
        size_t size = 0;
        int compressed = 0;
        bool pending = false, ready = false, stop = false;
        Slot() { Check( codec != nullptr, "Cannot allocate snapshot encoder" ); }
        ~Slot() { LZ4_freeStream( codec ); }
        void Run()
        {
            std::unique_lock lock( mutex );
            for( ;; )
            {
                signal.wait( lock, [&] {  return pending  ||  stop; } );
                if( stop ) return;
                compressed = LZ4_compress_fast_continue( codec, reinterpret_cast<const char*>( input.data() ), reinterpret_cast<char*>( output.data() ), int( size ), int( output.size() ), 1 );
                if( compressed > 0 ) LZ4_saveDict( codec, reinterpret_cast<char*>( dictionary.data() ), int( dictionary.size() ) );
                pending = false;
                ready = true;
                signal.notify_all();
            }
        }
    };
    SnapshotStreamOptions options;
    File file;
    Hash hash;
    SnapshotStreamStats stats;
    std::vector<std::unique_ptr<Slot>> slots;
    size_t current = 0, pending = 0, offset = 0;
    bool finished = false;
    Impl( const std::filesystem::path& path, const SnapshotStreamOptions& opts )
        : options( opts )
        , file( path, true )
    {
        Check( options.streams > 0 && options.streams <= 16 && options.maxFileBytes >= 6, "Invalid snapshot stream budget" );
        Cancel( options );
        const uint8_t header[] = { Signature[0], Signature[1], Signature[2], Signature[3], 0, options.streams };
        file.Write( header );
        stats.fileBytes = sizeof( header );
        for( uint8_t n = 0; n < options.streams; ++n ) slots.push_back( std::make_unique<Slot>() );
        try
        {
            for( auto& slot : slots ) slot->thread = std::thread( [p = slot.get()] {  p->Run(); } );
        }
        catch( ... )
        {
            Stop();
            throw;
        }
    }
    ~Impl() { Stop(); }
    void Stop()
    {
        for( auto& slot : slots )
        {
            {
                std::lock_guard lock( slot->mutex );
                slot->stop = true;
                slot->signal.notify_all();
            }
        }
        for( auto& slot : slots )
            if( slot->thread.joinable() ) slot->thread.join();
    }
    void Drain()
    {
        Cancel( options );
        auto& slot = *slots[( current + slots.size() - pending ) % slots.size()];
        std::unique_lock lock( slot.mutex );
        slot.signal.wait( lock, [&] {  return slot.ready; } );
        Check( slot.compressed > 0, "Snapshot compression failed" );
        const auto size = uint32_t( slot.compressed );
        Check( stats.fileBytes <= options.maxFileBytes && uint64_t( size ) + 4 <= options.maxFileBytes - stats.fileBytes, "Snapshot disk budget exceeded" );
        file.Write( { reinterpret_cast<const uint8_t*>( &size ), 4 } );
        file.Write( { slot.output.data(), size } );
        stats.fileBytes += size + 4;
        slot.ready = false;
        --pending;
    }
    void Enqueue()
    {
        hash.Update({slots[current]->input.data(),offset});
        auto& slot = *slots[current];
        {
            std::lock_guard lock( slot.mutex );
            slot.size = offset;
            slot.pending = true;
            slot.signal.notify_all();
        }
        ++pending;
        current = ( current + 1 ) % slots.size();
        offset = 0;
        if( pending == slots.size() ) Drain();
    }
};
SnapshotStreamWriter::SnapshotStreamWriter( const std::filesystem::path& path, const SnapshotStreamOptions& options )
{
    Check( options.streams > 0 && options.streams <= 16 && options.maxFileBytes >= 6, "Invalid snapshot stream budget" );
    Cancel( options );
    m_impl = std::make_unique<Impl>( path, options );
}
SnapshotStreamWriter::~SnapshotStreamWriter() = default;
void SnapshotStreamWriter::Write( std::span<const uint8_t> bytes )
{
    auto& s = *m_impl;
    Check( !s.finished, "Snapshot writer is finished" );
    Cancel( s.options );
    Check( bytes.size() <= UINT64_MAX - s.stats.decodedBytes, "Snapshot byte count overflow" );
    Check( bytes.size() <= s.options.maxDecodedBytes - s.stats.decodedBytes, "Snapshot decoded output budget exceeded" );
    while( !bytes.empty() )
    {
        Cancel( s.options );
        const auto n = std::min( bytes.size(), BlockBytes - s.offset );
        s.stats.decodedBytes += n;
        memcpy( s.slots[s.current]->input.data() + s.offset, bytes.data(), n );
        s.offset += n;
        bytes = bytes.subspan( n );
        if( s.offset == BlockBytes ) s.Enqueue();
    }
}
SnapshotStreamStats SnapshotStreamWriter::Finish()
{
    auto& s = *m_impl;
    if( s.finished ) return s.stats;
    Cancel( s.options );
    if( s.offset ) s.Enqueue();
    while( s.pending ) s.Drain();
    s.file.Flush();
    s.file.Close();
    s.stats.sha256 = s.hash.Finish();
    s.file.Keep();
    s.finished = true;
    s.Stop();
    return s.stats;
}
struct SnapshotStreamReader::Impl
{
    struct Slot
    {
        std::array<uint8_t, BlockBytes> dictionary{};
        int dictionaryBytes = 0;
    };
    SnapshotStreamOptions options;
    File file;
    Hash hash;
    SnapshotStreamStats stats;
    std::vector<Slot> slots;
    std::array<uint8_t, BlockBytes> decoded{};
    std::vector<uint8_t> compressed = std::vector<uint8_t>( LZ4_compressBound( int( BlockBytes ) ) );
    size_t current = 0, offset = 0, available = 0;
    bool eof = false, finished = false;
    Impl( const std::filesystem::path& path, const SnapshotStreamOptions& opts )
        : options( opts )
        , file( path, false )
    {
        uint8_t header[6];
        Exact( file, header );
        Check( memcmp( header, Signature, 4 ) == 0 && header[4] == 0 && header[5] > 0 && header[5] <= 16, "Unsupported snapshot compression envelope" );
        slots.resize( header[5] );
        stats.fileBytes = 6;
    }
    bool Next()
    {
        Cancel( options );
        if( eof ) return false;
        uint32_t size = 0;
        const auto n = file.Read( { reinterpret_cast<uint8_t*>( &size ), 4 } );
        if( !n )
        {
            eof = true;
            return false;
        }
        if( n < 4 ) Exact( file, { reinterpret_cast<uint8_t*>( &size ) + n, 4 - n } );
        Check( size > 0 && size <= compressed.size(), "Invalid snapshot compressed block size" );
        Check( stats.fileBytes <= options.maxFileBytes && uint64_t( size ) + 4 <= options.maxFileBytes - stats.fileBytes, "Snapshot input budget exceeded" );
        Exact( file, { compressed.data(), size } );
        stats.fileBytes += size + 4;
        auto& slot = slots[current];
        const auto decodedSize = LZ4_decompress_safe_usingDict( reinterpret_cast<const char*>( compressed.data() ), reinterpret_cast<char*>( decoded.data() ), int( size ), int( BlockBytes ), reinterpret_cast<const char*>( slot.dictionary.data() ), slot.dictionaryBytes );
        Check( decodedSize > 0 && decodedSize <= int( BlockBytes ), "Snapshot block decompression failed" );
        const auto retain = std::min<size_t>( slot.dictionaryBytes, BlockBytes - size_t( decodedSize ) );
        memmove( slot.dictionary.data(), slot.dictionary.data() + slot.dictionaryBytes - retain, retain );
        memcpy( slot.dictionary.data() + retain, decoded.data(), size_t( decodedSize ) );
        slot.dictionaryBytes = int( retain ) + decodedSize;
        hash.Update({decoded.data(),size_t(decodedSize)});
        current = ( current + 1 ) % slots.size();
        offset = 0;
        available = size_t( decodedSize );
        return true;
    }
};
SnapshotStreamReader::SnapshotStreamReader( const std::filesystem::path& path, const SnapshotStreamOptions& options )
{
    Check( options.maxFileBytes >= 6, "Invalid snapshot read budget" );
    Cancel( options );
    m_impl = std::make_unique<Impl>( path, options );
}
SnapshotStreamReader::~SnapshotStreamReader() = default;
size_t SnapshotStreamReader::Read( std::span<uint8_t> bytes )
{
    auto& s = *m_impl;
    Check( !s.finished, "Snapshot reader is finished" );
    Cancel( s.options );
    if( bytes.empty() ) return 0;
    if( s.offset == s.available && !s.Next() ) return 0;
    const auto n = std::min( bytes.size(), s.available - s.offset );
    Check( n <= s.options.maxDecodedBytes - s.stats.decodedBytes, "Snapshot decoded input budget exceeded" );
    memcpy( bytes.data(), s.decoded.data() + s.offset, n );
    Check( n <= UINT64_MAX - s.stats.decodedBytes, "Snapshot decoded byte count overflow" );
    s.stats.decodedBytes += n;
    s.offset += n;
    return n;
}
void SnapshotStreamReader::ReadExact( std::span<uint8_t> bytes )
{
    while( !bytes.empty() )
    {
        const auto n = Read( bytes );
        Check( n > 0, "Truncated snapshot payload" );
        bytes = bytes.subspan( n );
    }
}
SnapshotStreamStats SnapshotStreamReader::Finish()
{
    auto& s = *m_impl;
    if( s.finished ) return s.stats;
    std::array<uint8_t, BlockBytes> bytes;
    while( Read( bytes ) )
    {
    }
    s.stats.sha256 = s.hash.Finish();
    s.finished = true;
    return s.stats;
}
}
