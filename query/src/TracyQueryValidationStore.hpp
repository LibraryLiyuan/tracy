#ifndef __TRACYQUERYVALIDATIONSTORE_HPP__
#define __TRACYQUERYVALIDATIONSTORE_HPP__

#include "TracyPagedRecordStore.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracy::query
{
class QueryValidationStoreWriter
{
public:
    QueryValidationStoreWriter(std::filesystem::path root,std::string sourceFingerprint,
        analysis::PagedRecordOptions options={},uint64_t maximumBufferedCallstacks=1024*1024);
    ~QueryValidationStoreWriter();
    QueryValidationStoreWriter(const QueryValidationStoreWriter&)=delete;
    QueryValidationStoreWriter& operator=(const QueryValidationStoreWriter&)=delete;
    void AppendExtraNameState(uint8_t state);
    void AppendCallstack(uint32_t callstack);
    void Commit();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryValidationStoreReader
{
public:
    QueryValidationStoreReader(std::filesystem::path root,std::string sourceFingerprint,
        analysis::PagedRecordOptions options={});
    ~QueryValidationStoreReader();
    QueryValidationStoreReader(const QueryValidationStoreReader&)=delete;
    QueryValidationStoreReader& operator=(const QueryValidationStoreReader&)=delete;
    uint64_t ExtraCount() const;
    uint8_t ExtraNameState(uint64_t ordinal) const;
    void VisitUniqueCallstacks(size_t batchSize,
        const std::function<void(const std::vector<uint32_t>&)>& visitor) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}

#endif
