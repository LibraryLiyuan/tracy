[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$LegacyTrace,
    [string]$ExpectedWorkloadScene = 'Init'
)

$ErrorActionPreference = 'Stop'
$identityAcceptance = Join-Path $PSScriptRoot 'JnIdentityMcpAcceptance.ps1'
$requiredProducers = @(
    'cpu.zone.c-abi',
    'job.native',
    'cpu.unity-profiler-marker',
    'memory.cpu.selected',
    'io.aggregate',
    'network.aggregate',
    'memory.gpu.registry',
    'gfx.jobs',
    'frame.image',
    'sampling.context-switch',
    'csharp.direct',
    'lua.direct'
)
$disabledProducers = @(
    'gfx.jobs',
    'frame.image',
    'sampling.context-switch'
)

& $identityAcceptance -QueryExe $QueryExe -SnapshotTrace $SnapshotTrace -StreamTrace $StreamTrace `
    -ReplayTrace $ReplayTrace -AllowRoot $AllowRoot -LegacyTrace $LegacyTrace -CheckContextQuality `
    -SkipSyntheticProducerChecks -ExpectedWorkloadScene $ExpectedWorkloadScene `
    -RequiredProducerKeys $requiredProducers -ExpectedDisabledProducerKeys $disabledProducers `
    -RequireAnyEmittedProducer
