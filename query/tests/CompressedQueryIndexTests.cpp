#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyWorkerTraceSource.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>
int main(int argc,char** argv)
{
    try {
        if(argc!=3) throw std::runtime_error("expected input trace and isolated output directory");
        const std::filesystem::path root=argv[2];
        if(std::filesystem::exists(root)) throw std::runtime_error("refusing existing output");
        std::filesystem::create_directories(root);
        const auto input=root/"fixture.tracy";
        std::filesystem::copy_file(argv[1],input);
        auto full=tracy::analysis::WorkerTraceSource::Open(input);
        const auto originalCounts=full->GetTraceInfo().counts;full.reset();
        tracy::query::QueryResourceContextOptions deniedOptions;
        deniedOptions.traceBytes=1024;
        deniedOptions.derivedRoots={tracy::query::QueryIndex::ManifestPath(input)};
        deniedOptions.diagnosticMemoryReserveBytes=4096;
        deniedOptions.diagnosticDiskReserveBytes=4096;
        auto deniedResources=std::make_shared<tracy::query::QueryResourceContext>(std::move(deniedOptions));
        bool denied=false;
        try { tracy::query::QueryIndex::Build(input,{},deniedResources); }
        catch(const tracy::query::QueryResourceError& error) { denied=error.Code()=="RESOURCE_LIMIT"; }
        if(!denied) throw std::runtime_error("QueryIndex bypassed the injected shared disk limit");
        auto failurePath=tracy::query::QueryIndex::ManifestPath(input); failurePath+=".failure.json";
        for(const auto& entry:std::filesystem::directory_iterator(root))
            if(entry.path()!=input && entry.path()!=failurePath) throw std::runtime_error("resource-denied QueryIndex left a candidate behind");
        std::ifstream failureInput(failurePath,std::ios::binary);
        const auto failure=nlohmann::json::parse(failureInput);
        if(failure.at("failure_reason") != "disk_peak_limit" || failure.at("requested_bytes").is_null() ||
            failure.at("phase").get<std::string>().empty())
            throw std::runtime_error("resource-denied QueryIndex did not retain an actionable diagnostic");
        const auto manifest=tracy::query::QueryIndex::Build(input);
        if(manifest.gpuMemorySummary.count!=0)
            throw std::runtime_error("cold QueryIndex still built the full GPU-memory attribution summary");
        const auto validation=tracy::query::QueryIndex::Validate(input,true);
        if(!validation.manifest) throw std::runtime_error(validation.reason);
        if(!manifest.cpuZones.count || manifest.cpuZones.bytes>=manifest.cpuZones.count*52/2)
            throw std::runtime_error("CPU sidecar did not compress the repeated real fixture records");
        auto source=tracy::query::QueryIndex::Open(manifest);
        const auto info=source->GetTraceInfo();
        if(info.counts.cpuZones!=manifest.cpuZones.count) throw std::runtime_error("indexed count changed");
        if(info.counts.jobStages!=originalCounts.jobStages || info.counts.gfxLinks!=originalCounts.gfxLinks || info.counts.gpuReferencePasses!=originalCounts.gpuReferencePasses)
            throw std::runtime_error("indexed reader lost JN facts or changed stable domain ordinals");
        tracy::analysis::ScanRange range; range.limit=10;
        const auto zones=source->ScanCpuZones(range);
        if(zones.empty()) throw std::runtime_error("compressed indexed trace lost its zones");
        std::cout<<"compressed index passed cpu="<<manifest.cpuZones.count<<" stored="<<manifest.cpuZones.bytes<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
