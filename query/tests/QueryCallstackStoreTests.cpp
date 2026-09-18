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

        QueryCallstackFrameStoreWriter frameWriter(root/"frames",fingerprint,options,2);
        frameWriter.Begin(2);
        CallstackFrameId firstId{};firstId.data=0x1234;
        CallstackFrameId secondId{};secondId.data=0x8000000000005678ull;
        std::vector<CallstackFrame> firstFrames(2);firstFrames[0].name=StringIdx(1);firstFrames[0].file=StringIdx(2);firstFrames[0].line=10;firstFrames[0].symAddr=0x1100;firstFrames[1].symAddr=0x1200;
        std::vector<CallstackFrame> secondFrames(255);for(size_t i=0;i<secondFrames.size();++i)secondFrames[i].symAddr=0x2000+i;
        frameWriter.FrameBegin(0,firstId,uint8_t(firstFrames.size()),StringIdx(3));frameWriter.FrameRecords(0,0,firstFrames.data(),uint32_t(firstFrames.size()));frameWriter.FrameEnd(0);
        frameWriter.FrameBegin(1,secondId,uint8_t(secondFrames.size()),StringIdx{});frameWriter.FrameRecords(1,0,secondFrames.data(),64);frameWriter.FrameRecords(1,64,secondFrames.data()+64,uint32_t(secondFrames.size()-64));frameWriter.FrameEnd(1);frameWriter.Commit();
        QueryCallstackFrameStoreReader frames(root/"frames",fingerprint,options);assert(frames.Count()==2);
        const auto firstDefinition=frames.Find(firstId);assert(firstDefinition&&firstDefinition->imageName.Active()&&firstDefinition->imageName.Idx()==3&&firstDefinition->frames.size()==2&&firstDefinition->frames[0].line==10);
        const auto secondDefinition=frames.Find(secondId);assert(secondDefinition&&secondDefinition->frames.size()==255&&secondDefinition->frames[254].symAddr==0x20fe);
        CallstackFrameId missingId{};missingId.data=0x9999;assert(!frames.Find(missingId));frames.ValidateAll();
    }
    catch(const std::exception& error)
    {
        std::cerr<<error.what()<<'\n';std::filesystem::remove_all(root);return 1;
    }
    std::filesystem::remove_all(root);std::cout<<"query callstack store tests passed\n";return 0;
}
