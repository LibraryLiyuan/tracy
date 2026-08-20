#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

bool ReadPayload( std::ifstream& file, const tracy::stream::RecordInfo& record, std::vector<uint8_t>& payload )
{
    if( record.payloadSize > uint64_t( std::numeric_limits<size_t>::max() ) ) return false;
    payload.resize( size_t( record.payloadSize ) );
    file.clear();
    file.seekg( std::streamoff( record.offset + tracy::stream::RecordHeaderSize ), std::ios::beg );
    if( !file ) return false;
    if( payload.empty() ) return true;
    file.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) );
    return file && file.gcount() == std::streamsize( payload.size() );
}

bool CountFrame(
    const char* begin,
    const char* end,
    std::array<uint64_t, size_t( tracy::QueueType::NUM_TYPES )>& counts,
    uint64_t& total )
{
    auto ptr = begin;
    while( ptr < end )
    {
        if( size_t( end - ptr ) < sizeof( tracy::QueueHeader ) ) return false;
        const auto event = reinterpret_cast<const tracy::QueueItem*>( ptr );
        const auto index = size_t( event->hdr.idx );
        if( index >= counts.size() ) return false;
        counts[index]++;
        total++;
        if( event->hdr.idx >= int( tracy::QueueType::StringData ) )
        {
            ptr += sizeof( tracy::QueueHeader ) + sizeof( tracy::QueueStringTransfer );
            if( ptr > end ) return false;
            if( event->hdr.type == tracy::QueueType::FrameImageData ||
                event->hdr.type == tracy::QueueType::SymbolCode ||
                event->hdr.type == tracy::QueueType::SourceCode ||
                event->hdr.type == tracy::QueueType::JnGpuReferenceSetDefinition )
            {
                if( size_t( end - ptr ) < sizeof( uint32_t ) ) return false;
                uint32_t size;
                memcpy( &size, ptr, sizeof( size ) );
                ptr += sizeof( size );
                if( uint64_t( end - ptr ) < size ) return false;
                ptr += size;
            }
            else
            {
                if( size_t( end - ptr ) < sizeof( uint16_t ) ) return false;
                uint16_t size;
                memcpy( &size, ptr, sizeof( size ) );
                ptr += sizeof( size );
                if( size_t( end - ptr ) < size ) return false;
                ptr += size;
            }
        }
        else if( event->hdr.type == tracy::QueueType::SingleStringData ||
            event->hdr.type == tracy::QueueType::SecondStringData )
        {
            ptr += sizeof( tracy::QueueHeader );
            if( size_t( end - ptr ) < sizeof( uint16_t ) ) return false;
            uint16_t size;
            memcpy( &size, ptr, sizeof( size ) );
            ptr += sizeof( size );
            if( size_t( end - ptr ) < size ) return false;
            ptr += size;
        }
        else
        {
            const auto size = tracy::QueueDataSize[index];
            if( size_t( end - ptr ) < size ) return false;
            ptr += size;
        }
    }
    return ptr == end;
}

}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "Usage: tracy-stream-inspect input.tracy-stream\n" );
        return 1;
    }
    const auto path = std::filesystem::u8path( argv[1] );
    tracy::stream::ScanOptions options;
    options.maxCollectedRecords = 2'000'000;
    const auto scan = tracy::stream::ScanJournal( path, options );
    if( !scan.HasRecoverablePrefix() )
    {
        std::fprintf( stderr, "Scan failed: %s\n", scan.message.c_str() );
        return 2;
    }
    std::ifstream file( path, std::ios::binary );
    if( !file ) return 2;

    auto stream = tracy::LZ4_createStreamDecode();
    auto output = std::make_unique<char[]>( tracy::TargetFrameSize * 3 + 1 );
    size_t outputOffset = 0;
    std::vector<uint8_t> payload;
    std::array<uint64_t, size_t( tracy::QueueType::NUM_TYPES )> counts {};
    uint64_t frames = 0;
    uint64_t events = 0;
    uint64_t compressedBytes = 0;
    uint64_t decompressedBytes = 0;
    bool ok = true;
    for( const auto& record : scan.records )
    {
        if( record.type != tracy::stream::RecordType::ClientToServer ||
            ( record.flags & tracy::stream::RecordFlagCompressedFrame ) == 0 ) continue;
        if( !ReadPayload( file, record, payload ) || payload.size() < sizeof( tracy::lz4sz_t ) )
        {
            ok = false;
            break;
        }
        tracy::lz4sz_t compressedSize;
        memcpy( &compressedSize, payload.data(), sizeof( compressedSize ) );
        if( compressedSize == 0 || compressedSize > tracy::LZ4Size ||
            payload.size() != sizeof( compressedSize ) + compressedSize )
        {
            ok = false;
            break;
        }
        auto destination = output.get() + outputOffset;
        const auto decompressedSize = LZ4_decompress_safe_continue(
            stream, reinterpret_cast<const char*>( payload.data() + sizeof( compressedSize ) ),
            destination, compressedSize, tracy::TargetFrameSize );
        if( decompressedSize < 0 || !CountFrame( destination, destination + decompressedSize, counts, events ) )
        {
            ok = false;
            break;
        }
        compressedBytes += payload.size();
        decompressedBytes += decompressedSize;
        frames++;
        outputOffset += decompressedSize;
        if( outputOffset > tracy::TargetFrameSize * 2 ) outputOffset = 0;
    }
    tracy::LZ4_freeStreamDecode( stream );
    if( !ok )
    {
        std::fprintf( stderr, "Protocol frame scan failed after %llu frames.\n", static_cast<unsigned long long>( frames ) );
        return 3;
    }
    std::printf( "protocol=%u records=%llu frames=%llu events=%llu compressed=%llu decompressed=%llu complete=%s\n",
        scan.header.protocolVersion, static_cast<unsigned long long>( scan.recordCount ),
        static_cast<unsigned long long>( frames ), static_cast<unsigned long long>( events ),
        static_cast<unsigned long long>( compressedBytes ), static_cast<unsigned long long>( decompressedBytes ),
        scan.complete ? "true" : "false" );
    for( size_t index = 0; index < counts.size(); index++ )
    {
        if( counts[index] != 0 ) std::printf( "type=%zu count=%llu\n", index, static_cast<unsigned long long>( counts[index] ) );
    }
    return 0;
}
