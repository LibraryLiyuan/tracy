#include "TracyTraceSessionFrameImages.hpp"

#include "TracyGpuAnalysisPath.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"
#include "TracyTextureCompression.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <unordered_set>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t MetadataMagic = 0x31494d46534e4aull; // JNSFMI1
constexpr uint64_t DataMagic = 0x31444946534e4aull; // JNSFID1
constexpr uint64_t ManifestMagic = 0x31464d4946534eull; // NSIFMF1
constexpr const char* MetadataFileName = "frame-images.bin";
constexpr const char* DataFileName = "frame-images.bc1";
constexpr uint64_t CommonHeaderBytes = 40 + 64;

void Put32( std::vector<uint8_t>& out, uint32_t value )
{
    for( int i = 0; i < 4; ++i ) out.push_back( uint8_t( value >> ( i * 8 ) ) );
}

void Put64( std::vector<uint8_t>& out, uint64_t value )
{
    for( int i = 0; i < 8; ++i ) out.push_back( uint8_t( value >> ( i * 8 ) ) );
}

bool Get32( const std::vector<uint8_t>& in, size_t& offset, uint32_t& value )
{
    if( offset > in.size() || in.size() - offset < 4 ) return false;
    value = 0;
    for( int i = 0; i < 4; ++i ) value |= uint32_t( in[offset + i] ) << ( i * 8 );
    offset += 4;
    return true;
}

bool Get64( const std::vector<uint8_t>& in, size_t& offset, uint64_t& value )
{
    if( offset > in.size() || in.size() - offset < 8 ) return false;
    value = 0;
    for( int i = 0; i < 8; ++i ) value |= uint64_t( in[offset + i] ) << ( i * 8 );
    offset += 8;
    return true;
}

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_frame_image_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_frame_image_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

struct FrameImageManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t metadataFileBytes = 0;
    uint64_t dataFileBytes = 0;
    std::string metadataSha256;
    std::string dataSha256;
    TraceSessionFrameImageStats stats;
};

bool SaveManifest( const std::filesystem::path& root,
    const FrameImageManifest& manifest, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_frame_image_manifest_open_failed"; return false; }
    out << "magic " << ManifestMagic << '\n';
    out << "schema " << TraceSessionFrameImageIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( manifest.sourceSha256 ) << '\n';
    out << "source_size " << manifest.sourceSize << '\n';
    out << "generation " << std::quoted( manifest.generation ) << '\n';
    out << "metadata_file_bytes " << manifest.metadataFileBytes << '\n';
    out << "data_file_bytes " << manifest.dataFileBytes << '\n';
    out << "metadata_sha256 " << std::quoted( manifest.metadataSha256 ) << '\n';
    out << "data_sha256 " << std::quoted( manifest.dataSha256 ) << '\n';
    out << "images " << manifest.stats.images << '\n';
    out << "image_data_events " << manifest.stats.imageDataEvents << '\n';
    out << "image_events " << manifest.stats.imageEvents << '\n';
    out << "bc1_bytes " << manifest.stats.bc1Bytes << '\n';
    out.flush();
    if( !out ) { error = "session_frame_image_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    FrameImageManifest& manifest, std::string& error )
{
    manifest = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_frame_image_manifest_not_found"; return false; }
    uint64_t magic = 0;
    uint32_t schema = 0;
    std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( manifest.sourceSha256 );
        else if( key == "source_size" ) in >> manifest.sourceSize;
        else if( key == "generation" ) in >> std::quoted( manifest.generation );
        else if( key == "metadata_file_bytes" ) in >> manifest.metadataFileBytes;
        else if( key == "data_file_bytes" ) in >> manifest.dataFileBytes;
        else if( key == "metadata_sha256" ) in >> std::quoted( manifest.metadataSha256 );
        else if( key == "data_sha256" ) in >> std::quoted( manifest.dataSha256 );
        else if( key == "images" ) in >> manifest.stats.images;
        else if( key == "image_data_events" ) in >> manifest.stats.imageDataEvents;
        else if( key == "image_events" ) in >> manifest.stats.imageEvents;
        else if( key == "bc1_bytes" ) in >> manifest.stats.bc1Bytes;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_frame_image_manifest_parse_failed"; return false; }
    }
    manifest.stats.metadataFileBytes = manifest.metadataFileBytes;
    manifest.stats.dataFileBytes = manifest.dataFileBytes;
    if( magic != ManifestMagic || schema != TraceSessionFrameImageIndexSchemaVersion ||
        manifest.sourceSha256.size() != 64 || manifest.metadataSha256.size() != 64 ||
        manifest.dataSha256.size() != 64 )
    { error = "session_frame_image_manifest_invalid"; return false; }
    return true;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_frame_image_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_frame_image_protocol_type_mismatch"; return false; }
    return true;
}

bool DecodeLargePayload( const TraceSessionCanonicalRecord& record,
    std::vector<uint8_t>& payload, std::string& error )
{
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint32_t ) )
    { error = "session_frame_image_data_header_truncated"; return false; }
    uint32_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_frame_image_data_size_mismatch"; return false; }
    if( bytes == 0 || bytes % 8 != 0 )
    { error = "session_frame_image_data_block_size_invalid"; return false; }
    payload.assign( record.payload.begin() + fixed + sizeof( bytes ), record.payload.end() );
    return true;
}

struct BuildState
{
    std::ofstream data;
    std::vector<TraceSessionFrameImageRecord> images;
    std::vector<uint8_t> pending;
    std::unordered_set<uint32_t> usedFrames;
    TextureCompression compression;
    uint64_t dataHeaderBytes = 0;
    uint64_t dataBytes = 0;
    uint64_t frameOffset = 0;
    bool welcomeSeen = false;
    bool onDemand = false;
    bool awaitingOnDemandPayload = false;
    TraceSessionFrameImageStats stats;
};

bool VisitFrameImage( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    auto& state = *static_cast<BuildState*>( userData );
    if( record.kind == TraceSessionCanonicalRecordKind::TransportRecord )
    {
        if( record.type != uint8_t( stream::RecordType::ClientToServer ) ) return true;
        if( !state.welcomeSeen && record.payload.size() == sizeof( WelcomeMessage ) )
        {
            WelcomeMessage welcome {};
            std::memcpy( &welcome, record.payload.data(), sizeof( welcome ) );
            state.welcomeSeen = true;
            state.onDemand = ( welcome.flags & WelcomeFlag::OnDemand ) != 0;
            state.awaitingOnDemandPayload = state.onDemand;
            return true;
        }
        if( state.awaitingOnDemandPayload && record.payload.size() == sizeof( OnDemandPayloadMessage ) )
        {
            OnDemandPayloadMessage payload {};
            std::memcpy( &payload, record.payload.data(), sizeof( payload ) );
            state.frameOffset = payload.frames;
            state.awaitingOnDemandPayload = false;
        }
        return true;
    }
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    const auto type = QueueType( record.type );
    if( type == QueueType::FrameImageData )
    {
        state.stats.imageDataEvents++;
        if( !state.pending.empty() )
        { error = "session_frame_image_data_before_previous_metadata"; return false; }
        return DecodeLargePayload( record, state.pending, error );
    }
    if( type != QueueType::FrameImage ) return true;
    state.stats.imageEvents++;
    if( state.pending.empty() )
    { error = "session_frame_image_metadata_without_data"; return false; }
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const uint64_t expected = uint64_t( item.frameImage.w ) * item.frameImage.h / 2;
    if( item.frameImage.w == 0 || item.frameImage.h == 0 || expected != state.pending.size() )
    { error = "session_frame_image_dimensions_mismatch"; return false; }
    if( state.frameOffset > uint64_t( std::numeric_limits<int64_t>::max() ) )
    { error = "session_frame_image_frame_offset_invalid"; return false; }
    const auto frameIndex = int64_t( item.frameImage.frame ) - int64_t( state.frameOffset ) + 1;
    if( state.onDemand && frameIndex <= 1 )
    {
        state.pending.clear();
        return true;
    }
    if( frameIndex <= 0 || frameIndex > std::numeric_limits<uint32_t>::max() )
    { error = "session_frame_image_index_invalid"; return false; }
    if( !state.usedFrames.emplace( uint32_t( frameIndex ) ).second )
    { error = "session_frame_image_duplicate_frame"; return false; }
    state.compression.FixOrder( reinterpret_cast<char*>( state.pending.data() ), state.pending.size() / 8 );
    state.compression.Rdo( reinterpret_cast<char*>( state.pending.data() ), state.pending.size() / 8 );
    if( state.dataBytes > std::numeric_limits<uint64_t>::max() - state.pending.size() )
    { error = "session_frame_image_data_bytes_overflow"; return false; }
    const auto dataOffset = state.dataHeaderBytes + state.dataBytes;
    state.data.write( reinterpret_cast<const char*>( state.pending.data() ),
        std::streamsize( state.pending.size() ) );
    if( !state.data ) { error = "session_frame_image_data_write_failed"; return false; }
    state.images.push_back( { item.frameImage.w, item.frameImage.h,
        item.frameImage.flip != 0, uint32_t( frameIndex ), dataOffset,
        uint64_t( state.pending.size() ) } );
    state.dataBytes += state.pending.size();
    state.stats.bc1Bytes += state.pending.size();
    state.pending.clear();
    return true;
}

void WriteCommonHeader( std::vector<uint8_t>& bytes, uint64_t magic,
    const TraceSessionManifest& session, uint64_t countOrBytes )
{
    Put64( bytes, magic );
    Put32( bytes, TraceSessionFrameImageIndexSchemaVersion );
    Put32( bytes, 0 );
    Put64( bytes, session.source.fileSize );
    Put64( bytes, countOrBytes );
    Put32( bytes, uint32_t( session.generation.size() ) );
    Put32( bytes, 0 );
    bytes.insert( bytes.end(), session.source.sha256.begin(), session.source.sha256.end() );
    bytes.insert( bytes.end(), session.generation.begin(), session.generation.end() );
}

bool ValidateCommonHeader( const std::vector<uint8_t>& bytes, uint64_t expectedMagic,
    const TraceSessionManifest& session, uint64_t& countOrBytes, size_t& offset,
    std::string& error )
{
    offset = 0;
    uint64_t magic = 0, sourceSize = 0;
    uint32_t schema = 0, reserved0 = 0, generationBytes = 0, reserved1 = 0;
    if( !Get64( bytes, offset, magic ) || !Get32( bytes, offset, schema ) ||
        !Get32( bytes, offset, reserved0 ) || !Get64( bytes, offset, sourceSize ) ||
        !Get64( bytes, offset, countOrBytes ) || !Get32( bytes, offset, generationBytes ) ||
        !Get32( bytes, offset, reserved1 ) || magic != expectedMagic ||
        schema != TraceSessionFrameImageIndexSchemaVersion || reserved0 != 0 || reserved1 != 0 ||
        sourceSize != session.source.fileSize || session.source.sha256.size() != 64 ||
        offset > bytes.size() || 64 > bytes.size() - offset ||
        generationBytes > bytes.size() - offset - 64 )
    { error = "session_frame_image_file_header_invalid"; return false; }
    const std::string sourceSha( reinterpret_cast<const char*>( bytes.data() + offset ), 64 );
    offset += 64;
    const std::string generation( reinterpret_cast<const char*>( bytes.data() + offset ), generationBytes );
    offset += generationBytes;
    if( sourceSha != session.source.sha256 || generation != session.generation )
    { error = "session_frame_image_file_identity_invalid"; return false; }
    return true;
}

uint8_t Expand5( uint16_t value ) { return uint8_t( ( value << 3 ) | ( value >> 2 ) ); }
uint8_t Expand6( uint16_t value ) { return uint8_t( ( value << 2 ) | ( value >> 4 ) ); }

void DecodeBc1( const uint8_t* input, uint32_t width, uint32_t height,
    std::vector<uint8_t>& output )
{
    output.assign( size_t( width ) * height * 4, 0 );
    const uint32_t blocksX = ( width + 3 ) / 4;
    const uint32_t blocksY = ( height + 3 ) / 4;
    for( uint32_t by = 0; by < blocksY; by++ ) for( uint32_t bx = 0; bx < blocksX; bx++ )
    {
        const uint8_t* block = input + ( size_t( by ) * blocksX + bx ) * 8;
        const uint16_t c0 = uint16_t( block[0] ) | uint16_t( block[1] ) << 8;
        const uint16_t c1 = uint16_t( block[2] ) | uint16_t( block[3] ) << 8;
        uint8_t colors[4][4] = {
            { Expand5( ( c0 >> 11 ) & 31 ), Expand6( ( c0 >> 5 ) & 63 ), Expand5( c0 & 31 ), 255 },
            { Expand5( ( c1 >> 11 ) & 31 ), Expand6( ( c1 >> 5 ) & 63 ), Expand5( c1 & 31 ), 255 }, {}, {}
        };
        if( c0 > c1 )
        {
            for( size_t channel = 0; channel < 3; ++channel )
            {
                colors[2][channel] = uint8_t( ( 2 * colors[0][channel] + colors[1][channel] ) / 3 );
                colors[3][channel] = uint8_t( ( colors[0][channel] + 2 * colors[1][channel] ) / 3 );
            }
            colors[2][3] = colors[3][3] = 255;
        }
        else
        {
            for( size_t channel = 0; channel < 3; ++channel )
                colors[2][channel] = uint8_t( ( colors[0][channel] + colors[1][channel] ) / 2 );
            colors[2][3] = 255;
        }
        const uint32_t selectors = uint32_t( block[4] ) | uint32_t( block[5] ) << 8 |
            uint32_t( block[6] ) << 16 | uint32_t( block[7] ) << 24;
        for( uint32_t py = 0; py < 4; ++py ) for( uint32_t px = 0; px < 4; ++px )
        {
            const uint32_t x = bx * 4 + px, y = by * 4 + py;
            if( x >= width || y >= height ) continue;
            const auto selector = ( selectors >> ( 2 * ( py * 4 + px ) ) ) & 3;
            std::memcpy( output.data() + ( size_t( y ) * width + x ) * 4, colors[selector], 4 );
        }
    }
}

}

std::filesystem::path TraceSessionFrameImageIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "frame-image-index" / "1" / "exact";
}

bool BuildTraceSessionFrameImageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionFrameImageStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    if( session.source.sha256.size() != 64 ||
        session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_frame_image_identity_invalid"; return false; }
    const auto root = GpuAnalysisIoPath(
        TraceSessionFrameImageIndexRoot( sessionRoot, session ) );
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_frame_image_directory_failed:" + ec.message(); return false; }
    const auto dataTemporary = root / ( std::string( DataFileName ) + ".tmp" );
    BuildState state;
    state.data.open( dataTemporary, std::ios::binary | std::ios::trunc );
    if( !state.data ) { error = "session_frame_image_data_open_failed"; return false; }
    std::vector<uint8_t> dataHeader;
    WriteCommonHeader( dataHeader, DataMagic, session, 0 );
    state.dataHeaderBytes = dataHeader.size();
    state.data.write( reinterpret_cast<const char*>( dataHeader.data() ),
        std::streamsize( dataHeader.size() ) );
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, session,
        VisitFrameImage, &state, error ) ) return false;
    if( !state.pending.empty() )
    { error = "session_frame_image_data_without_metadata"; return false; }
    state.data.seekp( 24, std::ios::beg );
    const uint64_t dataBytes = state.dataBytes;
    state.data.write( reinterpret_cast<const char*>( &dataBytes ), sizeof( dataBytes ) );
    state.data.flush();
    if( !state.data ) { error = "session_frame_image_data_finalize_failed"; return false; }
    state.data.close();
    const auto dataTarget = root / DataFileName;
    if( !AtomicReplace( dataTemporary, dataTarget, error ) ) return false;

    std::vector<uint8_t> metadata;
    WriteCommonHeader( metadata, MetadataMagic, session, state.images.size() );
    for( const auto& image : state.images )
    {
        Put32( metadata, image.width ); Put32( metadata, image.height );
        Put32( metadata, image.rawFrameIndex ); Put32( metadata, image.flipped ? 1u : 0u );
        Put64( metadata, image.dataOffset ); Put64( metadata, image.dataBytes );
    }
    const auto metadataTemporary = root / ( std::string( MetadataFileName ) + ".tmp" );
    std::ofstream out( metadataTemporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_frame_image_metadata_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( metadata.data() ), std::streamsize( metadata.size() ) );
    out.flush();
    if( !out ) { error = "session_frame_image_metadata_write_failed"; return false; }
    out.close();
    const auto metadataTarget = root / MetadataFileName;
    if( !AtomicReplace( metadataTemporary, metadataTarget, error ) ) return false;

    FrameImageManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.metadataFileBytes = metadata.size();
    manifest.dataFileBytes = state.dataHeaderBytes + state.dataBytes;
    manifest.metadataSha256 = Sha256File( metadataTarget );
    manifest.dataSha256 = Sha256File( dataTarget );
    state.stats.images = state.images.size();
    state.stats.metadataFileBytes = manifest.metadataFileBytes;
    state.stats.dataFileBytes = manifest.dataFileBytes;
    manifest.stats = state.stats;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionFrameImageReader> TraceSessionFrameImageReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = GpuAnalysisIoPath(
        TraceSessionFrameImageIndexRoot( sessionRoot, session ) );
    FrameImageManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_frame_image_identity_mismatch"; return {}; }
    const auto metadataPath = root / MetadataFileName;
    const auto dataPath = root / DataFileName;
    std::error_code ec;
    const auto metadataBytes = std::filesystem::file_size( metadataPath, ec );
    if( ec || metadataBytes != manifest.metadataFileBytes )
    { error = "session_frame_image_metadata_size_mismatch"; return {}; }
    const auto dataBytes = std::filesystem::file_size( dataPath, ec );
    if( ec || dataBytes != manifest.dataFileBytes )
    { error = "session_frame_image_data_size_mismatch"; return {}; }
    if( Sha256File( metadataPath ) != manifest.metadataSha256 )
    { error = "session_frame_image_metadata_sha256_mismatch"; return {}; }
    if( metadataBytes > std::numeric_limits<size_t>::max() ||
        metadataBytes > uint64_t( std::numeric_limits<std::streamsize>::max() ) )
    { error = "session_frame_image_metadata_platform_limit"; return {}; }
    std::vector<uint8_t> metadata( size_t( metadataBytes ), uint8_t{} );
    std::ifstream in( metadataPath, std::ios::binary );
    if( !in ) { error = "session_frame_image_metadata_open_failed"; return {}; }
    if( !metadata.empty() ) in.read( reinterpret_cast<char*>( metadata.data() ),
        std::streamsize( metadata.size() ) );
    if( !in && !metadata.empty() ) { error = "session_frame_image_metadata_read_failed"; return {}; }
    uint64_t imageCount = 0;
    size_t offset = 0;
    if( !ValidateCommonHeader( metadata, MetadataMagic, session, imageCount, offset, error ) ||
        imageCount > std::numeric_limits<size_t>::max() ) return {};
    if( imageCount > ( metadata.size() - offset ) / 32 ||
        offset + imageCount * 32 != metadata.size() )
    { error = "session_frame_image_metadata_count_mismatch"; return {}; }
    auto reader = std::make_shared<TraceSessionFrameImageReader>();
    reader->m_images.reserve( size_t( imageCount ) );
    uint64_t summedBytes = 0;
    for( uint64_t i = 0; i < imageCount; ++i )
    {
        TraceSessionFrameImageRecord image;
        uint32_t flipped = 0;
        if( !Get32( metadata, offset, image.width ) || !Get32( metadata, offset, image.height ) ||
            !Get32( metadata, offset, image.rawFrameIndex ) || !Get32( metadata, offset, flipped ) ||
            !Get64( metadata, offset, image.dataOffset ) || !Get64( metadata, offset, image.dataBytes ) ||
            flipped > 1 || image.width == 0 || image.height == 0 ||
            image.dataBytes != uint64_t( image.width ) * image.height / 2 ||
            image.dataOffset > dataBytes || image.dataBytes > dataBytes - image.dataOffset )
        { error = "session_frame_image_metadata_record_invalid"; return {}; }
        image.flipped = flipped != 0;
        if( summedBytes > std::numeric_limits<uint64_t>::max() - image.dataBytes )
        { error = "session_frame_image_metadata_bytes_overflow"; return {}; }
        summedBytes += image.dataBytes;
        reader->m_images.push_back( image );
    }
    std::vector<uint8_t> dataHeader( size_t( CommonHeaderBytes + session.generation.size() ), uint8_t{} );
    std::ifstream data( dataPath, std::ios::binary );
    if( !data ) { error = "session_frame_image_data_open_failed"; return {}; }
    data.read( reinterpret_cast<char*>( dataHeader.data() ), std::streamsize( dataHeader.size() ) );
    if( !data ) { error = "session_frame_image_data_header_read_failed"; return {}; }
    uint64_t declaredDataBytes = 0;
    size_t dataOffset = 0;
    if( !ValidateCommonHeader( dataHeader, DataMagic, session,
        declaredDataBytes, dataOffset, error ) || dataOffset != dataHeader.size() ||
        declaredDataBytes != summedBytes || declaredDataBytes > dataBytes - dataHeader.size() )
    { if( error.empty() ) error = "session_frame_image_data_header_mismatch"; return {}; }
    if( manifest.stats.images != imageCount || manifest.stats.bc1Bytes != summedBytes )
    { error = "session_frame_image_manifest_count_mismatch"; return {}; }
    reader->m_dataPath = dataPath;
    reader->m_stats = manifest.stats;
    return reader;
}

BinaryResourceChunkDto TraceSessionFrameImageReader::ReadBc1(
    size_t imageId, size_t offset, size_t maxBytes ) const
{
    if( imageId >= m_images.size() ) throw std::out_of_range( "frame image resource was not found" );
    const auto& image = m_images[imageId];
    const auto begin = std::min<uint64_t>( offset, image.dataBytes );
    const auto bytes = std::min<uint64_t>( maxBytes, image.dataBytes - begin );
    BinaryResourceChunkDto result;
    result.offset = begin;
    result.totalBytes = image.dataBytes;
    result.bytes.resize( size_t( bytes ) );
    result.eof = begin + bytes == image.dataBytes;
    std::ifstream in( m_dataPath, std::ios::binary );
    if( !in ) throw std::runtime_error( "frame image data file is unavailable" );
    in.seekg( std::streamoff( image.dataOffset + begin ), std::ios::beg );
    if( !result.bytes.empty() ) in.read( reinterpret_cast<char*>( result.bytes.data() ),
        std::streamsize( result.bytes.size() ) );
    if( !in && !result.bytes.empty() ) throw std::runtime_error( "frame image data read failed" );
    return result;
}

FrameImageDto TraceSessionFrameImageReader::Decode( size_t imageId, size_t maxBytes ) const
{
    if( imageId >= m_images.size() ) throw std::out_of_range( "frame image resource was not found" );
    const auto& image = m_images[imageId];
    const uint64_t outputBytes = uint64_t( image.width ) * image.height * 4;
    if( image.width > 4096 || image.height > 4096 || outputBytes > maxBytes )
        throw std::runtime_error( "decoded frame image exceeds resource budget" );
    const auto raw = ReadBc1( imageId, 0, size_t( image.dataBytes ) );
    FrameImageDto result;
    result.width = image.width;
    result.height = image.height;
    result.flipped = image.flipped;
    DecodeBc1( raw.bytes.data(), image.width, image.height, result.rgba );
    return result;
}

bool AuditTraceSessionFrameImageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionFrameImageStats& stats,
    std::string& error )
{
    const auto root = GpuAnalysisIoPath(
        TraceSessionFrameImageIndexRoot( sessionRoot, session ) );
    FrameImageManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_frame_image_identity_mismatch"; return false; }
    std::error_code ec;
    const auto metadataBytes = std::filesystem::file_size( root / MetadataFileName, ec );
    if( ec || metadataBytes != manifest.metadataFileBytes )
    { error = "session_frame_image_metadata_size_mismatch"; return false; }
    const auto dataBytes = std::filesystem::file_size( root / DataFileName, ec );
    if( ec || dataBytes != manifest.dataFileBytes )
    { error = "session_frame_image_data_size_mismatch"; return false; }
    if( Sha256File( root / MetadataFileName ) != manifest.metadataSha256 )
    { error = "session_frame_image_metadata_sha256_mismatch"; return false; }
    if( Sha256File( root / DataFileName ) != manifest.dataSha256 )
    { error = "session_frame_image_data_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

}
