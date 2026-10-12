[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$MarkerCatalog,
    [Parameter(Mandatory = $true)][string]$MarkerAttribution,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$catalogObject = Read-JNJson -LiteralPath $MarkerCatalog
$mapping = Read-JNJson -LiteralPath $MarkerAttribution
$markers = if ($null -ne $catalogObject.PSObject.Properties['markers']) { @($catalogObject.markers) } else { @($catalogObject) }
$rules = @($mapping.rules)
$ruleHits = @{}
foreach ($rule in $rules) { $ruleHits[[string]$rule.category + ':' + [string]$rule.priority] = 0 }

$mapped = @()
$unmapped = @()
$ambiguous = @()
$totalWeight = 0.0
$mappedWeight = 0.0

foreach ($marker in $markers) {
    $name = [string]$marker.name
    $weight = if ($null -ne $marker.PSObject.Properties['self_time_ns']) { [double]$marker.self_time_ns } else { 1.0 }
    if ($weight -lt 0) { $weight = 0 }
    $totalWeight += $weight
    $matches = @()
    foreach ($rule in $rules) {
        $hit = $false
        if ($null -ne $rule.PSObject.Properties['exact_markers']) {
            if (@($rule.exact_markers) -contains $name) { $hit = $true }
        }
        if (-not $hit -and $null -ne $rule.PSObject.Properties['prefixes']) {
            foreach ($prefix in @($rule.prefixes)) {
                if ($name.StartsWith([string]$prefix, [System.StringComparison]::Ordinal)) { $hit = $true; break }
            }
        }
        if ($hit) { $matches += $rule }
    }

    if ($matches.Count -eq 0) {
        $unmapped += [ordered]@{ name = $name; weight = $weight }
        continue
    }
    $topPriority = ($matches | Measure-Object -Property priority -Maximum).Maximum
    $top = @($matches | Where-Object { [int]$_.priority -eq [int]$topPriority })
    $categories = @($top | ForEach-Object { [string]$_.category } | Sort-Object -Unique)
    foreach ($match in $matches) {
        $key = [string]$match.category + ':' + [string]$match.priority
        $ruleHits[$key] = [int]$ruleHits[$key] + 1
    }
    if ($categories.Count -ne 1) {
        $ambiguous += [ordered]@{ name = $name; categories = $categories; priority = $topPriority; weight = $weight }
        continue
    }
    $mappedWeight += $weight
    $mapped += [ordered]@{ name = $name; category = $categories[0]; priority = $topPriority; weight = $weight }
}

$stale = @()
foreach ($rule in $rules) {
    $key = [string]$rule.category + ':' + [string]$rule.priority
    if ([int]$ruleHits[$key] -eq 0) {
        $stale += [ordered]@{ category = [string]$rule.category; priority = [int]$rule.priority }
    }
}
$coverage = if ($totalWeight -gt 0) { $mappedWeight / $totalWeight } else { 0.0 }
$result = [ordered]@{
    schema_version = 1
    mapping_revision = [int]$mapping.mapping_revision
    catalog_marker_count = $markers.Count
    mapped_count = $mapped.Count
    unmapped_count = $unmapped.Count
    ambiguous_count = $ambiguous.Count
    weighted_coverage_ratio = $coverage
    minimum_required_coverage = [double]$mapping.minimum_main_thread_coverage
    coverage_passed = $coverage -ge [double]$mapping.minimum_main_thread_coverage
    mapped = $mapped
    new_unmapped = @($unmapped | Sort-Object -Property weight -Descending)
    stale_rules = $stale
    ambiguous = $ambiguous
    auto_modified = $false
}
Write-JNJsonAtomic -LiteralPath $OutputPath -Value $result
$result | ConvertTo-Json -Depth 50
