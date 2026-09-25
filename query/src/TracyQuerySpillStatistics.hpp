#ifndef __TRACYQUERYSPILLSTATISTICS_HPP__
#define __TRACYQUERYSPILLSTATISTICS_HPP__

#include "TracyAnalysis.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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

class QueryGroupedSpillStatistics
{
public:
    QueryGroupedSpillStatistics( const std::filesystem::path& temporaryRoot,
        std::shared_ptr<analysis::AnalysisDiskBudget> disk,
        std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
        std::function<void()> check = {}, size_t maximumBufferedValues = 524288 );
    ~QueryGroupedSpillStatistics();
    QueryGroupedSpillStatistics( const QueryGroupedSpillStatistics& ) = delete;
    QueryGroupedSpillStatistics& operator=( const QueryGroupedSpillStatistics& ) = delete;
    void Append( uint64_t group, uint8_t metric, int64_t value );
    void Finish( const std::function<void( uint64_t, uint8_t, const analysis::Statistics& )>& visit );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QuerySpillUInt64Set
{
public:
    QuerySpillUInt64Set( const std::filesystem::path& temporaryRoot,
        std::shared_ptr<analysis::AnalysisDiskBudget> disk,
        std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
        std::function<void()> check = {}, size_t maximumBufferedValues = 65536 );
    ~QuerySpillUInt64Set();
    QuerySpillUInt64Set( const QuerySpillUInt64Set& ) = delete;
    QuerySpillUInt64Set& operator=( const QuerySpillUInt64Set& ) = delete;
    void Append( uint64_t value );
    void Finish();
    uint64_t Count() const;
    bool Contains( uint64_t value ) const;
    void VisitValues( const std::function<bool( uint64_t )>& visit ) const;

private:
    void Check() const;
    void Flush();
    void Remove( const std::filesystem::path& path );
    std::filesystem::path NewRun();
    std::filesystem::path Merge( const std::filesystem::path& left, const std::filesystem::path& right );
    std::filesystem::path m_root;
    std::filesystem::path m_sorted;
    std::shared_ptr<analysis::AnalysisDiskBudget> m_disk;
    analysis::AnalysisWorkspaceReservation m_workspace;
    std::function<void()> m_check;
    size_t m_maximumBufferedValues;
    std::vector<uint64_t> m_values;
    std::array<std::filesystem::path, 64> m_runs;
    uint64_t m_uniqueCount = 0;
    uint64_t m_nextRun = 0;
    bool m_finished = false;
};

class QuerySpillUInt64Graph
{
public:
    QuerySpillUInt64Graph( const std::filesystem::path& temporaryRoot,
        std::shared_ptr<analysis::AnalysisDiskBudget> disk,
        std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
        std::function<void()> check = {} );
    ~QuerySpillUInt64Graph();
    QuerySpillUInt64Graph( const QuerySpillUInt64Graph& ) = delete;
    QuerySpillUInt64Graph& operator=( const QuerySpillUInt64Graph& ) = delete;
    void AppendUndirected( uint64_t left, uint64_t right );
    void Finish();
    uint64_t EdgeRecordCount() const;
    void VisitNeighbors( uint64_t node, const std::function<bool( uint64_t )>& visit ) const;

private:
    void Check() const;
    std::filesystem::path m_root;
    std::filesystem::path m_source;
    std::filesystem::path m_sorted;
    std::shared_ptr<analysis::AnalysisDiskBudget> m_disk;
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> m_workspace;
    std::function<void()> m_check;
    std::unique_ptr<std::ofstream> m_output;
    uint64_t m_edgeRecords = 0;
    bool m_finished = false;
};

}
#endif
