#include "TracyStreamJournal.hpp"
#include "TracyStreamProtocol.hpp"
#include "TracyStreamReplay.hpp"
#include "TracyStreamSnapshotMap.hpp"
#include "TracyStreamStore.hpp"

#include "../../public/common/TracyProtocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace tracy::stream;
using tracy::ProtocolChunk;
using tracy::ProtocolCloseReason;
using tracy::ProtocolDataSpan;
using tracy::ProtocolDirection;

class MemorySink final : public JournalSink
{
public:
    bool Write( const uint8_t* data, size_t size, size_t& written, std::string& error ) override
    {
        if( writeDelay.count() != 0 ) std::this_thread::sleep_for( writeDelay );
        if( m_position >= failAt )
        {
            written = 0;
            error = "injected write failure";
            return false;
        }
        const uint64_t beforeFailure = failAt - m_position;
        written = size_t( std::min<uint64_t>( { uint64_t( size ), uint64_t( maxWriteChunk ), beforeFailure } ) );
        if( written == 0 )
        {
            error = "injected zero-progress write";
            return false;
        }
        if( m_position + written > bytes.size() ) bytes.resize( size_t( m_position + written ) );
        std::copy_n( data, written, bytes.begin() + size_t( m_position ) );
        m_position += written;
        if( written < size && m_position >= failAt )
        {
            error = "injected write failure";
            return false;
        }
        return true;
    }

    bool Flush( bool durable, std::string& error ) override
    {
        flushCount++;
        if( durable ) durableFlushCount++;
        if( failFlush )
        {
            error = "injected flush failure";
            return false;
        }
        return true;
    }

    uint64_t Tell() const override { return m_position; }

    std::vector<uint8_t> bytes;
    size_t maxWriteChunk = std::numeric_limits<size_t>::max();
    uint64_t failAt = std::numeric_limits<uint64_t>::max();
    bool failFlush = false;
    std::chrono::microseconds writeDelay { 0 };
    size_t flushCount = 0;
    size_t durableFlushCount = 0;

private:
    uint64_t m_position = 0;
};

struct TestContext
{
    void Check( bool condition, const std::string& message )
    {
        if( condition ) return;
        failures++;
        std::cerr << "FAIL: " << message << '\n';
    }

    int failures = 0;
};

FileHeader DeterministicHeader()
{
    FileHeader header;
    header.flags = 7;
    header.protocolVersion = 69;
    header.createdUnixNs = 123456789;
    for( size_t i = 0; i < header.sessionId.size(); i++ )
        header.sessionId[i] = uint8_t( i + 1 );
    return header;
}

std::unique_ptr<JournalWriter> NewMemoryWriter( MemorySink*& sink, WriterOptions options, std::string& error )
{
    auto owned = std::make_unique<MemorySink>();
    sink = owned.get();
    return JournalWriter::Create( std::move( owned ), DeterministicHeader(), options, error );
}

std::vector<uint8_t> MakeValidJournal( TestContext& test, std::vector<RecordInfo>* records = nullptr )
{
    WriterOptions options;
    options.durableHeader = false;
    MemorySink* sink = nullptr;
    std::string error;
    auto writer = NewMemoryWriter( sink, options, error );
    test.Check( writer != nullptr, "create in-memory writer: " + error );
    if( !writer ) return {};

    test.Check( writer->Append( RecordType::SessionBegin, RecordFlagHandshake, "begin", 10, error ), "append SessionBegin: " + error );
    constexpr std::string_view ClientPrefix = "client ";
    constexpr std::string_view ClientBody = "frame bytes";
    const std::array<PayloadSpan, 2> clientSegments = {
        PayloadSpan { reinterpret_cast<const uint8_t*>( ClientPrefix.data() ), ClientPrefix.size() },
        PayloadSpan { reinterpret_cast<const uint8_t*>( ClientBody.data() ), ClientBody.size() }
    };
    test.Check( writer->Append( RecordType::ClientToServer, RecordFlagCompressedFrame, clientSegments, 20, error ), "append segmented client frame: " + error );
    test.Check( writer->Append( RecordType::ServerToClient, RecordFlagServerQuery, "query", 30, error ), "append server query: " + error );
    test.Check( writer->Append( RecordType::SessionEnd, RecordFlagTerminal, "clean", 40, error ), "append SessionEnd: " + error );
    test.Check( writer->DurableSize() == sink->bytes.size(), "SessionEnd makes the entire journal durable" );

    ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 16;
    const auto scan = ScanJournal( sink->bytes, scanOptions );
    test.Check( scan.code == ScanCode::Ok, "valid journal scans successfully: " + scan.message );
    test.Check( scan.complete, "valid journal reports complete" );
    test.Check( scan.recordCount == 4, "valid journal has four records" );
    test.Check( scan.lastSequence == 4, "valid journal last sequence is four" );
    test.Check( scan.lastMonotonicNs == 40, "valid journal exposes the last committed monotonic timestamp" );
    test.Check( scan.validSize == sink->bytes.size(), "valid journal prefix covers file" );
    test.Check( scan.header.protocolVersion == 69, "file header protocol version round-trips" );
    test.Check( scan.header.sessionId == DeterministicHeader().sessionId, "file header session id round-trips" );
    if( records ) *records = scan.records;
    return sink->bytes;
}

void Put64( std::vector<uint8_t>& bytes, size_t offset, uint64_t value )
{
    for( size_t i = 0; i < 8; i++ ) bytes[offset + i] = uint8_t( value >> ( i * 8 ) );
}

void Put32( std::vector<uint8_t>& bytes, size_t offset, uint32_t value )
{
    for( size_t i = 0; i < 4; i++ ) bytes[offset + i] = uint8_t( value >> ( i * 8 ) );
}

uint32_t HeaderCrc( const std::vector<uint8_t>& bytes, size_t offset, size_t size, size_t crcOffset )
{
    std::vector<uint8_t> copy( bytes.begin() + offset, bytes.begin() + offset + size );
    std::fill_n( copy.begin() + crcOffset, 4, uint8_t( 0 ) );
    return Crc32c( copy );
}

void RecomputeRecordIntegrity( std::vector<uint8_t>& bytes, const RecordInfo& record )
{
    const auto headerOffset = size_t( record.offset );
    const auto payloadOffset = headerOffset + size_t( RecordHeaderSize );
    const auto payloadSize = size_t( record.payloadSize );
    const auto trailerOffset = payloadOffset + payloadSize;

    Put32( bytes, headerOffset + 40, Crc32c( std::span<const uint8_t>( bytes.data() + payloadOffset, payloadSize ) ) );
    Put32( bytes, headerOffset + 44, 0 );
    Put32( bytes, headerOffset + 44, HeaderCrc( bytes, headerOffset, RecordHeaderSize, 44 ) );

    std::vector<uint8_t> commitBytes;
    commitBytes.reserve( size_t( RecordHeaderSize ) + payloadSize + 24 );
    commitBytes.insert( commitBytes.end(), bytes.begin() + headerOffset, bytes.begin() + payloadOffset + payloadSize );
    commitBytes.insert( commitBytes.end(), bytes.begin() + trailerOffset, bytes.begin() + trailerOffset + 24 );
    Put32( bytes, trailerOffset + 24, Crc32c( commitBytes ) );
    Put32( bytes, trailerOffset + 28, 0 );
    Put32( bytes, trailerOffset + 28, HeaderCrc( bytes, trailerOffset, RecordTrailerSize, 28 ) );
}

void TestCrc( TestContext& test )
{
    constexpr std::array<uint8_t, 9> Known = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    test.Check( Crc32c( Known ) == 0xE3069283u, "CRC32C matches the standard 123456789 vector" );
    test.Check( Crc32c( std::span<const uint8_t>() ) == 0, "CRC32C of empty input is zero" );
}

void TestEveryTruncation( TestContext& test, const std::vector<uint8_t>& valid, const std::vector<RecordInfo>& records )
{
    std::set<uint64_t> boundaries = { FileHeaderSize, valid.size() };
    for( size_t i = 1; i < records.size(); i++ ) boundaries.insert( records[i].offset );

    for( size_t cut = 0; cut <= valid.size(); cut++ )
    {
        const auto scan = ScanJournal( std::span<const uint8_t>( valid.data(), cut ) );
        if( cut < FileHeaderSize )
        {
            test.Check( scan.code == ScanCode::TruncatedFileHeader, "header truncation at byte " + std::to_string( cut ) );
        }
        else if( boundaries.contains( cut ) )
        {
            test.Check( scan.code == ScanCode::Ok, "record boundary is a valid prefix at byte " + std::to_string( cut ) );
            test.Check( scan.validSize == cut, "boundary valid size equals cut at byte " + std::to_string( cut ) );
        }
        else
        {
            test.Check( scan.code == ScanCode::TruncatedTail, "record truncation is recoverable at byte " + std::to_string( cut ) );
            test.Check( scan.validSize < cut, "truncated record is excluded at byte " + std::to_string( cut ) );
        }
    }
}

void TestCorruption( TestContext& test, const std::vector<uint8_t>& valid, const std::vector<RecordInfo>& records )
{
    {
        auto corrupt = valid;
        corrupt[3] ^= 0x20;
        const auto scan = ScanJournal( corrupt );
        test.Check( scan.code == ScanCode::InvalidFileHeader, "file magic corruption is not recoverable" );
        test.Check( !scan.HasValidHeader(), "invalid file header is rejected" );
    }
    {
        auto corrupt = valid;
        const auto& record = records[1];
        corrupt[size_t( record.offset + RecordHeaderSize + 2 )] ^= 0x40;
        const auto scan = ScanJournal( corrupt );
        test.Check( scan.code == ScanCode::CorruptTail, "payload corruption is detected" );
        test.Check( scan.validSize == record.offset, "payload corruption preserves only earlier records" );
    }
    {
        auto corrupt = valid;
        const auto& record = records[2];
        const auto trailer = record.offset + RecordHeaderSize + record.payloadSize;
        corrupt[size_t( trailer + 24 )] ^= 1;
        const auto scan = ScanJournal( corrupt );
        test.Check( scan.code == ScanCode::CorruptTail, "commit CRC corruption is detected" );
        test.Check( scan.validSize == record.offset, "commit corruption excludes the record" );
    }
    {
        auto corrupt = valid;
        const auto& record = records[1];
        Put64( corrupt, size_t( record.offset + 16 ), 99 );
        Put32( corrupt, size_t( record.offset + 44 ), 0 );
        Put32( corrupt, size_t( record.offset + 44 ), HeaderCrc( corrupt, size_t( record.offset ), RecordHeaderSize, 44 ) );
        const auto scan = ScanJournal( corrupt );
        test.Check( scan.code == ScanCode::CorruptTail, "sequence discontinuity is detected after a valid header CRC" );
        test.Check( scan.validSize == record.offset, "sequence discontinuity preserves the causal prefix" );
    }
    {
        auto corrupt = valid;
        const auto& record = records[1];
        Put64( corrupt, size_t( record.offset + 32 ), DefaultMaxPayloadSize + 1 );
        Put32( corrupt, size_t( record.offset + 44 ), 0 );
        Put32( corrupt, size_t( record.offset + 44 ), HeaderCrc( corrupt, size_t( record.offset ), RecordHeaderSize, 44 ) );
        const auto scan = ScanJournal( corrupt );
        test.Check( scan.code == ScanCode::CorruptTail, "oversized payload is rejected before reading it" );
        test.Check( scan.validSize == record.offset, "oversized payload preserves only earlier records" );
    }
}

void TestShortWritesAndFailures( TestContext& test )
{
    {
        WriterOptions options;
        options.durableHeader = false;
        auto owned = std::make_unique<MemorySink>();
        auto* sink = owned.get();
        sink->maxWriteChunk = 3;
        std::string error;
        auto writer = JournalWriter::Create( std::move( owned ), DeterministicHeader(), options, error );
        test.Check( writer != nullptr, "three-byte short writes are retried for the file header: " + error );
        if( writer )
        {
            test.Check( writer->Append( RecordType::Diagnostic, 0, "short writes", 1, error ), "three-byte short writes are retried for a record: " + error );
            test.Check( ScanJournal( sink->bytes ).code == ScanCode::Ok, "short-write output is valid" );
        }
    }
    {
        WriterOptions options;
        options.durableHeader = false;
        MemorySink* sink = nullptr;
        std::string error;
        auto writer = NewMemoryWriter( sink, options, error );
        test.Check( writer != nullptr, "create failure-injection writer" );
        if( writer )
        {
            test.Check( writer->Append( RecordType::SessionBegin, 0, "first", 1, error ), "append valid prefix before failure" );
            const auto prefix = writer->CommittedSize();
            sink->failAt = sink->bytes.size() + 10;
            test.Check( !writer->Append( RecordType::ClientToServer, 0, "will fail", 2, error ), "injected record write fails" );
            test.Check( !writer->Healthy(), "write failure poisons writer" );
            const auto scan = ScanJournal( sink->bytes );
            test.Check( scan.code == ScanCode::TruncatedTail, "partial failed write is a truncated tail" );
            test.Check( scan.validSize == prefix, "partial failed write does not advance valid prefix" );
            test.Check( !writer->Append( RecordType::Diagnostic, 0, "blocked", 3, error ), "poisoned writer rejects later records" );
        }
    }
    {
        WriterOptions options;
        options.durableHeader = false;
        MemorySink* sink = nullptr;
        std::string error;
        auto writer = NewMemoryWriter( sink, options, error );
        test.Check( writer != nullptr, "create flush-failure writer" );
        if( writer )
        {
            const auto published = writer->PublishedSize();
            sink->failFlush = true;
            test.Check( !writer->Append( RecordType::Diagnostic, 0, "complete record", 1, error ), "injected publish flush fails" );
            test.Check( !writer->Healthy(), "flush failure poisons writer" );
            test.Check( writer->CommittedSize() > published, "record was committed before publish failed" );
            test.Check( writer->PublishedSize() == published, "published boundary does not move after flush failure" );
            test.Check( ScanJournal( sink->bytes ).code == ScanCode::Ok, "fully written record remains structurally valid after flush failure" );
        }
    }
    {
        WriterOptions options;
        options.durableHeader = false;
        options.maxPayloadSize = 4;
        MemorySink* sink = nullptr;
        std::string error;
        auto writer = NewMemoryWriter( sink, options, error );
        test.Check( writer != nullptr, "create bounded-payload writer" );
        if( writer )
        {
            test.Check( !writer->Append( RecordType::Diagnostic, 0, "12345", 1, error ), "writer rejects oversized payload" );
            test.Check( writer->Healthy(), "input validation does not poison writer" );
            test.Check( writer->Append( RecordType::Diagnostic, 0, "1234", 2, error ), "writer accepts payload at exact limit" );
        }
    }
    {
        WriterOptions options;
        options.durableHeader = false;
        MemorySink* sink = nullptr;
        std::string error;
        auto writer = NewMemoryWriter( sink, options, error );
        test.Check( writer != nullptr, "create monotonic-timestamp writer" );
        if( writer )
        {
            test.Check( writer->Append( RecordType::Diagnostic, 0, "first", 10, error ), "append initial timestamp" );
            test.Check( !writer->Append( RecordType::Diagnostic, 0, "backwards", 9, error ), "writer rejects a backwards timestamp" );
            test.Check( writer->Healthy(), "timestamp validation does not poison writer" );
            test.Check( writer->NextSequence() == 2, "rejected timestamp does not consume a sequence" );
            test.Check( writer->Append( RecordType::Diagnostic, 0, "equal", 10, error ), "writer permits an equal timestamp" );
        }
    }
}

std::filesystem::path UniqueTestDirectory()
{
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ( "tracy-stream-tests-" + std::to_string( ticks ) );
}

void TestFileRecoveryAndResume( TestContext& test )
{
    const auto directory = UniqueTestDirectory();
    const auto path = directory / "resume.tracy-stream";
    std::error_code ec;
    std::filesystem::create_directories( directory, ec );
    test.Check( !ec, "create temporary test directory" );

    WriterOptions options;
    options.durableHeader = true;
    std::string error;
    {
        auto writer = JournalWriter::CreateFileJournal( path, DeterministicHeader(), false, options, error );
        test.Check( writer != nullptr, "create journal file: " + error );
        if( writer )
        {
            test.Check( writer->Append( RecordType::SessionBegin, 0, "begin", 1, error ), "append file SessionBegin: " + error );
            test.Check( writer->Append( RecordType::ClientToServer, RecordFlagCompressedFrame, "frame", 2, error ), "append file frame: " + error );
            test.Check( writer->Flush( FlushMode::Durable, error ), "durably flush file prefix: " + error );
        }
    }
    {
        std::ofstream tail( path, std::ios::binary | std::ios::app );
        constexpr std::array<char, 7> Garbage = { 'b', 'a', 'd', 't', 'a', 'i', 'l' };
        tail.write( Garbage.data(), Garbage.size() );
    }

    const auto damaged = ScanJournal( path );
    test.Check( damaged.code == ScanCode::TruncatedTail, "seven-byte garbage is an invalid tail" );
    test.Check( damaged.HasRecoverablePrefix(), "damaged file has a recoverable prefix" );

    {
        auto refused = JournalWriter::ResumeFile( path, false, options, error );
        test.Check( refused == nullptr, "resume refuses implicit tail truncation" );
    }
    {
        auto resumed = JournalWriter::ResumeFile( path, true, options, error );
        test.Check( resumed != nullptr, "explicit repair resumes journal: " + error );
        if( resumed )
        {
            test.Check( resumed->NextSequence() == 3, "resume continues the record sequence" );
            test.Check( !resumed->Append( RecordType::Diagnostic, 0, "backwards", 1, error ), "resume preserves monotonic timestamp validation" );
            test.Check( resumed->Healthy(), "resume timestamp validation does not poison writer" );
            test.Check( resumed->NextSequence() == 3, "rejected resume timestamp does not consume a sequence" );
            test.Check( resumed->Append( RecordType::SessionEnd, RecordFlagTerminal, "recovered", 3, error ), "append durable SessionEnd after recovery: " + error );
        }
    }

    const auto repaired = ScanJournal( path );
    test.Check( repaired.code == ScanCode::Ok, "repaired file scans successfully: " + repaired.message );
    test.Check( repaired.complete, "repaired file ends cleanly" );
    test.Check( repaired.recordCount == 3, "repaired file retained prefix and appended terminal record" );

    {
        auto collision = JournalWriter::CreateFileJournal( path, DeterministicHeader(), false, options, error );
        test.Check( collision == nullptr, "CreateNew refuses to overwrite an existing journal" );
    }

    std::filesystem::remove_all( directory, ec );
    test.Check( !ec, "remove temporary test directory" );
}

bool WritePrefix( const std::filesystem::path& path, const std::vector<uint8_t>& bytes, size_t size )
{
    if( size > bytes.size() ) return false;
    std::ofstream file( path, std::ios::binary | std::ios::trunc );
    if( !file ) return false;
    file.write( reinterpret_cast<const char*>( bytes.data() ), std::streamsize( size ) );
    return bool( file );
}

void TestConvertedSnapshotMap( TestContext& test )
{
    const auto journal = MakeValidJournal( test );
    if( journal.empty() ) return;
    const auto directory = UniqueTestDirectory();
    const auto streamPath = directory / "converted.tracy-stream";
    const auto snapshotPath = directory / "converted.tracy";
    std::error_code filesystemError;
    std::filesystem::create_directories( directory, filesystemError );
    test.Check( !filesystemError, "create snapshot-map temporary directory" );
    test.Check( WritePrefix( streamPath, journal, journal.size() ), "write snapshot-map journal" );
    {
        std::ofstream snapshot( snapshotPath, std::ios::binary | std::ios::trunc );
        snapshot << "deterministic snapshot";
        test.Check( bool( snapshot ), "write snapshot-map snapshot" );
    }

    const auto scan = ScanJournal( streamPath );
    std::string error;
    test.Check( WriteConvertedSnapshotMap( streamPath, scan, snapshotPath, error ), "publish converted snapshot map: " + error );
    test.Check( std::filesystem::is_regular_file( ConvertedSnapshotMapPath( streamPath ) ), "snapshot map is published beside the stream" );

    auto store = JournalStore::Open( streamPath, error );
    test.Check( store != nullptr, "open mapped stream: " + error );
    if( store )
    {
        ConvertedSnapshotMap map;
        const auto view = store->AcquireReadView();
        test.Check( view && ReadConvertedSnapshotMap( streamPath, *view, map, error ), "read matching converted snapshot map: " + error );
        test.Check( map.snapshotPath == snapshotPath, "snapshot map resolves only the sibling snapshot" );

        {
            std::ofstream changed( snapshotPath, std::ios::binary | std::ios::app );
            changed << '!';
        }
        test.Check( !ReadConvertedSnapshotMap( streamPath, *view, map, error ), "snapshot mutation invalidates the map" );
        test.Check( !error.empty(), "snapshot-map invalidation returns a diagnostic" );
    }

    std::filesystem::remove_all( directory, filesystemError );
    test.Check( !filesystemError, "remove snapshot-map temporary directory" );
}

void TestJournalStore( TestContext& test )
{
    std::vector<RecordInfo> records;
    const auto valid = MakeValidJournal( test, &records );
    if( valid.empty() || records.size() != 4 ) return;
    const auto validScan = ScanJournal( valid );
    test.Check( validScan.prefixCrc32c != 0, "valid journal exposes a committed-prefix fingerprint" );

    const auto directory = UniqueTestDirectory();
    const auto path = directory / "live.tracy-stream";
    std::error_code ec;
    std::filesystem::create_directories( directory, ec );
    test.Check( !ec, "create journal store temporary directory" );
    test.Check( WritePrefix( path, valid, FileHeaderSize ), "write header-only journal prefix" );

    std::string error;
    auto store = JournalStore::Open( path, error );
    test.Check( store != nullptr, "open header-only journal store: " + error );
    if( !store )
    {
        std::filesystem::remove_all( directory, ec );
        return;
    }

    const auto headerView = store->AcquireReadView();
    test.Check( headerView && headerView->revision == 0, "header-only read view starts at revision zero" );
    test.Check( headerView && headerView->validSize == FileHeaderSize, "header-only read view is bounded to the file header" );
    test.Check( headerView && !headerView->complete, "header-only read view is incomplete" );

    const auto firstBoundary = size_t( records[1].offset );
    test.Check( WritePrefix( path, valid, firstBoundary ), "publish first complete record" );
    const auto firstRefresh = store->Refresh();
    test.Check( firstRefresh.code == RefreshCode::Published, "first committed record publishes a new view" );
    const auto firstView = store->AcquireReadView();
    test.Check( firstView && firstView->revision == 1, "first record advances revision to one" );
    test.Check( firstView && firstView->watermarkNs == 10, "first record advances the watermark" );

    const auto partialSecond = size_t( records[1].offset + RecordHeaderSize + 3 );
    test.Check( WritePrefix( path, valid, partialSecond ), "expose a partial second record" );
    const auto partialRefresh = store->Refresh();
    test.Check( partialRefresh.code == RefreshCode::Unchanged, "partial record does not publish a new view" );
    test.Check( partialRefresh.scanCode == ScanCode::TruncatedTail, "partial record is observed as a truncated live tail" );
    const auto unchangedView = store->AcquireReadView();
    test.Check( unchangedView == firstView, "unchanged refresh retains the exact immutable view object" );

    const auto secondBoundary = size_t( records[2].offset );
    test.Check( WritePrefix( path, valid, secondBoundary ), "publish second complete record" );
    const auto secondRefresh = store->Refresh();
    test.Check( secondRefresh.code == RefreshCode::Published, "second committed record publishes a new view" );
    const auto secondView = store->AcquireReadView();
    test.Check( secondView && secondView->revision == 2, "second record advances revision to two" );
    test.Check( secondView && secondView->watermarkNs == 20, "second record advances the watermark" );
    test.Check( firstView && firstView->revision == 1 && firstView->validSize == firstBoundary, "previous read view remains stable after publication" );

    std::array<uint8_t, 8> magic = {};
    test.Check( firstView && store->ReadCommitted( *firstView, 0, magic, error ), "old view can still read its committed prefix after append: " + error );
    test.Check( std::string_view( reinterpret_cast<const char*>( magic.data() ), magic.size() ) == "TRCSTRM1", "committed read returns the journal magic" );
    std::array<uint8_t, 4> beyond = {};
    test.Check( firstView && !store->ReadCommitted( *firstView, firstView->validSize - 2, beyond, error ), "committed read rejects bytes beyond a fixed view" );

    auto validRewrite = valid;
    const auto firstPayloadOffset = size_t( records[0].offset + RecordHeaderSize );
    validRewrite[firstPayloadOffset] ^= 0x20;
    RecomputeRecordIntegrity( validRewrite, records[0] );
    const auto rewrittenScan = ScanJournal( validRewrite );
    test.Check( rewrittenScan.code == ScanCode::Ok, "in-place rewrite fixture remains structurally valid" );
    test.Check( rewrittenScan.prefixCrc32c != validScan.prefixCrc32c, "prefix fingerprint changes after a validly re-encoded payload rewrite" );
    test.Check( WritePrefix( path, validRewrite, secondBoundary ), "write a validly re-encoded committed prefix" );
    const auto rewrittenRefresh = store->Refresh();
    test.Check( rewrittenRefresh.code == RefreshCode::Rejected, "store rejects validly re-encoded bytes at the same revision" );
    test.Check( store->AcquireReadView() == secondView, "valid rewrite retains the previous immutable view" );
    test.Check( firstView && !store->ReadCommitted( *firstView, 0, magic, error ), "committed read rejects a validly re-encoded old prefix" );
    test.Check( WritePrefix( path, valid, secondBoundary ), "restore the original committed prefix" );
    const auto restoredRefresh = store->Refresh();
    test.Check( restoredRefresh.code == RefreshCode::Unchanged, "restoring the exact committed bytes restores the unchanged view" );

    test.Check( WritePrefix( path, valid, valid.size() ), "publish complete journal" );
    const auto completeRefresh = store->Refresh();
    test.Check( completeRefresh.code == RefreshCode::Published, "SessionEnd publishes the complete view" );
    const auto completeView = store->AcquireReadView();
    test.Check( completeView && completeView->revision == 4, "complete view exposes the terminal revision" );
    test.Check( completeView && completeView->watermarkNs == 40, "complete view exposes the terminal watermark" );
    test.Check( completeView && completeView->complete, "SessionEnd marks the read view complete" );

    auto extraTail = valid;
    extraTail.push_back( 0xA5 );
    test.Check( WritePrefix( path, extraTail, extraTail.size() ), "append bytes after SessionEnd for rejection test" );
    const auto terminalMutation = store->Refresh();
    test.Check( terminalMutation.code == RefreshCode::Rejected, "store rejects growth after SessionEnd" );
    test.Check( store->AcquireReadView() == completeView, "terminal mutation retains the last complete view" );

    test.Check( WritePrefix( path, valid, FileHeaderSize ), "truncate journal for rollback test" );
    const auto rollback = store->Refresh();
    test.Check( rollback.code == RefreshCode::Rejected, "store rejects committed-prefix rollback" );
    test.Check( store->AcquireReadView() == completeView, "rollback retains the last complete view" );
    test.Check( !store->ReadCommitted( *completeView, 0, magic, error ), "committed read detects external truncation" );

    std::filesystem::remove_all( directory, ec );
    test.Check( !ec, "remove journal store temporary directory" );
}

void TestProtocolObserver( TestContext& test )
{
    const auto directory = UniqueTestDirectory();
    const auto path = directory / "protocol.tracy-stream";
    std::error_code ec;
    std::filesystem::create_directories( directory, ec );
    test.Check( !ec, "create protocol observer temporary directory" );

    ProtocolJournalOptions options;
    options.writer.durableHeader = true;
    options.sessionFlags = SessionBeginFlagDeferredSymbolExpansion;
    options.durableIntervalBytes = 32;
    options.durableIntervalNs = 0;
    std::string error;
    auto observer = StreamProtocolObserver::CreateFileJournal( path, "127.0.0.1", 8086, 76, false, options, error );
    test.Check( observer != nullptr, "create protocol observer: " + error );
    if( observer )
    {
        constexpr std::array<uint8_t, 4> Prefix = { 1, 2, 3, 4 };
        constexpr std::array<uint8_t, 12> Body = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        const std::array<ProtocolDataSpan, 2> spans = {
            ProtocolDataSpan { Prefix.data(), Prefix.size() },
            ProtocolDataSpan { Body.data(), Body.size() }
        };

        std::atomic<int> rejected { 0 };
        auto record = [&]( ProtocolDirection direction, ProtocolChunk chunk ) {
            for( int i = 0; i < 25; i++ )
            {
                if( !observer->OnProtocolData( direction, chunk, spans ) ) rejected++;
            }
        };
        std::thread client( record, ProtocolDirection::ClientToServer, ProtocolChunk::CompressedFrame );
        std::thread server( record, ProtocolDirection::ServerToClient, ProtocolChunk::ServerQuery );
        client.join();
        server.join();

        test.Check( rejected.load() == 0, "concurrent protocol observer writes all succeed" );
        test.Check( observer->ClientBytes() == 25 * 16, "protocol observer counts client bytes" );
        test.Check( observer->ServerBytes() == 25 * 16, "protocol observer counts server bytes" );
        test.Check( observer->OnProtocolData( ProtocolDirection::LocalControl, ProtocolChunk::ControlState, spans ),
            "protocol observer records local control bytes" );
        test.Check( observer->ServerBytes() == 25 * 16, "local control bytes stay outside the replayable server byte count" );
        test.Check( observer->OnProtocolClose( ProtocolCloseReason::CaptureComplete ), "protocol observer appends terminal record" );
        test.Check( observer->Finalized(), "protocol observer reports finalized" );
        test.Check( observer->DurableSize() == observer->CommittedSize(), "terminal record makes protocol journal durable" );
        test.Check( !observer->OnProtocolData( ProtocolDirection::ClientToServer, ProtocolChunk::Handshake, spans ), "finalized observer rejects later protocol bytes" );
        test.Check( observer->OnProtocolClose( ProtocolCloseReason::ObserverDestroyed ), "duplicate protocol close is idempotent" );
    }
    observer.reset();

    ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 1000;
    const auto scan = ScanJournal( path, scanOptions );
    test.Check( scan.code == ScanCode::Ok, "protocol observer journal scans successfully: " + scan.message );
    test.Check( scan.complete, "protocol observer journal has SessionEnd" );
    test.Check( scan.header.protocolVersion == 76, "protocol observer stores wire version" );

    size_t beginCount = 0;
    size_t clientCount = 0;
    size_t serverCount = 0;
    size_t checkpointCount = 0;
    size_t endCount = 0;
    size_t localControlCount = 0;
    RecordInfo beginRecord;
    for( const auto& record : scan.records )
    {
        switch( record.type )
        {
        case RecordType::SessionBegin:
            beginCount++;
            beginRecord = record;
            break;
        case RecordType::ClientToServer: clientCount++; break;
        case RecordType::ServerToClient: serverCount++; break;
        case RecordType::Checkpoint: checkpointCount++; break;
        case RecordType::SessionEnd: endCount++; break;
        case RecordType::Diagnostic:
            if( ( record.flags & RecordFlagLocalControl ) != 0 ) localControlCount++;
            break;
        default: break;
        }
    }
    test.Check( beginCount == 1, "protocol observer writes one SessionBegin" );
    test.Check( clientCount == 25, "protocol observer writes all client records" );
    test.Check( serverCount == 25, "protocol observer writes all server records" );
    test.Check( checkpointCount > 0, "protocol observer writes byte-based durable checkpoints" );
    test.Check( endCount == 1, "protocol observer writes exactly one SessionEnd" );
    test.Check( localControlCount == 1, "protocol observer marks one replay-external local control record" );
    if( beginCount == 1 )
    {
        std::array<uint8_t, 24> metadata = {};
        std::ifstream input( path, std::ios::binary );
        input.seekg( std::streamoff( beginRecord.offset + RecordHeaderSize ), std::ios::beg );
        input.read( reinterpret_cast<char*>( metadata.data() ), std::streamsize( metadata.size() ) );
        test.Check( input.gcount() == std::streamsize( metadata.size() ), "read protocol observer SessionBegin metadata" );
        test.Check( metadata[0] == 2 && metadata[1] == 0, "protocol observer writes SessionBegin metadata version 2" );
        test.Check( metadata[2] == 24 && metadata[3] == 0, "protocol observer writes the version 2 metadata header size" );
        test.Check( metadata[16] == SessionBeginFlagDeferredSymbolExpansion &&
            metadata[17] == 0 && metadata[18] == 0 && metadata[19] == 0,
            "protocol observer persists deferred symbol-expansion mode" );
    }

    std::filesystem::remove_all( directory, ec );
    test.Check( !ec, "remove protocol observer temporary directory" );
}

void TestProtocolBackpressure( TestContext& test )
{
    WriterOptions writerOptions;
    writerOptions.durableHeader = false;
    auto ownedSink = std::make_unique<MemorySink>();
    auto* sink = ownedSink.get();
    sink->writeDelay = std::chrono::microseconds( 1000 );

    std::string error;
    auto writer = JournalWriter::Create( std::move( ownedSink ), DeterministicHeader(), writerOptions, error );
    test.Check( writer != nullptr, "create slow protocol journal writer: " + error );
    if( !writer ) return;

    ProtocolJournalOptions options;
    options.writer = writerOptions;
    options.bufferBytes = 16;
    options.durableIntervalBytes = 0;
    options.durableIntervalNs = 0;
    auto observer = StreamProtocolObserver::CreateJournal( std::move( writer ), "slow-disk", 8086, 69, options, error );
    test.Check( observer != nullptr, "create bounded slow-disk observer: " + error );
    if( !observer ) return;

    constexpr std::array<uint8_t, 16> Payload = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    const ProtocolDataSpan span { Payload.data(), Payload.size() };
    std::atomic<int> rejected { 0 };
    auto produce = [&] {
        for( int i = 0; i < 12; i++ )
        {
            if( !observer->OnProtocolData(
                ProtocolDirection::ClientToServer,
                ProtocolChunk::CompressedFrame,
                std::span<const ProtocolDataSpan>( &span, 1 ) ) )
            {
                rejected++;
            }
        }
    };

    std::array<std::thread, 4> producers = {
        std::thread( produce ), std::thread( produce ), std::thread( produce ), std::thread( produce )
    };
    for( auto& thread : producers ) thread.join();

    test.Check( rejected.load() == 0, "bounded observer never drops a slow-disk protocol record" );
    test.Check( observer->OnProtocolClose( ProtocolCloseReason::CaptureComplete ), "slow-disk observer drains before SessionEnd" );
    test.Check( observer->BackpressureWaitCount() > 0, "slow disk causes measurable producer backpressure" );
    test.Check( observer->PeakBufferedBytes() <= options.bufferBytes * 2, "two-buffer relay stays inside its strict memory cap" );
    test.Check( observer->PeakBufferedBytes() >= options.bufferBytes, "slow-disk test exercises a populated relay buffer" );
    test.Check( observer->ClientBytes() == 48 * Payload.size(), "slow-disk observer commits every client byte" );

    ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 128;
    const auto scan = ScanJournal( sink->bytes, scanOptions );
    test.Check( scan.code == ScanCode::Ok, "slow-disk journal remains structurally valid: " + scan.message );
    test.Check( scan.complete, "slow-disk journal ends with a durable SessionEnd" );
    test.Check( scan.recordCount == 50, "slow-disk journal contains begin, all protocol records, and end" );
}

void TestReplayServerTranscript( TestContext& test )
{
    auto packet = []( uint64_t sequence, uint32_t flags, std::initializer_list<uint8_t> payload ) {
        return ReplayServerPacket { sequence, flags, std::vector<uint8_t>( payload ) };
    };
    auto query = []( uint64_t sequence, uint8_t type, uint8_t tag ) {
        std::vector<uint8_t> payload( 13, 0 );
        payload[0] = type;
        payload[1] = tag;
        return ReplayServerPacket { sequence, RecordFlagServerQuery, std::move( payload ) };
    };

    const std::vector<ReplayServerPacket> recorded = {
        packet( 2, RecordFlagHandshake, { 1, 2, 3, 4 } ),
        query( 9, uint8_t( tracy::ServerQuerySourceLocation ), 1 ),
        query( 10, uint8_t( tracy::ServerQueryCallstackFrame ), 2 ),
        query( 11, uint8_t( tracy::ServerQueryThreadString ), 3 ),
        query( 12, uint8_t( tracy::ServerQueryDisconnect ), 0 ),
        query( 13, uint8_t( tracy::ServerQueryDataTransferPart ), 4 ),
        query( 14, uint8_t( tracy::ServerQueryDataTransferPart ), 5 ),
        query( 15, uint8_t( tracy::ServerQueryTerminate ), 0 )
    };

    std::string error;
    test.Check( VerifyServerTranscript( recorded, recorded, error ), "identical server transcript verifies: " + error );

    auto reordered = recorded;
    std::swap( reordered[1], reordered[2] );
    std::swap( reordered[2], reordered[3] );
    test.Check( VerifyServerTranscript( recorded, reordered, error ), "independent definition queries may reorder: " + error );

    auto changed = reordered;
    changed[2].payload[1] = 99;
    test.Check( !VerifyServerTranscript( recorded, changed, error ) && !error.empty(),
        "reordered batch still rejects changed query bytes" );

    auto crossedControl = reordered;
    std::swap( crossedControl[3], crossedControl[4] );
    test.Check( !VerifyServerTranscript( recorded, crossedControl, error ),
        "definition queries cannot reorder across Disconnect" );

    const std::vector<ReplayServerPacket> separatedByClient = {
        query( 20, uint8_t( tracy::ServerQueryString ), 8 ),
        query( 22, uint8_t( tracy::ServerQueryThreadString ), 9 )
    };
    auto crossedClient = separatedByClient;
    std::swap( crossedClient[0], crossedClient[1] );
    test.Check( VerifyServerTranscript( separatedByClient, crossedClient, error ),
        "definition queries may reorder across client compressed-frame boundaries: " + error );

    auto changedAcrossClient = crossedClient;
    changedAcrossClient[0].payload[1] = 10;
    test.Check( !VerifyServerTranscript( separatedByClient, changedAcrossClient, error ) && !error.empty(),
        "cross-frame definition-query multiset still rejects changed query bytes" );

    const std::vector<ReplayServerPacket> recordedThreadClassification = {
        query( 30, uint8_t( tracy::ServerQueryThreadString ), 1 ),
        query( 31, uint8_t( tracy::ServerQueryThreadString ), 2 )
    };
    const std::vector<ReplayServerPacket> replayedStringClassification = {
        query( 30, uint8_t( tracy::ServerQueryString ), 7 ),
        query( 31, uint8_t( tracy::ServerQueryString ), 8 )
    };
    test.Check( VerifyServerTranscript( recordedThreadClassification, replayedStringClassification, error ),
        "balanced ThreadString-to-String replay classification drift is tolerated" );

    auto batched = [&]( uint64_t sequence, std::initializer_list<ReplayServerPacket> queries ) {
        ReplayServerPacket value { sequence, RecordFlagServerQuery, {} };
        for( const auto& item : queries ) value.payload.insert( value.payload.end(), item.payload.begin(), item.payload.end() );
        return value;
    };
    const std::vector<ReplayServerPacket> recordedBatch = { batched( 40, {
        query( 40, uint8_t( tracy::ServerQuerySourceLocation ), 1 ),
        query( 40, uint8_t( tracy::ServerQueryCallstackFrame ), 2 ) } ) };
    const std::vector<ReplayServerPacket> replayedBatch = { batched( 40, {
        query( 40, uint8_t( tracy::ServerQueryCallstackFrame ), 2 ),
        query( 40, uint8_t( tracy::ServerQuerySourceLocation ), 1 ) } ) };
    test.Check( VerifyServerTranscript( recordedBatch, replayedBatch, error ),
        "coalesced server-query network records are split into protocol packets before verification" );

    auto reorderedTransfer = recorded;
    std::swap( reorderedTransfer[5], reorderedTransfer[6] );
    test.Check( !VerifyServerTranscript( recorded, reorderedTransfer, error ),
        "source-transfer fragments retain strict ordering" );

    auto parameterRecorded = recorded;
    parameterRecorded.insert( parameterRecorded.begin() + 2,
        query( 10, uint8_t( tracy::ServerQueryParameter ), 7 ) );
    for( size_t i = 3; i < parameterRecorded.size(); i++ ) parameterRecorded[i].sequence++;
    auto reorderedParameter = parameterRecorded;
    std::swap( reorderedParameter[1], reorderedParameter[2] );
    test.Check( !VerifyServerTranscript( parameterRecorded, reorderedParameter, error ),
        "state-changing parameter queries retain strict ordering" );

    auto missing = recorded;
    missing.pop_back();
    test.Check( !VerifyServerTranscript( recorded, missing, error ), "missing server packet is rejected" );
}

}

int main()
{
    TestContext test;
    TestCrc( test );

    std::vector<RecordInfo> records;
    const auto valid = MakeValidJournal( test, &records );
    if( !valid.empty() && records.size() == 4 )
    {
        TestEveryTruncation( test, valid, records );
        TestCorruption( test, valid, records );
    }
    TestShortWritesAndFailures( test );
    TestFileRecoveryAndResume( test );
    TestConvertedSnapshotMap( test );
    TestJournalStore( test );
    TestProtocolObserver( test );
    TestProtocolBackpressure( test );
    TestReplayServerTranscript( test );

    if( test.failures != 0 )
    {
        std::cerr << test.failures << " journal test(s) failed.\n";
        return 1;
    }
    std::cout << "All tracy-stream journal tests passed.\n";
    return 0;
}
