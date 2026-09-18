#include "TracyQueryValidationStore.hpp"
#include "TracyQueryIndex.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tracy::analysis;
using namespace tracy::query;

namespace
{
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
}

int main()
{
    static_assert(QueryIndexZoneExtraNameResolved(QueryIndexZoneExtraNameActive|QueryIndexZoneExtraNameResolvedFlag));
    static_assert(!QueryIndexZoneExtraNameResolved(QueryIndexZoneExtraNameActive));
    static_assert(!QueryIndexZoneExtraNameResolved(0));
    try
    {
        const auto root=std::filesystem::current_path()/
            ("query-validation-store-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto workspace=std::make_shared<AnalysisWorkspaceBudget>(32*1024*1024,16*1024*1024);
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{root});
        AnalysisDiskActivity activity(usage);
        auto disk=std::make_shared<AnalysisDiskBudget>(); disk->usage=usage; disk->maximumBytes=64*1024*1024;
        PagedRecordOptions options; options.workspace=workspace; options.disk=disk; options.blockBytes=4096;
        {
            QueryValidationStoreWriter writer(root,std::string(64,'a'),options,2);
            writer.AppendExtraNameState(2); writer.AppendExtraNameState(1); writer.AppendExtraNameState(0);
            for(const auto value:{9u,3u,9u,0u,5u}) writer.AppendCallstack(value);
            writer.Commit();
        }
        {
            QueryValidationStoreReader reader(root,std::string(64,'a'),options);
            Require(reader.ExtraCount()==3 && reader.ExtraNameState(0)==2 &&
                reader.ExtraNameState(1)==1 && reader.ExtraNameState(2)==0,
                "paged extra-name state changed");
            std::vector<uint32_t> callstacks;
            reader.VisitUniqueCallstacks(2,[&](const std::vector<uint32_t>& batch) {
                Require(batch.size()<=2,"callstack validation batch exceeded its bound");
                callstacks.insert(callstacks.end(),batch.begin(),batch.end());
            });
            Require(callstacks==std::vector<uint32_t>({3,5,9}),"external callstack sort did not deduplicate deterministically");
        }
        Require(workspace->Snapshot().peakBytes<=32*1024*1024,"validation store exceeded shared workspace");
        std::error_code ignored; AnalysisDiskRemove(root,disk,ignored,true);
        Require(!ignored && usage->Current()==0,"validation store cleanup did not release disk accounting");
        std::cout<<"query validation store tests passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
