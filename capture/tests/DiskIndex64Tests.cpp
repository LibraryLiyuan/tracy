#include <cstdio>
// P2 bounded Pass index regression.
#if __has_include("../../storage/TracyDiskIndex64.hpp")
#include "../../storage/TracyDiskIndex64.hpp"
#include <stdexcept>
#include <chrono>
#include <source_location>
static void Check(bool value,std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("disk index64 invariant at "+std::to_string(at.line()));}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=32ull<<20;limits.diskReserveBytes=0;
 tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources resources(limits,[&]{return sample;},[]{return 1ull<<40;});
 const auto controlBytes=resources.Workspace()->Snapshot().currentBytes;
 tracy::storage::DiskIndex64 index(root/"pass.index",std::string(64,'e'),resources,2*(16384+256));
 Check(!index.Find(0));
 for(uint64_t i=0;i<5000;++i)index.Upsert((i<<40)+3,i*7);
 for(uint64_t i=5000;i>0;--i)index.Upsert(((i-1)<<40)+3,5000-i);
 index.Upsert(UINT64_MAX,UINT64_MAX);index.Upsert(0,42);index.Seal();
 const auto statistics=index.Statistics();
 Check(statistics.upserts==10002&&statistics.evictions>0);
 Check(statistics.evictionCandidates==statistics.evictions); // O(1) LRU victim selection, never scan every cached page.
 Check(statistics.pageCopies==0); // Lookup/traversal must not copy a full 16 KiB page.
 Check(index.Count()==5002&&index.Find(0)==42&&index.Find(UINT64_MAX)==UINT64_MAX);
 for(uint64_t i=0;i<5000;++i)Check(index.Find((i<<40)+3)==4999-i);
 std::vector<std::pair<uint64_t,uint64_t>> range;index.VisitRange((100ull<<40)+3,(109ull<<40)+3,[&](uint64_t key,uint64_t value){range.emplace_back(key,value);});Check(range.size()==10);for(uint64_t i=0;i<10;++i)Check(range[i].first==((i+100)<<40)+3&&range[i].second==4899-i);
 Check(!index.Find(4));
 bool failed=false;try{index.Upsert(1,1);}catch(const std::exception&){failed=true;}Check(failed);
 Check(resources.Workspace()->Snapshot().peakBytes<controlBytes+(2ull<<20));
 index.ClearCache();{std::fstream file(tracy::analysis::AnalysisIoPath(root/"pass.index"),std::ios::in|std::ios::out|std::ios::binary);file.seekp(4096);file.put(0);file.flush();Check(bool(file));}
 failed=false;try{index.Find(3);}catch(const std::exception&){failed=true;}Check(failed);
 auto tight=limits;tight.streamBytes=1;tracy::storage::ConversionResources denied(tight,[&]{return sample;},[]{return 1ull<<40;});
 failed=false;try{tracy::storage::DiskIndex64 noSpace(root/"denied.index",std::string(64,'e'),denied);}catch(const std::exception&){failed=true;}Check(failed);
 resources.Cancel();failed=false;try{index.Find(0);}catch(const std::exception&){failed=true;}Check(failed);
 puts("64-bit disk index sparse keys, split, eviction and last-write values passed");return 0;
 }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){puts("missing 64-bit bounded disk index");return 1;}
#endif
