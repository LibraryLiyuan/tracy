#pragma once

#include "TracyStreamJournal.hpp"

#include "../../server/TracyProtocolObserver.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace tracy::stream
{

struct ProtocolJournalOptions
{
    WriterOptions writer;
    uint64_t durableIntervalNs = 1'000'000'000ull;
    uint64_t durableIntervalBytes = 16ull * 1024 * 1024;
};

class StreamProtocolObserver final : public ProtocolObserver
{
public:
    static std::unique_ptr<StreamProtocolObserver> CreateFileJournal( const std::filesystem::path& path, std::string_view address, uint16_t port, uint32_t protocolVersion, bool overwrite, const ProtocolJournalOptions& options, std::string& error );

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

private:
    StreamProtocolObserver( std::unique_ptr<JournalWriter> writer, const ProtocolJournalOptions& options );

    bool AppendSessionBegin( std::string_view address, uint16_t port, uint32_t protocolVersion, std::string& error );
    bool AppendCheckpoint( uint64_t timestampNs );
    uint64_t TimestampNs() const;
    void SetFailure( const std::string& error );

    mutable std::mutex m_lock;
    std::unique_ptr<JournalWriter> m_writer;
    ProtocolJournalOptions m_options;
    std::chrono::steady_clock::time_point m_started;
    uint64_t m_clientBytes = 0;
    uint64_t m_serverBytes = 0;
    uint64_t m_bytesAtLastDurable = 0;
    uint64_t m_lastDurableNs = 0;
    bool m_failed = false;
    bool m_finalized = false;
    std::string m_error;
};

}
