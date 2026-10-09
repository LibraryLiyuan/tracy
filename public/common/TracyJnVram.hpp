#ifndef __TRACYJNVRAM_HPP__
#define __TRACYJNVRAM_HPP__

#include <stdint.h>

// GPU memory by category. While a server is connected the JN engine samples
// its GPU memory registry every 250 ms and sends these plots: one per D3D12
// memory segment, ledger and resource category. A plot is sent when its value
// changes and once per connection, so each value holds until the next point.
//
// Segments: Local is video memory (DEFAULT and CUSTOM L1 heaps); NonLocal is
// system memory the GPU can reach (UPLOAD, READBACK and CUSTOM L0 heaps).
// DXGI budgets the two separately, so their values are never added.
//
// Ledgers:
//  - Physical: bytes the engine allocated from D3D12. Committed resources
//    count under their own category; heaps count under Heap, including the
//    placed resources inside them.
//  - Logical: bytes of the resources themselves, committed or placed.
//    Aliased placed resources can sum to more than the heaps they share.
//  - Resources: live resources of the category, and live heaps for Heap.
// The Physical and Logical totals of a segment are the EngineKnownPhysical
// and Logical plots below; DXGI usage minus the physical total is memory the
// engine registry does not see (swap chains, descriptor heaps, PSOs, driver).

namespace tracy
{
namespace JnVram
{

enum class Segment : uint8_t
{
    Local,
    NonLocal,
    NUM
};

enum class Ledger : uint8_t
{
    Physical,
    Logical,
    Resources,
    NUM
};

enum class Category : uint8_t
{
    Texture,
    RenderTarget,
    DepthStencil,
    Buffer,
    RayTracing,
    Heap,
    Other,
    NUM
};

constexpr int Segments = int( Segment::NUM );
constexpr int Ledgers = int( Ledger::NUM );
constexpr int Categories = int( Category::NUM );

constexpr const char* SegmentNames[Segments] = { "Local", "NonLocal" };
constexpr const char* LedgerNames[Ledgers] = { "Physical", "Logical", "Resources" };
constexpr const char* CategoryNames[Categories] = { "Texture", "RenderTarget", "DepthStencil", "Buffer", "RayTracing", "Heap", "Other" };

// Plot colors, 0xRRGGBB.
constexpr uint32_t CategoryColors[Categories] = { 0x24A99D, 0xEEA14D, 0xBA6EAC, 0x5185CE, 0x7D8D9D, 0xA1ADBF, 0x8B7FA8 };

// "GPU.VRAM.<Segment>.<Ledger>.<Category>". A literal table: Tracy identifies
// a plot by the address of its name.
#define TRACY_JN_VRAM_CATEGORIES( segment, ledger ) { \
    "GPU.VRAM." segment "." ledger ".Texture", "GPU.VRAM." segment "." ledger ".RenderTarget", \
    "GPU.VRAM." segment "." ledger ".DepthStencil", "GPU.VRAM." segment "." ledger ".Buffer", \
    "GPU.VRAM." segment "." ledger ".RayTracing", "GPU.VRAM." segment "." ledger ".Heap", \
    "GPU.VRAM." segment "." ledger ".Other" }
constexpr const char* PlotNames[Segments][Ledgers][Categories] = {
    { TRACY_JN_VRAM_CATEGORIES( "Local", "Physical" ), TRACY_JN_VRAM_CATEGORIES( "Local", "Logical" ), TRACY_JN_VRAM_CATEGORIES( "Local", "Resources" ) },
    { TRACY_JN_VRAM_CATEGORIES( "NonLocal", "Physical" ), TRACY_JN_VRAM_CATEGORIES( "NonLocal", "Logical" ), TRACY_JN_VRAM_CATEGORIES( "NonLocal", "Resources" ) }
};
#undef TRACY_JN_VRAM_CATEGORIES

// Totals of the ledgers, and what DXGI reports for each segment.
constexpr const char* PhysicalTotalPlots[Segments] = { "GPU.VRAM.EngineKnownPhysical.LocalBytes", "GPU.VRAM.EngineKnownPhysical.NonLocalBytes" };
constexpr const char* LogicalTotalPlots[Segments] = { "GPU.VRAM.Logical.LocalBytes", "GPU.VRAM.Logical.NonLocalBytes" };
constexpr const char* DxgiUsagePlots[Segments] = { "GPU.DXGI.Local.UsageBytes", "GPU.DXGI.NonLocal.UsageBytes" };
constexpr const char* DxgiBudgetPlots[Segments] = { "GPU.DXGI.Local.BudgetBytes", "GPU.DXGI.NonLocal.BudgetBytes" };
// Bytes the driver demoted out of the segment it was allocated in.
constexpr const char* EvictedBytesPlot = "GPU.VRAM.Residency.EvictedBytes";

}
}

#endif
