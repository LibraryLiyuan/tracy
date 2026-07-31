[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$LegacyTrace
)

$ErrorActionPreference = 'Stop'
$identityAcceptance = Join-Path $PSScriptRoot 'JnIdentityMcpAcceptance.ps1'
& $identityAcceptance -QueryExe $QueryExe -SnapshotTrace $SnapshotTrace -StreamTrace $StreamTrace `
    -ReplayTrace $ReplayTrace -AllowRoot $AllowRoot -LegacyTrace $LegacyTrace -CheckContextQuality
