#include "TracyAnalysisCacheSort.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace tracy::analysis
{
struct AnalysisCacheSortedWriter::Impl
{
    std::filesystem::path output, work;
    std::string identity, kind;
    AnalysisCacheTableOptions options;
    AnalysisWorkspaceReservation bufferMemory,metadataMemory;
    uint64_t bufferBudget, buffered = 0, records = 0, sequence = 0;
    std::vector<AnalysisCacheRecord> buffer;
    std::vector<std::filesystem::path> runs;
    bool failed = false, committed = false;
    Impl(std::filesystem::path output, std::string identity, std::string kind,
        AnalysisCacheTableOptions options, uint64_t bytes)
        : output(std::move(output)), identity(std::move(identity)), kind(std::move(kind)),
          options(std::move(options)), bufferMemory(this->options.workspace),
          metadataMemory(this->options.workspace),bufferBudget(bytes)
    {
        if(bytes < 1024 || bytes > 256*1024*1024 || this->output.filename().empty())
            throw std::runtime_error("analysis_cache_sort_budget");
        bufferMemory.Resize(bytes);
        metadataMemory.Resize(64*1024);
        work = this->output; work += ".sorting";
        if(std::filesystem::exists(this->output) || std::filesystem::exists(work))
            throw std::runtime_error("analysis_cache_already_exists");
        Check();
        std::filesystem::create_directories(work.parent_path());
        if(!std::filesystem::create_directory(work)) throw std::runtime_error("analysis_cache_already_exists");
    }
    void Check()
    {
        if(failed) throw std::runtime_error("analysis_cache_writer_failed");
        if(committed) throw std::runtime_error("analysis_cache_writer_closed");
        if(options.cancelled && options.cancelled()) throw std::runtime_error("analysis_cache_cancelled");
    }
    std::filesystem::path Next() { return work / ("run-"+std::to_string(sequence++)); }
    void ChargeRuns(size_t count)
    {
        // Cover both current and next merge-pass path vectors. Resize follows
        // retained paths, not the cumulative number of runs ever produced.
        const uint64_t perPath=4*(work.native().size()*sizeof(std::filesystem::path::value_type)+64)+256;
        if(count>(UINT64_MAX-64*1024)/perPath) throw std::runtime_error("analysis_workspace_budget");
        metadataMemory.Resize(64*1024+count*perPath);
    }
    void Flush()
    {
        if(buffer.empty()) return;
        Check();
        std::sort(buffer.begin(), buffer.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        const auto path = Next();
        AnalysisCacheTableWriter writer(path, identity, kind, options);
        for(const auto& value : buffer) writer.Append(value.key, value.payload);
        writer.Commit();
        ChargeRuns(runs.size()+1);
        runs.push_back(path); buffer.clear(); buffered = 0;
    }
    std::filesystem::path Merge(size_t first, size_t last)
    {
        struct Head { AnalysisCacheRecord record; size_t run; uint64_t ordinal; };
        const auto later = [](const Head& a, const Head& b) {
            return a.record.key != b.record.key ? a.record.key > b.record.key : a.run > b.run;
        };
        const auto destination = Next();
        AnalysisCacheTableWriter writer(destination, identity, kind, options);
        std::vector<std::unique_ptr<AnalysisCacheTableReader>> readers;
        std::vector<Head> heads;
        readers.reserve(last-first); heads.reserve(last-first);
        uint64_t expected = 0;
        for(size_t i = first; i < last; ++i)
        {
            readers.push_back(std::make_unique<AnalysisCacheTableReader>(runs[i], identity, kind, options));
            auto& reader = *readers.back();
            if(reader.RecordCount() > UINT64_MAX-expected) throw std::runtime_error("analysis_cache_sort_count_overflow");
            expected += reader.RecordCount();
            if(reader.RecordCount()) heads.push_back({reader.GetAt(0),readers.size()-1,0});
        }
        std::make_heap(heads.begin(), heads.end(), later);
        while(!heads.empty())
        {
            Check();
            std::pop_heap(heads.begin(), heads.end(), later);
            auto head = std::move(heads.back()); heads.pop_back();
            // Writer rejects equal keys across separate runs as well as
            // duplicates within a run. No record is silently coalesced.
            writer.Append(head.record.key, head.record.payload);
            auto& reader = *readers[head.run];
            if(++head.ordinal < reader.RecordCount())
            {
                head.record = reader.GetAt(head.ordinal);
                heads.push_back(std::move(head));
                std::push_heap(heads.begin(), heads.end(), later);
            }
        }
        if(writer.Commit().records != expected) throw std::runtime_error("analysis_cache_sort_count_mismatch");
        return destination;
    }
};
AnalysisCacheSortedWriter::AnalysisCacheSortedWriter(std::filesystem::path output,
    std::string identity, std::string kind, AnalysisCacheTableOptions options, uint64_t bytes)
    : m_impl(std::make_unique<Impl>(std::move(output),std::move(identity),std::move(kind),std::move(options),bytes)) {}
AnalysisCacheSortedWriter::~AnalysisCacheSortedWriter() = default;
void AnalysisCacheSortedWriter::Append(std::string_view key,std::string_view value)
{
    auto& state = *m_impl;
    try
    {
        state.Check();
        // Half the budget is headroom for vector capacity / allocator metadata.
        // The process-level guard remains authoritative for total Query memory.
        const uint64_t cost = uint64_t(key.size())+value.size()+sizeof(AnalysisCacheRecord)+64;
        if(cost > state.bufferBudget/2) throw std::runtime_error("analysis_cache_sort_record_budget");
        if(state.buffered+cost > state.bufferBudget/2) state.Flush();
        state.buffer.push_back({std::string(key),std::string(value)});
        state.buffered += cost; ++state.records;
    }
    catch(...) { state.failed = true; throw; }
}
AnalysisCacheTableDescriptor AnalysisCacheSortedWriter::Commit()
{
    auto& state = *m_impl;
    try
    {
        state.Check(); state.Flush();
        if(state.runs.empty())
        {
            const auto empty = state.Next();
            AnalysisCacheTableWriter writer(empty,state.identity,state.kind,state.options);
            writer.Commit(); state.ChargeRuns(1); state.runs.push_back(empty);
        }
        constexpr size_t FanIn = 8;
        while(state.runs.size() > 1)
        {
            std::vector<std::filesystem::path> next;
            for(size_t first = 0; first < state.runs.size(); first += FanIn)
            {
                state.Check();
                const auto last = std::min(first+FanIn,state.runs.size());
                next.push_back(last-first == 1 ? state.runs[first] : state.Merge(first,last));
            }
            state.runs = std::move(next);
            state.ChargeRuns(state.runs.size());
        }
        AnalysisCacheTableDescriptor description;
        {
            AnalysisCacheTableReader reader(state.runs.front(),state.identity,state.kind,state.options);
            description = reader.Descriptor();
        }
        if(description.records != state.records) throw std::runtime_error("analysis_cache_sort_count_mismatch");
        state.Check();
        if(std::filesystem::exists(state.output)) throw std::runtime_error("analysis_cache_already_exists");
        std::filesystem::rename(state.runs.front(),state.output);
        state.committed = true;
        // Own unpublished merge intermediates only; a cleanup failure cannot
        // invalidate or delete the now-published complete output.
        std::error_code ignored;
        AnalysisDiskRemove(state.work,state.options.disk,ignored,true);
        return description;
    }
    catch(...) { state.failed = true; throw; }
}
}
