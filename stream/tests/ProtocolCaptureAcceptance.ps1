[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ProducerExe,

    [Parameter(Mandatory = $true)]
    [string] $CaptureExe,

    [Parameter(Mandatory = $true)]
    [string] $ConverterExe,

    [Parameter(Mandatory = $true)]
    [string] $InspectorExe,

    [Parameter(Mandatory = $true)]
    [string] $QueryExe,

    [Parameter(Mandatory = $true)]
    [string] $OutputRoot,

    [int] $CaptureSeconds = 2,
    [int] $ReplayPort = 18087
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param(
        [bool] $Condition,
        [string] $Message
    )
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

foreach ($requiredPath in @($ProducerExe, $CaptureExe, $ConverterExe, $InspectorExe, $QueryExe, $OutputRoot)) {
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
}
Assert-Condition ($CaptureSeconds -ge 1 -and $CaptureSeconds -le 60) 'CaptureSeconds must be between 1 and 60'
Assert-Condition ($ReplayPort -ge 1 -and $ReplayPort -le 65535) 'ReplayPort is invalid'

$artifactRoot = Join-Path (Resolve-Path -LiteralPath $OutputRoot).Path ("stream-acceptance-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $artifactRoot)

$journalPath = Join-Path $artifactRoot 'capture.tracy-stream'
$directPath = Join-Path $artifactRoot 'direct.tracy'
$replayedPath = Join-Path $artifactRoot 'replayed.tracy'
$producerSeconds = $CaptureSeconds + 4
$producer = $null
$capture = $null
$crashProducer = $null
$crashCapture = $null

try {
    $producer = Start-Process -FilePath (Resolve-Path -LiteralPath $ProducerExe).Path `
        -ArgumentList ([string]$producerSeconds) `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $ProducerExe).Path) `
        -WindowStyle Hidden `
        -PassThru

    Start-Sleep -Milliseconds 300
    $capture = Start-Process -FilePath (Resolve-Path -LiteralPath $CaptureExe).Path `
        -ArgumentList @('-o', $directPath, '-j', $journalPath, '-s', [string]$CaptureSeconds, '-f') `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $CaptureExe).Path) `
        -WindowStyle Hidden `
        -PassThru

    $liveRevisions = [System.Collections.Generic.HashSet[UInt64]]::new()
    $liveWatermarks = [System.Collections.Generic.List[UInt64]]::new()
    $observedIncomplete = $false
    $pollDeadline = [DateTime]::UtcNow.AddSeconds($CaptureSeconds + 15)
    while (-not $capture.HasExited -and [DateTime]::UtcNow -lt $pollDeadline) {
        if (Test-Path -LiteralPath $journalPath) {
            try {
                $rawLiveInspect = (& $InspectorExe inspect $journalPath 2>$null) -join [Environment]::NewLine
                if (-not [string]::IsNullOrWhiteSpace($rawLiveInspect)) {
                    $liveInspect = $rawLiveInspect | ConvertFrom-Json
                    if ([bool]$liveInspect.recoverable_prefix) {
                        Assert-Condition ([UInt64]$liveInspect.valid_size -le [UInt64]$liveInspect.file_size) 'live reader exposed bytes beyond the observed file'
                        [void]$liveRevisions.Add([UInt64]$liveInspect.last_sequence)
                        $liveWatermarks.Add([UInt64]$liveInspect.watermark_ns)
                        if (-not [bool]$liveInspect.complete) {
                            $observedIncomplete = $true
                        }
                    }
                }
            }
            catch {
                # A scan may race a writer between stat and read. It is a
                # transient observation, not a published read view.
            }
        }
        Start-Sleep -Milliseconds 40
    }
    Assert-Condition ($capture.WaitForExit(10000)) 'capture did not exit after its configured duration'
    Assert-Condition ($capture.ExitCode -eq 0) "capture failed with exit code $($capture.ExitCode)"
    Assert-Condition ($liveRevisions.Count -ge 2) "live journal did not expose at least two committed revisions (observed $($liveRevisions.Count))"
    Assert-Condition $observedIncomplete 'live polling never observed an incomplete committed prefix'
    for ($i = 1; $i -lt $liveWatermarks.Count; ++$i) {
        Assert-Condition ($liveWatermarks[$i] -ge $liveWatermarks[$i - 1]) 'live watermark moved backwards'
    }

    $inspectJson = (& $InspectorExe inspect $journalPath | ConvertFrom-Json)
    Assert-Condition ($LASTEXITCODE -eq 0) "journal inspection failed with exit code $LASTEXITCODE"
    Assert-Condition ([string]$inspectJson.status -eq 'OK') "journal status is $($inspectJson.status)"
    Assert-Condition ([bool]$inspectJson.complete) 'journal does not have a clean SessionEnd'
    Assert-Condition ([UInt64]$inspectJson.record_count -gt 6) 'journal did not capture protocol frames'
    Assert-Condition ([UInt64]$inspectJson.file_size -eq [UInt64]$inspectJson.valid_size) 'journal has an invalid tail'

    & $ConverterExe -i $journalPath -o $replayedPath -p $ReplayPort -f
    Assert-Condition ($LASTEXITCODE -eq 0) "journal conversion failed with exit code $LASTEXITCODE"

    foreach ($trace in @($directPath, $replayedPath)) {
        & $QueryExe --doctor --trace $trace --allow-root $artifactRoot | Out-Null
        Assert-Condition ($LASTEXITCODE -eq 0) "tracy-query doctor rejected $trace"
    }

    $repositoryRoot = Split-Path (Split-Path $PSScriptRoot)
    $overviewRequest = Join-Path $repositoryRoot 'query\tests\requests\overview.json'
    Assert-Condition (Test-Path -LiteralPath $overviewRequest) "overview request not found: $overviewRequest"
    $directOverview = (& $QueryExe --allow-root $artifactRoot --trace $directPath --request $overviewRequest | ConvertFrom-Json)
    Assert-Condition ($LASTEXITCODE -eq 0 -and [bool]$directOverview.ok) 'direct overview failed'
    $replayedOverview = (& $QueryExe --allow-root $artifactRoot --trace $replayedPath --request $overviewRequest | ConvertFrom-Json)
    Assert-Condition ($LASTEXITCODE -eq 0 -and [bool]$replayedOverview.ok) 'replayed overview failed'

    $directCounts = $directOverview.data.trace.counts | ConvertTo-Json -Compress
    $replayedCounts = $replayedOverview.data.trace.counts | ConvertTo-Json -Compress
    Assert-Condition ($directCounts -eq $replayedCounts) 'direct/replayed trace counts differ'
    Assert-Condition ([string]$directOverview.data.trace.last_time_ns -eq [string]$replayedOverview.data.trace.last_time_ns) 'direct/replayed watermarks differ'
    Assert-Condition (
        ($directOverview.data.primary_frame_statistics | ConvertTo-Json -Compress) -eq
        ($replayedOverview.data.primary_frame_statistics | ConvertTo-Json -Compress)
    ) 'direct/replayed frame statistics differ'

    $mcpAcceptance = Join-Path $repositoryRoot 'query\tests\RealTpsMcpAcceptance.ps1'
    Assert-Condition (Test-Path -LiteralPath $mcpAcceptance) "MCP acceptance script not found: $mcpAcceptance"
    & $mcpAcceptance -QueryExe $QueryExe -BaselineTrace $directPath -CandidateTrace $replayedPath -AllowRoot $artifactRoot
    Assert-Condition ($LASTEXITCODE -eq 0) "MCP A/B acceptance failed with exit code $LASTEXITCODE"

    if ($null -ne $producer -and -not $producer.HasExited) {
        Assert-Condition ($producer.WaitForExit(10000)) 'clean-capture producer did not exit'
    }
    $producer = $null

    $crashJournalPath = Join-Path $artifactRoot 'forced-stop.tracy-stream'
    $crashReplayPath = Join-Path $artifactRoot 'forced-stop-replayed.tracy'
    $crashProducer = Start-Process -FilePath (Resolve-Path -LiteralPath $ProducerExe).Path `
        -ArgumentList '12' `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $ProducerExe).Path) `
        -WindowStyle Hidden `
        -PassThru
    Start-Sleep -Milliseconds 300
    $crashCapture = Start-Process -FilePath (Resolve-Path -LiteralPath $CaptureExe).Path `
        -ArgumentList @('-j', $crashJournalPath, '-s', '30', '-f') `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $CaptureExe).Path) `
        -WindowStyle Hidden `
        -PassThru

    $crashReady = $false
    $crashDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $crashCapture.HasExited -and [DateTime]::UtcNow -lt $crashDeadline) {
        if (Test-Path -LiteralPath $crashJournalPath) {
            try {
                $rawCrashInspect = (& $InspectorExe inspect $crashJournalPath 2>$null) -join [Environment]::NewLine
                if (-not [string]::IsNullOrWhiteSpace($rawCrashInspect)) {
                    $candidateCrashInspect = $rawCrashInspect | ConvertFrom-Json
                    if ([bool]$candidateCrashInspect.recoverable_prefix -and [UInt64]$candidateCrashInspect.record_count -ge 15) {
                        $crashReady = $true
                        break
                    }
                }
            }
            catch {
                # Retry a stat/read race while the recorder is appending.
            }
        }
        Start-Sleep -Milliseconds 20
    }
    Assert-Condition $crashReady 'forced-stop capture did not publish enough protocol records'
    Stop-Process -Id $crashCapture.Id -Force
    $crashCapture.WaitForExit()
    $crashCapture = $null

    $rawCrashFinal = (& $InspectorExe inspect $crashJournalPath 2>$null) -join [Environment]::NewLine
    $crashInspect = $rawCrashFinal | ConvertFrom-Json
    Assert-Condition ([bool]$crashInspect.recoverable_prefix) 'forced-stop journal has no recoverable prefix'
    Assert-Condition (-not [bool]$crashInspect.complete) 'forced-stop journal unexpectedly has SessionEnd'
    Assert-Condition ([UInt64]$crashInspect.record_count -ge 15) 'forced-stop journal lost its previously published prefix'

    $crashReplayPort = $ReplayPort + 1
    Assert-Condition ($crashReplayPort -le 65535) 'ReplayPort leaves no port for forced-stop replay'
    & $ConverterExe -i $crashJournalPath -o $crashReplayPath -p $crashReplayPort -f
    Assert-Condition ($LASTEXITCODE -eq 0) "forced-stop valid-prefix conversion failed with exit code $LASTEXITCODE"
    & $QueryExe --doctor --trace $crashReplayPath --allow-root $artifactRoot | Out-Null
    Assert-Condition ($LASTEXITCODE -eq 0) 'tracy-query doctor rejected the forced-stop replay'

    [Console]::Out.WriteLine("RESULT=PASS ARTIFACT_ROOT=$artifactRoot JOURNAL_RECORDS=$($inspectJson.record_count) JOURNAL_BYTES=$($inspectJson.file_size) LIVE_REVISIONS=$($liveRevisions.Count) FORCED_STOP_RECORDS=$($crashInspect.record_count)")
}
finally {
    if ($null -ne $crashCapture -and -not $crashCapture.HasExited) {
        Stop-Process -Id $crashCapture.Id -Force
        $crashCapture.WaitForExit()
    }
    if ($null -ne $crashProducer -and -not $crashProducer.HasExited) {
        Stop-Process -Id $crashProducer.Id -Force
        $crashProducer.WaitForExit()
    }
    if ($null -ne $capture -and -not $capture.HasExited) {
        Stop-Process -Id $capture.Id -Force
        $capture.WaitForExit()
    }
    if ($null -ne $producer -and -not $producer.HasExited) {
        if (-not $producer.WaitForExit(10000)) {
            Stop-Process -Id $producer.Id -Force
            $producer.WaitForExit()
        }
    }
}
