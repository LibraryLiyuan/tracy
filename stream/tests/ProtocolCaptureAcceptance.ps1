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

function Read-JournalPayloadByte {
    param(
        [string] $Path,
        [UInt64] $RecordOffset,
        [UInt64] $PayloadOffset = 0
    )
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $stream.Position = $RecordOffset + 48 + $PayloadOffset
        return $stream.ReadByte()
    }
    finally {
        $stream.Dispose()
    }
}

function Read-JournalPayloadUInt32 {
    param(
        [string] $Path,
        [UInt64] $RecordOffset,
        [UInt64] $PayloadOffset
    )
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = $null
    try {
        $stream.Position = $RecordOffset + 48 + $PayloadOffset
        $reader = [System.IO.BinaryReader]::new($stream)
        return $reader.ReadUInt32()
    }
    finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        }
        else {
            $stream.Dispose()
        }
    }
}

foreach ($requiredPath in @($ProducerExe, $CaptureExe, $ConverterExe, $InspectorExe, $QueryExe, $OutputRoot)) {
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
}
Assert-Condition ($CaptureSeconds -ge 1 -and $CaptureSeconds -le 60) 'CaptureSeconds must be between 1 and 60'
Assert-Condition ($ReplayPort -ge 1 -and $ReplayPort -le 65533) 'ReplayPort must leave two ports for drain and recovery replay'

$artifactRoot = Join-Path (Resolve-Path -LiteralPath $OutputRoot).Path ("stream-acceptance-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $artifactRoot)

$journalPath = Join-Path $artifactRoot 'capture.tracy-stream'
$directPath = Join-Path $artifactRoot 'direct.tracy'
$replayedPath = Join-Path $artifactRoot 'replayed.tracy'
$producerSeconds = $CaptureSeconds + 4
$producer = $null
$capture = $null
$drainProducer = $null
$drainCapture = $null
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

    $inspectRaw = (& $InspectorExe inspect $journalPath) -join [Environment]::NewLine
    $inspectExitCode = $LASTEXITCODE
    Assert-Condition ($inspectExitCode -eq 0) "journal inspection failed with exit code $inspectExitCode"
    $inspectJson = $inspectRaw | ConvertFrom-Json
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

    $drainJournalPath = Join-Path $artifactRoot 'protocol-only-drain.tracy-stream'
    $drainReplayPath = Join-Path $artifactRoot 'protocol-only-drain-replayed.tracy'
    $drainStopPath = Join-Path $artifactRoot 'protocol-only-drain.stop'
    $drainProducer = Start-Process -FilePath (Resolve-Path -LiteralPath $ProducerExe).Path `
        -ArgumentList ([string]($CaptureSeconds + 20)) `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $ProducerExe).Path) `
        -WindowStyle Hidden `
        -PassThru
    Start-Sleep -Milliseconds 300
    $drainCapture = Start-Process -FilePath (Resolve-Path -LiteralPath $CaptureExe).Path `
        -ArgumentList @('-j', $drainJournalPath, '-x', $drainStopPath, '-f') `
        -WorkingDirectory (Split-Path (Resolve-Path -LiteralPath $CaptureExe).Path) `
        -WindowStyle Hidden `
        -PassThru

    Start-Sleep -Seconds $CaptureSeconds
    Assert-Condition (-not $drainCapture.HasExited) 'stop-file capture exited before the marker was created'
    [IO.File]::WriteAllText($drainStopPath, 'stop', [Text.UTF8Encoding]::new($false))
    Assert-Condition ($drainCapture.WaitForExit(($CaptureSeconds + 15) * 1000)) 'protocol-only capture did not finish its definition drain'
    Assert-Condition ($drainCapture.ExitCode -eq 0) "protocol-only capture failed with exit code $($drainCapture.ExitCode)"
    Assert-Condition (-not $drainProducer.HasExited) 'protocol-only producer exited instead of continuing after recorder disconnect'
    Assert-Condition (Test-Path -LiteralPath $drainStopPath -PathType Leaf) 'stop-file marker was unexpectedly removed'
    $drainCapture = $null

    $drainInspectRaw = (& $InspectorExe inspect $drainJournalPath --records 1000000) -join [Environment]::NewLine
    $drainInspectExitCode = $LASTEXITCODE
    Assert-Condition ($drainInspectExitCode -eq 0) 'protocol-only journal inspection failed'
    $drainInspect = $drainInspectRaw | ConvertFrom-Json
    Assert-Condition ([bool]$drainInspect.complete) 'protocol-only journal does not have a clean SessionEnd'
    $drainRecords = @($drainInspect.records)
    Assert-Condition ($drainRecords.Count -eq [UInt64]$drainInspect.record_count) 'protocol-only journal record enumeration was truncated'
    $sessionRecords = @($drainRecords | Where-Object { [string]$_.type -eq 'SessionBegin' })
    Assert-Condition ($sessionRecords.Count -eq 1) "expected one SessionBegin, found $($sessionRecords.Count)"
    $sessionVersion = Read-JournalPayloadByte -Path $drainJournalPath -RecordOffset ([UInt64]$sessionRecords[0].offset)
    Assert-Condition ($sessionVersion -eq 2) "ProtocolOnly SessionBegin version is $sessionVersion instead of 2"
    $sessionFlags = Read-JournalPayloadUInt32 -Path $drainJournalPath -RecordOffset ([UInt64]$sessionRecords[0].offset) -PayloadOffset 16
    Assert-Condition (($sessionFlags -band 1) -ne 0) "ProtocolOnly SessionBegin omitted the deferred-symbol-expansion flag"
    $drainMarkers = @($drainRecords | Where-Object {
        (([UInt32]$_.flags -band 32) -ne 0) -and (([UInt32]$_.flags -band 4) -eq 0)
    })
    Assert-Condition ($drainMarkers.Count -eq 1) "expected one protocol drain marker, found $($drainMarkers.Count)"
    $drainMarker = $drainMarkers[0]
    Assert-Condition ([UInt64]$drainMarker.payload_size -eq 5) "protocol drain marker payload is $($drainMarker.payload_size) bytes instead of 5"
    $drainControl = Read-JournalPayloadByte -Path $drainJournalPath -RecordOffset ([UInt64]$drainMarker.offset)
    Assert-Condition ($drainControl -eq 4) "protocol drain marker payload is $drainControl instead of 4"
    $recordedQueryWindow = Read-JournalPayloadUInt32 -Path $drainJournalPath -RecordOffset ([UInt64]$drainMarker.offset) -PayloadOffset 1
    Assert-Condition ($recordedQueryWindow -ge 1 -and $recordedQueryWindow -le 8192) "recorded server-query window is outside [1, 8192]: $recordedQueryWindow"

    $disconnectRecords = @($drainRecords | Where-Object {
        ([UInt64]$_.sequence -gt [UInt64]$drainMarker.sequence) -and
        ([string]$_.type -eq 'ServerToClient') -and
        (([UInt32]$_.flags -band 4) -ne 0) -and
        ([UInt64]$_.payload_size -eq 13)
    })
    Assert-Condition ($disconnectRecords.Count -ge 1) 'protocol drain did not record a server query after its marker'
    $disconnectType = Read-JournalPayloadByte -Path $drainJournalPath -RecordOffset ([UInt64]$disconnectRecords[0].offset)
    Assert-Condition ($disconnectType -eq 9) "first post-drain server query is $disconnectType instead of ServerQueryDisconnect (9)"

    [UInt64]$postDrainClientBytes = 0
    foreach ($record in $drainRecords) {
        if ([UInt64]$record.sequence -gt [UInt64]$drainMarker.sequence -and [string]$record.type -eq 'ClientToServer') {
            $postDrainClientBytes += [UInt64]$record.payload_size
        }
    }
    Assert-Condition ($postDrainClientBytes -lt 32MB) "protocol drain accepted $postDrainClientBytes client bytes after disconnect"

    $drainReplayPort = $ReplayPort + 1
    & $ConverterExe -i $drainJournalPath -o $drainReplayPath -p $drainReplayPort -f
    Assert-Condition ($LASTEXITCODE -eq 0) 'protocol-only drain conversion failed'
    foreach ($trace in @($drainJournalPath, $drainReplayPath)) {
        & $QueryExe --doctor --trace $trace --allow-root $artifactRoot | Out-Null
        Assert-Condition ($LASTEXITCODE -eq 0) "tracy-query doctor rejected protocol drain artifact $trace"
    }

    Stop-Process -Id $drainProducer.Id -Force
    $drainProducer.WaitForExit()
    $drainProducer = $null

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

    $crashReplayPort = $ReplayPort + 2
    & $ConverterExe -i $crashJournalPath -o $crashReplayPath -p $crashReplayPort -f
    Assert-Condition ($LASTEXITCODE -eq 0) "forced-stop valid-prefix conversion failed with exit code $LASTEXITCODE"
    & $QueryExe --doctor --trace $crashReplayPath --allow-root $artifactRoot | Out-Null
    Assert-Condition ($LASTEXITCODE -eq 0) 'tracy-query doctor rejected the forced-stop replay'

    [Console]::Out.WriteLine("RESULT=PASS ARTIFACT_ROOT=$artifactRoot JOURNAL_RECORDS=$($inspectJson.record_count) JOURNAL_BYTES=$($inspectJson.file_size) LIVE_REVISIONS=$($liveRevisions.Count) DRAIN_RECORDS=$($drainInspect.record_count) POST_DRAIN_CLIENT_BYTES=$postDrainClientBytes FORCED_STOP_RECORDS=$($crashInspect.record_count)")
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
    if ($null -ne $drainCapture -and -not $drainCapture.HasExited) {
        Stop-Process -Id $drainCapture.Id -Force
        $drainCapture.WaitForExit()
    }
    if ($null -ne $drainProducer -and -not $drainProducer.HasExited) {
        Stop-Process -Id $drainProducer.Id -Force
        $drainProducer.WaitForExit()
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
