#include <cstdio>
#if __has_include("../../storage/TracyMappedHashIndex64.hpp")
#include "../../storage/TracyMappedHashIndex64.hpp"
#include <chrono>
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto root=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(root);
 tracy::storage::ResourceLimits limits;limits.streamBytes=64ull<<20;limits.diskReserveBytes=0;tracy::storage::ResourceSample sample;sample.systemCommitLimit=64ull<<30;
 tracy::storage::ConversionResources budget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::MappedHashIndex64 index(root/"hash.index",budget,64);
 Check(index.Upsert(0,7)&&index.Upsert(UINT64_MAX,9),"mapped_hash_boundary_insert");
 for(uint64_t i=0;i<5000;++i)Check(index.Upsert(i*0x9E3779B97F4A7C15ull,i+100)==(i!=0),"mapped_hash_insert");
 Check(!index.Upsert(0,11)&&index.Find(0)==11&&index.Find(UINT64_MAX)==9,"mapped_hash_update");
 for(uint64_t i=1;i<5000;++i)Check(index.Find(i*0x9E3779B97F4A7C15ull)==i+100,"mapped_hash_lookup");
 Check(!index.Find(123456789),"mapped_hash_missing");const auto count=index.Count();Check(count==5001&&index.Capacity()>=count*10/7,"mapped_hash_count");
 uint64_t digestProgress=0,digestTotal=0,flushProgress=0;
 const auto seal=index.Seal([&](tracy::storage::MappedHashIndex64::SealPhase phase,uint64_t completed,uint64_t total){
     if(phase==tracy::storage::MappedHashIndex64::SealPhase::Digest){digestProgress=completed;digestTotal=total;}
     else flushProgress=completed;
 });
 Check(digestProgress==digestTotal&&digestTotal==index.FileBytes()&&flushProgress==1,"mapped_hash_seal_progress");
 Check(seal.digestBytes==digestTotal&&seal.digestNanoseconds>0&&seal.flushNanoseconds>0,"mapped_hash_seal_stats");
 index.Validate();
 bool sealed=false;try{index.Upsert(1,2);}catch(const std::exception&){sealed=true;}Check(sealed,"mapped_hash_sealed");
 Check(budget.DiskCurrent()>0&&index.CloseAndRemove()&&budget.DiskCurrent()==0,"mapped_hash_early_cleanup");
 bool closed=false;try{index.Find(0);}catch(const std::exception&){closed=true;}Check(closed,"mapped_hash_closed");
 tracy::storage::ConversionResources cancelledBudget(limits,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::MappedHashIndex64 cancelledIndex(root/"cancelled.index",cancelledBudget,64);cancelledIndex.Upsert(1,2);bool cancelled=false;
 try{cancelledIndex.Seal([&](auto phase,uint64_t completed,uint64_t){if(phase==tracy::storage::MappedHashIndex64::SealPhase::Digest&&completed==0)cancelledBudget.Cancel();});}catch(const std::exception& e){cancelled=std::string(e.what())=="conversion_cancelled";}
 Check(cancelled&&cancelledIndex.CloseAndRemove()&&cancelledBudget.DiskCurrent()==0,"mapped_hash_digest_cancel");
 tracy::storage::ResourceLimits tiny;tiny.streamBytes=8192;tiny.diskReserveBytes=0;tiny.absoluteDiskLimit=32768;tracy::storage::ConversionResources tight(tiny,[&]{return sample;},[]{return 1ull<<40;});
 tracy::storage::MappedHashIndex64 limited(root/"limited.index",tight,64);bool rejected=false;try{for(uint64_t i=0;i<5000;++i)limited.Upsert(i,i);}catch(const std::exception&){rejected=true;}Check(rejected&&limited.Count()<5000,"mapped_hash_budget");
 std::puts("mapped hash boundary, resize, update, validation and budget tests passed");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#else
int main(){std::puts("missing mapped hash index");return 1;}
#endif
