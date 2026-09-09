param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [long[]]$Counts = @(10000,100000,1506514),
    [ValidateSet('table','streamed-statistics','neutral-cache','candidate-cache','native-manager')][string]$Mode = 'table',
    [ValidateRange(64,4096)][int]$MemoryLimitMiB = 512,
    [int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$cacheExe = (Resolve-Path -LiteralPath $Executable).Path
$cacheOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $cacheOutput) { throw 'Choose a new scale evidence directory' }
New-Item -ItemType Directory -Path $cacheOutput | Out-Null
$cacheResults = @()
foreach ($cacheCount in $Counts) {
    $cacheStdout = Join-Path $cacheOutput "$cacheCount.stdout.json"
    $cacheStderr = Join-Path $cacheOutput "$cacheCount.stderr.log"
    $cacheModeArgument = switch ($Mode) { 'table' { '--scale' } 'streamed-statistics' { '--scale-streamed' } 'neutral-cache' { '--scale-cache' } 'candidate-cache' { '--scale-policy' } 'native-manager' { '--scale-native' } }
    $cacheProcess = Start-Process -FilePath $cacheExe -ArgumentList @($cacheModeArgument, $cacheCount.ToString()) -WindowStyle Hidden -PassThru -RedirectStandardOutput $cacheStdout -RedirectStandardError $cacheStderr
    $cacheClock = [Diagnostics.Stopwatch]::StartNew()
    $cachePeakPrivate = 0L
    $cachePeakWorking = 0L
    $cacheGuard = ''
    while (-not $cacheProcess.HasExited) {
        $cacheProcess.Refresh()
        $cachePeakPrivate = [Math]::Max($cachePeakPrivate, $cacheProcess.PrivateMemorySize64)
        $cachePeakWorking = [Math]::Max($cachePeakWorking, $cacheProcess.PeakWorkingSet64)
        if ($cachePeakPrivate -gt ($MemoryLimitMiB * 1MB) -or $cachePeakWorking -gt ($MemoryLimitMiB * 1MB)) { $cacheGuard = 'component_memory_guard' }
        if ($cacheClock.Elapsed.TotalSeconds -gt $TimeoutSeconds) { $cacheGuard = 'component_timeout' }
        if ($cacheGuard) {
            if (-not $cacheProcess.HasExited) { $cacheProcess.Kill() }
            break
        }
        Start-Sleep -Milliseconds 100
    }
    $cacheProcess.WaitForExit()
    $cacheClock.Stop()
    $cacheResult = [pscustomobject]@{
        records=$cacheCount; mode=$Mode; elapsed_seconds=$cacheClock.Elapsed.TotalSeconds; memory_limit_mib=$MemoryLimitMiB
        peak_private_bytes=$cachePeakPrivate; peak_working_set_bytes=$cachePeakWorking
        process_memory_measurement='sampled_process_private_and_os_peak_working_set'
        exit_code=$cacheProcess.ExitCode; guard=$cacheGuard
        exe_sha256=(Get-FileHash -LiteralPath $cacheExe -Algorithm SHA256).Hash
        stdout=$cacheStdout; stderr=$cacheStderr
    }
    $cacheResults += $cacheResult
    $cacheResults | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $cacheOutput 'process-metrics.json') -Encoding utf8
    $cacheResult | ConvertTo-Json -Compress
    if ($cacheGuard -or $cacheProcess.ExitCode -ne 0) { throw 'Scale test failed; no next size will run' }
    $cacheVerified = Get-Content -LiteralPath $cacheStdout -Raw | ConvertFrom-Json
    if ($cacheVerified.verified -ne $true -or $cacheVerified.records -ne $cacheCount) { throw 'Scale content verification missing; no next size will run' }
}
