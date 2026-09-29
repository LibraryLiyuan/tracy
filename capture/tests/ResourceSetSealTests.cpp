#include "../../storage/TracyResourceSetStore.hpp"
#include <fstream>
#include <cstdio>
int main(int argc,char** argv){
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1]);std::filesystem::create_directories(root);
 for(int index=0;index<2;++index){
  tracy::storage::ResourceLimits limits;limits.streamBytes=16ull<<20;limits.diskReserveBytes=0;
  tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
  tracy::storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
  auto path=root/std::to_string(index);tracy::storage::ResourceSetStore store(path,std::string(64,'e'),resources);
  std::vector<tracy::storage::ResourceSetMember> data(20000);for(size_t i=0;i<data.size();++i)data[i]={i+1,1};
  if(!store.Define(71,data))return 3;store.ClearCaches();
  {std::fstream file(path/(index?"index.bin":"members/data.bin"),std::ios::binary|std::ios::in|std::ios::out);file.seekp(index?16384:0);char bad=0;file.write(&bad,1);}
  bool refused=false;try{store.Seal();}catch(...){refused=true;}
  if(!refused){std::puts("FAIL: seal accepted corrupted storage");return 1;}
 }
 std::puts("sealed ResourceSet integrity passed");
}
