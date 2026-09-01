$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw $Message }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$traceSource = Get-Content -Raw (Join-Path $root 'analysis\TracyTraceSource.hpp')
$sidecarSource = Get-Content -Raw (Join-Path $root 'analysis\TracyGpuAnalysisSidecar.cpp')
$storeSource = Get-Content -Raw (Join-Path $root 'analysis\TracyGpuAnalysisStore.cpp')
$lazySource = Get-Content -Raw (Join-Path $root 'analysis\TracyGpuAnalysisTraceSource.cpp')
$sessionGpuSource = Get-Content -Raw (Join-Path $root 'analysis\TracyTraceSessionGpuCanonical.cpp')
$sessionManager = Get-Content -Raw (Join-Path $root 'query\src\TracySessionManager.cpp')
$service = Get-Content -Raw (Join-Path $root 'query\src\TracyQueryService.cpp')
$converter = Get-Content -Raw (Join-Path $root 'capture\src\stream-convert.cpp')
$builder = Get-Content -Raw (Join-Path $root 'query\src\gpu-analysis-build.cpp')
$controller = Get-Content -Raw (Join-Path $root 'profiler\src\profiler\TracyGpuAnalysisController.cpp')
$view = Get-Content -Raw (Join-Path $root 'profiler\src\profiler\TracyView_JnGpuResources.cpp')

Require ($traceSource.Contains('BackingPath()')) 'TraceSource does not expose a backing path for sidecar lookup.'
Require ($traceSource.Contains('PrepareForQuery')) 'TraceSource does not expose lazy full-Worker preparation.'
Require ($lazySource.Contains('GpuAnalysisStoreReader::Open')) 'Lazy trace source does not open the N29 store.'
Require ($lazySource.Contains('WorkerTraceSource::Open')) 'Lazy trace source cannot materialize non-GPU domains on demand.'
Require ($sessionManager.Contains('GpuAnalysisTraceSource::OpenIfReady')) 'SessionManager does not prefer completed GPU sidecars.'
Require ($service.Contains('n29_gpu_resource_analysis_sidecar')) 'GPU Query responses do not disclose the N29 sidecar backend.'
Require (($service | Select-String -Pattern 'CachedGpuStoreReader\(' -AllMatches).Matches.Count -ge 2) 'GPU Query routes do not consistently use the shared sidecar reader.'

Require ($converter.Contains('--no-gpu-analysis')) 'Converter lacks the diagnostic sidecar opt-out.'
Require ($converter.Contains('WriteGpuAnalysisRawSidecar')) 'Converter does not emit raw GPU analysis data from replayed Worker data.'
Require ($converter.Contains('trace_complete_analysis_failed')) 'Converter lacks independent trace/analysis failure semantics.'
Require (-not ($converter -match 'WorkerTraceSource::Open\([\s\S]{0,1024}WriteGpuAnalysisRawSidecar')) 'Converter rereads the newly written trace before raw sidecar emission.'

Require ($sidecarSource.Contains('.writer-lease')) 'Builder single-writer lease is missing.'
Require ($sidecarSource.Contains('stale_lease_recovery_failed')) 'Stale lease recovery is missing.'
Require ($sidecarSource.Contains('trace_quick_identity_mismatch')) 'Quick trace identity validation is missing.'
Require ($sidecarSource.Contains('trace_sha256_mismatch')) 'Strong trace identity validation is missing.'
Require ($sidecarSource.Contains('BuildGpuAnalysisSnapshotConsuming')) 'Offline builder does not release raw vectors while deriving indexes.'
Require ($storeSource.Contains('store-page-committed')) 'Derived store does not expose committed-shard checkpoints.'
Require ($storeSource.Contains('generations.size()')) 'Derived generation retention is missing.'
Require ($builder.Contains('BELOW_NORMAL_PRIORITY_CLASS')) 'External builder is not assigned below-normal priority.'
Require ($builder.Contains('--cancel-file')) 'External builder cancellation contract is missing.'
Require ($builder.Contains('--pause-file')) 'External builder pause contract is missing.'

Require ($controller.Contains('GpuAnalysisStoreReader::Open')) 'Profiler controller does not use the sidecar store reader.'
Require ($controller.Contains('LoadResourcePage')) 'Profiler resource paging is missing.'
Require ($controller.Contains('LoadPassFrame')) 'Profiler frame-priority loading is missing.'
Require ($controller.Contains('RunExternalBuilder')) 'Profiler does not attach to or launch the external builder.'
Require ($controller.Contains('builder-continues-external')) 'Profiler exit cannot detach from an active builder.'
Require ($controller.Contains('StopPageLoad')) 'Profiler page-load cancellation is missing.'
Require (-not $view.Contains('BuildGpuAnalysisSnapshot(')) 'GPU Memory & Resources still builds the full in-memory snapshot on the GUI path.'
Require (-not ($sessionGpuSource -match 'BuildTraceSessionGpuAnalysisDerived[\s\S]*?LoadTraceSessionGpuCanonicalData\([\s\S]*?BuildGpuAnalysisSnapshotConsuming\(')) `
    'N30 Session GPU derived construction still materializes full JnTraceData and GpuAnalysisSnapshot.'
Require ($sessionGpuSource.Contains('sessionRoot / "checkpoints" / "gpu-pass-spool"')) `
    'N30 Session GPU spool is still nested below the long public derived path.'
Require (($sessionGpuSource | Select-String -Pattern 'GpuAnalysisIoPath\(' -AllMatches).Matches.Count -ge 10) `
    'N30 Session GPU spool I/O does not consistently use the Windows long-path helper.'
Require (-not $sessionGpuSource.Contains('std::vector<ResourceLifetimeEntry> logicalLifetimes')) `
    'N30 Session GPU logical lifetime resolution still grows with total Logical record count.'
Require ($sessionGpuSource.Contains('AddLogicalFile')) `
    'N30 Session GPU pass resolution does not use the disk-backed Logical lifetime index.'
Require ($sessionGpuSource.Contains('state.control->stopToken.stop_requested()')) `
    'N30 Session GPU enrichment scan does not provide bounded graceful cancellation.'

$coverage = Get-Content -Raw (Join-Path $root 'query\schema\coverage-v1.json') | ConvertFrom-Json
$fields = Get-Content -Raw (Join-Path $root 'query\schema\coverage-fields-v1.json') | ConvertFrom-Json
$mcp = Get-Content -Raw (Join-Path $root 'query\schema\coverage-mcp-v1.json') | ConvertFrom-Json
Require ($coverage.schema_version -eq '1.34.0') 'Domain coverage schema is not 1.34.0.'
Require ($fields.schema_version -eq '1.34.0') 'Field coverage schema is not 1.34.0.'
Require ($mcp.schema_version -eq '1.34.0') 'MCP coverage schema is not 1.34.0.'

Write-Host 'N29 GPU Resource Analysis Sidecar static checks passed.'
