#ifndef __TRACYQUERYSPILLSTATISTICS_HPP__
#define __TRACYQUERYSPILLSTATISTICS_HPP__

#include "TracyAnalysis.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <vector>

namespace tracy::query
{

class QuerySpillStatistics
{
public:
    QuerySpillStatistics( const std::filesystem::path& temporaryRoot,
        std::shared_ptr<analysis::AnalysisDiskBudget> disk,
        std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
        std::function<void()> check = {}, size_t maximumBufferedValues = 65536 );
    ~QuerySpillStatistics();
    QuerySpillStatistics( const QuerySpillStatistics& ) = delete;
    QuerySpillStatistics& operator=( const QuerySpillStatistics& ) = delete;
    void Append( int64_t value );
    analysis::Statistics Finish();

private:
    void Check() const;
    void Flush();
    void Remove( const std::filesystem::path& path );
    std::filesystem::path NewRun();
    std::filesystem::path Merge( const std::filesystem::path& left, const std::filesystem::path& right );
    std::filesystem::path m_root;
    std::shared_ptr<analysis::AnalysisDiskBudget> m_disk;
    analysis::AnalysisWorkspaceReservation m_workspace;
    std::function<void()> m_check;
    size_t m_maximumBufferedValues;
    std::vector<int64_t> m_values;
    std::array<std::filesystem::path, 64> m_runs;
    uint64_t m_count = 0;
    uint64_t m_nextRun = 0;
    bool m_finished = false;
};

}
#endif
