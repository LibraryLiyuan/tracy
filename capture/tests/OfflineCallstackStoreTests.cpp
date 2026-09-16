#include <cstdio>
#include "../../storage/TracyOfflineCallstackStore.hpp"
#include <array>
#include <chrono>
#include <vector>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::OfflineCallstackStore store(root/"collisions",std::string(64,'d'),budget,[](const void*,size_t){return 5;});
 const std::array<uint64_t,3> a{1,2,3},b{1,2,4};const auto first=store.Intern(a.data(),a.size()),second=store.Intern(b.data(),b.size()),again=store.Intern(a.data(),a.size());
 Check(first.id==1&&second.id==2&&again.id==1&&again.frames==first.frames,"offline_callstack_collision_dedup");Check(std::memcmp(store.Get(2).frames,b.data(),sizeof(b))==0,"offline_callstack_get");
 std::array<uint64_t,64> large{};for(uint64_t i=0;i<large.size();++i)large[i]=i*17;const auto third=store.Intern(large.data(),large.size());Check(third.count==64&&std::memcmp(third.frames,large.data(),sizeof(large))==0,"offline_callstack_large");store.Seal();
 std::vector<uint32_t> ids;store.Visit([&](uint32_t id,const uint64_t* frames,uint32_t count){ids.push_back(id);if(id==1)Check(count==a.size()&&std::memcmp(frames,a.data(),sizeof(a))==0,"offline_callstack_visit_a");else if(id==2)Check(count==b.size()&&std::memcmp(frames,b.data(),sizeof(b))==0,"offline_callstack_visit_b");else Check(id==3&&count==large.size()&&std::memcmp(frames,large.data(),sizeof(large))==0,"offline_callstack_visit_large");});Check(ids==std::vector<uint32_t>({1,2,3})&&store.Hash().size()==64,"offline_callstack_order");
 tracy::storage::OfflineCallstackStore many(root/"many",std::string(64,'e'),budget);std::vector<const uint64_t*> stable;for(uint32_t i=0;i<5000;++i){const uint64_t frames[2]={i,uint64_t(i)+1};const auto value=many.Intern(frames,2);Check(value.id==i+1,"offline_callstack_id");if(i<10)stable.push_back(value.frames);}for(uint32_t i=0;i<10;++i)Check(stable[i][0]==i&&stable[i][1]==i+1,"offline_callstack_stable");many.Seal();
 std::vector<uint64_t> oversized(50000,7);tracy::storage::ResourceLimits tiny;tiny.streamBytes=8192;tiny.diskReserveBytes=0;tiny.absoluteDiskLimit=65536;tracy::storage::ConversionResources tight(tiny,[&]{return sample;},[]{return 1ull<<40;});bool rejected=false;try{tracy::storage::OfflineCallstackStore limited(root/"limited",std::string(64,'f'),tight);limited.Intern(oversized.data(),uint32_t(oversized.size()));}catch(const std::exception&){rejected=true;}Check(rejected,"offline_callstack_budget");
 std::puts("offline callstack dedup, collision, stable body, order and budget tests passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
