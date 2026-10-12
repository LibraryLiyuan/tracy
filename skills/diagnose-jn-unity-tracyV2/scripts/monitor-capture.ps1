[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ResolvedProfile,
    [Parameter(Mandatory = $true)][string]$TaskDirectory,
    [switch]$ApplyProtection
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$profile = Read-JNJson -LiteralPath $ResolvedProfile
$taskRoot = [System.IO.Path]::GetFullPath($TaskDirectory)
$recordPath = Join-Path $taskRoot 'capture-process.json'
$record = Read-JNJson -LiteralPath $recordPath
$pidValue = [int]$record.process.process_id
$startIdentity = [string]$record.process.start_time_utc
$alive = Test-JNProcessIdentity -Id $pidValue -StartTimeUtc $startIdentity

$workingSet = 0L
if ($alive) {
    $workingSet = [int64](Get-Process -Id $pidValue).WorkingSet64
}

$streamSize = 0L
$streamWrite = $null
if (Test-Path -LiteralPath $record.stream_path -PathType Leaf) {
    $streamItem = Get-Item -LiteralPath $record.stream_path
    $streamSize = [int64]$streamItem.Length
    $streamWrite = $streamItem.LastWriteTimeUtc.ToString('o')
}

$rootPath = [System.IO.Path]::GetPathRoot([System.IO.Path]::GetFullPath([string]$record.stream_path))
$drive = New-Object System.IO.DriveInfo($rootPath)
$freeBytes = [int64]$drive.AvailableFreeSpace
$warnBytes = [int64]([double]$profile.local.protection.warn_free_disk_gib * 1GB)
$stopBytes = [int64]([double]$profile.local.protection.stop_free_disk_gib * 1GB)
$memoryLimit = [int64]([double]$profile.local.protection.capture_memory_limit_mib * 1MB)

$reasons = @()
if ($freeBytes -le $warnBytes) { $reasons += 'disk_warning' }
if ($freeBytes -le $stopBytes) { $reasons += 'disk_protective_stop' }
if ($workingSet -ge $memoryLimit) { $reasons += 'capture_memory_protective_stop' }
$protectionRequired = ($reasons -contains 'disk_protective_stop') -or ($reasons -contains 'capture_memory_protective_stop')

$stopRequested = Test-Path -LiteralPath $record.stop_file -PathType Leaf
$protectionApplied = $false
if ($ApplyProtection -and $protectionRequired -and -not $stopRequested -and $alive) {
    New-JNExclusiveMarker -LiteralPath $record.stop_file -Content ("protective_stop " + (Get-JNUtcNow))
    $stopRequested = $true
    $protectionApplied = $true
}

$result = [ordered]@{
    schema_version = 1
    observed_at_utc = Get-JNUtcNow
    process_id = $pidValue
    process_identity_match = $alive
    working_set_bytes = $workingSet
    stream_path = [string]$record.stream_path
    stream_size_bytes = $streamSize
    stream_last_write_utc = $streamWrite
    free_disk_bytes = $freeBytes
    protection_reasons = $reasons
    protection_required = $protectionRequired
    protection_applied = $protectionApplied
    stop_requested = $stopRequested
}
Write-JNJsonAtomic -LiteralPath (Join-Path $taskRoot 'capture-monitor-latest.json') -Value $result
$result | ConvertTo-Json -Depth 20
