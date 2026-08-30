#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::stream
{

inline constexpr uint16_t JournalVersionMajor = 1;
inline constexpr uint16_t JournalVersionMinor = 0;
inline constexpr uint64_t FileHeaderSize = 64;
inline constexpr uint64_t RecordHeaderSize = 48;
inline constexpr uint64_t RecordTrailerSize = 32;
inline constexpr uint64_t DefaultMaxPayloadSize = 64ull * 1024 * 1024;
inline constexpr uint32_t SessionBeginFlagDeferredSymbolExpansion = 1u << 0;
inline constexpr uint32_t SessionBeginSupportedFlags = SessionBeginFlagDeferredSymbolExpansion;

enum class RecordType : uint16_t
{
    SessionBegin = 1,
    ClientToServer = 2,
    ServerToClient = 3,
    Checkpoint = 4,
    SessionEnd = 5,
    Diagnostic = 6
};

enum RecordFlags : uint32_t
{
    RecordFlagNone = 0,
    RecordFlagHandshake = 1u << 0,
    RecordFlagCompressedFrame = 1u << 1,
    RecordFlagServerQuery = 1u << 2,
    RecordFlagDurabilityBoundary = 1u << 3,
    RecordFlagTerminal = 1u << 4,
    RecordFlagLocalControl = 1u << 5
};

enum class FlushMode
{
    Published,
    Durable
};

struct FileHeader
{
    uint32_t flags = 0;
    uint32_t protocolVersion = 0;
    std::array<uint8_t, 16> sessionId = {};
    uint64_t createdUnixNs = 0;
};

struct WriterOptions
{
    uint64_t maxPayloadSize = DefaultMaxPayloadSize;
    bool publishEachRecord = true;
    bool durableHeader = true;
};

struct PayloadSpan
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

class JournalSink
{
public:
    virtual ~JournalSink() = default;

    virtual bool Write( const uint8_t* data, size_t size, size_t& written, std::string& error ) = 0;
    virtual bool Flush( bool durable, std::string& error ) = 0;
    virtual uint64_t Tell() const = 0;
};

class JournalWriter
{
public:
    static std::unique_ptr<JournalWriter> CreateFileJournal( const std::filesystem::path& path, const FileHeader& header, bool overwrite, const WriterOptions& options, std::string& error );
    static std::unique_ptr<JournalWriter> Create( std::unique_ptr<JournalSink> sink, const FileHeader& header, const WriterOptions& options, std::string& error );
    static std::unique_ptr<JournalWriter> ResumeFile( const std::filesystem::path& path, bool truncateInvalidTail, const WriterOptions& options, std::string& error );

    ~JournalWriter();

    JournalWriter( const JournalWriter& ) = delete;
    JournalWriter& operator=( const JournalWriter& ) = delete;

    bool Append( RecordType type, uint32_t flags, std::span<const uint8_t> payload, uint64_t monotonicNs, std::string& error );
    bool Append( RecordType type, uint32_t flags, std::string_view payload, uint64_t monotonicNs, std::string& error );
    bool Append( RecordType type, uint32_t flags, std::span<const PayloadSpan> payload, uint64_t monotonicNs, std::string& error );
    bool Flush( FlushMode mode, std::string& error );

    bool Healthy() const { return m_healthy; }
    uint64_t NextSequence() const { return m_nextSequence; }
    uint64_t CommittedSize() const { return m_committedSize; }
    uint64_t PublishedSize() const { return m_publishedSize; }
    uint64_t DurableSize() const { return m_durableSize; }
    const FileHeader& Header() const { return m_header; }

private:
    JournalWriter( std::unique_ptr<JournalSink> sink, const FileHeader& header, const WriterOptions& options );

    bool WriteAll( const uint8_t* data, size_t size, std::string& error );
    bool WriteInitialHeader( std::string& error );
    bool FailIo( const std::string& message, std::string& error );

    std::unique_ptr<JournalSink> m_sink;
    FileHeader m_header;
    WriterOptions m_options;
    uint64_t m_nextSequence = 1;
    uint64_t m_committedSize = 0;
    uint64_t m_publishedSize = 0;
    uint64_t m_durableSize = 0;
    uint64_t m_lastMonotonicNs = 0;
    bool m_healthy = true;
};

enum class ScanCode
{
    Ok,
    Stopped,
    TruncatedFileHeader,
    InvalidFileHeader,
    TruncatedTail,
    CorruptTail,
    IoError
};

struct RecordInfo
{
    uint64_t offset = 0;
    uint64_t sequence = 0;
    uint64_t monotonicNs = 0;
    uint64_t payloadSize = 0;
    uint32_t flags = 0;
    RecordType type = RecordType::Diagnostic;
};

using ScanRecordVisitor = void ( * )( const RecordInfo& record, void* userData );
using ScanStopRequested = bool ( * )( void* userData );

struct ScanOptions
{
    uint64_t maxPayloadSize = DefaultMaxPayloadSize;
    size_t maxCollectedRecords = 256;
    // Called only after the complete record header, payload, trailer and all
    // checksums have been validated. The callback must not retain pointers to
    // scanner-owned buffers. It enables bounded-memory inventory consumers
    // without duplicating the journal validation rules.
    ScanRecordVisitor recordVisitor = nullptr;
    void* recordVisitorUserData = nullptr;
    // Checked only after a complete record has been validated and committed to
    // ScanResult. This makes cancellation a recoverable record-boundary stop.
    ScanStopRequested stopRequested = nullptr;
    void* stopRequestedUserData = nullptr;
};

struct ScanResult
{
    ScanCode code = ScanCode::IoError;
    FileHeader header;
    uint64_t fileSize = 0;
    uint64_t validSize = 0;
    uint64_t recordCount = 0;
    uint64_t lastSequence = 0;
    uint64_t lastMonotonicNs = 0;
    uint32_t prefixCrc32c = 0;
    bool complete = false;
    std::string message;
    std::vector<RecordInfo> records;

    bool HasValidHeader() const;
    bool HasRecoverablePrefix() const;
};

ScanResult ScanJournal( const std::filesystem::path& path, const ScanOptions& options = {} );
ScanResult ScanJournalPrefix( const std::filesystem::path& path, uint64_t prefixSize, const ScanOptions& options = {} );
ScanResult ScanJournal( std::span<const uint8_t> bytes, const ScanOptions& options = {} );
bool TruncateToValidPrefix( const std::filesystem::path& path, const ScanResult& scan, std::string& error );

FileHeader MakeFileHeader( uint32_t protocolVersion, uint32_t flags = 0 );
uint32_t Crc32c( std::span<const uint8_t> bytes );
const char* RecordTypeName( RecordType type );
const char* ScanCodeName( ScanCode code );

}
