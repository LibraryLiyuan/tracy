#include <cstdio>
// FZ3 verifies the real page-backed generation/kind directory.
#if __has_include("../../storage/TracyCatalogBatchDirectory.hpp")
#include "../../storage/TracyCatalogBatchDirectory.hpp"
#include <chrono>
#include <source_location>
#include <stdexcept>
#include <vector>
static void Check(bool value,std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("catalog batch directory invariant at "+std::to_string(at.line()));}
template<class F> static void Refuses(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=32ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::CatalogBatchDirectory directory(root/"directory",std::string(64,'c'),resources);
 directory.AddGeneration(99);directory.AddGeneration(3);directory.AddGeneration(UINT64_MAX);Refuses([&]{directory.AddGeneration(99);});
 directory.AddBatch(5,99,0);directory.AddBatch(1,3,7);directory.AddBatch(8,99,0);directory.AddBatch(2,3,7);directory.AddBatch(0,99,9);directory.AddBatch(UINT64_MAX,UINT64_MAX,8);
 Refuses([&]{directory.AddBatch(7,777,0);});Refuses([&]{directory.AddBatch(7,99,10);});
 directory.Seal();
 std::vector<uint64_t> seen;Check(directory.Visit(99,0,[&](uint64_t batch){seen.push_back(batch);})==2);Check((seen==std::vector<uint64_t>{5,8}));
 seen.clear();Check(directory.Visit(3,7,[&](uint64_t batch){seen.push_back(batch);})==2);Check((seen==std::vector<uint64_t>{1,2}));
 seen.clear();Check(directory.Visit(99,9,[&](uint64_t batch){seen.push_back(batch);})==1&&seen[0]==0);
 seen.clear();Check(directory.Visit(UINT64_MAX,8,[&](uint64_t batch){seen.push_back(batch);})==1&&seen[0]==UINT64_MAX);
 Check(directory.Visit(99,4,[&](uint64_t){Check(false);})==0);Check(directory.Visit(777,0,[&](uint64_t){Check(false);})==0);
 const auto stats=directory.Statistics();Check(stats.generations==3&&stats.batches==6&&stats.nonEmptyChains==4&&stats.visited==6);
 Refuses([&]{directory.AddBatch(9,99,0);});Check(directory.CloseAndRemove());Check(resources.DiskCurrent()==0);
 puts("Catalog batch directory preserves sparse generations, interleaving, order and cleanup");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing Catalog batch directory");return 1;}
#endif
