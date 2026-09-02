#include "TracyTraceSessionGpuVerifier.hpp"

#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyJnGpuCatalog.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t VerifierReportMagic = 0x3156525647504e4aull; // JNPGVRV1
constexpr uint64_t VerifierRunMagic = 0x31524e5552565047ull;    // GPVRUNR1
constexpr uint64_t FnvOffset = 1469598103934665603ull;
constexpr uint64_t ResourceSetFnvOffset = 14695981039346656037ull;

uint64_t FnvUpdate( uint64_t hash, const void* data, size_t size )
{
    const auto* bytes = static_cast<const uint8_t*>( data );
    for( size_t i = 0; i < size; ++i )
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

#pragma pack( push, 1 )
struct AllocationRaw
{
    uint64_t ordinal = 0;
    JnGpuCatalogAllocationRecordV1 value {};
};

struct PhysicalDelta
{
    int64_t time = 0;
    uint64_t ordinal = 0;
    int64_t bytes = 0;
};

struct PassRelationRaw
{
    uint64_t passId = 0;
    uint64_t resourceId = 0;
    uint8_t direct = 0;
    uint8_t reserved[7] {};
};

struct RunHeader
{
    uint64_t magic = VerifierRunMagic;
    uint32_t schema = 1;
    uint32_t recordBytes = 0;
    uint64_t recordCount = 0;
    uint64_t checksum = 0;
};
#pragma pack( pop )

static_assert( std::is_trivially_copyable_v<AllocationRaw> );
static_assert( std::is_trivially_copyable_v<PhysicalDelta> );
static_assert( std::is_trivially_copyable_v<PassRelationRaw> );

uint64_t RelationToken( uint64_t resourceId, uint64_t passId, uint8_t inclusive )
{
    uint64_t hash = FnvOffset;
    hash = FnvUpdate( hash, &resourceId, sizeof( resourceId ) );
    hash = FnvUpdate( hash, &passId, sizeof( passId ) );
    hash = FnvUpdate( hash, &inclusive, sizeof( inclusive ) );
    return hash;
}

uint64_t ReportHash( const TraceSessionGpuVerifierReport& value )
{
    uint64_t hash = FnvOffset;
    const auto addString = [&]( const std::string& text ) {
        const uint64_t size = text.size();
        hash = FnvUpdate( hash, &size, sizeof( size ) );
        if( !text.empty() ) hash = FnvUpdate( hash, text.data(), text.size() );
    };
    const auto add = [&]( const auto& item ) { hash = FnvUpdate( hash, &item, sizeof( item ) ); };
    add( value.schema ); addString( value.sourceSha256 ); add( value.sourceSize );
    addString( value.sessionGeneration ); addString( value.gpuGeneration );
    add( value.sourceGpuCatalogEvents ); add( value.sourceCatalogRecordCount );
    add( value.sourceReferenceRelationCount ); add( value.sourcePayloadBytes );
    add( value.sourceRecordHash ); add( value.resourceCount ); add( value.allocationCount );
    add( value.passCount ); add( value.rangeCount ); add( value.resourcePassRelationCount );
    add( value.storePageBytes ); add( value.resourceCapacityBytes ); add( value.livePhysicalBytes );
    add( value.engineKnownPhysicalPeakBytes ); add( value.engineKnownPhysicalPeakTimeNs );
    add( value.directMemberCount ); add( value.inclusiveMemberCount );
    add( value.directMemberHash ); add( value.inclusiveMemberHash );
    add( value.forwardRelationHash ); add( value.reverseRelationHash );
    add( value.verifiedPassCount ); add( value.mismatchCount ); add( value.complete );
    addString( value.reason );
    return hash;
}

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "gpu_verifier_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec; std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "gpu_verifier_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

template<typename T, typename Compare>
bool FlushRun( std::vector<T>& records, const std::filesystem::path& root,
    const char* prefix, std::vector<std::filesystem::path>& runs,
    Compare compare, std::string& error )
{
    if( records.empty() ) return true;
    std::sort( records.begin(), records.end(), compare );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "gpu_verifier_run_directory_failed:" + ec.message(); return false; }
    std::ostringstream name; name << prefix << '-' << std::setw( 6 ) << std::setfill( '0' )
        << runs.size() << ".bin";
    const auto path = root / name.str(); const auto temporary = path.string() + ".tmp";
    RunHeader header; header.recordBytes = sizeof( T ); header.recordCount = records.size();
    header.checksum = FnvUpdate( FnvOffset, records.data(), records.size() * sizeof( T ) );
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "gpu_verifier_run_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( reinterpret_cast<const char*>( records.data() ),
        std::streamsize( records.size() * sizeof( T ) ) );
    out.flush(); out.close();
    if( !out ) { error = "gpu_verifier_run_write_failed"; return false; }
    if( !AtomicReplace( temporary, path, error ) ) return false;
    runs.push_back( path ); records.clear(); return true;
}

template<typename T>
class RunReader
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        m_in.open( path, std::ios::binary );
        if( !m_in )
        { error = "gpu_verifier_run_open_failed"; return false; }
        RunHeader header;
        if( !m_in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
            header.magic != VerifierRunMagic || header.schema != 1 ||
            header.recordBytes != sizeof( T ) )
        { error = "gpu_verifier_run_header_invalid"; return false; }
        m_remaining = header.recordCount; m_expected = header.checksum; m_hash = FnvOffset;
        return Advance( error );
    }
    bool Empty() const { return !m_has; }
    const T& Front() const { return m_value; }
    bool Pop( T& value, std::string& error )
    {
        if( !m_has ) { error = "gpu_verifier_run_underflow"; return false; }
        value = m_value; return Advance( error );
    }
private:
    bool Advance( std::string& error )
    {
        if( m_remaining == 0 )
        {
            m_has = false;
            if( m_hash != m_expected ) { error = "gpu_verifier_run_checksum_mismatch"; return false; }
            char trailing = 0;
            if( m_in.read( &trailing, 1 ) ) { error = "gpu_verifier_run_trailing_bytes"; return false; }
            return true;
        }
        if( !m_in.read( reinterpret_cast<char*>( &m_value ), sizeof( m_value ) ) )
        { error = "gpu_verifier_run_truncated"; return false; }
        m_hash = FnvUpdate( m_hash, &m_value, sizeof( m_value ) ); --m_remaining; m_has = true;
        return true;
    }
    std::ifstream m_in;
    T m_value {};
    uint64_t m_remaining = 0, m_expected = 0, m_hash = FnvOffset;
    bool m_has = false;
};

template<typename T, typename Compare, typename Consumer>
bool MergeRuns( const std::vector<std::filesystem::path>& paths,
    Compare compare, Consumer consumer, std::stop_token stopToken, std::string& error )
{
    std::vector<RunReader<T>> readers( paths.size() );
    struct Node { size_t run = 0; T value {}; };
    const auto later = [&]( const Node& lhs, const Node& rhs ) {
        return compare( rhs.value, lhs.value );
    };
    std::priority_queue<Node, std::vector<Node>, decltype( later )> queue( later );
    for( size_t i = 0; i < paths.size(); ++i )
    {
        if( !readers[i].Open( paths[i], error ) ) return false;
        if( !readers[i].Empty() ) queue.push( { i, readers[i].Front() } );
    }
    uint64_t checks = 0;
    while( !queue.empty() )
    {
        if( ( ++checks & 0x3fff ) == 0 && stopToken.stop_requested() )
        { error = "cancelled_resumable"; return false; }
        const auto node = queue.top(); queue.pop();
        T value {};
        if( !readers[node.run].Pop( value, error ) || !consumer( node.value, error ) ) return false;
        if( !readers[node.run].Empty() ) queue.push( { node.run, readers[node.run].Front() } );
    }
    return true;
}

template<typename T, typename Compare>
bool MergeRunGroup( const std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& target, Compare compare,
    const std::stop_token& stopToken, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( target.parent_path(), ec );
    if( ec )
    { error = "gpu_verifier_run_directory_failed:" + ec.message(); return false; }
    const auto temporary = target.string() + ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "gpu_verifier_run_open_failed"; return false; }
    RunHeader header;
    header.recordBytes = sizeof( T );
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    uint64_t count = 0;
    uint64_t checksum = FnvOffset;
    const auto write = [&]( const T& value, std::string& consumeError ) {
        out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !out )
        { consumeError = "gpu_verifier_run_write_failed"; return false; }
        checksum = FnvUpdate( checksum, &value, sizeof( value ) );
        ++count;
        return true;
    };
    if( !MergeRuns<T>( paths, compare, write, stopToken, error ) )
    {
        out.close();
        std::filesystem::remove( temporary, ec );
        return false;
    }
    header.recordCount = count;
    header.checksum = checksum;
    out.seekp( 0, std::ios::beg );
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.flush();
    out.close();
    if( !out )
    {
        error = "gpu_verifier_run_write_failed";
        std::filesystem::remove( temporary, ec );
        return false;
    }
    return AtomicReplace( temporary, target, error );
}

template<typename T, typename Compare, typename Consumer>
bool MergeRunsBounded( const std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& mergeRoot, const char* prefix,
    uint32_t maximumOpenRunReaders, Compare compare, Consumer consumer,
    const std::stop_token& stopToken, TraceSessionGpuVerifierReport& report,
    std::string& error )
{
    if( maximumOpenRunReaders < 2 )
    { error = "gpu_verifier_run_fan_in_invalid"; return false; }
    auto current = paths;
    uint32_t round = 0;
    while( current.size() > maximumOpenRunReaders )
    {
        std::vector<std::filesystem::path> next;
        next.reserve( ( current.size() + maximumOpenRunReaders - 1 ) /
            maximumOpenRunReaders );
        const auto roundRoot = mergeRoot / ( "round-" + std::to_string( round ) );
        for( size_t begin = 0; begin < current.size(); begin += maximumOpenRunReaders )
        {
            const auto end = std::min( current.size(),
                begin + size_t( maximumOpenRunReaders ) );
            std::vector<std::filesystem::path> group(
                current.begin() + begin, current.begin() + end );
            report.peakOpenRunReaders = std::max<uint64_t>(
                report.peakOpenRunReaders, group.size() );
            std::ostringstream name;
            name << prefix << '-' << std::setw( 6 ) << std::setfill( '0' )
                << next.size() << ".bin";
            const auto target = roundRoot / name.str();
            if( !MergeRunGroup<T>( group, target, compare, stopToken, error ) )
                return false;
            next.push_back( target );
        }
        current = std::move( next );
        ++round;
        ++report.runMergePassCount;
    }
    report.peakOpenRunReaders = std::max<uint64_t>(
        report.peakOpenRunReaders, current.size() );
    return MergeRuns<T>( current, compare, consumer, stopToken, error );
}

bool IsDefinition( JnGpuCatalogRecordOperation operation )
{
    return operation == JnGpuCatalogRecordOperation::Create ||
        operation == JnGpuCatalogRecordOperation::Update ||
        operation == JnGpuCatalogRecordOperation::Bind ||
        operation == JnGpuCatalogRecordOperation::Open ||
        operation == JnGpuCatalogRecordOperation::Snapshot;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "gpu_verifier_protocol_record_invalid"; return false; }
    item = {}; std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "gpu_verifier_protocol_type_mismatch"; return false; }
    return true;
}

bool DecodeLargePayload( const TraceSessionCanonicalRecord& record,
    const QueueItem& item, uint64_t& payloadId, const uint8_t*& bytes,
    uint32_t& size, std::string& error )
{
    const auto fixed = QueueDataSize[record.type];
    if( record.payload.size() < fixed + sizeof( uint32_t ) )
    { error = "gpu_verifier_large_payload_header_truncated"; return false; }
    std::memcpy( &size, record.payload.data() + fixed, sizeof( size ) );
    if( size != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( uint32_t ) + size )
    { error = "gpu_verifier_large_payload_size_mismatch"; return false; }
    payloadId = item.stringTransfer.ptr;
    bytes = record.payload.data() + fixed + sizeof( uint32_t );
    return true;
}

struct SourceState
{
    const TraceSessionTimeTransform* transform = nullptr;
    const TraceSessionGpuVerifierControl* control = nullptr;
    TraceSessionGpuVerifierReport* report = nullptr;
    std::filesystem::path runRoot;
    std::unordered_map<uint64_t, std::vector<uint8_t>> payloads;
    std::vector<AllocationRaw> allocations;
    std::vector<std::filesystem::path> allocationRuns;
    uint64_t allocationOrdinal = 0;
};

bool FlushAllocations( SourceState& state, std::string& error )
{
    return FlushRun( state.allocations, state.runRoot / "allocation", "allocation",
        state.allocationRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.value.allocationId != rhs.value.allocationId )
                return lhs.value.allocationId < rhs.value.allocationId;
            if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
            return lhs.ordinal < rhs.ordinal;
        }, error );
}

bool VisitSource( const TraceSessionCanonicalRecord& record, void* userData,
    std::string& error )
{
    auto& state = *static_cast<SourceState*>( userData );
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    const auto domain = ClassifyTraceProtocolEvent( record.type );
    if( domain != TraceSessionProtocolDomain::GpuCatalog &&
        domain != TraceSessionProtocolDomain::GpuMemory ) return true;
    if( state.control->stopToken.stop_requested() )
    { error = "cancelled_resumable"; return false; }
    auto& report = *state.report;
    if( domain == TraceSessionProtocolDomain::GpuCatalog )
    {
        ++report.sourceGpuCatalogEvents;
        report.sourcePayloadBytes += record.payload.size();
        report.sourceRecordHash = FnvUpdate( report.sourceRecordHash,
            &record.sourceSequence, sizeof( record.sourceSequence ) );
        report.sourceRecordHash = FnvUpdate( report.sourceRecordHash, &record.type, sizeof( record.type ) );
        if( !record.payload.empty() ) report.sourceRecordHash = FnvUpdate(
            report.sourceRecordHash, record.payload.data(), record.payload.size() );
    }

    QueueItem item {}; if( !DecodeItem( record, item, error ) ) return false;
    if( item.hdr.type == QueueType::JnGpuCatalogBatchData )
    {
        uint64_t payloadId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, payloadId, bytes, size, error ) ) return false;
        if( payloadId == 0 || size < sizeof( JnGpuCatalogBatchEnvelopeV1 ) ||
            !state.payloads.emplace( payloadId, std::vector<uint8_t>( bytes, bytes + size ) ).second )
        { error = "gpu_verifier_catalog_payload_invalid"; return false; }
        return true;
    }
    if( item.hdr.type == QueueType::JnGpuCatalogBatch )
    {
        const auto& event = item.jnGpuCatalogBatch;
        const auto found = state.payloads.find( event.payloadId );
        if( found == state.payloads.end() )
        { error = "gpu_verifier_catalog_payload_missing"; return false; }
        const auto payload = std::move( found->second ); state.payloads.erase( found );
        if( payload.size() != event.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
        { error = "gpu_verifier_catalog_payload_size_mismatch"; return false; }
        JnGpuCatalogBatchEnvelopeV1 envelope {};
        std::memcpy( &envelope, payload.data(), sizeof( envelope ) );
        const auto* records = payload.data() + sizeof( envelope );
        const auto recordBytes = payload.size() - sizeof( envelope );
        if( envelope.magic != JnGpuCatalogBatchMagic ||
            envelope.catalogSchema != JnGpuCatalogSchemaVersion ||
            envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion ||
            envelope.recordCount != event.recordCount || envelope.payloadBytes != recordBytes ||
            JnGpuCatalogChecksum64( records, recordBytes ) != envelope.checksum )
        { error = "gpu_verifier_catalog_envelope_invalid"; return false; }
        report.sourceCatalogRecordCount += event.recordCount;
        const auto kind = JnGpuCatalogBatchKind( event.kind );
        if( kind == JnGpuCatalogBatchKind::Allocation )
        {
            if( envelope.recordBytes != sizeof( JnGpuCatalogAllocationRecordV1 ) ||
                uint64_t( event.recordCount ) * sizeof( JnGpuCatalogAllocationRecordV1 ) != recordBytes )
            { error = "gpu_verifier_allocation_batch_invalid"; return false; }
            for( uint32_t i = 0; i < event.recordCount; ++i )
            {
                AllocationRaw raw; raw.ordinal = state.allocationOrdinal++;
                std::memcpy( &raw.value, records + size_t( i ) * sizeof( raw.value ), sizeof( raw.value ) );
                raw.value.time = state.transform->ToNanoseconds( raw.value.time );
                state.allocations.push_back( raw );
                if( state.allocations.size() >= state.control->maximumBufferedAllocationRecords &&
                    !FlushAllocations( state, error ) ) return false;
            }
        }
        return true;
    }
    if( item.hdr.type == QueueType::JnGpuReferenceUse )
        ++report.sourceReferenceRelationCount;
    else if( item.hdr.type == QueueType::JnGpuReferenceSetUse )
        report.sourceReferenceRelationCount += item.jnGpuReferenceSetUse.entryCount;
    return true;
}

bool ComputePhysicalPeak( SourceState& source, TraceSessionGpuVerifierReport& report,
    std::string& error )
{
    if( !FlushAllocations( source, error ) ) return false;
    std::vector<PhysicalDelta> deltas;
    std::vector<std::filesystem::path> deltaRuns;
    uint64_t currentAllocation = 0;
    uint64_t currentSize = 0;
    uint64_t currentParent = 0;
    bool currentLive = false;
    const auto flushDeltas = [&]() {
        return FlushRun( deltas, source.runRoot / "delta", "delta", deltaRuns,
            []( const auto& lhs, const auto& rhs ) {
                if( lhs.time != rhs.time ) return lhs.time < rhs.time;
                return lhs.ordinal < rhs.ordinal;
            }, error );
    };
    const auto consumeAllocation = [&]( const AllocationRaw& raw, std::string& consumeError ) {
        const auto& value = raw.value;
        if( value.allocationId == 0 ) return true;
        if( currentAllocation != 0 && currentAllocation != value.allocationId )
        { currentSize = 0; currentParent = 0; currentLive = false; }
        if( currentAllocation != value.allocationId ) currentAllocation = value.allocationId;
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        const uint64_t before = currentLive && currentParent == 0 ? currentSize : 0;
        if( operation == JnGpuCatalogRecordOperation::Destroy ||
            operation == JnGpuCatalogRecordOperation::Close )
        { currentSize = 0; currentParent = 0; currentLive = false; }
        else if( IsDefinition( operation ) )
        { currentSize = value.sizeBytes; currentParent = value.parentAllocationId; currentLive = true; }
        const uint64_t after = currentLive && currentParent == 0 ? currentSize : 0;
        if( before != after )
        {
            const auto signedBefore = int64_t( std::min<uint64_t>( before, uint64_t( std::numeric_limits<int64_t>::max() ) ) );
            const auto signedAfter = int64_t( std::min<uint64_t>( after, uint64_t( std::numeric_limits<int64_t>::max() ) ) );
            deltas.push_back( { value.time, raw.ordinal, signedAfter - signedBefore } );
            if( deltas.size() >= source.control->maximumBufferedDeltas && !flushDeltas() )
            { consumeError = error; return false; }
        }
        return true;
    };
    const auto allocationCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.value.allocationId != rhs.value.allocationId )
            return lhs.value.allocationId < rhs.value.allocationId;
        if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
        return lhs.ordinal < rhs.ordinal;
    };
    if( !MergeRunsBounded<AllocationRaw>( source.allocationRuns,
        source.runRoot / "allocation-merge", "allocation",
        source.control->maximumOpenRunReaders, allocationCompare,
        consumeAllocation, source.control->stopToken, report, error ) ||
        !flushDeltas() ) return false;

    int64_t current = 0;
    const auto consumeDelta = [&]( const PhysicalDelta& value, std::string& consumeError ) {
        if( ( value.bytes > 0 && current > std::numeric_limits<int64_t>::max() - value.bytes ) ||
            ( value.bytes < 0 && current < std::numeric_limits<int64_t>::min() - value.bytes ) )
        { consumeError = "gpu_verifier_physical_overflow"; return false; }
        current += value.bytes;
        if( current < 0 ) { consumeError = "gpu_verifier_physical_underflow"; return false; }
        if( uint64_t( current ) > report.engineKnownPhysicalPeakBytes )
        { report.engineKnownPhysicalPeakBytes = uint64_t( current ); report.engineKnownPhysicalPeakTimeNs = value.time; }
        return true;
    };
    const auto deltaCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.time != rhs.time ) return lhs.time < rhs.time;
        return lhs.ordinal < rhs.ordinal;
    };
    return MergeRunsBounded<PhysicalDelta>( deltaRuns,
        source.runRoot / "delta-merge", "delta",
        source.control->maximumOpenRunReaders, deltaCompare, consumeDelta,
        source.control->stopToken, report, error );
}

struct PassAuditFact
{
    uint64_t parentPassId = 0;
    uint64_t directResourceCount = 0;
    uint64_t inclusiveResourceCount = 0;
    uint64_t directResourceHash = 0;
    uint64_t inclusiveResourceHash = 0;
    uint64_t directPhysicalBytes = 0;
    uint64_t inclusivePhysicalBytes = 0;
};

bool AuditStoreRelationsLinear( const GpuAnalysisStoreReader& reader,
    const TraceSessionGpuVerifierControl& control,
    TraceSessionGpuVerifierReport& report, const std::filesystem::path& runRoot,
    std::string& error )
{
    const auto& store = reader.Manifest();
    std::unordered_map<uint64_t, PassAuditFact> facts;
    facts.reserve( size_t( std::min<uint64_t>( store.passCount, 4ull * 1024 * 1024 ) ) );
    std::vector<PassRelationRaw> relationBuffer;
    relationBuffer.reserve( size_t( std::min<uint64_t>(
        control.maximumBufferedPassRelations, 4ull * 1024 * 1024 ) ) );
    std::vector<std::filesystem::path> passRuns;
    const auto passCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        return lhs.direct > rhs.direct;
    };
    const auto flushPassRun = [&]() {
        return FlushRun( relationBuffer, runRoot / "pass-relation-runs", "pass-relation",
            passRuns, passCompare, error );
    };

    uint64_t previousPassId = 0;
    for( size_t pageIndex = 0; pageIndex < reader.PassPageCount(); ++pageIndex )
    {
        if( control.stopToken.stop_requested() )
        { error = "cancelled_resumable"; return false; }
        std::vector<GpuPassWorkingSet> passes;
        if( !reader.LoadPassPage( pageIndex, passes, error ) ) return false;
        for( const auto& pass : passes )
        {
            if( pass.passId == 0 || pass.passId <= previousPassId ||
                !std::is_sorted( pass.directResources.begin(), pass.directResources.end() ) ||
                std::adjacent_find( pass.directResources.begin(),
                    pass.directResources.end() ) != pass.directResources.end() ||
                std::find( pass.directResources.begin(), pass.directResources.end(), 0 ) !=
                    pass.directResources.end() )
            { error = "gpu_verifier_pass_source_invalid"; return false; }
            previousPassId = pass.passId;
            PassAuditFact fact;
            fact.parentPassId = pass.parentPassId;
            fact.directResourceCount = pass.directResources.size();
            fact.directResourceHash = GpuAnalysisResourceSetHash( pass.directResources );
            fact.directPhysicalBytes = pass.directPhysicalBytes;
            fact.inclusivePhysicalBytes = pass.inclusivePhysicalBytes;
            if( !facts.emplace( pass.passId, fact ).second )
            { error = "gpu_verifier_pass_duplicate"; return false; }
            ++report.passCount;

            for( const auto resourceId : pass.directResources )
            {
                uint64_t current = pass.passId;
                uint32_t depth = 0;
                while( current != 0 )
                {
                    if( ++depth > 65 )
                    { error = "gpu_verifier_pass_parent_depth_exceeded"; return false; }
                    const auto ancestor = facts.find( current );
                    if( ancestor == facts.end() )
                    { error = "gpu_verifier_pass_parent_missing"; return false; }
                    relationBuffer.push_back( { current, resourceId,
                        uint8_t( current == pass.passId ), {} } );
                    if( relationBuffer.size() >= control.maximumBufferedPassRelations &&
                        !flushPassRun() ) return false;
                    if( ancestor->second.parentPassId != 0 &&
                        ancestor->second.parentPassId >= current )
                    { error = "gpu_verifier_pass_parent_order_invalid"; return false; }
                    current = ancestor->second.parentPassId;
                }
            }
        }
    }
    if( !flushPassRun() ) return false;

    const auto inclusivePath = runRoot / "computed-pass-relations.bin";
    std::ofstream inclusiveOut( inclusivePath, std::ios::binary | std::ios::trunc );
    if( !inclusiveOut ) { error = "gpu_verifier_relation_output_open_failed"; return false; }
    PassRelationRaw pending {};
    bool hasPending = false;
    const auto commitPending = [&]() -> bool {
        if( !hasPending ) return true;
        const auto fact = facts.find( pending.passId );
        if( fact == facts.end() )
        { error = "gpu_verifier_relation_pass_missing"; return false; }
        GpuAnalysisResourcePassEntry value;
        value.resourceId = pending.resourceId;
        value.passId = pending.passId;
        value.inclusive = uint8_t( !pending.direct );
        inclusiveOut.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !inclusiveOut )
        { error = "gpu_verifier_relation_output_write_failed"; return false; }
        ++fact->second.inclusiveResourceCount;
        report.forwardRelationHash ^= RelationToken(
            value.resourceId, value.passId, value.inclusive );
        return true;
    };
    const auto consumePassRelation = [&]( const PassRelationRaw& value,
        std::string& consumeError ) {
        if( !hasPending || value.passId != pending.passId ||
            value.resourceId != pending.resourceId )
        {
            if( !commitPending() ) { consumeError = error; return false; }
            pending = value;
            hasPending = true;
        }
        else pending.direct = uint8_t( pending.direct || value.direct );
        return true;
    };
    if( !MergeRunsBounded<PassRelationRaw>( passRuns,
        runRoot / "pass-relation-merge", "pass-relation",
        control.maximumOpenRunReaders, passCompare, consumePassRelation,
        control.stopToken, report, error ) || !commitPending() ) return false;
    inclusiveOut.flush(); inclusiveOut.close();
    if( !inclusiveOut )
    { error = "gpu_verifier_relation_output_flush_failed"; return false; }

    for( auto& [_, fact] : facts )
    {
        fact.inclusiveResourceHash = ResourceSetFnvOffset;
        fact.inclusiveResourceHash = FnvUpdate( fact.inclusiveResourceHash,
            &fact.inclusiveResourceCount, sizeof( fact.inclusiveResourceCount ) );
    }
    std::ifstream inclusiveIn( inclusivePath, std::ios::binary );
    GpuAnalysisResourcePassEntry entry;
    std::vector<GpuAnalysisResourcePassEntry> resourceBuffer;
    resourceBuffer.reserve( relationBuffer.capacity() );
    std::vector<std::filesystem::path> resourceRuns;
    const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
        return lhs.inclusive < rhs.inclusive;
    };
    const auto flushResourceRun = [&]() {
        return FlushRun( resourceBuffer, runRoot / "resource-relation-runs",
            "resource-relation", resourceRuns, resourceCompare, error );
    };
    while( inclusiveIn.read( reinterpret_cast<char*>( &entry ), sizeof( entry ) ) )
    {
        const auto fact = facts.find( entry.passId );
        if( fact == facts.end() )
        { error = "gpu_verifier_relation_hash_pass_missing"; return false; }
        fact->second.inclusiveResourceHash = FnvUpdate(
            fact->second.inclusiveResourceHash, &entry.resourceId,
            sizeof( entry.resourceId ) );
        resourceBuffer.push_back( entry );
        if( resourceBuffer.size() >= control.maximumBufferedPassRelations &&
            !flushResourceRun() ) return false;
    }
    if( !inclusiveIn.eof() || !flushResourceRun() )
    { if( error.empty() ) error = "gpu_verifier_relation_output_read_failed"; return false; }

    size_t storePage = 0, storeIndex = 0;
    std::vector<GpuAnalysisResourcePassEntry> storeValues;
    const auto nextStore = [&]( GpuAnalysisResourcePassEntry& value,
        bool& present ) -> bool {
        while( storeIndex >= storeValues.size() )
        {
            if( storePage >= reader.ResourcePassPageCount() )
            { present = false; return true; }
            storeValues.clear(); storeIndex = 0;
            if( !reader.LoadResourcePassPage( storePage++, storeValues, error ) )
                return false;
        }
        value = storeValues[storeIndex++]; present = true; return true;
    };
    const auto compareStore = [&]( const GpuAnalysisResourcePassEntry& computed,
        std::string& consumeError ) {
        GpuAnalysisResourcePassEntry stored; bool present = false;
        if( !nextStore( stored, present ) ) { consumeError = error; return false; }
        if( !present || stored.resourceId != computed.resourceId ||
            stored.passId != computed.passId || stored.inclusive != computed.inclusive )
        { consumeError = "gpu_verifier_resource_pass_exact_mismatch"; return false; }
        ++report.resourcePassRelationCount;
        report.reverseRelationHash ^= RelationToken(
            stored.resourceId, stored.passId, stored.inclusive );
        return true;
    };
    if( !MergeRunsBounded<GpuAnalysisResourcePassEntry>( resourceRuns,
        runRoot / "resource-relation-merge", "resource-relation",
        control.maximumOpenRunReaders, resourceCompare, compareStore,
        control.stopToken, report, error ) ) return false;
    GpuAnalysisResourcePassEntry trailing; bool hasTrailing = false;
    if( !nextStore( trailing, hasTrailing ) ) return false;
    if( hasTrailing )
    { error = "gpu_verifier_resource_pass_store_has_extra"; return false; }

    uint64_t summaryCount = 0;
    report.directMemberHash = FnvOffset;
    report.inclusiveMemberHash = FnvOffset;
    for( size_t pageIndex = 0; pageIndex < reader.PassSummaryPageCount(); ++pageIndex )
    {
        std::vector<GpuAnalysisPassSummary> summaries;
        if( !reader.LoadPassSummaryPage( pageIndex, summaries, error ) ) return false;
        for( const auto& summary : summaries )
        {
            const auto fact = facts.find( summary.passId );
            if( fact == facts.end() )
            { error = "gpu_verifier_pass_summary_without_pass"; return false; }
            const auto& value = fact->second;
            if( summary.directResourceCount != value.directResourceCount ||
                summary.inclusiveResourceCount != value.inclusiveResourceCount ||
                summary.directResourceHash != value.directResourceHash ||
                summary.inclusiveResourceHash != value.inclusiveResourceHash ||
                summary.directPhysicalBytes != value.directPhysicalBytes ||
                summary.inclusivePhysicalBytes != value.inclusivePhysicalBytes )
            {
                error = "gpu_verifier_pass_summary_mismatch:pass=" +
                    std::to_string( summary.passId ) + ":count=" +
                    std::to_string( summary.directResourceCount ) + "/" +
                    std::to_string( value.directResourceCount ) + "," +
                    std::to_string( summary.inclusiveResourceCount ) + "/" +
                    std::to_string( value.inclusiveResourceCount ) + ":hash=" +
                    std::to_string( summary.directResourceHash ) + "/" +
                    std::to_string( value.directResourceHash ) + "," +
                    std::to_string( summary.inclusiveResourceHash ) + "/" +
                    std::to_string( value.inclusiveResourceHash ) + ":bytes=" +
                    std::to_string( summary.directPhysicalBytes ) + "/" +
                    std::to_string( value.directPhysicalBytes ) + "," +
                    std::to_string( summary.inclusivePhysicalBytes ) + "/" +
                    std::to_string( value.inclusivePhysicalBytes );
                return false;
            }
            report.directMemberCount += value.directResourceCount;
            report.inclusiveMemberCount += value.inclusiveResourceCount;
            report.directMemberHash = FnvUpdate( report.directMemberHash,
                &summary.passId, sizeof( summary.passId ) );
            report.directMemberHash = FnvUpdate( report.directMemberHash,
                &value.directResourceHash, sizeof( value.directResourceHash ) );
            report.inclusiveMemberHash = FnvUpdate( report.inclusiveMemberHash,
                &summary.passId, sizeof( summary.passId ) );
            report.inclusiveMemberHash = FnvUpdate( report.inclusiveMemberHash,
                &value.inclusiveResourceHash, sizeof( value.inclusiveResourceHash ) );
            ++summaryCount;
        }
    }
    if( summaryCount != facts.size() )
    { error = "gpu_verifier_pass_summary_count_mismatch"; return false; }
    report.verifiedPassCount = summaryCount;
    return true;
}

bool SaveReport( const std::filesystem::path& root,
    TraceSessionGpuVerifierReport& value, std::string& error )
{
    value.reportHash = ReportHash( value );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "gpu_verifier_report_directory_failed:" + ec.message(); return false; }
    const auto target = root / "report"; const auto temporary = root / "report.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "gpu_verifier_report_open_failed"; return false; }
#define W( name ) out << #name << ' ' << value.name << '\n'
    out << "magic " << VerifierReportMagic << '\n';
    W( schema ); out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    W( sourceSize ); out << "session_generation " << std::quoted( value.sessionGeneration ) << '\n';
    out << "gpu_generation " << std::quoted( value.gpuGeneration ) << '\n';
    W( sourceGpuCatalogEvents ); W( sourceCatalogRecordCount ); W( sourceReferenceRelationCount );
    W( sourcePayloadBytes ); W( sourceRecordHash ); W( resourceCount ); W( allocationCount );
    W( passCount ); W( rangeCount ); W( resourcePassRelationCount ); W( storePageBytes );
    W( resourceCapacityBytes ); W( livePhysicalBytes ); W( engineKnownPhysicalPeakBytes );
    W( engineKnownPhysicalPeakTimeNs ); W( directMemberCount ); W( inclusiveMemberCount );
    W( directMemberHash ); W( inclusiveMemberHash ); W( forwardRelationHash ); W( reverseRelationHash );
    W( verifiedPassCount ); W( mismatchCount ); W( reportHash );
    out << "complete " << ( value.complete ? 1 : 0 ) << '\n';
    out << "reason " << std::quoted( value.reason ) << '\n';
#undef W
    out.flush(); out.close();
    if( !out ) { error = "gpu_verifier_report_write_failed"; return false; }
    return AtomicReplace( temporary, target, error );
}

}

std::filesystem::path TraceSessionGpuVerifierRoot(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "audit" /
        "gpu-resource-analysis-verifier" / std::to_string( TraceSessionGpuVerifierSchemaVersion );
}

bool VerifyTraceSessionGpuDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    const TraceSessionGpuVerifierControl& control,
    TraceSessionGpuVerifierReport& report, std::string& error )
{
    error.clear(); report = {};
    if( control.maximumBufferedAllocationRecords == 0 ||
        control.maximumBufferedDeltas == 0 ||
        control.maximumBufferedPassRelations == 0 ||
        control.maximumOpenRunReaders < 2 )
    { error = "gpu_verifier_buffer_limit_invalid"; return false; }
    report.sourceSha256 = manifest.source.sha256; report.sourceSize = manifest.source.fileSize;
    report.sessionGeneration = manifest.generation; report.sourceRecordHash = FnvOffset;

    TraceSessionTimeTransform transform;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, transform, error ) ) return false;
    const auto root = TraceSessionGpuVerifierRoot( sessionRoot, manifest );
    const auto runRoot = root / "work";
    std::error_code ec; std::filesystem::remove_all( runRoot, ec ); ec.clear();
    SourceState source { &transform, &control, &report, runRoot };
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest, VisitSource, &source, error ) ||
        !source.payloads.empty() )
    { if( error.empty() ) error = "gpu_verifier_unresolved_payload"; return false; }
    if( !ComputePhysicalPeak( source, report, error ) ) return false;

    const auto expectedGpuEvents = inventory.protocolInventory.domains[
        size_t( TraceSessionProtocolDomain::GpuCatalog )].count;
    if( report.sourceGpuCatalogEvents != expectedGpuEvents )
    { error = "gpu_verifier_source_count_mismatch"; return false; }

    const auto gpuRoot = TraceSessionGpuAnalysisRoot( sessionRoot, manifest );
    auto reader = GpuAnalysisStoreReader::OpenAt( gpuRoot,
        manifest.source.sha256, manifest.source.fileSize, error );
    if( !reader ) return false;
    const auto& store = reader->Manifest(); const auto& overview = reader->Overview();
    report.gpuGeneration = store.generation;
    for( const auto& page : store.pages ) report.storePageBytes += page.fileBytes;
    if( report.storePageBytes != store.totalBytes ) ++report.mismatchCount;

    for( size_t pageIndex = 0; pageIndex < reader->ResourcePageCount(); ++pageIndex )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled_resumable"; return false; }
        std::vector<GpuResourceAnalysisRecord> values;
        if( !reader->LoadResourcePage( pageIndex, values, error ) ) return false;
        report.resourceCount += values.size();
        for( const auto& value : values )
            report.resourceCapacityBytes += value.capacityBytes;
    }
    for( size_t pageIndex = 0; pageIndex < reader->RangePageCount(); ++pageIndex )
    {
        if( control.stopToken.stop_requested() )
        { error = "cancelled_resumable"; return false; }
        std::vector<GpuAnalysisRangeStoreEntry> values;
        if( !reader->LoadRangePage( pageIndex, values, error ) ) return false;
        report.rangeCount += values.size();
    }
    for( size_t pageIndex = 0; pageIndex < reader->AllocationPageCount(); ++pageIndex )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled_resumable"; return false; }
        std::vector<GpuAllocationAnalysisRecord> values;
        if( !reader->LoadAllocationPage( pageIndex, values, error ) ) return false;
        report.allocationCount += values.size();
        for( const auto& value : values ) if( value.aliveAtEnd && value.parentAllocationId == 0 )
            report.livePhysicalBytes += value.sizeBytes;
    }

    if( !AuditStoreRelationsLinear( *reader, control, report, runRoot, error ) )
        return false;

    if( report.resourceCount != store.resourceCount ||
        report.allocationCount != store.allocationCount || report.passCount != store.passCount ||
        report.rangeCount != store.rangeCount ||
        report.resourcePassRelationCount != store.resourcePassRelationCount ||
        report.resourcePassRelationCount != report.inclusiveMemberCount ||
        report.forwardRelationHash != report.reverseRelationHash ||
        report.livePhysicalBytes != overview.engineKnownPhysicalBytes ||
        report.engineKnownPhysicalPeakBytes != overview.engineKnownPhysicalPeakBytes ||
        report.engineKnownPhysicalPeakTimeNs != overview.engineKnownPhysicalPeakTimeNs )
        ++report.mismatchCount;
    std::filesystem::remove_all( runRoot, ec );
    if( report.mismatchCount != 0 )
    { report.reason = "gpu_verifier_derived_mismatch"; error = report.reason; return false; }
    report.complete = true; report.reason.clear();
    return SaveReport( root, report, error );
}

std::optional<TraceSessionGpuVerifierReport> LoadTraceSessionGpuVerifierReport(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
    std::string& error )
{
    error.clear(); TraceSessionGpuVerifierReport value;
    std::ifstream in( TraceSessionGpuVerifierRoot( sessionRoot, manifest ) / "report", std::ios::binary );
    if( !in ) { error = "gpu_verifier_report_not_found"; return std::nullopt; }
    uint64_t magic = 0; std::string key;
    while( in >> key )
    {
#define R( name ) else if( key == #name ) in >> value.name
        if( key == "magic" ) in >> magic;
        R( schema ); else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        R( sourceSize ); else if( key == "session_generation" ) in >> std::quoted( value.sessionGeneration );
        else if( key == "gpu_generation" ) in >> std::quoted( value.gpuGeneration );
        R( sourceGpuCatalogEvents ); R( sourceCatalogRecordCount ); R( sourceReferenceRelationCount );
        R( sourcePayloadBytes ); R( sourceRecordHash ); R( resourceCount ); R( allocationCount );
        R( passCount ); R( rangeCount ); R( resourcePassRelationCount ); R( storePageBytes );
        R( resourceCapacityBytes ); R( livePhysicalBytes ); R( engineKnownPhysicalPeakBytes );
        R( engineKnownPhysicalPeakTimeNs ); R( directMemberCount ); R( inclusiveMemberCount );
        R( directMemberHash ); R( inclusiveMemberHash ); R( forwardRelationHash ); R( reverseRelationHash );
        R( verifiedPassCount ); R( mismatchCount ); R( reportHash );
        else if( key == "complete" ) { uint32_t complete = 0; in >> complete; value.complete = complete != 0; }
        else if( key == "reason" ) in >> std::quoted( value.reason );
        else { std::string ignored; std::getline( in, ignored ); }
#undef R
        if( !in ) { error = "gpu_verifier_report_parse_failed"; return std::nullopt; }
    }
    const auto savedHash = value.reportHash; value.reportHash = 0;
    if( magic != VerifierReportMagic || value.schema != TraceSessionGpuVerifierSchemaVersion ||
        value.sourceSha256 != manifest.source.sha256 || value.sourceSize != manifest.source.fileSize ||
        value.sessionGeneration != manifest.generation || !value.complete ||
        savedHash != ReportHash( value ) )
    { error = "gpu_verifier_report_invalid"; return std::nullopt; }
    value.reportHash = savedHash; return value;
}

}
