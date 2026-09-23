#ifndef __TRACYANALYSISEXTERNALSORT_HPP__
#define __TRACYANALYSISEXTERNALSORT_HPP__

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

namespace tracy::analysis
{

struct AnalysisUInt64Pair
{
    uint64_t key = 0;
    uint64_t value = 0;
};

static_assert( sizeof( AnalysisUInt64Pair ) == 16 );

struct AnalysisKeyMetadataRecord
{
    uint64_t keyHigh = 0;
    uint64_t keyLow = 0;
    uint64_t metadata = 0;
    uint32_t auxiliary = 0;
    uint8_t kind = 0;
    uint8_t reserved[3] {};
};

static_assert( sizeof( AnalysisKeyMetadataRecord ) == 32 );

// Logical bytes processed by this sort, not operating-system physical I/O.
// On failure, only completed phases are reflected in these counters.
struct AnalysisExternalSortStatistics
{
    uint64_t inputRecords = 0;
    uint64_t initialRuns = 0;
    uint64_t mergePasses = 0;
    uint64_t mergedGroups = 0;
    uint64_t sourceReadBytes = 0;
    uint64_t initialRunWriteBytes = 0;
    uint64_t mergeReadBytes = 0;
    uint64_t mergeWriteBytes = 0;
};

// Choose a bounded run length that aims for one 64-way merge while leaving
// at least half of the currently available shared workspace for other users.
uint64_t ChooseAnalysisExternalSortBufferRecords( uint64_t inputRecords, uint32_t recordBytes,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace = {} );

bool SortAnalysisUInt64Pairs(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    uint64_t expectedCount,
    uint64_t maximumBufferedPairs,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk = {},
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace = {} );

bool SortAnalysisKeyMetadataRecords(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    uint64_t expectedCount,
    uint64_t maximumBufferedRecords,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk = {},
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace = {},
    const std::function<bool()>& cancelled = {},
    AnalysisExternalSortStatistics* statistics = nullptr );

// Generate sorted runs as records arrive, avoiding a complete unsorted source
// file for high-volume joins. The producer must call emit exactly once per
// record; exceptions propagate so the owner can clean its scratch directory.
bool SortAnalysisKeyMetadataRecordsGenerated(
    const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    uint64_t maximumBufferedRecords,
    const std::function<void(const std::function<void(const AnalysisKeyMetadataRecord&)>& emit)>& produce,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk = {},
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace = {},
    const std::function<bool()>& cancelled = {},
    AnalysisExternalSortStatistics* statistics = nullptr );

bool CleanupAnalysisExternalSortFiles(
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk = {} );

}

#endif
