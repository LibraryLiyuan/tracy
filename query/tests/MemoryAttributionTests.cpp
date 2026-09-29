#include <cstdio>
#if __has_include("../../server/TracyMemoryAttribution.hpp") && __has_include("../../capture/src/MemoryPressure.hpp")
#include "../../server/TracyMemoryAttribution.hpp"
#include "../../capture/src/MemoryPressure.hpp"
#include "../../server/TracyVector.hpp"
#include <stdexcept>
namespace tracy { std::atomic<int64_t> memUsage {0}; }
static void Check(bool ok) { if(!ok) throw std::runtime_error("memory attribution mismatch"); }
int main()
{
    tracy::MemoryAttribution report;
    report.Add("records",3,24,80);
    Check(report.rows[0].count==3 && report.rows[0].logicalBytes==24);
    Check(report.knownOwnedBytes==80);
    tracy::Slab<512> slab;
    tracy::Vector<uint64_t> borrowed;
    borrowed.reserve_exact(3,slab);
    Check(borrowed.OwnedCapacityBytes()==0);
    Check(slab.AllocatedBytes()==512);
    tracy::Vector<uint64_t> owned;
    owned.reserve_and_use(3);
    Check(owned.OwnedCapacityBytes()==32);
    report.Add("slab",1,0,slab.AllocatedBytes());
    report.Add("borrowed",3,24,borrowed.OwnedCapacityBytes());
    report.Add("owned",3,24,owned.OwnedCapacityBytes());
    Check(report.knownOwnedBytes==624); // 80 + 512 + 0 + 32, no duplicate slab charge.
    constexpr uint64_t G=1ull<<30;
    using tracy::conversion::MemoryPressureFlags;
    Check(MemoryPressureFlags(40*G,64*G,12*G,80)==1);
    Check(MemoryPressureFlags(20*G,64*G,7*G,86)==6);
    Check(MemoryPressureFlags(20*G,64*G,12*G,80)==0);
    Check(MemoryPressureFlags(20*G,0,0,0)==0);
    std::puts("memory attribution and pressure cases passed");
}
#else
int main() { std::puts("FAIL: owned-versus-borrowed capacity and pressure attribution are unavailable"); return 1; }
#endif
