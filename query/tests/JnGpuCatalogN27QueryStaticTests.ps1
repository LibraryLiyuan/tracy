$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$service = Get-Content -Raw (Join-Path $root 'query\src\TracyQueryService.cpp')
$mcp = Get-Content -Raw (Join-Path $root 'query\src\TracyMcpServer.cpp')
$traceSource = Get-Content -Raw (Join-Path $root 'analysis\TracyTraceSource.hpp')
$workerSource = Get-Content -Raw (Join-Path $root 'analysis\TracyWorkerTraceSource.cpp')
$queryIndex = Get-Content -Raw (Join-Path $root 'query\src\TracyQueryIndex.cpp')
$segmentReplay = Get-Content -Raw (Join-Path $root 'query\src\TracySegmentTraceSource.cpp')
$converterReplay = Get-Content -Raw (Join-Path $root 'capture\src\stream-convert.cpp')
$tracyWorker = Get-Content -Raw (Join-Path $root 'server\TracyWorker.cpp')

$methods = @(
    'gpu.catalog.status', 'gpu.catalog.validation', 'gpu.resource.search',
    'gpu.resource.get', 'gpu.resource.explain', 'gpu.resource.lifetime',
    'gpu.resource.allocations', 'gpu.resource.references', 'gpu.resource.views',
    'gpu.resource.mesh_buffers', 'gpu.resource.raytracing_chain', 'gpu.resource.vg_pages',
    'gpu.pass.resources', 'gpu.pass.vg_evidence', 'gpu.memory.peak',
    'gpu.memory.by_type', 'gpu.memory.by_pass', 'gpu.memory.churn'
)
foreach ($method in $methods) {
    Require ($service.Contains('"' + $method + '"')) "Query method missing: $method"
}

Require ($traceSource.Contains('GetGpuCatalogData')) 'TraceSource does not expose immutable GPU Catalog data.'
Require ($workerSource.Contains('gpuCatalogGenerations')) 'Worker snapshot does not copy Catalog generations.'
Require ($queryIndex.Contains('return m_source->GetGpuCatalogData()')) 'Indexed trace open drops GPU Catalog data.'
Require ($service.Contains('unavailable_catalog_invalid')) 'Catalog Core invalidation is not isolated explicitly.'
Require ($service.Contains('not_sampled')) 'Periodic evidence does not distinguish not-sampled from empty.'
Require ($service.Contains('unavailable_n27_gpu_layer_only')) 'N27 high-level ownership boundary is not explicit.'
Require ($service.Contains('ResourceSetV2')) 'ResourceSetV2 is not joined with Catalog range evidence.'
Require ($service -match 'definitionRecord[\s\S]{0,2048}definitionRecord\s*&&\s*\(\s*!state\.latest') `
    'Destroyed GPU resources can overwrite their last queryable definition.'
Require ($service -match 'evidencePassIds[\s\S]{0,2048}JnGfxRelation::ReferencesResources[\s\S]{0,16384}evidencePassIds\.contains\( value\.passInstanceId \)') `
    'Explicit GPU passes are not joined to their typed GPU-reference pass evidence.'
Require (($service | Select-String -Pattern 'CachedGpuAttribution\( trace\.id, source \)' -AllMatches).Matches.Count -ge 2) `
    'GPU Catalog resource/pass queries do not reuse the bounded attribution cache.'
Require ($service -match 'attribution->passById\.find\( evidencePassId \)') `
    'GPU pass resource queries still linearly scan every attribution pass.'
Require ($service -match 'ScanGpuMemoryPasses\( 0, 1, evidencePassId, useOffset, MaximumPageSize \)') `
    'Indexed GPU pass resource queries do not use the paged pass/use sidecar.'
Require ($service -match 'resolveLogicalResource[\s\S]{0,8192}"logical_catalog_exact"') `
    'ResourceSet logical ids are not resolved through the time-bounded Catalog logical binding history.'
Require ($service -match 'resolvePointerResource[\s\S]{0,8192}"pointer_lifetime_exact"') `
    'ResourceSet D3D12 pointer tokens are not resolved through Catalog resource lifetime intervals.'
Require ($service -match 'Resource metadata and resource identity are deliberately separate') `
    'ResourceSet pointer resolution must use full lifecycle records rather than the latest metadata projection.'
Require ($traceSource.Contains('ScanGpuMemoryUsesByResource')) `
    'TraceSource does not expose the bounded reverse ResourceSet sidecar.'
Require ($queryIndex.Contains('ScanGpuMemoryUsesByResource')) `
    'Indexed Query does not provide bounded reverse ResourceSet lookup.'
Require ($service -match 'gpu\.resource\.raytracing_chain[\s\S]{0,4096}"has_more"') `
    'RTAS relation results are not paginated.'
Require (($service | Select-String -Pattern 'resolvedPhysicalAllocationId' -AllMatches).Matches.Count -ge 3) `
    'GPU Catalog queries do not join logical ResourceSet ids to resolved physical allocations.'
Require ($service.Contains('source->GetTraceInfo().counts.gpuReferencePasses')) `
    'Catalog validation does not preserve GPU-reference pass counts for indexed traces.'
Require ($service.Contains('referencePassIds.emplace( value.targetId )')) `
    'Catalog validation does not recover typed ReferencesResources targets in indexed traces.'
Require ($service -match 'validation\.run[\s\S]{0,8192}gpuCatalogControls[\s\S]{0,1024}gpuCatalogBatches[\s\S]{0,2048}sequenceGaps') `
    'validation.run checks Batch sequences without including Catalog control records from the shared sequence space.'
Require ($service -match 'gpu\.pass\.vg_evidence[\s\S]{0,4096}"page"[\s\S]{0,512}"has_more"') `
    'Detailed GPU evidence query is not paginated.'
Require ($tracyWorker.Contains('entity.kind == uint8_t( JnGfxEntityKind::ExplicitGpuPass )')) `
    'RangeSet lifetime resolution does not use the authoritative explicit GPU pass time.'
Require ($tracyWorker.Contains('link.relation == uint8_t( JnGfxRelation::RangeEvidenceComplete )')) `
    'RangeSet lifetime resolution does not prefer the authoritative pass-completion time.'
Require ($mcp.Contains('gpu_resource')) 'MCP search route for GPU resources is missing.'
foreach ($replaySource in @($segmentReplay, $converterReplay)) {
    Require ($replaySource.Contains('waitForWorkerProgress')) 'Large replay tail is not progress-aware.'
    Require ($replaySource.Contains('made no protocol progress for 120 seconds while processing the pre-drain')) 'Large pre-drain replay does not report a bounded no-progress timeout.'
    Require (-not $replaySource.Contains('const auto prefixDeadline')) 'Large pre-drain replay still uses a fixed wall-clock deadline.'
    Require (-not $replaySource.Contains('const auto drainDeadline')) 'Protocol drain activation still uses a fixed wall-clock deadline.'
}

$coverage = Get-Content -Raw (Join-Path $root 'query\schema\coverage-v1.json') | ConvertFrom-Json
$fields = Get-Content -Raw (Join-Path $root 'query\schema\coverage-fields-v1.json') | ConvertFrom-Json
$mcpCoverage = Get-Content -Raw (Join-Path $root 'query\schema\coverage-mcp-v1.json') | ConvertFrom-Json
Require ($coverage.schema_version -eq '1.34.0') 'Domain coverage schema is not 1.34.0.'
Require ($fields.schema_version -eq '1.34.0') 'Field coverage schema is not 1.34.0.'
Require ($mcpCoverage.schema_version -eq '1.34.0') 'MCP coverage schema is not 1.34.0.'
foreach ($domain in @('gpu.catalog', 'gpu.resource', 'gpu.memory')) {
    Require ($coverage.domains.domain -contains $domain) "Coverage domain missing: $domain"
}

Write-Host 'N27 GPU Catalog Query/MCP static checks passed.'
