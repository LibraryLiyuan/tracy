#include "TracyQueryCallstackStore.hpp"

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace tracy;
using namespace tracy::analysis;
using namespace tracy::query;

int main()
{
    const auto root=std::filesystem::temp_directory_path()/
        ("query-callstack-store-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const std::string fingerprint(64,'c');
    auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{root});
    auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,64*1024*1024,{}});
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(16*1024*1024,8*1024*1024);
    PagedRecordOptions options; options.blockBytes=4096; options.disk=disk; options.workspace=workspace;
    try
    {
        QueryCallstackStoreWriter writer(root/"store",fingerprint,options);
        writer.Begin(3);
        const std::vector<std::vector<CallstackFrameId>> values={
            {{0x1000,0,0},{0x2000,1,0}},
            std::vector<CallstackFrameId>(9000,CallstackFrameId{0x3000,0,0}),
            {{0x4000,0,1}}
        };
        for(uint32_t id=1;id<=values.size();++id)
        {
            const auto& frames=values[id-1];writer.CallstackBegin(id,uint32_t(frames.size()));
            for(uint32_t offset=0;offset<frames.size();offset+=997)
            {
                const auto count=std::min<uint32_t>(997,uint32_t(frames.size()-offset));
                writer.CallstackFrames(id,offset,frames.data()+offset,count);
            }
            writer.CallstackEnd(id);
        }
        writer.Commit();
        QueryCallstackStoreReader reader(root/"store",fingerprint,options);
        assert(reader.Count()==3);
        const auto first=reader.Frames(1);assert(first.size()==2&&first[0].data==0x1000&&first[1].sel==1);
        const auto large=reader.Frames(2);assert(large.size()==9000&&large[8999].data==0x3000);
        const auto final=reader.Frames(3);assert(final.size()==1&&final[0].custom==1);
        assert(first.data()==reader.Frames(1).data());
        bool missing=false;try{(void)reader.Frames(0);}catch(const std::out_of_range&){missing=true;}assert(missing);
        missing=false;try{(void)reader.Frames(4);}catch(const std::out_of_range&){missing=true;}assert(missing);
        uint64_t visited=0;reader.VisitFrames(2,1024,[&](const CallstackFrameId* frames,uint32_t count){assert(frames&&count);visited+=count;});assert(visited==9000);
        reader.ValidateAll();
    }
    catch(const std::exception& error)
    {
        std::cerr<<error.what()<<'\n';std::filesystem::remove_all(root);return 1;
    }
    std::filesystem::remove_all(root);std::cout<<"query callstack store tests passed\n";return 0;
}
