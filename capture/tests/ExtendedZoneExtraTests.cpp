#include "../../server/TracyWorker.hpp"
#include "../src/OfflineConversionStorage.hpp"
#include <cstdio>
static void Check(bool x){if(!x)throw std::runtime_error("wide zone extra invariant");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;using namespace tracy;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
 storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
 storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
 {conversion::OfflineConversionStorage store(root/"store",std::string(64,'a'),resources);
 ZoneExtra value{};value.callstack.SetVal(0x123456);value.text=StringIdx(16777215);value.name=StringIdx(4294967294ull);value.color.SetVal(0x654321);
 auto id=store.AppendZoneExtra(&value);ZoneExtra restored{};store.ReadZoneExtra(id,&restored);Check(!std::memcmp(&value,&restored,14));
 value.text=StringIdx(0);store.PutZoneExtra(id,&value);store.Seal();uint64_t seen=0;
 store.VisitZoneExtras([&](const void* data,uint64_t n){auto* p=static_cast<const uint8_t*>(data);for(uint64_t i=0;i<n;++i){if(seen==1)Check(!std::memcmp(&value,p+i*14,14));++seen;}});Check(seen==2);
 }Check(resources.CleanupOwned());std::puts("wide ZoneExtra mutable storage and serialization passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
