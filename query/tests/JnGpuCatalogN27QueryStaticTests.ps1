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
Require ($service.Contains('source->GetTraceInfo().counts.gpuReferencePasses')) `
    'Catalog validation does not preserve GPU-reference pass counts for indexed traces.'
Require ($service.Contains('referencePassIds.emplace( value.targetId )')) `
    'Catalog validation does not recover typed ReferencesResources targets in indexed traces.'
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
Require ($coverage.schema_version -eq '1.32.0') 'Domain coverage schema is not 1.32.0.'
Require ($fields.schema_version -eq '1.32.0') 'Field coverage schema is not 1.32.0.'
Require ($mcpCoverage.schema_version -eq '1.32.0') 'MCP coverage schema is not 1.32.0.'
foreach ($domain in @('gpu.catalog', 'gpu.resource', 'gpu.memory')) {
    Require ($coverage.domains.domain -contains $domain) "Coverage domain missing: $domain"
}

Write-Host 'N27 GPU Catalog Query/MCP static checks passed.'
