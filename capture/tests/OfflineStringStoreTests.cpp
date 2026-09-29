#include <cstdio>
#if __has_include("../../storage/TracyOfflineStringStore.hpp")
#include "../../storage/TracyOfflineStringStore.hpp"
#include <chrono>
#include <string>
#include <vector>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::OfflineStringStore collisions(root/"collisions",std::string(64,'a'),budget,[](const void*,size_t){return 7;});
 const auto empty=collisions.Intern("",0),alpha=collisions.Intern("alpha",5),beta=collisions.Intern("beta",4),again=collisions.Intern("alpha",5);
 Check(empty.id==0&&alpha.id==1&&beta.id==2&&again.id==alpha.id&&again.data==alpha.data,"offline_string_collision_dedup");
 Check(std::strcmp(collisions.Get(0),"")==0&&std::strcmp(collisions.Get(2),"beta")==0,"offline_string_get");
 std::string large(100000,'x');large[99999]='z';const auto huge=collisions.Intern(large.data(),large.size());Check(std::memcmp(huge.data,large.data(),large.size())==0&&huge.data[large.size()]=='\0',"offline_string_large");
 collisions.Seal();std::vector<std::string> values;collisions.Visit([&](uint32_t id,const char* value,uint64_t size){Check(id==values.size(),"offline_string_order");values.emplace_back(value,size);});
 Check(values==std::vector<std::string>({"","alpha","beta",large})&&collisions.Hash().size()==64,"offline_string_visit");
 tracy::storage::OfflineStringStore many(root/"many",std::string(64,'b'),budget);std::vector<const char*> stable;for(uint32_t i=0;i<5000;++i){const auto value="string-"+std::to_string(i);const auto row=many.Intern(value.data(),value.size());Check(row.id==i,"offline_string_id");if(i<10)stable.push_back(row.data);}for(uint32_t i=0;i<10;++i)Check(std::strcmp(stable[i],("string-"+std::to_string(i)).c_str())==0,"offline_string_stable_pointer");many.Seal();
 std::string oversized(300000,'q');tracy::storage::ResourceLimits tiny;tiny.streamBytes=8192;tiny.diskReserveBytes=0;tiny.absoluteDiskLimit=65536;tracy::storage::ConversionResources tight(tiny,[&]{return sample;},[]{return 1ull<<40;});bool rejected=false;try{tracy::storage::OfflineStringStore limited(root/"limited",std::string(64,'c'),tight);limited.Intern(oversized.data(),oversized.size());}catch(const std::exception&){rejected=true;}Check(rejected,"offline_string_budget");
 std::puts("offline string dedup, collision, stable mapping, long value, order and budget tests passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){std::puts("missing offline string store");return 1;}
#endif
