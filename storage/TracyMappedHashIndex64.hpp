#ifndef TRACY_MAPPED_HASH_INDEX64_HPP
#define TRACY_MAPPED_HASH_INDEX64_HPP

#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../analysis/TracyHash.hpp"
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tracy::storage
{

// Task-local, file-backed open-addressing table. It is never resumed after an
// interrupted conversion; Seal/Validate protect the completed temporary table.
class MappedHashIndex64
{
    static constexpr uint64_t HeaderBytes = 4096;
    struct Header
    {
        uint64_t magic = 0x31485341484e4aull;
        uint32_t schema = 1;
        uint32_t sealed = 0;
        uint64_t capacity = 0;
        uint64_t count = 0;
        char checksum[64] {};
        char reserved[HeaderBytes-96] {};
    };
    static_assert( sizeof( Header ) == HeaderBytes );
    struct Slot
    {
        uint64_t key = 0;
        uint64_t value = 0;
        uint8_t occupied = 0;
        uint8_t reserved[7] {};
    };
    static_assert( sizeof( Slot ) == 24 );

    class Mapping
    {
    public:
        Mapping( const std::filesystem::path& path, uint64_t bytes, bool create ) : m_bytes( bytes )
        {
            Require( bytes >= HeaderBytes && bytes <= SIZE_MAX, "mapped_hash_size" );
#ifdef _WIN32
            const auto mode = create ? CREATE_NEW : OPEN_EXISTING;
            m_file = CreateFileW( analysis::AnalysisIoPath( path ).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, mode, FILE_ATTRIBUTE_NORMAL, nullptr );
            Require( m_file != INVALID_HANDLE_VALUE, "mapped_hash_open" );
            if( create )
            {
                LARGE_INTEGER size;
                size.QuadPart = LONGLONG( bytes );
                Require( SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) && SetEndOfFile( m_file ), "mapped_hash_resize" );
            }
            else
            {
                LARGE_INTEGER size;
                Require( GetFileSizeEx( m_file, &size ) && uint64_t( size.QuadPart ) == bytes, "mapped_hash_file_size" );
            }
            m_object = CreateFileMappingW( m_file, nullptr, PAGE_READWRITE, DWORD( bytes >> 32 ), DWORD( bytes ), nullptr );
            Require( m_object != nullptr, "mapped_hash_mapping" );
            m_data = static_cast<unsigned char*>( MapViewOfFile( m_object, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T( bytes ) ) );
            Require( m_data != nullptr, "mapped_hash_view" );
#else
            m_file = open( analysis::AnalysisIoPath( path ).c_str(), O_RDWR | ( create ? O_CREAT | O_EXCL : 0 ), 0600 );
            Require( m_file >= 0, "mapped_hash_open" );
            if( create ) Require( ftruncate( m_file, off_t( bytes ) ) == 0, "mapped_hash_resize" );
            else { struct stat info; Require( fstat( m_file, &info ) == 0 && uint64_t( info.st_size ) == bytes, "mapped_hash_file_size" ); }
            m_data = static_cast<unsigned char*>( mmap( nullptr, size_t( bytes ), PROT_READ | PROT_WRITE, MAP_SHARED, m_file, 0 ) );
            Require( m_data != MAP_FAILED, "mapped_hash_view" );
#endif
        }
        ~Mapping()
        {
#ifdef _WIN32
            if( m_data ) UnmapViewOfFile( m_data );
            if( m_object ) CloseHandle( m_object );
            if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
#else
            if( m_data && m_data != MAP_FAILED ) munmap( m_data, size_t( m_bytes ) );
            if( m_file >= 0 ) close( m_file );
#endif
        }
        void Flush()
        {
#ifdef _WIN32
            Require( FlushViewOfFile( m_data, SIZE_T( m_bytes ) ) != 0 && FlushFileBuffers( m_file ) != 0, "mapped_hash_flush" );
#else
            Require( msync( m_data, size_t( m_bytes ), MS_SYNC ) == 0 && fsync( m_file ) == 0, "mapped_hash_flush" );
#endif
        }
        unsigned char* Data() const { return m_data; }
    private:
        static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
        uint64_t m_bytes;
        unsigned char* m_data = nullptr;
#ifdef _WIN32
        HANDLE m_file = INVALID_HANDLE_VALUE;
        HANDLE m_object = nullptr;
#else
        int m_file = -1;
#endif
    };

public:
    MappedHashIndex64( std::filesystem::path path, ConversionResources& resources, uint64_t initialCapacity = 1024 )
        : m_path( std::move( path ) ), m_resources( resources ), m_control( resources.Workspace(), 65536 )
    {
        uint64_t capacity = 64;
        while( capacity < initialCapacity )
        {
            Require( capacity <= UINT64_MAX / 2, "mapped_hash_capacity" );
            capacity *= 2;
        }
        Create( capacity );
    }

    std::optional<uint64_t> Find( uint64_t key ) const
    {
        Check();
        const auto* slots = Slots();
        auto position = Mix( key ) & ( m_header->capacity - 1 );
        for( uint64_t probe = 0; probe < m_header->capacity; ++probe )
        {
            const auto& slot = slots[position];
            if( !slot.occupied ) return {};
            if( slot.key == key ) return slot.value;
            position = ( position + 1 ) & ( m_header->capacity - 1 );
        }
        throw std::runtime_error( "mapped_hash_probe" );
    }

    bool Upsert( uint64_t key, uint64_t value )
    {
        Check();
        Require( !m_header->sealed, "mapped_hash_sealed" );
        try
        {
            if( auto* found = Locate( key ) )
            {
                found->value = value;
                return false;
            }
            if( ( m_header->count + 1 ) * 10 > m_header->capacity * 7 ) Resize( m_header->capacity * 2 );
            InsertNew( Slots(), m_header->capacity, key, value );
            ++m_header->count;
            return true;
        }
        catch( const std::exception& e )
        {
            m_failed = true;
            m_resources.Fail( e.what() );
            throw;
        }
    }

    void Seal()
    {
        Check();
        Require( !m_header->sealed, "mapped_hash_sealed" );
        m_header->sealed = 1;
        std::memset( m_header->checksum, 0, sizeof( m_header->checksum ) );
        const auto digest = Digest();
        std::memcpy( m_header->checksum, digest.data(), digest.size() );
        m_mapping->Flush();
    }

    void Validate() const
    {
        Check();
        Require( m_header->magic == Header{}.magic && m_header->schema == 1 && m_header->sealed == 1 && IsPowerOfTwo( m_header->capacity ), "mapped_hash_header" );
        const auto expected = std::string( m_header->checksum, 64 );
        Require( expected == Digest(), "mapped_hash_checksum" );
        uint64_t count = 0;
        for( uint64_t i = 0; i < m_header->capacity; ++i ) if( Slots()[i].occupied ) ++count;
        Require( count == m_header->count, "mapped_hash_count" );
    }

    uint64_t Count() const { return m_header ? m_header->count : 0; }
    uint64_t Capacity() const { return m_header ? m_header->capacity : 0; }
    bool CloseAndRemove() noexcept
    {
        if(m_closed)return true;m_mapping.reset();m_header=nullptr;const bool ok=m_resources.RemoveOwned(m_path);m_closed=true;if(!ok)m_resources.Fail("mapped_hash_cleanup_failed");return ok;
    }

private:
    static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
    static bool IsPowerOfTwo( uint64_t value ) { return value >= 64 && ( value & ( value - 1 ) ) == 0; }
    static uint64_t Mix( uint64_t value )
    {
        value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27; value *= 0x94d049bb133111ebull;
        return value ^ ( value >> 31 );
    }
    uint64_t Bytes( uint64_t capacity ) const
    {
        Require( IsPowerOfTwo( capacity ) && capacity <= ( UINT64_MAX - HeaderBytes ) / sizeof( Slot ), "mapped_hash_capacity" );
        return HeaderBytes + capacity * sizeof( Slot );
    }
    void Check() const { m_resources.CheckFast(); Require(!m_closed,"mapped_hash_closed");Require( !m_failed && m_mapping && m_header, "mapped_hash_failed" ); }
    Slot* Slots() const { return reinterpret_cast<Slot*>( m_mapping->Data() + HeaderBytes ); }
    Slot* Locate( uint64_t key ) const
    {
        auto* slots = Slots();
        auto position = Mix( key ) & ( m_header->capacity - 1 );
        for( uint64_t probe = 0; probe < m_header->capacity; ++probe )
        {
            auto& slot = slots[position];
            if( !slot.occupied ) return nullptr;
            if( slot.key == key ) return &slot;
            position = ( position + 1 ) & ( m_header->capacity - 1 );
        }
        throw std::runtime_error( "mapped_hash_probe" );
    }
    static void InsertNew( Slot* slots, uint64_t capacity, uint64_t key, uint64_t value )
    {
        auto position = Mix( key ) & ( capacity - 1 );
        for( uint64_t probe = 0; probe < capacity; ++probe )
        {
            auto& slot = slots[position];
            if( !slot.occupied )
            {
                slot.key = key;
                slot.value = value;
                slot.occupied = 1;
                return;
            }
            position = ( position + 1 ) & ( capacity - 1 );
        }
        throw std::runtime_error( "mapped_hash_full" );
    }
    void Create( uint64_t capacity )
    {
        Require( !std::filesystem::exists( analysis::AnalysisIoPath( m_path ) ), "mapped_hash_exists" );
        const auto bytes = Bytes( capacity );
        m_resources.BeforeFileGrowth( m_path, bytes );
        m_mapping = std::make_unique<Mapping>( m_path, bytes, true );
        m_header = reinterpret_cast<Header*>( m_mapping->Data() );
        *m_header = Header{};
        m_header->capacity = capacity;
    }
    void Resize( uint64_t capacity )
    {
        Require( capacity > m_header->capacity && capacity <= UINT64_MAX / 2, "mapped_hash_capacity" );
        auto temporary = m_path;
        temporary += ".resize";
        Require( !std::filesystem::exists( analysis::AnalysisIoPath( temporary ) ), "mapped_hash_resize_exists" );
        const auto bytes = Bytes( capacity );
        m_resources.BeforeFileGrowth( temporary, bytes );
        auto next = std::make_unique<Mapping>( temporary, bytes, true );
        auto* nextHeader = reinterpret_cast<Header*>( next->Data() );
        *nextHeader = Header{};
        nextHeader->capacity = capacity;
        auto* nextSlots = reinterpret_cast<Slot*>( next->Data() + HeaderBytes );
        for( uint64_t i = 0; i < m_header->capacity; ++i )
        {
            const auto& slot = Slots()[i];
            if( slot.occupied ) InsertNew( nextSlots, capacity, slot.key, slot.value );
        }
        nextHeader->count = m_header->count;
        next->Flush();
        next.reset();
        m_mapping.reset();
        m_header = nullptr;
        Require( m_resources.RemoveOwned( m_path ), "mapped_hash_remove" );
        m_resources.RenameOwned( temporary, m_path );
        m_mapping = std::make_unique<Mapping>( m_path, bytes, false );
        m_header = reinterpret_cast<Header*>( m_mapping->Data() );
        Require( m_header->capacity == capacity, "mapped_hash_resize_header" );
    }
    std::string Digest() const
    {
        Header header = *m_header;
        std::memset( header.checksum, 0, sizeof( header.checksum ) );
        analysis::Sha256Builder hash;
        hash.Update( &header, sizeof( header ) );
        hash.Update( Slots(), size_t( m_header->capacity * sizeof( Slot ) ) );
        return hash.FinalHex();
    }

    std::filesystem::path m_path;
    ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_control;
    std::unique_ptr<Mapping> m_mapping;
    Header* m_header = nullptr;
    bool m_failed = false,m_closed=false;
};

}
#endif
