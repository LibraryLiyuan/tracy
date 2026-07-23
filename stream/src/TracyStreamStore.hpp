#pragma once

#include "TracyStreamJournal.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace tracy::stream
{

// An immutable boundary over the longest fully committed journal prefix.
// A view never includes a partial record, even while the writer is appending.
struct JournalReadView
{
    std::filesystem::path path;
    FileHeader header;
    uint64_t revision = 0;
    uint64_t validSize = 0;
    uint64_t recordCount = 0;
    uint64_t watermarkNs = 0;
    uint32_t prefixCrc32c = 0;
    uint64_t observedFileSize = 0;
    ScanCode observedCode = ScanCode::IoError;
    bool complete = false;

private:
    friend class JournalStore;
    std::shared_ptr<const uint8_t> storeIdentity;
};

enum class RefreshCode
{
    Unchanged,
    Published,
    Rejected,
    IoError
};

struct RefreshResult
{
    RefreshCode code = RefreshCode::IoError;
    ScanCode scanCode = ScanCode::IoError;
    uint64_t previousRevision = 0;
    uint64_t revision = 0;
    uint64_t validSize = 0;
    std::string message;
};

class JournalStore
{
public:
    static std::unique_ptr<JournalStore> Open( const std::filesystem::path& path, const ScanOptions& options, std::string& error );
    static std::unique_ptr<JournalStore> Open( const std::filesystem::path& path, std::string& error );

    JournalStore( const JournalStore& ) = delete;
    JournalStore& operator=( const JournalStore& ) = delete;

    RefreshResult Refresh();
    std::shared_ptr<const JournalReadView> AcquireReadView() const;
    ScanResult LastObservation() const;

    // Reads only bytes covered by the supplied immutable committed view.
    // The size check also detects obvious truncate/replace violations.
    bool ReadCommitted( const JournalReadView& view, uint64_t offset, std::span<uint8_t> output, std::string& error ) const;

private:
    JournalStore( std::filesystem::path path, ScanOptions options );

    RefreshResult RefreshLocked();
    static bool SameHeader( const FileHeader& left, const FileHeader& right );

    const std::filesystem::path m_path;
    const std::shared_ptr<const uint8_t> m_identity;
    ScanOptions m_options;
    mutable std::mutex m_lock;
    std::shared_ptr<const JournalReadView> m_view;
    ScanResult m_lastObservation;
};

const char* RefreshCodeName( RefreshCode code );

}
