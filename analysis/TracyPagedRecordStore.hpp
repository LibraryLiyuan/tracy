#ifndef TRACY_PAGED_RECORD_STORE_HPP
#define TRACY_PAGED_RECORD_STORE_HPP
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
namespace tracy::analysis
{
struct PagedRecordOptions
{
    uint32_t blockBytes=4*1024*1024;
    std::shared_ptr<AnalysisWorkspaceBudget> workspace;
    std::shared_ptr<AnalysisDiskBudget> disk;
    std::function<bool()> cancelled;
    // Optional conversion-owned file accounting. Sizes are cumulative per file.
    std::function<void(const std::filesystem::path&,uint64_t)> beforeFileGrowth;
    std::function<void(const std::filesystem::path&,const std::filesystem::path&)> renameFile;
    uint64_t activeReadCacheBytes=0;
    std::function<void(uint64_t)> beforeMemoryAllocation;
};
// Fixed-width records (including byte records for string/blob storage). The
// ordinal directory is on disk; neither open nor lookup materializes it.
class PagedRecordWriter
{
public:
    PagedRecordWriter(std::filesystem::path root,std::string identity,uint32_t recordBytes,PagedRecordOptions options={});
    ~PagedRecordWriter();
    void Append(const void* records,uint64_t count);
    void Commit();
    uint64_t Count() const;
    void Read(uint64_t ordinal,void* output,uint64_t count);
    void ClearReadCache();
    void LimitReadCache(uint64_t bytes);
    std::string ContentHash() const;
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
class PagedRecordReader
{
public:
    struct VisitStats {uint64_t blocks=0,records=0,decodedBytes=0,storedBytes=0;};
    PagedRecordReader(std::filesystem::path root,std::string identity,uint32_t recordBytes,PagedRecordOptions options={});
    ~PagedRecordReader();
    uint64_t Count() const;
    void Read(uint64_t ordinal,void* output,uint64_t count);
    void ValidateAll();
    VisitStats VisitValidated(const std::function<void(const void*,uint64_t)>&);
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
}
#endif
