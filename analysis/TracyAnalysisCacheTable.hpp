#ifndef __TRACYANALYSISCACHETABLE_HPP__
#define __TRACYANALYSISCACHETABLE_HPP__
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyAnalysisDiskBudget.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{
// A Query-internal immutable, sorted record table. This is not a TraceSource
// and does not decode or replace .tracy/stream. Callers retain exact semantics.
struct AnalysisCacheTableOptions
{
    uint64_t blockBytes = 4 * 1024 * 1024;
    uint64_t manifestBytes = 8 * 1024 * 1024;
    std::function<bool()> cancelled;
    std::shared_ptr<AnalysisWorkspaceBudget> workspace;
    std::shared_ptr<AnalysisDiskBudget> disk;
};
struct AnalysisCacheTableDescriptor
{
    uint64_t records = 0;
    uint64_t blocks = 0;
    std::string contentSha256;
};
struct AnalysisCacheRecord
{
private:
    // Declared first so the strings die before their charge is released. The
    // per-record overhead also covers ReadPage's vector capacity growth.
    AnalysisWorkspaceReservation m_workspace;
public:
    std::string key;
    std::string payload;
    AnalysisCacheRecord(std::string_view key={},std::string_view payload={},
        std::shared_ptr<AnalysisWorkspaceBudget> workspace={})
        :m_workspace(std::move(workspace),512+2ull*(key.size()+payload.size())),key(key),payload(payload) {}
    AnalysisCacheRecord(const AnalysisCacheRecord&)=delete;
    AnalysisCacheRecord(AnalysisCacheRecord&&) noexcept=default;
    AnalysisCacheRecord& operator=(AnalysisCacheRecord other) noexcept
    {
        std::swap(m_workspace,other.m_workspace);
        key.swap(other.key); payload.swap(other.payload);
        return *this; // Replaced strings are destroyed before the old lease.
    }
};
struct AnalysisCachePage
{
    std::vector<AnalysisCacheRecord> records;
    uint64_t nextOrdinal = 0;
    bool done = false;
};
struct AnalysisCacheReadMetrics
{
    uint64_t blocksLoaded = 0;
    uint64_t payloadBytesRead = 0;
    uint64_t peakBlockBytes = 0;
};

class AnalysisCacheTableWriter
{
public:
    AnalysisCacheTableWriter(std::filesystem::path root, std::string identity,
        std::string kind, AnalysisCacheTableOptions options = {});
    ~AnalysisCacheTableWriter();
    AnalysisCacheTableWriter(const AnalysisCacheTableWriter&) = delete;
    AnalysisCacheTableWriter& operator=(const AnalysisCacheTableWriter&) = delete;
    // Keys are strictly increasing byte strings. Duplicate/unsorted input
    // invalidates this writer; it is never silently coalesced or overwritten.
    void Append(std::string_view key, std::string_view payload);
    AnalysisCacheTableDescriptor Commit();
    uint64_t PeakBufferedBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class AnalysisCacheTableReader
{
public:
    AnalysisCacheTableReader(std::filesystem::path root, std::string expectedIdentity,
        std::string expectedKind, AnalysisCacheTableOptions options = {});
    ~AnalysisCacheTableReader();
    AnalysisCacheTableReader(const AnalysisCacheTableReader&) = delete;
    AnalysisCacheTableReader& operator=(const AnalysisCacheTableReader&) = delete;
    uint64_t RecordCount() const;
    const AnalysisCacheTableDescriptor& Descriptor() const;
    AnalysisCacheRecord GetAt(uint64_t ordinal);
    uint64_t LowerBound(std::string_view key);
    std::optional<AnalysisCacheRecord> Find(std::string_view key);
    AnalysisCachePage ReadPage(uint64_t ordinal, size_t limit, uint64_t maxResponseBytes);
    AnalysisCacheReadMetrics Metrics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
#endif
