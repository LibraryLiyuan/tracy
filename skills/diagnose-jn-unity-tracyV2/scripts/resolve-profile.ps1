[CmdletBinding()]
param(
    [string]$ProjectProfile = (Join-Path $PSScriptRoot '..\config\project-profile.json'),
    [string]$Budgets = (Join-Path $PSScriptRoot '..\config\performance-budgets.json'),
    [string]$MarkerAttribution = (Join-Path $PSScriptRoot '..\config\marker-attribution.json'),
    [string]$LocalProfile = (Join-Path $PSScriptRoot '..\config\local-profile.json'),
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

function Assert-Value {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$project = Read-JNJson -LiteralPath $ProjectProfile
$budgetsConfig = Read-JNJson -LiteralPath $Budgets
$markers = Read-JNJson -LiteralPath $MarkerAttribution
$local = Read-JNJson -LiteralPath $LocalProfile

Assert-Value ($project.kind -eq 'jntracy_project_profile') 'Unexpected project profile kind.'
Assert-Value ($budgetsConfig.kind -eq 'jntracy_performance_budgets') 'Unexpected budgets profile kind.'
Assert-Value ($markers.kind -eq 'jntracy_marker_attribution') 'Unexpected marker attribution kind.'
Assert-Value ($local.kind -eq 'jntracy_local_profile') 'Unexpected local profile kind.'
Assert-Value ($project.schema_version -eq 2) 'Unsupported project profile schema.'
Assert-Value ($budgetsConfig.schema_version -eq 1) 'Unsupported budgets schema.'
Assert-Value ($markers.schema_version -eq 1) 'Unsupported marker attribution schema.'
Assert-Value ($local.schema_version -eq 1) 'Unsupported local profile schema.'
Assert-Value ($project.trace_contract.protocol -eq 90) 'This skill requires Tracy Protocol 90.'
Assert-Value ($project.trace_contract.query_schema -eq '1.32.0') 'This skill requires Query schema 1.32.0.'
Assert-Value ($project.trace_contract.jn_section -eq 12) 'This skill requires JN section 12.'
Assert-Value ($project.trace_contract.gpu_catalog_schema -eq 1) 'This skill requires GPU Catalog schema 1.'
Assert-Value ($project.capture.output_mode -eq 'stream_only') 'Capture output mode must be stream_only.'
Assert-Value ($project.analysis.single_trace_only -eq $true) 'This skill only supports single-trace analysis.'
Assert-Value ($project.analysis.deep_query_batch_size -eq 12) 'deep_query_batch_size must be 12; it is a query batching limit, not an analysis-frame limit.'
Assert-Value ($project.analysis.spike_detection.recurrent_min_count -eq 3) 'recurrent_min_count must be 3.'
Assert-Value ($project.analysis.query_limits.max_attempts -eq 3) 'MCP max_attempts must be 3.'

$toolRoot = [System.IO.Path]::GetFullPath([string]$local.paths.toolchain_root)
Assert-Value (Test-Path -LiteralPath $toolRoot -PathType Container) "Toolchain root not found: $toolRoot"
$toolNames = [ordered]@{
    capture = 'tracy-capture.exe'
    converter = 'tracy-stream-convert.exe'
    query = 'tracy-query.exe'
    profiler = 'tracy-profiler.exe'
}
$toolchain = [ordered]@{}
foreach ($entry in $toolNames.GetEnumerator()) {
    $path = Join-Path $toolRoot $entry.Value
    Assert-Value (Test-Path -LiteralPath $path -PathType Leaf) "Required tool not found: $path"
    $toolchain[$entry.Key] = Get-JNFileIdentity -LiteralPath $path
}
$pythonPath = [System.IO.Path]::GetFullPath([string]$local.paths.python_exe)
Assert-Value (Test-Path -LiteralPath $pythonPath -PathType Leaf) "Python executable not found: $pythonPath"
$toolchain['python'] = Get-JNFileIdentity -LiteralPath $pythonPath

$sourceRoots = @()
foreach ($key in @($local.source_roots)) {
    $property = $local.paths.PSObject.Properties[[string]$key]
    Assert-Value ($null -ne $property) "Unknown source root key: $key"
    $path = [System.IO.Path]::GetFullPath([string]$property.Value)
    Assert-Value (Test-Path -LiteralPath $path -PathType Container) "Source root not found: $path"
    $sourceRoots += [ordered]@{ role = [string]$key; path = $path }
}

Assert-Value ($budgetsConfig.frame.hard_budget_ms -gt 0) 'Frame budget must be positive.'
Assert-Value ($budgetsConfig.capacity.gpu_local.limit_bytes -gt 0) 'GPU Local capacity limit must be positive.'
Assert-Value ($budgetsConfig.capacity.process_memory.limit_bytes -gt 0) 'Process memory capacity limit must be positive.'
Assert-Value ($budgetsConfig.units.gigabyte -eq 'decimal') 'Capacity display must use the configured decimal GB convention.'
Assert-Value ($markers.minimum_main_thread_coverage -ge 0 -and $markers.minimum_main_thread_coverage -le 1) 'Marker coverage must be in [0,1].'

$configIdentity = [ordered]@{
    project = Get-JNFileIdentity -LiteralPath $ProjectProfile
    budgets = Get-JNFileIdentity -LiteralPath $Budgets
    marker_attribution = Get-JNFileIdentity -LiteralPath $MarkerAttribution
    local = Get-JNFileIdentity -LiteralPath $LocalProfile
}

$resolved = [ordered]@{
    schema_version = 1
    resolved_at_utc = Get-JNUtcNow
    project = $project
    budgets = $budgetsConfig
    marker_attribution = $markers
    local = $local
    config_identity = $configIdentity
    toolchain = $toolchain
    source_roots = $sourceRoots
}

if ($OutputPath) {
    Write-JNJsonAtomic -LiteralPath $OutputPath -Value $resolved
}

$resolved | ConvertTo-Json -Depth 100
