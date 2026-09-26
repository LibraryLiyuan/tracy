#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyWorkerTraceSource.hpp"
#include <chrono>
#include <array>
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
            if(!std::filesystem::equivalent(entry.path(),input) &&
                !std::filesystem::equivalent(entry.path(),failurePath))
                throw std::runtime_error("resource-denied QueryIndex left a candidate behind: "+entry.path().string());
        std::ifstream failureInput(failurePath,std::ios::binary);
        const auto failure=nlohmann::json::parse(failureInput);
        if(failure.at("failure_reason") != "disk_peak_limit" || failure.at("requested_bytes").is_null() ||
            failure.at("phase").get<std::string>().empty())
            throw std::runtime_error("resource-denied QueryIndex did not retain an actionable diagnostic");
        auto checkpointPath=tracy::query::QueryIndex::ManifestPath(input);
        checkpointPath+=".checkpoint.json";
        bool checkpointFaultObserved=false;
        try {
            tracy::query::QueryIndex::Build(input,{}, {}, [&](std::string_view phase) {
                if(phase=="post-compact-checkpoint") {
                    checkpointFaultObserved=true;
                    throw std::runtime_error("injected post-compact checkpoint stop");
                }
            });
        } catch(const std::exception& error) {
            if(std::string_view(error.what())!="injected post-compact checkpoint stop") throw;
        }
        if(!checkpointFaultObserved || !std::filesystem::is_regular_file(checkpointPath) ||
            std::filesystem::exists(tracy::query::QueryIndex::ManifestPath(input)))
            throw std::runtime_error("post-compact checkpoint failure was not retained transactionally");
        {
            std::ifstream checkpointInput(checkpointPath,std::ios::binary);
            const auto checkpoint=nlohmann::json::parse(checkpointInput);
            const auto useName=checkpoint.at("candidate_manifest").at("sections")
                .at("gpu_reference_use").at("file").get<std::string>();
            const auto interruptedOutput=root/useName/"by-resource";
            std::filesystem::create_directories(interruptedOutput);
            std::ofstream(interruptedOutput/"partial")<<"interrupted";
        }
        auto staleManifestControl=tracy::query::QueryIndex::ManifestPath(input);
        staleManifestControl+=".tmp.999";
        auto staleCheckpointControl=checkpointPath;
        staleCheckpointControl+=".tmp.999.1";
        std::ofstream(staleManifestControl)<<"stale";
        std::ofstream(staleCheckpointControl)<<"stale";
        size_t resumedWorkerStates=0;
        bool resumedPhaseObserved=false;
        const auto manifest=tracy::query::QueryIndex::Build(input,
            [&](tracy::analysis::TraceSourceState) { resumedWorkerStates++; }, {},
            [&](std::string_view phase) {
                if(phase=="post-compact-resume") resumedPhaseObserved=true;
            });
        if(!resumedPhaseObserved || resumedWorkerStates!=0 || std::filesystem::exists(checkpointPath) ||
            std::filesystem::exists(staleManifestControl) || std::filesystem::exists(staleCheckpointControl))
            throw std::runtime_error("QueryIndex did not resume exclusively from the post-compact checkpoint");
        if(manifest.gpuMemorySummary.count!=0)
            throw std::runtime_error("cold QueryIndex still built the full GPU-memory attribution summary");
        const auto validation=tracy::query::QueryIndex::Validate(input,true);
        if(!validation.manifest) throw std::runtime_error(validation.reason);
        const auto freshRoot=root/"fresh";
        std::filesystem::create_directories(freshRoot);
        const auto freshInput=freshRoot/"fixture.tracy";
        std::filesystem::copy_file(argv[1],freshInput);
        const auto freshManifest=tracy::query::QueryIndex::Build(freshInput);
        const auto freshResources=tracy::query::QueryIndex::CreateResourceContext(freshInput);
        const auto freshValidation=tracy::query::QueryIndex::Validate(
            freshInput,true,freshResources,&freshManifest);
        if(!freshValidation.manifest) throw std::runtime_error(freshValidation.reason);
        bool reusedGpuDerivedValidation=false;
        bool phasedSampleValidation=false;
        bool phasedJobValidation=false;
        bool phasedCatalogStructure=false;
        bool phasedCatalogSemantics=false;
        for(const auto& phase:freshResources->Snapshot().phaseHistory)
        {
            if(phase.phase=="index-validate:deep:gpu-derived-reused")
                reusedGpuDerivedValidation=true;
            else if(phase.phase=="index-validate:deep:samples")
                phasedSampleValidation=true;
            else if(phase.phase=="index-validate:deep:jobs")
                phasedJobValidation=true;
            else if(phase.phase=="index-validate:deep:catalog-structure")
                phasedCatalogStructure=true;
            else if(phase.phase=="index-validate:deep:catalog-semantics")
                phasedCatalogSemantics=true;
        }
        if(!reusedGpuDerivedValidation)
            throw std::runtime_error("fresh build repeated GPU derived deep validation");
        if(!phasedSampleValidation || !phasedJobValidation)
            throw std::runtime_error("deep validation did not expose per-domain phase telemetry");
        if(!phasedCatalogStructure || !phasedCatalogSemantics)
            throw std::runtime_error("Catalog deep validation did not expose structure and semantic phases");
        auto mismatchedReceipt=freshManifest;
        mismatchedReceipt.gpuReferenceUses.bytes++;
        const auto fallbackResources=tracy::query::QueryIndex::CreateResourceContext(freshInput);
        const auto fallbackValidation=tracy::query::QueryIndex::Validate(
            freshInput,true,fallbackResources,&mismatchedReceipt);
        if(!fallbackValidation.manifest) throw std::runtime_error(fallbackValidation.reason);
        bool validatedGpuDerived=false;
        for(const auto& phase:fallbackResources->Snapshot().phaseHistory)
            if(phase.phase=="index-validate:deep:gpu-derived")
                validatedGpuDerived=true;
        if(!validatedGpuDerived)
            throw std::runtime_error("mismatched build receipt bypassed GPU derived deep validation");
        // The compact Tracy container carries writer metadata that is not byte-stable
        // across independent builds.  Both copies are deep-validated separately;
        // all deterministic Query sidecars must remain hash-identical.
        if(manifest.sampleMainCount!=freshManifest.sampleMainCount ||
            manifest.sampleContextSwitchCount!=freshManifest.sampleContextSwitchCount)
            throw std::runtime_error("resumed compact output differs from a fresh cold build");
        const std::array resumedSections={
            &manifest.zoneExtras,&manifest.cpuZones,&manifest.gpuZones,&manifest.samples,
            &manifest.globalStrings,&manifest.globalCallstacks,&manifest.globalCallstackFrames,
            &manifest.gpuCatalog,&manifest.jobStages,&manifest.gfxEntities,&manifest.gfxLinks,
            &manifest.relations,&manifest.gpuReferencePasses,&manifest.gpuReferenceUses,
            &manifest.gpuReferenceEnds,&manifest.gpuMemoryCpuZones,
            &manifest.gpuMemorySummaryCpuZones,&manifest.gpuMemorySummary};
        const std::array freshSections={
            &freshManifest.zoneExtras,&freshManifest.cpuZones,&freshManifest.gpuZones,&freshManifest.samples,
            &freshManifest.globalStrings,&freshManifest.globalCallstacks,&freshManifest.globalCallstackFrames,
            &freshManifest.gpuCatalog,&freshManifest.jobStages,&freshManifest.gfxEntities,&freshManifest.gfxLinks,
            &freshManifest.relations,&freshManifest.gpuReferencePasses,&freshManifest.gpuReferenceUses,
            &freshManifest.gpuReferenceEnds,&freshManifest.gpuMemoryCpuZones,
            &freshManifest.gpuMemorySummaryCpuZones,&freshManifest.gpuMemorySummary};
        for(size_t index=0;index<resumedSections.size();index++)
            if(resumedSections[index]->fingerprint!=freshSections[index]->fingerprint ||
                resumedSections[index]->bytes!=freshSections[index]->bytes ||
                resumedSections[index]->count!=freshSections[index]->count)
                throw std::runtime_error("resumed section differs from a fresh cold build");

        const auto derivedRoot=root/"derived-resume";
        std::filesystem::create_directories(derivedRoot);
        const auto derivedInput=derivedRoot/"fixture.tracy";
        std::filesystem::copy_file(argv[1],derivedInput);
        auto derivedCheckpoint=tracy::query::QueryIndex::ManifestPath(derivedInput);
        derivedCheckpoint+=".checkpoint.json";
        try {
            tracy::query::QueryIndex::Build(derivedInput,{}, {}, [](std::string_view phase) {
                if(phase=="post-derived-checkpoint") throw std::runtime_error("derived checkpoint stop");
            });
        } catch(const std::exception& error) {
            if(std::string_view(error.what())!="derived checkpoint stop") throw;
        }
        size_t derivedWorkerStates=0; bool derivedResumed=false;
        const auto derivedManifest=tracy::query::QueryIndex::Build(derivedInput,
            [&](tracy::analysis::TraceSourceState) { derivedWorkerStates++; }, {},
            [&](std::string_view phase) { if(phase=="post-derived-resume") derivedResumed=true; });
        if(!derivedResumed || derivedWorkerStates!=0 || std::filesystem::exists(derivedCheckpoint))
            throw std::runtime_error("post-derived checkpoint did not skip all completed build stages");
        const auto derivedValidation=tracy::query::QueryIndex::Validate(derivedInput,true);
        if(!derivedValidation.manifest ||
            derivedManifest.gpuReferenceUses.fingerprint!=freshManifest.gpuReferenceUses.fingerprint)
            throw std::runtime_error("post-derived checkpoint changed final Query data");
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
        source.reset();

        const auto rejectRoot=root/"reject";
        std::filesystem::create_directories(rejectRoot);
        const auto rejectInput=rejectRoot/"fixture.tracy";
        std::filesystem::copy_file(argv[1],rejectInput);
        auto rejectCheckpoint=tracy::query::QueryIndex::ManifestPath(rejectInput);
        rejectCheckpoint+=".checkpoint.json";
        try {
            tracy::query::QueryIndex::Build(rejectInput,{}, {}, [](std::string_view phase) {
                if(phase=="post-compact-checkpoint") throw std::runtime_error("reject checkpoint stop");
            });
        } catch(const std::exception& error) {
            if(std::string_view(error.what())!="reject checkpoint stop") throw;
        }
        std::ifstream rejectCheckpointInput(rejectCheckpoint,std::ios::binary);
        const auto checkpoint=nlohmann::json::parse(rejectCheckpointInput);
        rejectCheckpointInput.close();
        const auto compactName=checkpoint.at("candidate_manifest").at("data").at("file").get<std::string>();
        std::ofstream(rejectRoot/compactName,std::ios::binary|std::ios::app)<<"corrupt";
        size_t rejectedWorkerStates=0; bool rejectedResumed=false;
        const auto rebuilt=tracy::query::QueryIndex::Build(rejectInput,
            [&](tracy::analysis::TraceSourceState) { rejectedWorkerStates++; }, {},
            [&](std::string_view phase) { if(phase=="post-compact-resume") rejectedResumed=true; });
        if(rejectedResumed || rejectedWorkerStates==0 || std::filesystem::exists(rejectCheckpoint))
            throw std::runtime_error("corrupted post-compact checkpoint was not rejected and rebuilt");
        const auto rebuiltValidation=tracy::query::QueryIndex::Validate(rejectInput,true);
        if(!rebuiltValidation.manifest) throw std::runtime_error(rebuiltValidation.reason);
        for(const auto& entry:std::filesystem::directory_iterator(rejectRoot))
            if(entry.path().filename().string().find(".tmp.")!=std::string::npos)
                throw std::runtime_error("checkpoint rejection left an owned temporary candidate");

        const auto profileRoot=root/"profile-mismatch";
        std::filesystem::create_directories(profileRoot);
        const auto profileInput=profileRoot/"fixture.tracy";
        std::filesystem::copy_file(argv[1],profileInput);
        tracy::query::QueryResourceContextOptions profileOptions;
        profileOptions.privateBytesSoftTarget=15ull*1024*1024*1024;
        auto profileResources=tracy::query::QueryIndex::CreateResourceContext(profileInput,profileOptions);
        try {
            tracy::query::QueryIndex::Build(profileInput,{},profileResources,[](std::string_view phase) {
                if(phase=="post-compact-checkpoint") throw std::runtime_error("profile checkpoint stop");
            });
        } catch(const std::exception& error) {
            if(std::string_view(error.what())!="profile checkpoint stop") throw;
        }
        profileResources.reset();
        size_t profileWorkerStates=0; bool profileResumed=false;
        tracy::query::QueryIndex::Build(profileInput,
            [&](tracy::analysis::TraceSourceState) { profileWorkerStates++; }, {},
            [&](std::string_view phase) { if(phase=="post-compact-resume") profileResumed=true; });
        if(profileResumed || profileWorkerStates==0)
            throw std::runtime_error("resource-profile-mismatched checkpoint was reused");

        const auto identityRoot=root/"source-mismatch";
        std::filesystem::create_directories(identityRoot);
        const auto identityInput=identityRoot/"fixture.tracy";
        std::filesystem::copy_file(argv[1],identityInput);
        try {
            tracy::query::QueryIndex::Build(identityInput,{}, {}, [](std::string_view phase) {
                if(phase=="post-compact-checkpoint") throw std::runtime_error("identity checkpoint stop");
            });
        } catch(const std::exception& error) {
            if(std::string_view(error.what())!="identity checkpoint stop") throw;
        }
        std::filesystem::last_write_time(identityInput,
            std::filesystem::last_write_time(identityInput)+std::chrono::seconds(1));
        size_t identityWorkerStates=0; bool identityResumed=false;
        tracy::query::QueryIndex::Build(identityInput,
            [&](tracy::analysis::TraceSourceState) { identityWorkerStates++; }, {},
            [&](std::string_view phase) { if(phase=="post-compact-resume") identityResumed=true; });
        if(identityResumed || identityWorkerStates==0)
            throw std::runtime_error("source-identity-mismatched checkpoint was reused");

        std::cout<<"compressed index passed cpu="<<manifest.cpuZones.count<<" stored="<<manifest.cpuZones.bytes<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
