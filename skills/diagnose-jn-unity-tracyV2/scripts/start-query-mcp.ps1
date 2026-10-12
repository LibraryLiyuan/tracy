[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ResolvedProfile,
    [Parameter(Mandatory = $true)][string]$TraceRoot,
    [string]$SessionManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$profile = Read-JNJson -LiteralPath $ResolvedProfile
$query = [string]$profile.toolchain.query.path
$traceRootFull = [System.IO.Path]::GetFullPath($TraceRoot)
if (-not (Test-Path -LiteralPath $traceRootFull -PathType Container)) {
    throw "Trace root does not exist: $traceRootFull"
}

$arguments = @('--mcp', '--allow-root', $traceRootFull)
foreach ($source in @($profile.source_roots)) {
    $arguments += '--allow-source-root'
    $arguments += [string]$source.path
}

if ($SessionManifestPath) {
    $manifest = [ordered]@{
        schema_version = 1
        started_at_utc = Get-JNUtcNow
        query = Get-JNFileIdentity -LiteralPath $query
        trace_root = $traceRootFull
        source_roots = @($profile.source_roots)
        max_attempts = [int]$profile.project.analysis.query_limits.max_attempts
        mode = 'persistent_mcp_only'
    }
    Write-JNJsonAtomic -LiteralPath $SessionManifestPath -Value $manifest
}

& $query @arguments
exit $LASTEXITCODE
