#include "TracyQueryValidationStore.hpp"

#include "TracyAnalysisExternalSort.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tracy::query
{
namespace
{
std::string Identity(std::string_view source,std::string_view domain)
{
    if(source.size()!=64) throw std::runtime_error("query_validation_identity");
    analysis::Sha256Builder hash; hash.Update(source.data(),source.size()); hash.Update(domain.data(),domain.size());
    return hash.FinalHex();
}
}

struct QueryValidationStoreWriter::Impl
{
    Impl(std::filesystem::path output,std::string identity,analysis::PagedRecordOptions storage,uint64_t maximum)
        :root(std::move(output)),source(std::move(identity)),options(std::move(storage)),maximumBuffered(std::max<uint64_t>(1,maximum)),
        extraNames(root/"extra-name-state",Identity(source,"query-validation-extra-names-v1"),1,options),
        bufferWorkspace(options.workspace,maximumBuffered*sizeof(analysis::AnalysisUInt64Pair))
    {
        if(std::filesystem::exists(root/"callstack-refs.bin") || std::filesystem::exists(root/"callstack-sorted.bin"))
            throw std::runtime_error("query_validation_exists");
        callstacks.open(root/"callstack-refs.bin",std::ios::binary|std::ios::trunc);
        if(!callstacks) throw std::runtime_error("query_validation_callstack_open");
        buffer.reserve(size_t(std::min<uint64_t>(maximumBuffered,std::numeric_limits<size_t>::max())));
    }
    void Flush()
    {
        if(buffer.empty()) return;
        const auto bytes=uint64_t(buffer.size())*sizeof(analysis::AnalysisUInt64Pair);
        analysis::AnalysisDiskGrow(options.disk,bytes);
        callstacks.write(reinterpret_cast<const char*>(buffer.data()),std::streamsize(bytes));
        if(!callstacks) throw std::runtime_error("query_validation_callstack_write");
        buffer.clear();
    }
    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    uint64_t maximumBuffered=0,count=0;
    analysis::PagedRecordWriter extraNames;
    analysis::AnalysisWorkspaceReservation bufferWorkspace;
    std::ofstream callstacks;
    std::vector<analysis::AnalysisUInt64Pair> buffer;
    bool committed=false;
};

QueryValidationStoreWriter::QueryValidationStoreWriter(std::filesystem::path root,std::string sourceFingerprint,
    analysis::PagedRecordOptions options,uint64_t maximumBufferedCallstacks)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(sourceFingerprint),std::move(options),maximumBufferedCallstacks))
{}
QueryValidationStoreWriter::~QueryValidationStoreWriter()=default;
void QueryValidationStoreWriter::AppendExtraNameState(uint8_t state)
{
    if(m_impl->committed || state>2) throw std::runtime_error("query_validation_extra_state");
    m_impl->extraNames.Append(&state,1);
}
void QueryValidationStoreWriter::AppendCallstack(uint32_t callstack)
{
    if(m_impl->committed) throw std::runtime_error("query_validation_committed");
    if(!callstack) return;
    m_impl->buffer.push_back({callstack,0}); ++m_impl->count;
    if(m_impl->buffer.size()==m_impl->maximumBuffered) m_impl->Flush();
}
void QueryValidationStoreWriter::Commit()
{
    auto& value=*m_impl;
    if(value.committed) throw std::runtime_error("query_validation_committed");
    value.extraNames.Commit(); value.Flush(); value.callstacks.flush();
    if(!value.callstacks) throw std::runtime_error("query_validation_callstack_flush");
    value.callstacks.close();
    std::string error;
    if(!analysis::SortAnalysisUInt64Pairs(value.root/"callstack-refs.bin",value.root/"callstack-sorted.bin",
        value.root,"callstack",value.count,value.maximumBuffered,error,value.options.disk,value.options.workspace))
        throw std::runtime_error(error);
    std::error_code removeError;
    analysis::AnalysisDiskRemove(value.root/"callstack-refs.bin",value.options.disk,removeError);
    if(removeError) throw std::runtime_error("query_validation_callstack_cleanup:"+removeError.message());
    value.committed=true;
}

struct QueryValidationStoreReader::Impl
{
    Impl(std::filesystem::path input,std::string identity,analysis::PagedRecordOptions storage)
        :root(std::move(input)),source(std::move(identity)),options(std::move(storage)),
        extraNames(root/"extra-name-state",Identity(source,"query-validation-extra-names-v1"),1,options)
    {
        const auto bytes=std::filesystem::file_size(root/"callstack-sorted.bin");
        if(bytes%sizeof(analysis::AnalysisUInt64Pair)) throw std::runtime_error("query_validation_callstack_size");
        callstackCount=bytes/sizeof(analysis::AnalysisUInt64Pair);
    }
    std::filesystem::path root;
    std::string source;
    analysis::PagedRecordOptions options;
    mutable analysis::PagedRecordReader extraNames;
    uint64_t callstackCount=0;
};

QueryValidationStoreReader::QueryValidationStoreReader(std::filesystem::path root,std::string sourceFingerprint,
    analysis::PagedRecordOptions options)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(sourceFingerprint),std::move(options)))
{}
QueryValidationStoreReader::~QueryValidationStoreReader()=default;
uint64_t QueryValidationStoreReader::ExtraCount() const { return m_impl->extraNames.Count(); }
uint8_t QueryValidationStoreReader::ExtraNameState(uint64_t ordinal) const
{
    uint8_t value=0; m_impl->extraNames.Read(ordinal,&value,1); return value;
}
void QueryValidationStoreReader::VisitUniqueCallstacks(size_t batchSize,
    const std::function<void(const std::vector<uint32_t>&)>& visitor) const
{
    if(!batchSize || !visitor) throw std::runtime_error("query_validation_batch");
    analysis::AnalysisWorkspaceReservation batchWorkspace(m_impl->options.workspace,
        uint64_t(batchSize)*sizeof(uint32_t)+65536);
    std::ifstream input(m_impl->root/"callstack-sorted.bin",std::ios::binary);
    if(!input) throw std::runtime_error("query_validation_callstack_read_open");
    std::vector<uint32_t> batch; batch.reserve(batchSize);
    uint64_t seen=0,previous=0; bool first=true;
    for(uint64_t index=0;index<m_impl->callstackCount;++index)
    {
        if(m_impl->options.cancelled && m_impl->options.cancelled()) throw std::runtime_error("query_validation_cancelled");
        analysis::AnalysisUInt64Pair row;
        input.read(reinterpret_cast<char*>(&row),sizeof(row));
        if(!input || row.key>std::numeric_limits<uint32_t>::max() || (!first && row.key<previous))
            throw std::runtime_error("query_validation_callstack_order");
        if(first || row.key!=previous)
        {
            batch.push_back(uint32_t(row.key));
            if(batch.size()==batchSize) { visitor(batch); batch.clear(); }
        }
        previous=row.key; first=false; ++seen;
    }
    if(seen!=m_impl->callstackCount) throw std::runtime_error("query_validation_callstack_count");
    if(!batch.empty()) visitor(batch);
}
}
