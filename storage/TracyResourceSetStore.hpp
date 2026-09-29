#ifndef TRACY_RESOURCE_SET_STORE_HPP
#define TRACY_RESOURCE_SET_STORE_HPP
#include "TracyConversionResources.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string>
namespace tracy::storage
{
#pragma pack(push,1)
struct ResourceSetMember { uint64_t resourceId;uint32_t usageMask; };
#pragma pack(pop)
static_assert(sizeof(ResourceSetMember)==12);
struct ResourceSetDefinition { uint64_t first=0;uint32_t count=0; };
class ResourceSetStore
{
public:
    struct Stats { uint64_t lookups=0,indexPageLoads=0,indexPageWrites=0,evictions=0,slotAllocations=0,slotReuses=0; };
    ResourceSetStore(std::filesystem::path root,std::string identity,ConversionResources& resources,
        uint64_t indexCacheBytes=32ull<<20,uint64_t dataCacheBytes=64ull<<20);
    ~ResourceSetStore();
    bool Define(uint32_t id,std::span<const ResourceSetMember> members);
    std::optional<ResourceSetDefinition> Lookup(uint32_t id);
    void Read(ResourceSetDefinition definition,uint32_t offset,std::span<ResourceSetMember> output);
    void ClearCaches();
    void ReduceCaches();
    void Seal();
    uint64_t Count() const;
    Stats Statistics() const;
private:
    struct Impl;std::unique_ptr<Impl> m_impl;
};
}
#endif
