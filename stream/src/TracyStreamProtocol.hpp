#pragma once

#include "TracyStreamJournal.hpp"

#include "../../server/TracyProtocolObserver.hpp"

#include <condition_variable>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tracy::stream
{

struct ProtocolJournalOptions
{
    WriterOptions writer;
    uint64_t durableIntervalNs = 1'000'000'000ull;
    uint64_t durableIntervalBytes = 16ull * 1024 * 1024;
    // The writer drains one buffer while producers may fill one queued
    // buffer. A callback blocks when the queued slot is occupied, allowing
    // TCP backpressure to reach the instrumented process without dropping
    // protocol bytes.
    uint64_t bufferBytes = 512ull * 1024;
};

class StreamProtocolObserver final : public ProtocolObserver
{
public:
    static std::unique_ptr<StreamProtocolObserver> CreateFileJournal( const std::filesystem::path& path, std::string_view address, uint16_t port, uint32_t protocolVersion, bool overwrite, const ProtocolJournalOptions& options, std::string& error );
    static std::unique_ptr<StreamProtocolObserver> CreateJournal( std::unique_ptr<JournalWriter> writer, std::string_view address, uint16_t port, uint32_t protocolVersion, const ProtocolJournalOptions& options, std::string& error );

    ~StreamProtocolObserver() override;

    StreamProtocolObserver( const StreamProtocolObserver& ) = delete;
    StreamProtocolObserver& operator=( const StreamProtocolObserver& ) = delete;

    bool OnProtocolData( ProtocolDirection direction, ProtocolChunk chunk, std::span<const ProtocolDataSpan> data ) override;
    bool OnProtocolClose( ProtocolCloseReason reason ) override;

    bool Failed() const;
    bool Finalized() const;
    std::string LastError() const;
    uint64_t ClientBytes() const;
    uint64_t ServerBytes() const;
    uint64_t CommittedSize() const;
    uint64_t DurableSize() const;
    uint64_t BackpressureWaitCount() const;
    uint64_t PeakBufferedBytes() const;

private:
    struct PendingRecord
    {
        RecordType type = RecordType::Diagnostic;
        uint32_t flags = 0;
        uint64_t timestampNs = 0;
        uint64_t clientBytesAfter = 0;
        uint64_t serverBytesAfter = 0;
        std::vector<uint8_t> payload;
    };

    StreamProtocolObserver( std::unique_ptr<JournalWriter> writer, const ProtocolJournalOptions& options );

    bool AppendSessionBegin( std::string_view address, uint16_t port, uint32_t protocolVersion, std::string& error );
    bool AppendCheckpoint( uint64_t timestampNs, uint64_t clientBytes, uint64_t serverBytes );
    bool AppendTerminal( ProtocolCloseReason reason, uint64_t clientBytes, uint64_t serverBytes );
    bool WriteProtocolRecord( PendingRecord& record );
    void WriterLoop();
    uint64_t TimestampNs() const;
    void SetFailureLocked( const std::string& error );

    mutable std::mutex m_lock;
    std::mutex m_enqueueLock;
    std::condition_variable m_writerReady;
    std::condition_variable m_queueSpace;
    std::deque<PendingRecord> m_queue;
    std::thread m_writerThread;
    std::unique_ptr<JournalWriter> m_writer;
    ProtocolJournalOptions m_options;
    std::chrono::steady_clock::time_point m_started;
    uint64_t m_clientBytes = 0;
    uint64_t m_serverBytes = 0;
    uint64_t m_bytesAtLastDurable = 0;
    uint64_t m_lastDurableNs = 0;
    uint64_t m_activeBytes = 0;
    uint64_t m_queuedBytes = 0;
    uint64_t m_peakBufferedBytes = 0;
    uint64_t m_backpressureWaitCount = 0;
    uint64_t m_committedSize = 0;
    uint64_t m_durableSize = 0;
    ProtocolCloseReason m_closeReason = ProtocolCloseReason::ObserverDestroyed;
    bool m_closeRequested = false;
    bool m_failed = false;
    bool m_finalized = false;
    std::string m_error;
};

}
