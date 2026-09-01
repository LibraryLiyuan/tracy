#include "TracyTraceSessionIoGfx.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionExternalSort.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t FileMagic = 0x3147464f494e4aull;     // JNIOFG1
constexpr uint64_t ManifestMagic = 0x314d464f494e4aull; // JNIOFM1
constexpr const char* FileName = "io-gfx.bin";

#pragma pack( push, 1 )
struct FileHeader
{
    uint64_t magic = FileMagic;
    uint32_t schema = TraceSessionIoGfxIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t ioRequests = 0;
    uint64_t ioConfigs = 0;
    uint64_t ioStages = 0;
    uint64_t gfxDispatches = 0;
    uint64_t gfxEntities = 0;
    uint64_t gfxLinks = 0;
    uint64_t correlatedFrames = 0;
    uint64_t ioRequestOffset = 0;
    uint64_t ioConfigOffset = 0;
    uint64_t ioStageOffset = 0;
    uint64_t gfxDispatchOffset = 0;
    uint64_t gfxEntityOffset = 0;
    uint64_t gfxLinkOffset = 0;
    uint64_t frameOffset = 0;
    uint64_t dispatchFramePostingOffset = 0;
    uint64_t correlatedFramePostingOffset = 0;
    uint64_t gfxEntityIdPostingOffset = 0;
    uint64_t gfxParentPostingOffset = 0;
    uint64_t gfxLinkSourcePostingOffset = 0;
    uint64_t gfxLinkTargetPostingOffset = 0;
    uint64_t ioRequestIdPostingOffset = 0;
    uint64_t ioRequestIdPostingCount = 0;
    uint64_t ioRequestIds = 0;
    uint64_t gfxParentLinks = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredIoRequest
{
    int64_t timeNs = 0;
    uint64_t requestId = 0;
    uint64_t resourceId = 0;
    uint32_t thread = 0;
    uint8_t operation = 0;
    uint8_t source = 0;
    uint8_t priority = 0;
    uint8_t subsystem = 0;
    uint8_t flags = 0;
    uint8_t reserved[3] {};
};

struct StoredIoConfig
{
    uint64_t requestId = 0;
    uint64_t parentId = 0;
    uint64_t requestedBytes = 0;
    uint32_t originFrameSequence = 0;
    uint8_t parentKind = 0;
    uint8_t flags = 0;
    uint8_t reserved[2] {};
};

struct StoredIoStage
{
    int64_t timeNs = 0;
    uint64_t requestId = 0;
    uint64_t bytes = 0;
    uint32_t detail = 0;
    uint32_t thread = 0;
    uint8_t stage = 0;
    uint8_t status = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
};

struct StoredGfxDispatch
{
    int64_t timeNs = 0;
    uint64_t dispatchId = 0;
    uint64_t frameIndex = 0;
    uint32_t expectedJobs = 0;
    uint32_t thread = 0;
    uint8_t threadingMode = 0;
    uint8_t flags = 0;
    uint8_t reserved[2] {};
};

struct StoredGfxEntity
{
    int64_t timeNs = 0;
    uint64_t entityId = 0;
    uint64_t parentId = 0;
    uint32_t gpuQueryId = 0;
    uint32_t thread = 0;
    uint8_t gpuContext = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
};

struct StoredGfxLink
{
    int64_t timeNs = 0;
    uint64_t sourceId = 0;
    uint64_t targetId = 0;
    uint32_t thread = 0;
    uint8_t relation = 0;
    uint8_t flags = 0;
    uint8_t reserved[2] {};
};

struct StoredFrame
{
    int64_t timeNs = 0;
    uint64_t frameId = 0;
    uint64_t domainIndex = 0;
    uint32_t thread = 0;
    uint8_t domain = 0;
    uint8_t phase = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
};
#pragma pack( pop )

static_assert( sizeof( StoredIoRequest ) == 36 );
static_assert( sizeof( StoredIoConfig ) == 32 );
static_assert( sizeof( StoredIoStage ) == 36 );
static_assert( sizeof( StoredGfxDispatch ) == 36 );
static_assert( sizeof( StoredGfxEntity ) == 36 );
static_assert( sizeof( StoredGfxLink ) == 32 );
static_assert( sizeof( StoredFrame ) == 32 );

struct LocalManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionIoGfxStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_io_gfx_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_io_gfx_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_io_gfx_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_io_gfx_protocol_type_mismatch"; return false; }
    return true;
}

bool WriteRecord( std::ofstream& out, const void* data, size_t bytes,
    const char* failure, std::string& error )
{
    out.write( static_cast<const char*>( data ), std::streamsize( bytes ) );
    if( out ) return true;
    error = failure;
    return false;
}

struct BuildState
{
    TraceSessionTimeTransform transform;
    std::ofstream ioRequest;
    std::ofstream ioConfig;
    std::ofstream ioStage;
    std::ofstream gfxDispatch;
    std::ofstream gfxEntity;
    std::ofstream gfxLink;
    std::ofstream frame;
    std::ofstream dispatchFramePosting;
    std::ofstream correlatedFramePosting;
    std::ofstream gfxEntityIdPosting;
    std::ofstream gfxParentPosting;
    std::ofstream gfxLinkSourcePosting;
    std::ofstream gfxLinkTargetPosting;
    std::ofstream ioRequestIdPosting;
    TraceSessionIoGfxStats stats;
};

bool Visit( const TraceSessionCanonicalRecord& record, void* userData,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    switch( QueueType( record.type ) )
    {
    case QueueType::JnIoRequest:
    {
        if( state.stats.ioRequests >= ( uint64_t( 1 ) << 62 ) )
        { error = "session_io_request_ordinal_overflow"; return false; }
        StoredIoRequest value;
        value.timeNs = state.transform.ToNanoseconds( item.jnIoRequest.time );
        value.requestId = item.jnIoRequest.requestId;
        value.resourceId = item.jnIoRequest.resourceId;
        value.thread = record.threadContext;
        value.operation = item.jnIoRequest.operation;
        value.source = item.jnIoRequest.source;
        value.priority = item.jnIoRequest.priority;
        value.subsystem = item.jnIoRequest.subsystem;
        value.flags = item.jnIoRequest.flags;
        if( !WriteRecord( state.ioRequest, &value, sizeof( value ),
            "session_io_request_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair posting { value.requestId,
            state.stats.ioRequests };
        if( !WriteRecord( state.ioRequestIdPosting, &posting, sizeof( posting ),
            "session_io_request_id_posting_write_failed", error ) ) return false;
        ++state.stats.ioRequests;
        break;
    }
    case QueueType::JnIoConfig:
    {
        if( state.stats.ioConfigs >= ( uint64_t( 1 ) << 62 ) )
        { error = "session_io_config_ordinal_overflow"; return false; }
        StoredIoConfig value;
        value.requestId = item.jnIoConfig.requestId;
        value.parentId = item.jnIoConfig.parentId;
        value.requestedBytes = item.jnIoConfig.requestedBytes;
        value.originFrameSequence = item.jnIoConfig.originFrameSequence;
        value.parentKind = item.jnIoConfig.parentKind;
        value.flags = item.jnIoConfig.flags;
        if( !WriteRecord( state.ioConfig, &value, sizeof( value ),
            "session_io_config_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair posting { value.requestId,
            ( uint64_t( 1 ) << 62 ) | state.stats.ioConfigs };
        if( !WriteRecord( state.ioRequestIdPosting, &posting, sizeof( posting ),
            "session_io_config_id_posting_write_failed", error ) ) return false;
        ++state.stats.ioConfigs;
        break;
    }
    case QueueType::JnIoStage:
    {
        if( state.stats.ioStages >= ( uint64_t( 1 ) << 62 ) )
        { error = "session_io_stage_ordinal_overflow"; return false; }
        StoredIoStage value;
        value.timeNs = state.transform.ToNanoseconds( item.jnIoStage.time );
        value.requestId = item.jnIoStage.requestId;
        value.bytes = item.jnIoStage.bytes;
        value.detail = item.jnIoStage.detail;
        value.thread = record.threadContext;
        value.stage = item.jnIoStage.stage;
        value.status = item.jnIoStage.status;
        value.flags = item.jnIoStage.flags;
        if( !WriteRecord( state.ioStage, &value, sizeof( value ),
            "session_io_stage_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair posting { value.requestId,
            ( uint64_t( 2 ) << 62 ) | state.stats.ioStages };
        if( !WriteRecord( state.ioRequestIdPosting, &posting, sizeof( posting ),
            "session_io_stage_id_posting_write_failed", error ) ) return false;
        ++state.stats.ioStages;
        break;
    }
    case QueueType::JnGfxDispatch:
    {
        StoredGfxDispatch value;
        value.timeNs = state.transform.ToNanoseconds( item.jnGfxDispatch.time );
        value.dispatchId = item.jnGfxDispatch.dispatchId;
        value.frameIndex = item.jnGfxDispatch.frameIndex;
        value.expectedJobs = item.jnGfxDispatch.expectedJobs;
        value.thread = record.threadContext;
        value.threadingMode = item.jnGfxDispatch.threadingMode;
        value.flags = item.jnGfxDispatch.flags;
        if( !WriteRecord( state.gfxDispatch, &value, sizeof( value ),
            "session_gfx_dispatch_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair posting { value.frameIndex, state.stats.gfxDispatches };
        if( !WriteRecord( state.dispatchFramePosting, &posting, sizeof( posting ),
            "session_gfx_dispatch_frame_posting_write_failed", error ) ) return false;
        ++state.stats.gfxDispatches;
        break;
    }
    case QueueType::JnGfxEntity:
    {
        StoredGfxEntity value;
        value.timeNs = state.transform.ToNanoseconds( item.jnGfxEntity.time );
        value.entityId = item.jnGfxEntity.entityId;
        value.parentId = item.jnGfxEntity.parentId;
        value.gpuQueryId = item.jnGfxEntity.gpuQueryId;
        value.thread = record.threadContext;
        value.gpuContext = item.jnGfxEntity.gpuContext;
        value.kind = item.jnGfxEntity.kind;
        value.flags = item.jnGfxEntity.flags;
        if( !WriteRecord( state.gfxEntity, &value, sizeof( value ),
            "session_gfx_entity_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair identity { value.entityId, state.stats.gfxEntities };
        if( !WriteRecord( state.gfxEntityIdPosting, &identity, sizeof( identity ),
            "session_gfx_entity_id_posting_write_failed", error ) ) return false;
        if( value.parentId != 0 )
        {
            const TraceSessionUInt64Pair parent { value.parentId, state.stats.gfxEntities };
            if( !WriteRecord( state.gfxParentPosting, &parent, sizeof( parent ),
                "session_gfx_parent_posting_write_failed", error ) ) return false;
            ++state.stats.gfxParentLinks;
        }
        ++state.stats.gfxEntities;
        break;
    }
    case QueueType::JnGfxLink:
    {
        StoredGfxLink value;
        value.timeNs = state.transform.ToNanoseconds( item.jnGfxLink.time );
        value.sourceId = item.jnGfxLink.sourceId;
        value.targetId = item.jnGfxLink.targetId;
        value.thread = record.threadContext;
        value.relation = item.jnGfxLink.relation;
        value.flags = item.jnGfxLink.flags;
        if( !WriteRecord( state.gfxLink, &value, sizeof( value ),
            "session_gfx_link_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair source { value.sourceId, state.stats.gfxLinks };
        const TraceSessionUInt64Pair target { value.targetId, state.stats.gfxLinks };
        if( !WriteRecord( state.gfxLinkSourcePosting, &source, sizeof( source ),
                "session_gfx_link_source_posting_write_failed", error ) ||
            !WriteRecord( state.gfxLinkTargetPosting, &target, sizeof( target ),
                "session_gfx_link_target_posting_write_failed", error ) ) return false;
        ++state.stats.gfxLinks;
        break;
    }
    case QueueType::JnFrame:
    {
        StoredFrame value;
        value.timeNs = state.transform.ToNanoseconds( item.jnFrame.time );
        value.frameId = item.jnFrame.frameId;
        value.domainIndex = item.jnFrame.domainIndex;
        value.thread = record.threadContext;
        value.domain = item.jnFrame.domain;
        value.phase = item.jnFrame.phase;
        value.flags = item.jnFrame.flags;
        if( !WriteRecord( state.frame, &value, sizeof( value ),
            "session_correlated_frame_write_failed", error ) ) return false;
        const TraceSessionUInt64Pair posting { value.frameId, state.stats.correlatedFrames };
        if( !WriteRecord( state.correlatedFramePosting, &posting, sizeof( posting ),
            "session_correlated_frame_posting_write_failed", error ) ) return false;
        ++state.stats.correlatedFrames;
        break;
    }
    default: break;
    }
    return true;
}

bool CopyFile( const std::filesystem::path& path, std::ofstream& out,
    uint64_t& bytes, std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_io_gfx_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    bytes = 0;
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 )
        {
            out.write( buffer.data(), count );
            bytes += uint64_t( count );
        }
    }
    if( !in.eof() || !out ) { error = "session_io_gfx_work_copy_failed"; return false; }
    return true;
}

bool CountDistinctPairKeys( const std::filesystem::path& path,
    uint64_t expectedCount, uint64_t& distinct, std::string& error )
{
    distinct = 0;
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_io_request_id_posting_open_failed"; return false; }
    uint64_t previous = 0;
    bool havePrevious = false;
    for( uint64_t index = 0; index < expectedCount; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
        { error = "session_io_request_id_posting_truncated"; return false; }
        if( !havePrevious || pair.key != previous )
        {
            ++distinct;
            previous = pair.key;
            havePrevious = true;
        }
    }
    char trailing = 0;
    if( in.read( &trailing, 1 ) )
    { error = "session_io_request_id_posting_trailing_bytes"; return false; }
    return true;
}

bool CheckedAppend( uint64_t& value, uint64_t count, uint64_t itemBytes,
    std::string& error )
{
    if( itemBytes != 0 && count > std::numeric_limits<uint64_t>::max() / itemBytes )
    { error = "session_io_gfx_file_size_overflow"; return false; }
    const auto bytes = count * itemBytes;
    if( value > std::numeric_limits<uint64_t>::max() - bytes )
    { error = "session_io_gfx_file_size_overflow"; return false; }
    value += bytes;
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const LocalManifest& manifest, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_io_gfx_manifest_open_failed"; return false; }
    out << "magic " << ManifestMagic << '\n';
    out << "schema " << TraceSessionIoGfxIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( manifest.sourceSha256 ) << '\n';
    out << "source_size " << manifest.sourceSize << '\n';
    out << "generation " << std::quoted( manifest.generation ) << '\n';
    out << "file_bytes " << manifest.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( manifest.fileSha256 ) << '\n';
    out << "io_requests " << manifest.stats.ioRequests << '\n';
    out << "io_configs " << manifest.stats.ioConfigs << '\n';
    out << "io_stages " << manifest.stats.ioStages << '\n';
    out << "io_request_ids " << manifest.stats.ioRequestIds << '\n';
    out << "gfx_dispatches " << manifest.stats.gfxDispatches << '\n';
    out << "gfx_entities " << manifest.stats.gfxEntities << '\n';
    out << "gfx_parent_links " << manifest.stats.gfxParentLinks << '\n';
    out << "gfx_links " << manifest.stats.gfxLinks << '\n';
    out << "correlated_frames " << manifest.stats.correlatedFrames << '\n';
    out.flush();
    if( !out ) { error = "session_io_gfx_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    LocalManifest& manifest, std::string& error )
{
    manifest = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_io_gfx_manifest_not_found"; return false; }
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
        else if( key == "file_bytes" ) in >> manifest.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( manifest.fileSha256 );
        else if( key == "io_requests" ) in >> manifest.stats.ioRequests;
        else if( key == "io_configs" ) in >> manifest.stats.ioConfigs;
        else if( key == "io_stages" ) in >> manifest.stats.ioStages;
        else if( key == "io_request_ids" ) in >> manifest.stats.ioRequestIds;
        else if( key == "gfx_dispatches" ) in >> manifest.stats.gfxDispatches;
        else if( key == "gfx_entities" ) in >> manifest.stats.gfxEntities;
        else if( key == "gfx_parent_links" ) in >> manifest.stats.gfxParentLinks;
        else if( key == "gfx_links" ) in >> manifest.stats.gfxLinks;
        else if( key == "correlated_frames" ) in >> manifest.stats.correlatedFrames;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_io_gfx_manifest_parse_failed"; return false; }
    }
    manifest.stats.fileBytes = manifest.fileBytes;
    if( magic != ManifestMagic || schema != TraceSessionIoGfxIndexSchemaVersion ||
        manifest.sourceSha256.size() != 64 || manifest.fileSha256.size() != 64 )
    { error = "session_io_gfx_manifest_invalid"; return false; }
    return true;
}

bool ValidateFile( std::ifstream& in, const TraceSessionManifest& session,
    const LocalManifest& manifest, FileHeader& header, std::string& error )
{
    in.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    const bool ioPostingCountValid =
        header.ioRequests <= std::numeric_limits<uint64_t>::max() - header.ioConfigs &&
        header.ioRequests + header.ioConfigs <=
            std::numeric_limits<uint64_t>::max() - header.ioStages &&
        header.ioRequestIdPostingCount ==
            header.ioRequests + header.ioConfigs + header.ioStages;
    if( !in || header.magic != FileMagic ||
        header.schema != TraceSessionIoGfxIndexSchemaVersion || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.generationBytes != session.generation.size() ||
        header.reserved != 0 || header.ioRequests != manifest.stats.ioRequests ||
        header.ioConfigs != manifest.stats.ioConfigs || header.ioStages != manifest.stats.ioStages ||
        !ioPostingCountValid ||
        header.ioRequestIds != manifest.stats.ioRequestIds ||
        header.gfxDispatches != manifest.stats.gfxDispatches ||
        header.gfxEntities != manifest.stats.gfxEntities ||
        header.gfxParentLinks != manifest.stats.gfxParentLinks ||
        header.gfxLinks != manifest.stats.gfxLinks ||
        header.correlatedFrames != manifest.stats.correlatedFrames )
    { error = "session_io_gfx_file_header_invalid"; return false; }
    std::string source( 64, '\0' ), generation( header.generationBytes, '\0' );
    in.read( source.data(), std::streamsize( source.size() ) );
    if( !generation.empty() ) in.read( generation.data(), std::streamsize( generation.size() ) );
    uint64_t expected = sizeof( header );
    bool valid = CheckedAppend( expected, 1, source.size(), error ) &&
        CheckedAppend( expected, 1, generation.size(), error ) && header.ioRequestOffset == expected;
    valid = valid && CheckedAppend( expected, header.ioRequests, sizeof( StoredIoRequest ), error ) &&
        header.ioConfigOffset == expected;
    valid = valid && CheckedAppend( expected, header.ioConfigs, sizeof( StoredIoConfig ), error ) &&
        header.ioStageOffset == expected;
    valid = valid && CheckedAppend( expected, header.ioStages, sizeof( StoredIoStage ), error ) &&
        header.gfxDispatchOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxDispatches, sizeof( StoredGfxDispatch ), error ) &&
        header.gfxEntityOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxEntities, sizeof( StoredGfxEntity ), error ) &&
        header.gfxLinkOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxLinks, sizeof( StoredGfxLink ), error ) &&
        header.frameOffset == expected;
    valid = valid && CheckedAppend( expected, header.correlatedFrames, sizeof( StoredFrame ), error ) &&
        header.dispatchFramePostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxDispatches,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.correlatedFramePostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.correlatedFrames,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.gfxEntityIdPostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxEntities,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.gfxParentPostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxParentLinks,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.gfxLinkSourcePostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxLinks,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.gfxLinkTargetPostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.gfxLinks,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        header.ioRequestIdPostingOffset == expected;
    valid = valid && CheckedAppend( expected, header.ioRequestIdPostingCount,
        sizeof( TraceSessionUInt64Pair ), error ) &&
        expected == manifest.fileBytes;
    if( !in || source != session.source.sha256 || generation != session.generation || !valid )
    {
        if( error == "session_io_gfx_file_size_overflow" )
            error = "session_io_gfx_file_identity_or_layout_mismatch";
        else if( error.empty() ) error = "session_io_gfx_file_identity_or_layout_mismatch";
        return false;
    }
    return true;
}

bool VerifyFiles( const std::filesystem::path& root, const TraceSessionManifest& session,
    LocalManifest& manifest, FileHeader& header, std::string& error )
{
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_io_gfx_identity_mismatch"; return false; }
    const auto path = root / FileName;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( path, ec );
    if( ec || bytes != manifest.fileBytes )
    { error = "session_io_gfx_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_io_gfx_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_io_gfx_file_open_failed"; return false; }
    return ValidateFile( in, session, manifest, header, error );
}

template<typename T>
std::vector<T> ReadFixed( const std::filesystem::path& path, uint64_t offset,
    uint64_t count, const char* failure )
{
    if( count > std::numeric_limits<size_t>::max() ||
        count > uint64_t( std::numeric_limits<std::streamsize>::max() ) / sizeof( T ) )
        throw std::runtime_error( "I/O/Gfx Session result exceeds platform capacity" );
    std::vector<T> values( static_cast<size_t>( count ) );
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "I/O/Gfx Session index is unavailable" );
    in.seekg( std::streamoff( offset ), std::ios::beg );
    if( !values.empty() ) in.read( reinterpret_cast<char*>( values.data() ),
        std::streamsize( values.size() * sizeof( T ) ) );
    if( !in && !values.empty() ) throw std::runtime_error( failure );
    return values;
}

template<typename Stored, typename Dto, typename Decode>
std::vector<Dto> ReadFramePosting( const std::filesystem::path& path,
    uint64_t postingOffset, uint64_t postingCount, uint64_t frameId,
    size_t offset, size_t limit, uint64_t recordOffset, uint64_t recordCount,
    Decode&& decode )
{
    std::vector<Dto> result;
    if( postingCount == 0 || limit == 0 ) return result;
    std::ifstream pairs( path, std::ios::binary );
    std::ifstream records( path, std::ios::binary );
    if( !pairs || !records )
        throw std::runtime_error( "Session I/O/Gfx Frame posting is unavailable" );
    const auto lowerBound = [&]( bool upper )
    {
        uint64_t first = 0, last = postingCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            TraceSessionUInt64Pair pair;
            pairs.clear();
            pairs.seekg( std::streamoff( postingOffset + middle * sizeof( pair ) ) );
            if( !pairs.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
                throw std::runtime_error( "Session I/O/Gfx Frame posting binary search failed" );
            if( pair.key < frameId || ( upper && pair.key == frameId ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lowerBound( false );
    const auto end = lowerBound( true );
    if( begin >= end ) return result;
    result.reserve( std::min<uint64_t>( limit, end - begin ) );
    pairs.clear();
    pairs.seekg( std::streamoff( postingOffset + begin * sizeof( TraceSessionUInt64Pair ) ) );
    size_t skipped = 0;
    for( uint64_t index = begin; index < end; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !pairs.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) || pair.key != frameId )
            throw std::runtime_error( "Session I/O/Gfx Frame posting is truncated" );
        if( pair.value >= recordCount )
            throw std::runtime_error( "Session I/O/Gfx Frame posting record is out of range" );
        if( skipped++ < offset ) continue;
        Stored stored;
        records.clear();
        records.seekg( std::streamoff( recordOffset + pair.value * sizeof( Stored ) ) );
        if( !records.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) )
            throw std::runtime_error( "Session I/O/Gfx Frame posting target is truncated" );
        result.emplace_back( decode( stored, pair.value ) );
        if( result.size() >= limit ) break;
    }
    return result;
}

std::vector<uint64_t> ReadPostingValues( const std::filesystem::path& path,
    uint64_t postingOffset, uint64_t postingCount, uint64_t key )
{
    std::vector<uint64_t> result;
    if( postingCount == 0 ) return result;
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "Session I/O posting is unavailable" );
    const auto lowerBound = [&]( bool upper )
    {
        uint64_t first = 0, last = postingCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            TraceSessionUInt64Pair pair;
            in.clear();
            in.seekg( std::streamoff( postingOffset + middle * sizeof( pair ) ) );
            if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
                throw std::runtime_error( "Session I/O posting binary search failed" );
            if( pair.key < key || ( upper && pair.key == key ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lowerBound( false );
    const auto end = lowerBound( true );
    result.reserve( size_t( end - begin ) );
    in.clear();
    in.seekg( std::streamoff( postingOffset + begin * sizeof( TraceSessionUInt64Pair ) ) );
    for( uint64_t index = begin; index < end; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) || pair.key != key )
            throw std::runtime_error( "Session I/O posting is truncated" );
        result.emplace_back( pair.value );
    }
    return result;
}

template<typename Stored, typename FrameKey>
bool ValidateFramePosting( const std::filesystem::path& path,
    uint64_t postingOffset, uint64_t postingCount, uint64_t recordOffset,
    uint64_t recordCount, FrameKey&& frameKey, const char* failure,
    std::string& error )
{
    std::ifstream pairs( path, std::ios::binary );
    std::ifstream records( path, std::ios::binary );
    if( !pairs || !records ) { error = std::string( failure ) + "_open_failed"; return false; }
    pairs.seekg( std::streamoff( postingOffset ) );
    TraceSessionUInt64Pair previous {};
    bool havePrevious = false;
    for( uint64_t index = 0; index < postingCount; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !pairs.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
        { error = std::string( failure ) + "_truncated"; return false; }
        if( havePrevious && ( pair.key < previous.key ||
            ( pair.key == previous.key && pair.value < previous.value ) ) )
        { error = std::string( failure ) + "_not_sorted"; return false; }
        if( pair.value >= recordCount )
        { error = std::string( failure ) + "_record_out_of_range"; return false; }
        Stored stored;
        records.clear();
        records.seekg( std::streamoff( recordOffset + pair.value * sizeof( Stored ) ) );
        if( !records.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) )
        { error = std::string( failure ) + "_target_truncated"; return false; }
        if( frameKey( stored ) != pair.key )
        { error = std::string( failure ) + "_key_mismatch"; return false; }
        previous = pair;
        havePrevious = true;
    }
    return true;
}

bool ValidateIoRequestPosting( const std::filesystem::path& path,
    const FileHeader& header, std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    std::ifstream records( path, std::ios::binary );
    if( !in || !records )
    { error = "session_io_request_id_posting_open_failed"; return false; }
    in.seekg( std::streamoff( header.ioRequestIdPostingOffset ) );
    TraceSessionUInt64Pair previous {};
    bool havePrevious = false;
    uint64_t distinct = 0;
    constexpr uint64_t TypeShift = 62;
    constexpr uint64_t OrdinalMask = ( uint64_t( 1 ) << TypeShift ) - 1;
    for( uint64_t index = 0; index < header.ioRequestIdPostingCount; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
        { error = "session_io_request_id_posting_truncated"; return false; }
        if( havePrevious && ( pair.key < previous.key ||
            ( pair.key == previous.key && pair.value < previous.value ) ) )
        { error = "session_io_request_id_posting_not_sorted"; return false; }
        if( !havePrevious || pair.key != previous.key ) ++distinct;
        const auto type = pair.value >> TypeShift;
        const auto ordinal = pair.value & OrdinalMask;
        uint64_t targetId = 0;
        if( type == 0 && ordinal < header.ioRequests )
        {
            StoredIoRequest value;
            records.clear(); records.seekg( std::streamoff(
                header.ioRequestOffset + ordinal * sizeof( value ) ) );
            if( !records.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            { error = "session_io_request_id_posting_target_truncated"; return false; }
            targetId = value.requestId;
        }
        else if( type == 1 && ordinal < header.ioConfigs )
        {
            StoredIoConfig value;
            records.clear(); records.seekg( std::streamoff(
                header.ioConfigOffset + ordinal * sizeof( value ) ) );
            if( !records.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            { error = "session_io_config_id_posting_target_truncated"; return false; }
            targetId = value.requestId;
        }
        else if( type == 2 && ordinal < header.ioStages )
        {
            StoredIoStage value;
            records.clear(); records.seekg( std::streamoff(
                header.ioStageOffset + ordinal * sizeof( value ) ) );
            if( !records.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            { error = "session_io_stage_id_posting_target_truncated"; return false; }
            targetId = value.requestId;
        }
        else
        { error = "session_io_request_id_posting_record_out_of_range"; return false; }
        if( targetId != pair.key )
        { error = "session_io_request_id_posting_key_mismatch"; return false; }
        previous = pair;
        havePrevious = true;
    }
    if( distinct != header.ioRequestIds )
    { error = "session_io_request_id_count_mismatch"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

IoRequestDto MakeIoRequest( const std::string& fingerprint, uint64_t requestId )
{
    IoRequestDto result;
    result.ref = MakeRef( fingerprint, "io-request", requestId );
    result.requestId = requestId;
    result.orphan = true;
    return result;
}

void ApplyIoRequest( IoRequestDto& request, const StoredIoRequest& value,
    const std::string& fingerprint )
{
    request.resourceId = value.resourceId;
    request.queueThreadRef = MakeRef( fingerprint, "thread", value.thread );
    request.queueNs = value.timeNs;
    request.operation = value.operation;
    request.source = value.source;
    request.priority = value.priority;
    request.subsystem = value.subsystem;
    request.flags = value.flags;
    request.captureBoundary = request.captureBoundary ||
        ( value.flags & uint8_t( JnIoFlags::CaptureBoundary ) ) != 0;
    request.orphan = false;
}

void ApplyIoConfig( IoRequestDto& request, const StoredIoConfig& value )
{
    request.parentId = value.parentId;
    request.requestedBytes = value.requestedBytes;
    request.originFrameSequence = value.originFrameSequence;
    request.parentKind = value.parentKind;
    request.configFlags = value.flags;
    request.captureBoundary = request.captureBoundary ||
        ( value.flags & uint8_t( JnIoFlags::CaptureBoundary ) ) != 0;
}

void ApplyIoStage( IoRequestDto& request, const StoredIoStage& value,
    const std::string& fingerprint )
{
    request.stages.push_back( { value.timeNs,
        MakeRef( fingerprint, "thread", value.thread ), value.bytes, value.detail,
        value.stage, value.status, value.flags } );
    request.captureBoundary = request.captureBoundary ||
        ( value.flags & uint8_t( JnIoFlags::CaptureBoundary ) ) != 0;
    switch( JnIoStage( value.stage ) )
    {
    case JnIoStage::Start:
        if( !request.startNs || value.timeNs < *request.startNs )
            request.startNs = value.timeNs;
        break;
    case JnIoStage::Complete:
    case JnIoStage::Error:
    case JnIoStage::Cancel:
        ++request.terminalCount;
        if( !request.endNs || value.timeNs > *request.endNs )
            request.endNs = value.timeNs;
        request.transferredBytes = value.bytes;
        request.status = value.status;
        break;
    case JnIoStage::RequestCallstack: request.requestCallstack = value.detail; break;
    case JnIoStage::Requeue: request.status = value.status; break;
    }
}

void FinalizeIoRequest( IoRequestDto& request )
{
    request.truncated = !request.endNs.has_value();
    std::sort( request.stages.begin(), request.stages.end(),
        []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );
}

}

std::filesystem::path TraceSessionIoGfxIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "io-gfx-index" / std::to_string( TraceSessionIoGfxIndexSchemaVersion ) / "exact";
}

bool BuildTraceSessionIoGfxDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionIoGfxStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    if( session.source.sha256.size() != 64 ||
        session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_io_gfx_identity_invalid"; return false; }
    const auto root = TraceSessionIoGfxIndexRoot( sessionRoot, session );
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_io_gfx_directory_failed:" + ec.message(); return false; }
    const std::array<std::filesystem::path, 7> work = {
        root / "io-request.work", root / "io-config.work", root / "io-stage.work",
        root / "gfx-dispatch.work", root / "gfx-entity.work", root / "gfx-link.work",
        root / "frame.work" };
    const std::array<std::filesystem::path, 7> postingSource = {
        root / "dispatch-frame-posting-source.work",
        root / "correlated-frame-posting-source.work",
        root / "gfx-entity-id-posting-source.work",
        root / "gfx-parent-posting-source.work",
        root / "gfx-link-source-posting-source.work",
        root / "gfx-link-target-posting-source.work",
        root / "io-request-id-posting-source.work" };
    const std::array<std::filesystem::path, 7> postingSorted = {
        root / "dispatch-frame-posting-sorted.work",
        root / "correlated-frame-posting-sorted.work",
        root / "gfx-entity-id-posting-sorted.work",
        root / "gfx-parent-posting-sorted.work",
        root / "gfx-link-source-posting-sorted.work",
        root / "gfx-link-target-posting-sorted.work",
        root / "io-request-id-posting-sorted.work" };
    BuildState state;
    state.ioRequest.open( work[0], std::ios::binary | std::ios::trunc );
    state.ioConfig.open( work[1], std::ios::binary | std::ios::trunc );
    state.ioStage.open( work[2], std::ios::binary | std::ios::trunc );
    state.gfxDispatch.open( work[3], std::ios::binary | std::ios::trunc );
    state.gfxEntity.open( work[4], std::ios::binary | std::ios::trunc );
    state.gfxLink.open( work[5], std::ios::binary | std::ios::trunc );
    state.frame.open( work[6], std::ios::binary | std::ios::trunc );
    state.dispatchFramePosting.open( postingSource[0], std::ios::binary | std::ios::trunc );
    state.correlatedFramePosting.open( postingSource[1], std::ios::binary | std::ios::trunc );
    state.gfxEntityIdPosting.open( postingSource[2], std::ios::binary | std::ios::trunc );
    state.gfxParentPosting.open( postingSource[3], std::ios::binary | std::ios::trunc );
    state.gfxLinkSourcePosting.open( postingSource[4], std::ios::binary | std::ios::trunc );
    state.gfxLinkTargetPosting.open( postingSource[5], std::ios::binary | std::ios::trunc );
    state.ioRequestIdPosting.open( postingSource[6], std::ios::binary | std::ios::trunc );
    if( !state.ioRequest || !state.ioConfig || !state.ioStage || !state.gfxDispatch ||
        !state.gfxEntity || !state.gfxLink || !state.frame ||
        !state.dispatchFramePosting || !state.correlatedFramePosting ||
        !state.gfxEntityIdPosting || !state.gfxParentPosting ||
        !state.gfxLinkSourcePosting || !state.gfxLinkTargetPosting ||
        !state.ioRequestIdPosting )
    { error = "session_io_gfx_work_open_failed"; return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, session, Visit, &state, error ) ) return false;
    state.ioRequest.close(); state.ioConfig.close(); state.ioStage.close();
    state.gfxDispatch.close(); state.gfxEntity.close(); state.gfxLink.close(); state.frame.close();
    state.dispatchFramePosting.close(); state.correlatedFramePosting.close();
    state.gfxEntityIdPosting.close(); state.gfxParentPosting.close();
    state.gfxLinkSourcePosting.close(); state.gfxLinkTargetPosting.close();
    state.ioRequestIdPosting.close();
    if( state.stats.ioRequests > std::numeric_limits<uint64_t>::max() - state.stats.ioConfigs ||
        state.stats.ioRequests + state.stats.ioConfigs >
            std::numeric_limits<uint64_t>::max() - state.stats.ioStages )
    { error = "session_io_request_id_posting_count_overflow"; return false; }
    const auto ioRequestIdPostingCount = state.stats.ioRequests +
        state.stats.ioConfigs + state.stats.ioStages;
    constexpr uint64_t MaximumBufferedPostingPairs = 4ull * 1024 * 1024;
    if( !SortTraceSessionUInt64Pairs( postingSource[0], postingSorted[0], root,
            "io-gfx-dispatch-frame-posting", state.stats.gfxDispatches,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[1], postingSorted[1], root,
            "io-gfx-correlated-frame-posting", state.stats.correlatedFrames,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[2], postingSorted[2], root,
            "io-gfx-entity-id-posting", state.stats.gfxEntities,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[3], postingSorted[3], root,
            "io-gfx-parent-posting", state.stats.gfxParentLinks,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[4], postingSorted[4], root,
            "io-gfx-link-source-posting", state.stats.gfxLinks,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[5], postingSorted[5], root,
            "io-gfx-link-target-posting", state.stats.gfxLinks,
            MaximumBufferedPostingPairs, error ) ||
        !SortTraceSessionUInt64Pairs( postingSource[6], postingSorted[6], root,
            "io-request-id-posting", ioRequestIdPostingCount,
            MaximumBufferedPostingPairs, error ) ||
        !CountDistinctPairKeys( postingSorted[6], ioRequestIdPostingCount,
            state.stats.ioRequestIds, error ) ) return false;

    FileHeader header;
    header.sourceSize = session.source.fileSize;
    header.ioRequests = state.stats.ioRequests;
    header.ioConfigs = state.stats.ioConfigs;
    header.ioStages = state.stats.ioStages;
    header.gfxDispatches = state.stats.gfxDispatches;
    header.gfxEntities = state.stats.gfxEntities;
    header.gfxLinks = state.stats.gfxLinks;
    header.correlatedFrames = state.stats.correlatedFrames;
    header.gfxParentLinks = state.stats.gfxParentLinks;
    header.ioRequestIdPostingCount = ioRequestIdPostingCount;
    header.ioRequestIds = state.stats.ioRequestIds;
    header.generationBytes = uint32_t( session.generation.size() );
    uint64_t next = sizeof( header );
    if( !CheckedAppend( next, 1, 64 + session.generation.size(), error ) ) return false;
    header.ioRequestOffset = next;
    if( !CheckedAppend( next, state.stats.ioRequests, sizeof( StoredIoRequest ), error ) ) return false;
    header.ioConfigOffset = next;
    if( !CheckedAppend( next, state.stats.ioConfigs, sizeof( StoredIoConfig ), error ) ) return false;
    header.ioStageOffset = next;
    if( !CheckedAppend( next, state.stats.ioStages, sizeof( StoredIoStage ), error ) ) return false;
    header.gfxDispatchOffset = next;
    if( !CheckedAppend( next, state.stats.gfxDispatches, sizeof( StoredGfxDispatch ), error ) ) return false;
    header.gfxEntityOffset = next;
    if( !CheckedAppend( next, state.stats.gfxEntities, sizeof( StoredGfxEntity ), error ) ) return false;
    header.gfxLinkOffset = next;
    if( !CheckedAppend( next, state.stats.gfxLinks, sizeof( StoredGfxLink ), error ) ) return false;
    header.frameOffset = next;
    if( !CheckedAppend( next, state.stats.correlatedFrames, sizeof( StoredFrame ), error ) ) return false;
    header.dispatchFramePostingOffset = next;
    if( !CheckedAppend( next, state.stats.gfxDispatches,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.correlatedFramePostingOffset = next;
    if( !CheckedAppend( next, state.stats.correlatedFrames,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.gfxEntityIdPostingOffset = next;
    if( !CheckedAppend( next, state.stats.gfxEntities,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.gfxParentPostingOffset = next;
    if( !CheckedAppend( next, state.stats.gfxParentLinks,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.gfxLinkSourcePostingOffset = next;
    if( !CheckedAppend( next, state.stats.gfxLinks,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.gfxLinkTargetPostingOffset = next;
    if( !CheckedAppend( next, state.stats.gfxLinks,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;
    header.ioRequestIdPostingOffset = next;
    if( !CheckedAppend( next, ioRequestIdPostingCount,
        sizeof( TraceSessionUInt64Pair ), error ) ) return false;

    const auto temporary = root / ( std::string( FileName ) + ".tmp" );
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_io_gfx_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    if( !session.generation.empty() ) out.write( session.generation.data(),
        std::streamsize( session.generation.size() ) );
    const std::array<uint64_t, 14> expected = {
        state.stats.ioRequests * sizeof( StoredIoRequest ),
        state.stats.ioConfigs * sizeof( StoredIoConfig ),
        state.stats.ioStages * sizeof( StoredIoStage ),
        state.stats.gfxDispatches * sizeof( StoredGfxDispatch ),
        state.stats.gfxEntities * sizeof( StoredGfxEntity ),
        state.stats.gfxLinks * sizeof( StoredGfxLink ),
        state.stats.correlatedFrames * sizeof( StoredFrame ),
        state.stats.gfxDispatches * sizeof( TraceSessionUInt64Pair ),
        state.stats.correlatedFrames * sizeof( TraceSessionUInt64Pair ),
        state.stats.gfxEntities * sizeof( TraceSessionUInt64Pair ),
        state.stats.gfxParentLinks * sizeof( TraceSessionUInt64Pair ),
        state.stats.gfxLinks * sizeof( TraceSessionUInt64Pair ),
        state.stats.gfxLinks * sizeof( TraceSessionUInt64Pair ),
        ioRequestIdPostingCount * sizeof( TraceSessionUInt64Pair ) };
    uint64_t copied = 0;
    for( size_t i = 0; i < work.size(); ++i )
        if( !CopyFile( work[i], out, copied, error ) || copied != expected[i] )
        { if( error.empty() ) error = "session_io_gfx_work_size_mismatch"; return false; }
    for( size_t i = 0; i < postingSorted.size(); ++i )
        if( !CopyFile( postingSorted[i], out, copied, error ) ||
            copied != expected[work.size() + i] )
        { if( error.empty() ) error = "session_io_gfx_posting_size_mismatch"; return false; }
    out.flush();
    if( !out ) { error = "session_io_gfx_file_finalize_failed"; return false; }
    out.close();
    const auto target = root / FileName;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    for( const auto& path : work ) { ec.clear(); std::filesystem::remove( path, ec ); }
    for( const auto& path : postingSource ) { ec.clear(); std::filesystem::remove( path, ec ); }
    for( const auto& path : postingSorted ) { ec.clear(); std::filesystem::remove( path, ec ); }

    LocalManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = next;
    manifest.fileSha256 = Sha256File( target );
    state.stats.fileBytes = next;
    manifest.stats = state.stats;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionIoGfxReader> TraceSessionIoGfxReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    LocalManifest manifest;
    FileHeader header;
    const auto root = TraceSessionIoGfxIndexRoot( sessionRoot, session );
    if( !VerifyFiles( root, session, manifest, header, error ) ) return {};
    auto reader = std::make_shared<TraceSessionIoGfxReader>();
    reader->m_path = root / FileName;
    reader->m_fingerprint = session.source.sha256;
    reader->m_ioRequestOffset = header.ioRequestOffset;
    reader->m_ioConfigOffset = header.ioConfigOffset;
    reader->m_ioStageOffset = header.ioStageOffset;
    reader->m_gfxDispatchOffset = header.gfxDispatchOffset;
    reader->m_gfxEntityOffset = header.gfxEntityOffset;
    reader->m_gfxLinkOffset = header.gfxLinkOffset;
    reader->m_frameOffset = header.frameOffset;
    reader->m_dispatchFramePostingOffset = header.dispatchFramePostingOffset;
    reader->m_correlatedFramePostingOffset = header.correlatedFramePostingOffset;
    reader->m_gfxEntityIdPostingOffset = header.gfxEntityIdPostingOffset;
    reader->m_gfxParentPostingOffset = header.gfxParentPostingOffset;
    reader->m_gfxLinkSourcePostingOffset = header.gfxLinkSourcePostingOffset;
    reader->m_gfxLinkTargetPostingOffset = header.gfxLinkTargetPostingOffset;
    reader->m_ioRequestIdPostingOffset = header.ioRequestIdPostingOffset;
    reader->m_ioRequestIdPostingCount = header.ioRequestIdPostingCount;
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<IoRequestDto> TraceSessionIoGfxReader::IoRequests() const
{
    const auto requests = ReadFixed<StoredIoRequest>( m_path, m_ioRequestOffset,
        m_stats.ioRequests, "I/O request index read failed" );
    const auto configs = ReadFixed<StoredIoConfig>( m_path, m_ioConfigOffset,
        m_stats.ioConfigs, "I/O config index read failed" );
    const auto stages = ReadFixed<StoredIoStage>( m_path, m_ioStageOffset,
        m_stats.ioStages, "I/O stage index read failed" );
    std::map<uint64_t, IoRequestDto> values;
    const auto ensure = [&]( uint64_t requestId ) -> IoRequestDto& {
        auto [it, inserted] = values.try_emplace( requestId );
        if( inserted ) it->second = MakeIoRequest( m_fingerprint, requestId );
        return it->second;
    };
    for( const auto& value : requests ) ApplyIoRequest( ensure( value.requestId ), value, m_fingerprint );
    for( const auto& value : configs ) ApplyIoConfig( ensure( value.requestId ), value );
    for( const auto& value : stages ) ApplyIoStage( ensure( value.requestId ), value, m_fingerprint );
    std::vector<IoRequestDto> result;
    result.reserve( values.size() );
    for( auto& [id, request] : values )
    {
        FinalizeIoRequest( request );
        result.emplace_back( std::move( request ) );
    }
    return result;
}

std::optional<IoRequestDto> TraceSessionIoGfxReader::IoRequest( uint64_t requestId ) const
{
    const auto records = ReadPostingValues( m_path, m_ioRequestIdPostingOffset,
        m_ioRequestIdPostingCount, requestId );
    if( records.empty() ) return std::nullopt;
    IoRequestDto result = MakeIoRequest( m_fingerprint, requestId );
    std::ifstream in( m_path, std::ios::binary );
    if( !in ) throw std::runtime_error( "Session I/O request index is unavailable" );
    constexpr uint64_t TypeShift = 62;
    constexpr uint64_t OrdinalMask = ( uint64_t( 1 ) << TypeShift ) - 1;
    for( const auto encoded : records )
    {
        const auto type = encoded >> TypeShift;
        const auto ordinal = encoded & OrdinalMask;
        if( type == 0 )
        {
            if( ordinal >= m_stats.ioRequests )
                throw std::runtime_error( "Session I/O request posting is out of range" );
            StoredIoRequest value;
            in.clear(); in.seekg( std::streamoff( m_ioRequestOffset + ordinal * sizeof( value ) ) );
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
                throw std::runtime_error( "Session I/O request posting target is truncated" );
            ApplyIoRequest( result, value, m_fingerprint );
        }
        else if( type == 1 )
        {
            if( ordinal >= m_stats.ioConfigs )
                throw std::runtime_error( "Session I/O config posting is out of range" );
            StoredIoConfig value;
            in.clear(); in.seekg( std::streamoff( m_ioConfigOffset + ordinal * sizeof( value ) ) );
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
                throw std::runtime_error( "Session I/O config posting target is truncated" );
            ApplyIoConfig( result, value );
        }
        else if( type == 2 )
        {
            if( ordinal >= m_stats.ioStages )
                throw std::runtime_error( "Session I/O stage posting is out of range" );
            StoredIoStage value;
            in.clear(); in.seekg( std::streamoff( m_ioStageOffset + ordinal * sizeof( value ) ) );
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
                throw std::runtime_error( "Session I/O stage posting target is truncated" );
            ApplyIoStage( result, value, m_fingerprint );
        }
        else throw std::runtime_error( "Session I/O request posting type is invalid" );
    }
    FinalizeIoRequest( result );
    return result;
}

std::vector<GfxDispatchDto> TraceSessionIoGfxReader::GfxDispatches() const
{
    const auto stored = ReadFixed<StoredGfxDispatch>( m_path, m_gfxDispatchOffset,
        m_stats.gfxDispatches, "Gfx dispatch index read failed" );
    std::vector<GfxDispatchDto> result;
    result.reserve( stored.size() );
    for( const auto& value : stored ) result.push_back( {
        MakeRef( m_fingerprint, "gfx-dispatch", value.dispatchId ), value.dispatchId,
        value.frameIndex, value.timeNs, MakeRef( m_fingerprint, "thread", value.thread ),
        value.expectedJobs, value.threadingMode, value.flags } );
    return result;
}

std::vector<GfxDispatchDto> TraceSessionIoGfxReader::GfxDispatchesForFrame(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    return ReadFramePosting<StoredGfxDispatch, GfxDispatchDto>( m_path,
        m_dispatchFramePostingOffset, m_stats.gfxDispatches, frameId, offset, limit,
        m_gfxDispatchOffset, m_stats.gfxDispatches,
        [&]( const StoredGfxDispatch& value, uint64_t ) {
            return GfxDispatchDto { MakeRef( m_fingerprint, "gfx-dispatch", value.dispatchId ),
                value.dispatchId, value.frameIndex, value.timeNs,
                MakeRef( m_fingerprint, "thread", value.thread ), value.expectedJobs,
                value.threadingMode, value.flags };
        } );
}

std::vector<GfxEntityDto> TraceSessionIoGfxReader::GfxEntities() const
{
    const auto stored = ReadFixed<StoredGfxEntity>( m_path, m_gfxEntityOffset,
        m_stats.gfxEntities, "Gfx entity index read failed" );
    std::vector<GfxEntityDto> result;
    result.reserve( stored.size() );
    for( const auto& value : stored ) result.push_back( {
        MakeRef( m_fingerprint, "gfx-entity", value.entityId ), value.entityId, value.parentId,
        value.timeNs, MakeRef( m_fingerprint, "thread", value.thread ), value.gpuQueryId,
        value.gpuContext, value.kind, value.flags } );
    return result;
}

std::vector<GfxLinkDto> TraceSessionIoGfxReader::GfxLinks() const
{
    const auto stored = ReadFixed<StoredGfxLink>( m_path, m_gfxLinkOffset,
        m_stats.gfxLinks, "Gfx link index read failed" );
    std::vector<GfxLinkDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "gfx-link", i ), value.sourceId,
            value.targetId, value.timeNs, MakeRef( m_fingerprint, "thread", value.thread ),
            value.relation, value.flags } );
    }
    return result;
}

GfxEvidenceSlice TraceSessionIoGfxReader::EvidenceGfx( uint64_t frameId,
    const std::vector<uint64_t>& seedIds ) const
{
    GfxEvidenceSlice result;
    result.dispatches = GfxDispatchesForFrame( frameId, 0,
        std::numeric_limits<size_t>::max() );
    const auto readEntities = [&]( uint64_t postingOffset, uint64_t postingCount,
        uint64_t key ) {
        return ReadFramePosting<StoredGfxEntity, GfxEntityDto>( m_path,
            postingOffset, postingCount, key, 0, std::numeric_limits<size_t>::max(),
            m_gfxEntityOffset, m_stats.gfxEntities,
            [&]( const StoredGfxEntity& value, uint64_t ) {
                return GfxEntityDto { MakeRef( m_fingerprint, "gfx-entity", value.entityId ),
                    value.entityId, value.parentId, value.timeNs,
                    MakeRef( m_fingerprint, "thread", value.thread ), value.gpuQueryId,
                    value.gpuContext, value.kind, value.flags };
            } );
    };
    const auto readLinks = [&]( uint64_t postingOffset, uint64_t key ) {
        return ReadFramePosting<StoredGfxLink, GfxLinkDto>( m_path,
            postingOffset, m_stats.gfxLinks, key, 0, std::numeric_limits<size_t>::max(),
            m_gfxLinkOffset, m_stats.gfxLinks,
            [&]( const StoredGfxLink& value, uint64_t ordinal ) {
                return GfxLinkDto { MakeRef( m_fingerprint, "gfx-link", ordinal ),
                    value.sourceId, value.targetId, value.timeNs,
                    MakeRef( m_fingerprint, "thread", value.thread ),
                    value.relation, value.flags };
            } );
    };

    std::unordered_set<uint64_t> reachable( seedIds.begin(), seedIds.end() );
    std::queue<uint64_t> pending;
    for( const auto seed : seedIds ) pending.push( seed );
    for( const auto& dispatch : result.dispatches )
        if( reachable.emplace( dispatch.dispatchId ).second ) pending.push( dispatch.dispatchId );
    for( const auto& link : readLinks( m_gfxLinkTargetPostingOffset, frameId ) )
        if( link.relation == uint8_t( JnGfxRelation::BelongsToFrame ) &&
            reachable.emplace( link.sourceId ).second )
            pending.push( link.sourceId );

    std::map<std::string, GfxLinkDto> selectedLinks;
    while( !pending.empty() )
    {
        const auto current = pending.front();
        pending.pop();
        for( const auto& entity : readEntities( m_gfxParentPostingOffset,
            m_stats.gfxParentLinks, current ) )
            if( reachable.emplace( entity.entityId ).second ) pending.push( entity.entityId );
        for( auto& link : readLinks( m_gfxLinkSourcePostingOffset, current ) )
        {
            selectedLinks.try_emplace( link.ref, link );
            if( reachable.emplace( link.targetId ).second ) pending.push( link.targetId );
        }
    }

    std::map<std::string, GfxEntityDto> selectedEntities;
    for( const auto entityId : reachable )
        for( auto& entity : readEntities( m_gfxEntityIdPostingOffset,
            m_stats.gfxEntities, entityId ) )
            selectedEntities.try_emplace( entity.ref, std::move( entity ) );
    for( auto& [ref, entity] : selectedEntities ) result.entities.emplace_back( std::move( entity ) );
    for( auto& [ref, link] : selectedLinks ) result.links.emplace_back( std::move( link ) );
    std::sort( result.entities.begin(), result.entities.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.timeNs != rhs.timeNs ? lhs.timeNs < rhs.timeNs : lhs.entityId < rhs.entityId; } );
    std::sort( result.links.begin(), result.links.end(), []( const auto& lhs, const auto& rhs ) {
        if( lhs.timeNs != rhs.timeNs ) return lhs.timeNs < rhs.timeNs;
        if( lhs.sourceId != rhs.sourceId ) return lhs.sourceId < rhs.sourceId;
        return lhs.targetId < rhs.targetId; } );
    return result;
}

std::vector<CorrelatedFrameEventDto> TraceSessionIoGfxReader::CorrelatedFrames() const
{
    const auto stored = ReadFixed<StoredFrame>( m_path, m_frameOffset,
        m_stats.correlatedFrames, "correlated Frame index read failed" );
    std::vector<CorrelatedFrameEventDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "frame-identity-event", i ), value.frameId,
            value.domainIndex, value.timeNs, MakeRef( m_fingerprint, "thread", value.thread ),
            value.domain, value.phase, value.flags } );
    }
    return result;
}

std::vector<CorrelatedFrameEventDto> TraceSessionIoGfxReader::CorrelatedFramesForFrame(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    return ReadFramePosting<StoredFrame, CorrelatedFrameEventDto>( m_path,
        m_correlatedFramePostingOffset, m_stats.correlatedFrames, frameId, offset, limit,
        m_frameOffset, m_stats.correlatedFrames,
        [&]( const StoredFrame& value, uint64_t ordinal ) {
            return CorrelatedFrameEventDto {
                MakeRef( m_fingerprint, "frame-identity-event", ordinal ), value.frameId,
                value.domainIndex, value.timeNs, MakeRef( m_fingerprint, "thread", value.thread ),
                value.domain, value.phase, value.flags };
        } );
}

bool AuditTraceSessionIoGfxDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionIoGfxStats& stats,
    std::string& error )
{
    LocalManifest manifest;
    FileHeader header;
    const auto root = TraceSessionIoGfxIndexRoot( sessionRoot, session );
    if( !VerifyFiles( root, session,
        manifest, header, error ) ) return false;
    const auto path = root / FileName;
    if( !ValidateFramePosting<StoredGfxDispatch>( path,
            header.dispatchFramePostingOffset, header.gfxDispatches,
            header.gfxDispatchOffset, header.gfxDispatches,
            []( const StoredGfxDispatch& value ) { return value.frameIndex; },
            "session_gfx_dispatch_frame_posting", error ) ||
        !ValidateFramePosting<StoredFrame>( path,
            header.correlatedFramePostingOffset, header.correlatedFrames,
            header.frameOffset, header.correlatedFrames,
            []( const StoredFrame& value ) { return value.frameId; },
            "session_correlated_frame_posting", error ) ||
        !ValidateFramePosting<StoredGfxEntity>( path,
            header.gfxEntityIdPostingOffset, header.gfxEntities,
            header.gfxEntityOffset, header.gfxEntities,
            []( const StoredGfxEntity& value ) { return value.entityId; },
            "session_gfx_entity_id_posting", error ) ||
        !ValidateFramePosting<StoredGfxEntity>( path,
            header.gfxParentPostingOffset, header.gfxParentLinks,
            header.gfxEntityOffset, header.gfxEntities,
            []( const StoredGfxEntity& value ) { return value.parentId; },
            "session_gfx_parent_posting", error ) ||
        !ValidateFramePosting<StoredGfxLink>( path,
            header.gfxLinkSourcePostingOffset, header.gfxLinks,
            header.gfxLinkOffset, header.gfxLinks,
            []( const StoredGfxLink& value ) { return value.sourceId; },
            "session_gfx_link_source_posting", error ) ||
        !ValidateFramePosting<StoredGfxLink>( path,
            header.gfxLinkTargetPostingOffset, header.gfxLinks,
            header.gfxLinkOffset, header.gfxLinks,
            []( const StoredGfxLink& value ) { return value.targetId; },
            "session_gfx_link_target_posting", error ) ||
        !ValidateIoRequestPosting( path, header, error ) ) return false;
    stats = manifest.stats;
    return true;
}

}
