[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TaskDirectory,
    [ValidateSet('StopFile', 'ManualCtrlC')][string]$StopSource = 'StopFile',
    [ValidateRange(1, 3600)][int]$WaitSeconds = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$taskRoot = [System.IO.Path]::GetFullPath($TaskDirectory)
$record = Read-JNJson -LiteralPath (Join-Path $taskRoot 'capture-process.json')
$pidValue = [int]$record.process.process_id
$startIdentity = [string]$record.process.start_time_utc

if (-not (Test-JNProcessIdentity -Id $pidValue -StartTimeUtc $startIdentity)) {
    $streamExists = Test-Path -LiteralPath $record.stream_path -PathType Leaf
    $size1 = 0L
    $size2 = 0L
    $stable = $false
    if ($streamExists) {
        $size1 = [int64](Get-Item -LiteralPath $record.stream_path).Length
        Start-Sleep -Milliseconds 1000
        $size2 = [int64](Get-Item -LiteralPath $record.stream_path).Length
        $stable = $size1 -eq $size2
    }
    $existingResult = [ordered]@{
        schema_version = 1
        stop_source = $StopSource
        requested_at_utc = Get-JNUtcNow
        observed_at_utc = Get-JNUtcNow
        process_id = $pidValue
        process_identity_match = $false
        registered_process_already_exited = $true
        graceful_exit_observed = $null
        timed_out = $false
        exit_code = $null
        stream_path = [string]$record.stream_path
        stream_size_bytes = $size2
        stream_stable = $stable
        stop_file_present = Test-Path -LiteralPath $record.stop_file -PathType Leaf
        completeness = 'requires_mcp_validation'
        reason = if ($streamExists -and $stable -and $size2 -gt 0) { 'capture_already_exited_requires_mcp_validation' } else { 'capture_already_exited_without_stable_stream' }
    }
    Write-JNJsonAtomic -LiteralPath (Join-Path $taskRoot 'capture-stop-result.json') -Value $existingResult
    $existingResult | ConvertTo-Json -Depth 20
    if (-not $streamExists -or -not $stable -or $size2 -le 0) { exit 2 }
    exit 0
}

$requestedAt = Get-JNUtcNow
if ($StopSource -eq 'StopFile') {
    if (-not (Test-Path -LiteralPath $record.stop_file -PathType Leaf)) {
        New-JNExclusiveMarker -LiteralPath $record.stop_file -Content ("requested " + $requestedAt)
    }
}

$process = Get-Process -Id $pidValue
$exited = $process.WaitForExit($WaitSeconds * 1000)
$exitCode = $null
if ($exited) {
    try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
}

$stable = $false
$size1 = 0L
$size2 = 0L
if (Test-Path -LiteralPath $record.stream_path -PathType Leaf) {
    $size1 = [int64](Get-Item -LiteralPath $record.stream_path).Length
    Start-Sleep -Milliseconds 1000
    $size2 = [int64](Get-Item -LiteralPath $record.stream_path).Length
    $stable = $size1 -eq $size2
}

$result = [ordered]@{
    schema_version = 1
    stop_source = $StopSource
    requested_at_utc = $requestedAt
    observed_at_utc = Get-JNUtcNow
    process_id = $pidValue
    process_identity_match = $true
    graceful_exit_observed = $exited
    timed_out = -not $exited
    exit_code = $exitCode
    stream_path = [string]$record.stream_path
    stream_size_bytes = $size2
    stream_stable = $stable
    stop_file_present = Test-Path -LiteralPath $record.stop_file -PathType Leaf
    completeness = 'requires_mcp_validation'
}
Write-JNJsonAtomic -LiteralPath (Join-Path $taskRoot 'capture-stop-result.json') -Value $result
$result | ConvertTo-Json -Depth 20
if (-not $exited) { exit 3 }
