#include <cstdio>
#if __has_include("../../storage/TracyOrderedForest.hpp")
#include "../../storage/TracyOrderedForest.hpp"
#include <chrono>
#include <vector>
static void Check(bool value){if(!value)throw std::runtime_error("ordered forest invariant");}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;
 tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 uint64_t systemSamples=0;tracy::storage::ConversionResources budget(limits,[&]{++systemSamples;return sample;},[]{return 1ull<<40;});
 const auto beforeForest=systemSamples;
 tracy::storage::OrderedForest forest(root/"cpu",8,budget,64*1024);
 Check(systemSamples-beforeForest==2);
 const auto a=forest.CreateRoot(),b=forest.CreateRoot();uint64_t value=10;
 const auto parent=forest.Append(a,&value);value=20;const auto child=forest.Append(parent,&value);value=30;forest.Append(a,&value);
 value=99;forest.Put(child,&value);value=0;forest.Read(child,&value);Check(value==99);
 // Interleaved threads and enough records/depth to require eviction; traversal must not recurse.
 uint64_t deep=b;for(uint64_t i=0;i<10000;++i){value=i;deep=forest.Append(deep,&value);}
 forest.Seal();Check(forest.Count()==10003&&forest.RootCount()==2&&forest.BranchCount()==10002&&forest.Children(a)==2);
 std::vector<uint64_t> events;forest.Visit(a,[&](uint64_t,const void* p,uint32_t n,bool begin){uint64_t v;std::memcpy(&v,p,8);events.push_back(begin?v:v+1000);if(v==10)Check(n==1);});
 Check(events==std::vector<uint64_t>({10,99,1099,1010,30,1030}));
 uint64_t entered=0,exited=0;forest.Visit(b,[&](uint64_t,const void* p,uint32_t n,bool begin){uint64_t v;std::memcpy(&v,p,8);if(begin){Check(v==entered++);Check(n==(v==9999?0:1));}else Check(v==9999-exited++);});Check(entered==10000&&exited==10000);
 bool rejected=false;try{forest.Put(child,&value);}catch(const std::exception&){rejected=true;}Check(rejected);
 puts("ordered forest interleaving, updates, eviction and constant-stack traversal passed");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing ordered forest storage");return 1;}
#endif
