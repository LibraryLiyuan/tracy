[CmdletBinding()]
param(
    [string]$DestinationRoot = (Join-Path $env:USERPROFILE '.codex\skills'),
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$skillName = 'diagnose-jn-unity-tracy'
$sourceRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$destinationRootFull = [System.IO.Path]::GetFullPath($DestinationRoot)
$destination = Join-Path $destinationRootFull $skillName
$exampleProfile = Join-Path $sourceRoot 'config\local-profile.example.json'
$exampleAnalysisProfile = Join-Path $sourceRoot 'config\JNTracy.AnalysisProfile.example.yaml'

if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'SKILL.md') -PathType Leaf)) {
    throw "Skill source is incomplete: $sourceRoot"
}
if (-not (Test-Path -LiteralPath $exampleProfile -PathType Leaf)) {
    throw "Local profile example is missing: $exampleProfile"
}
if (-not (Test-Path -LiteralPath $exampleAnalysisProfile -PathType Leaf)) {
    throw "Analysis profile example is missing: $exampleAnalysisProfile"
}
if ([System.IO.Path]::GetFullPath($destination).Equals($sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The installation destination cannot be the Skill source directory.'
}
if ((Test-Path -LiteralPath $destination) -and -not $Force) {
    throw "Skill is already installed. Re-run with -Force to upgrade while preserving local JSON and analysis YAML profiles: $destination"
}

[void](New-Item -ItemType Directory -Path $destinationRootFull -Force)
$transactionId = [guid]::NewGuid().ToString('N')
$staging = Join-Path $destinationRootFull ('.' + $skillName + '-staging-' + $transactionId)
$backup = Join-Path $destinationRootFull ('.' + $skillName + '-backup-' + $transactionId)
function Assert-InstallTarget {
    param([string]$Path)
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $separator = [System.IO.Path]::DirectorySeparatorChar
    foreach ($allowedRoot in @($destination, $staging, $backup)) {
        $allowed = [System.IO.Path]::GetFullPath($allowedRoot).TrimEnd('\', '/')
        if ($resolved.Equals($allowed, [System.StringComparison]::OrdinalIgnoreCase) -or
            $resolved.StartsWith($allowed + $separator, [System.StringComparison]::OrdinalIgnoreCase)) { return }
    }
    throw "Install mutation escaped the verified destinations: $resolved"
}
$preservedProfileBytes = $null
$existingProfile = Join-Path $destination 'config\local-profile.json'
if (Test-Path -LiteralPath $existingProfile -PathType Leaf) {
    $preservedProfileBytes = [System.IO.File]::ReadAllBytes($existingProfile)
}
$preservedAnalysisProfileBytes = $null
$existingAnalysisProfile = Join-Path $destination 'config\JNTracy.AnalysisProfile.yaml'
if (Test-Path -LiteralPath $existingAnalysisProfile -PathType Leaf) {
    $preservedAnalysisProfileBytes = [System.IO.File]::ReadAllBytes($existingAnalysisProfile)
}
$preservedStreamProfiles = @{}
foreach ($profileName in @('stream-local.json', 'stream-profile.json', 'stream-viewer-local.json')) {
    $profilePath = Join-Path $destination ('config\' + $profileName)
    if (Test-Path -LiteralPath $profilePath -PathType Leaf) {
        $preservedStreamProfiles[$profileName] = [System.IO.File]::ReadAllBytes($profilePath)
    }
}

try {
    Copy-Item -LiteralPath $sourceRoot -Destination $staging -Recurse
    $stagedDebugLog = Join-Path $staging 'debug.log'
    if (Test-Path -LiteralPath $stagedDebugLog) {
        Remove-Item -LiteralPath $stagedDebugLog -Force
    }
    Get-ChildItem -LiteralPath $staging -Recurse -Directory -Filter __pycache__ | Sort-Object FullName -Descending | ForEach-Object {
        Assert-InstallTarget $_.FullName
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
    }
    Get-ChildItem -LiteralPath $staging -Recurse -File -Filter *.pyc | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force
    }
    $stagedProfile = Join-Path $staging 'config\local-profile.json'
    if (Test-Path -LiteralPath $stagedProfile) {
        Remove-Item -LiteralPath $stagedProfile -Force
    }
    if ($null -ne $preservedProfileBytes) {
        [System.IO.File]::WriteAllBytes($stagedProfile, $preservedProfileBytes)
    }
    else {
        Copy-Item -LiteralPath (Join-Path $staging 'config\local-profile.example.json') -Destination $stagedProfile
    }
    $stagedAnalysisProfile = Join-Path $staging 'config\JNTracy.AnalysisProfile.yaml'
    if (Test-Path -LiteralPath $stagedAnalysisProfile) {
        Remove-Item -LiteralPath $stagedAnalysisProfile -Force
    }
    if ($null -ne $preservedAnalysisProfileBytes) {
        [System.IO.File]::WriteAllBytes($stagedAnalysisProfile, $preservedAnalysisProfileBytes)
    }
    else {
        Copy-Item -LiteralPath (Join-Path $staging 'config\JNTracy.AnalysisProfile.example.yaml') -Destination $stagedAnalysisProfile
    }

    foreach ($profileName in $preservedStreamProfiles.Keys) {
        [System.IO.File]::WriteAllBytes((Join-Path $staging ('config\' + $profileName)), $preservedStreamProfiles[$profileName])
    }

    if (Test-Path -LiteralPath $destination) {
        Assert-InstallTarget $destination
        Assert-InstallTarget $backup
        Move-Item -LiteralPath $destination -Destination $backup
    }
    Assert-InstallTarget $staging
    Assert-InstallTarget $destination
    Move-Item -LiteralPath $staging -Destination $destination
    if (Test-Path -LiteralPath $backup) {
        Assert-InstallTarget $backup
        Remove-Item -LiteralPath $backup -Recurse -Force
    }

    [ordered]@{
        installed = $true
        skill = $skillName
        source = $sourceRoot
        destination = $destination
        local_profile = Join-Path $destination 'config\local-profile.json'
        local_profile_preserved = ($null -ne $preservedProfileBytes)
        analysis_profile = Join-Path $destination 'config\JNTracy.AnalysisProfile.yaml'
        analysis_profile_preserved = ($null -ne $preservedAnalysisProfileBytes)
    } | ConvertTo-Json -Depth 5
}
catch {
    if (Test-Path -LiteralPath $staging) {
        Assert-InstallTarget $staging
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    if ((Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $destination)) {
        Assert-InstallTarget $backup
        Assert-InstallTarget $destination
        Move-Item -LiteralPath $backup -Destination $destination
    }
    throw
}
