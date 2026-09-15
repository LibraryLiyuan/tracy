#include <cstdio>
#if __has_include("../../server/TracyAllocationDiagnostics.hpp")
#include "../../server/TracyAllocationDiagnostics.hpp"
#include "../../server/TracyVector.hpp"
#include "../../analysis/TracyProcessCommitLimit.hpp"
#include <vector>
#include <cstring>
namespace tracy {std::atomic<int64_t> memUsage{0};}
struct ZoneExtraProbe {char bytes[12];};
static void Check(bool v){if(!v)throw std::runtime_error("allocation diagnostic invariant");}
static uint64_t PrivateBytes(){PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);Check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p))!=0);return p.PrivateUsage;}
int main(int argc,char** argv){
 const bool scaled=argc>1&&std::strcmp(argv[1],"scaled")==0;
 tracy::analysis::ProcessCommitLimit limit((scaled?80ull:128ull)<<20);
 if(scaled){
  tracy::Vector<ZoneExtraProbe> values;values.reserve(1<<19);values.set_size(1<<19);
  const auto initial=PrivateBytes();Check(initial<(65ull<<20));
  std::vector<char> ballast((70ull<<20)-initial);
  tracy::AllocationDiagnostics diagnostics;
  bool refused=false;try{values.reserve(1<<20);}catch(const std::bad_alloc&){refused=true;}
  const auto d=tracy::GetAllocationFailure();Check(refused&&d.ready&&d.requestedBytes==(12ull<<20)&&d.oldCapacity==(1<<19)&&d.elementBytes==12);
  Check(values.size()==(1<<19)&&values.OwnedCapacityBytes()==(6ull<<20));
  void* delta=std::malloc(6ull<<20);Check(delta!=nullptr);std::free(delta);
  std::printf("scaled peak reproduced: private=%llu old=%llu requested=%llu cap=%llu; net increment fits\n",d.privateBytes,uint64_t(d.oldCapacity)*d.elementBytes,d.requestedBytes,80ull<<20);
 }else{
  tracy::Vector<uint64_t> values;values.push_back(123);
  bool refused=false;try{values.reserve(1ull<<28);}catch(const std::bad_alloc&){refused=true;}
  Check(refused&&!tracy::GetAllocationFailure().ready);
  {tracy::AllocationDiagnostics diagnostics;
   refused=false;try{values.reserve(1ull<<28);}catch(const std::bad_alloc&){refused=true;}
   const auto d=tracy::GetAllocationFailure();Check(refused&&d.ready&&d.requestedBytes==(2ull<<30)&&d.oldSize==1&&d.oldCapacity==1&&d.elementBytes==8&&d.owner==uintptr_t(&values));
   Check(d.frameCount>0&&d.privateBytes>0&&d.threadId!=0&&std::strstr(d.site,"Vector")!=nullptr);
   refused=false;try{std::vector<char> other(1ull<<30);}catch(const std::bad_alloc&){refused=true;}
   Check(refused&&tracy::GetAllocationFailure().requestedBytes==d.requestedBytes);
  }
  Check(!tracy::AllocationDiagnosticsEnabled());
  {tracy::AllocationDiagnostics diagnostics;refused=false;try{std::vector<char> other(1ull<<30);}catch(const std::bad_alloc&){refused=true;}
   const auto d=tracy::GetAllocationFailure();Check(refused&&d.ready&&d.requestedBytes>=(1ull<<30)&&d.owner==0&&d.frameCount>0);
  }
  std::puts("allocation diagnostics: disabled, Vector context, first failure, CRT fallback passed");
 }
}
#else
int main(){std::puts("FAIL: allocation-point diagnosis unavailable");return 1;}
#endif
