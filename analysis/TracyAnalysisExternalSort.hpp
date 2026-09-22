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
    const std::function<bool()>& cancelled = {} );

bool CleanupAnalysisExternalSortFiles(
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk = {} );

}

#endif
