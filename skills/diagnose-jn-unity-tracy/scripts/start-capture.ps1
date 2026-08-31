[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ResolvedProfile,
    [Parameter(Mandatory = $true)][string]$TaskDirectory,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$TaskId,
    [string]$Address = '',
    [int]$Port = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$profile = Read-JNJson -LiteralPath $ResolvedProfile
$taskRoot = New-JNTaskDirectories -TaskDirectory $TaskDirectory
$captureRecord = Join-Path $taskRoot 'capture-process.json'
if (Test-Path -LiteralPath $captureRecord -PathType Leaf) {
    throw "Capture process record already exists: $captureRecord"
}

$effectiveAddress = if ($Address) { $Address } else { [string]$profile.project.capture.address }
$effectivePort = if ($Port -gt 0) { $Port } else { [int]$profile.project.capture.port }
$drainSeconds = [int]$profile.project.capture.drain_idle_seconds
$captureExe = [string]$profile.toolchain.capture.path
$streamPath = Join-Path $taskRoot ($TaskId + '.tracy-stream')
$stopPath = Join-Path $taskRoot ($TaskId + '.stop')
$stdoutPath = Join-Path $taskRoot 'logs\capture.stdout.log'
$stderrPath = Join-Path $taskRoot 'logs\capture.stderr.log'

foreach ($path in @($streamPath, $stopPath, $stdoutPath, $stderrPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite existing capture artifact: $path"
    }
}

$arguments = @(
    '-a', $effectiveAddress,
    '-p', [string]$effectivePort,
    '-j', $streamPath,
    '-d', [string]$drainSeconds,
    '-x', $stopPath
)
$argumentString = Join-JNProcessArguments -Values $arguments
$process = Start-Process -FilePath $captureExe -ArgumentList $argumentString -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
$identity = Get-JNProcessIdentity -Id $process.Id

$record = [ordered]@{
    schema_version = 1
    task_id = $TaskId
    state = 'recording'
    started_at_utc = Get-JNUtcNow
    process = $identity
    executable = Get-JNFileIdentity -LiteralPath $captureExe
    address = $effectiveAddress
    port = $effectivePort
    drain_idle_seconds = $drainSeconds
    arguments = $arguments
    stream_path = $streamPath
    stop_file = $stopPath
    stdout_log = $stdoutPath
    stderr_log = $stderrPath
}
Write-JNJsonAtomic -LiteralPath $captureRecord -Value $record
$record | ConvertTo-Json -Depth 20
