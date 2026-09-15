#include "../../server/TracyVector.hpp"
#include "../../analysis/TracyProcessCommitLimit.hpp"
#include <cstdio>
namespace tracy {std::atomic<int64_t> memUsage{0};}
int main(){
 tracy::analysis::ProcessCommitLimit limit(128ull<<20);
 tracy::Vector<uint64_t> values;values.push_back(123);
 const auto capacity=values.OwnedCapacityBytes();bool refused=false;
 try{values.reserve(1ull<<28);}catch(const std::bad_alloc&){refused=true;}
 if(!refused||values.size()!=1||values[0]!=123||values.OwnedCapacityBytes()!=capacity||tracy::memUsage!=int64_t(capacity))return 1;
 tracy::Slab<64> slab;refused=false;
 try{slab.AllocBig(1ull<<30);}catch(const std::bad_alloc&){refused=true;}
 if(!refused||slab.AllocatedBytes()!=64||tracy::memUsage!=int64_t(capacity+64))return 1;
 std::puts("Vector allocation rejection preserves state");
}
