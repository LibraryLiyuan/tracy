#ifndef TRACY_MAPPED_APPEND_ARENA_HPP
#define TRACY_MAPPED_APPEND_ARENA_HPP

#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace tracy::storage
{

// Append-only file mappings. Every chunk has its own mapping, so addresses
// returned to the offline Worker remain valid until the store is destroyed.
class MappedAppendArena
{
    class Chunk
    {
    public:
        Chunk( std::filesystem::path path, uint64_t capacity, ConversionResources& resources )
            : m_path( std::move( path ) ), m_capacity( capacity )
        {
            Require( capacity > 0 && capacity <= SIZE_MAX, "mapped_arena_capacity" );
            resources.BeforeFileGrowth( m_path, capacity );
#ifdef _WIN32
            m_file = CreateFileW( analysis::AnalysisIoPath( m_path ).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr );
            Require( m_file != INVALID_HANDLE_VALUE, "mapped_arena_open" );
            LARGE_INTEGER size; size.QuadPart = LONGLONG( capacity );
            Require( SetFilePointerEx( m_file, size, nullptr, FILE_BEGIN ) && SetEndOfFile( m_file ), "mapped_arena_resize" );
            m_object = CreateFileMappingW( m_file, nullptr, PAGE_READWRITE, DWORD( capacity >> 32 ), DWORD( capacity ), nullptr );
            Require( m_object != nullptr, "mapped_arena_mapping" );
            m_data = static_cast<char*>( MapViewOfFile( m_object, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T( capacity ) ) );
            Require( m_data != nullptr, "mapped_arena_view" );
#else
            m_file = open( analysis::AnalysisIoPath( m_path ).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600 );
            Require( m_file >= 0, "mapped_arena_open" );
            Require( ftruncate( m_file, off_t( capacity ) ) == 0, "mapped_arena_resize" );
            m_data = static_cast<char*>( mmap( nullptr, size_t( capacity ), PROT_READ | PROT_WRITE, MAP_SHARED, m_file, 0 ) );
            Require( m_data != MAP_FAILED, "mapped_arena_view" );
#endif
        }
        ~Chunk()
        {
#ifdef _WIN32
            if( m_data ) UnmapViewOfFile( m_data );
            if( m_object ) CloseHandle( m_object );
            if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
#else
            if( m_data && m_data != MAP_FAILED ) munmap( m_data, size_t( m_capacity ) );
            if( m_file >= 0 ) close( m_file );
#endif
        }
        char* Append( const void* data, uint64_t bytes )
        {
            Require( bytes <= m_capacity - m_used && ( bytes == 0 || data ), "mapped_arena_append" );
            auto* result = m_data + m_used;
            if( bytes ) std::memcpy( result, data, size_t( bytes ) );
            m_used += bytes;
            return result;
        }
        const char* At( uint64_t offset, uint64_t bytes ) const
        {
            Require( offset <= m_used && bytes <= m_used - offset, "mapped_arena_range" );
            return m_data + offset;
        }
        void Flush()
        {
#ifdef _WIN32
            Require( FlushViewOfFile( m_data, SIZE_T( m_capacity ) ) != 0 && FlushFileBuffers( m_file ) != 0, "mapped_arena_flush" );
#else
            Require( msync( m_data, size_t( m_capacity ), MS_SYNC ) == 0 && fsync( m_file ) == 0, "mapped_arena_flush" );
#endif
        }
        uint64_t Used() const { return m_used; }
        uint64_t Capacity() const { return m_capacity; }
    private:
        static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
        std::filesystem::path m_path;
        uint64_t m_capacity;
        uint64_t m_used = 0;
        char* m_data = nullptr;
#ifdef _WIN32
        HANDLE m_file = INVALID_HANDLE_VALUE;
        HANDLE m_object = nullptr;
#else
        int m_file = -1;
#endif
    };

public:
    struct Location { uint32_t chunk; uint64_t offset; char* data; };
    MappedAppendArena( std::filesystem::path root, ConversionResources& resources ) : m_root( std::move( root ) ), m_resources( resources )
    {
        if( std::filesystem::exists( analysis::AnalysisIoPath( m_root ) ) ) throw std::runtime_error( "mapped_arena_exists" );
        m_resources.BeforeFileGrowth( m_root, 0 );
        std::filesystem::create_directories( analysis::AnalysisIoPath( m_root ) );
    }
    Location Append( const void* data, uint64_t bytes )
    {
        m_resources.CheckFast();
        Require( bytes < UINT64_MAX, "mapped_arena_size" );
        const auto required = bytes + 1;
        if( m_chunks.empty() || required > m_chunks.back()->Capacity() - m_chunks.back()->Used() ) AddChunk( required );
        auto& chunk = *m_chunks.back();
        const auto offset = chunk.Used();
        auto* result = chunk.Append( data, bytes );
        const char zero = 0;
        chunk.Append( &zero, 1 );
        m_used += required;
        return { uint32_t( m_chunks.size() - 1 ), offset, result };
    }
    const char* Get( uint32_t chunk, uint64_t offset, uint64_t bytes ) const
    {
        Require( chunk < m_chunks.size(), "mapped_arena_chunk" );
        const auto* result = m_chunks[chunk]->At( offset, bytes + 1 );
        Require( result[bytes] == 0, "mapped_arena_terminator" );
        return result;
    }
    void Flush() { for( auto& chunk : m_chunks ) chunk->Flush(); }
    uint64_t UsedBytes() const { return m_used; }
    uint64_t ReservedBytes() const { uint64_t bytes=0;for(const auto& chunk:m_chunks)bytes+=chunk->Capacity();return bytes; }
private:
    static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
    void AddChunk( uint64_t required )
    {
        Require( m_chunks.size() < UINT32_MAX, "mapped_arena_chunks" );
        uint64_t capacity = m_nextCapacity;
        if( required > capacity )
        {
            constexpr uint64_t alignment = 64 * 1024;
            Require( required <= UINT64_MAX - alignment + 1, "mapped_arena_capacity" );
            capacity = ( required + alignment - 1 ) / alignment * alignment;
        }
        char name[32];
        std::snprintf( name, sizeof( name ), "chunk-%08x.bin", unsigned( m_chunks.size() ) );
        m_chunks.emplace_back( std::make_unique<Chunk>( m_root / name, capacity, m_resources ) );
        m_nextCapacity = std::min<uint64_t>( 16ull << 20, std::max<uint64_t>( 64ull << 10, capacity * 2 ) );
    }
    std::filesystem::path m_root;
    ConversionResources& m_resources;
    std::vector<std::unique_ptr<Chunk>> m_chunks;
    uint64_t m_nextCapacity = 32ull << 10;
    uint64_t m_used = 0;
};

}
#endif
