[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string[]] $LegacySnapshots,
    [Parameter(Mandatory = $true)][string[]] $LegacyStreams,
    [Parameter(Mandatory = $true)][string[]] $SummarySnapshots,
    [Parameter(Mandatory = $true)][string[]] $SummaryStreams,
    [Parameter(Mandatory = $true)][string[]] $PassAndPhaseSnapshots,
    [Parameter(Mandatory = $true)][string[]] $PassAndPhaseStreams,
    [Parameter(Mandatory = $true)][string[]] $DetailSnapshots,
    [Parameter(Mandatory = $true)][string[]] $DetailStreams,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][string] $OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool] $Condition, [string] $Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }

$groups = @($LegacySnapshots, $LegacyStreams, $SummarySnapshots, $SummaryStreams,
    $PassAndPhaseSnapshots, $PassAndPhaseStreams, $DetailSnapshots, $DetailStreams)
foreach ($group in $groups) { Assert-Condition ($group.Count -eq 3) 'each N13 performance group requires exactly three captures' }
foreach ($path in @($QueryExe, $AllowRoot) + $LegacySnapshots + $LegacyStreams +
    $SummarySnapshots + $SummaryStreams + $PassAndPhaseSnapshots + $PassAndPhaseStreams +
    $DetailSnapshots + $DetailStreams) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $QueryExe
$start.Arguments = "--mcp --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$start.WorkingDirectory = $AllowRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$script:nextId = 1
$script:opened = @()

function Send-Rpc([string] $Method, [hashtable] $Params = @{}) {
    $id = $script:nextId++
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } |
        ConvertTo-Json -Compress -Depth 60
    $script:process.StandardInput.WriteLine($payload)
    $script:process.StandardInput.Flush()
    $read = $script:process.StandardOutput.ReadLineAsync()
    Assert-Condition ($read.Wait(600000)) "MCP timeout: $Method"
    Assert-Condition ($null -ne $read.Result) "MCP stdout closed: $Method"
    $response = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$response.id -eq [string]$id) "unexpected MCP response id: $Method"
    return $response
}

function Tool([string] $Name, [hashtable] $Arguments = @{}) {
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result.structuredContent) "$Name omitted structuredContent"
    $value = $response.result.structuredContent
    Assert-Condition (-not [bool]$response.result.isError) "$Name returned MCP error: $($value | ConvertTo-Json -Compress -Depth 12)"
    Assert-Condition ([bool]$value.ok) "$Name query failed"
    return $value
}

function Wait-Ready([string] $TraceId) {
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return $status }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached $state" }
        Start-Sleep -Milliseconds 100
    }
    throw "trace not ready: $TraceId"
}

function Open-Trace([string] $Path) {
    $opened = Tool 'tracy_trace_open' @{ path = $Path }
    $id = [string]$opened.data.trace_id
    $script:opened += $id
    $status = Wait-Ready $id
    return [pscustomobject]@{ Id = $id; Status = $status }
}

function Close-Trace([string] $TraceId) {
    [void](Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:opened = @($script:opened | Where-Object { $_ -ne $TraceId })
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{}) {
    return Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Expected-Profile([string] $Mode) {
    switch ($Mode) {
        'Legacy' { return 'Legacy' }
        'Summary' { return 'Summary' }
        'PassAndPhase' { return 'PassAndPhase' }
        'Detail8' { return 'Detail' }
    }
}

function Assert-IoMode($Context, $Producer, $IoStatistics, [string] $Label, [string] $Mode) {
    $expectedProfile = Expected-Profile $Mode
    $expectedDepth = if ($Mode -eq 'Detail8') { 8 } else { 0 }
    $expectedEnabled = $Mode -in @('PassAndPhase', 'Detail8')
    Assert-Condition ([string]$Context.data.context.capture_config.profile -eq $expectedProfile) "$Label capture profile mismatch"
    Assert-Condition ([int]$Context.data.context.capture_config.callstack.domains.io.requested -eq $expectedDepth) "$Label requested I/O depth mismatch"
    Assert-Condition ([int]$Context.data.context.capture_config.callstack.domains.io.effective -eq $expectedDepth) "$Label effective I/O depth mismatch"
    Assert-Condition ([bool]$Producer.data.producer.enabled -eq $expectedEnabled) "$Label producer enabled state mismatch"
    Assert-Condition ([bool]$Producer.data.producer.effective -eq $expectedEnabled) "$Label producer effective state mismatch"
    Assert-Condition ([UInt64]$Producer.data.producer.counters.emitted -eq 0) "$Label stable window unexpectedly emitted structured I/O"
    Assert-Condition ([UInt64]$Producer.data.producer.counters.dropped -eq 0) "$Label dropped structured I/O"
    Assert-Condition ([UInt64]$Producer.data.producer.counters.overflow -eq 0) "$Label overflowed structured I/O"
    Assert-Condition (-not [bool]$IoStatistics.data.present) "$Label must not advertise I/O capability for a real-zero stable window"
    if ($expectedEnabled) {
        Assert-Condition ([string]$Producer.data.producer.state -eq 'real_zero') "$Label enabled stable window is not real_zero"
    } else {
        Assert-Condition ([string]$Producer.data.producer.reason -eq 'capture_profile_or_command_line_disabled') "$Label disabled reason mismatch"
    }
}

function Measure-Trace([string] $Snapshot, [string] $Stream, [string] $Label, [string] $Mode) {
    $opened = Open-Trace $Snapshot
    try {
        $traceId = $opened.Id
        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Label capture context incomplete"
        Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context invalid"
        Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Label graphics API mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Label gfx-jobs mismatch"
        Assert-Condition ([string]$context.data.context.workload.scene -eq 'taijibase_constructedarmor_main_01') "$Label scene mismatch"
        $producer = Inspect $traceId 'producer.get' @{ key = 'io.structured' }
        $ioStatistics = Inspect $traceId 'io.statistics'
        Assert-IoMode $context $producer $ioStatistics $Label $Mode

        $overview = Inspect $traceId 'trace.overview'
        $frames = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $frames -and [UInt64]$frames.count -gt 0) "$Label frame statistics absent"
        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Label validation failed"

        $result = [ordered]@{
            label = $Label
            mode = $Mode
            frame_count = [UInt64]$frames.count
            frame_mean_ns = [double]$frames.mean_ns
            frame_p50_ns = [double]$frames.p50_ns
            frame_p95_ns = [double]$frames.p95_ns
            producer_enabled = [bool]$producer.data.producer.enabled
            producer_state = [string]$producer.data.producer.state
            producer_emitted = [UInt64]$producer.data.producer.counters.emitted
            io_present = [bool]$ioStatistics.data.present
            io_callstack_depth = [int]$context.data.context.capture_config.callstack.domains.io.effective
            stream_bytes = [UInt64](Get-Item -LiteralPath $Stream).Length
            stream_bytes_per_frame = [Math]::Round((Get-Item -LiteralPath $Stream).Length / [double]$frames.count, 3)
            validation_errors = [UInt64]$validation.data.error_count
        }
    }
    finally { Close-Trace $opened.Id }

    $streamOpened = Open-Trace $Stream
    try {
        Assert-Condition ([bool]$streamOpened.Status.data.status.complete) "$Label stream is not complete"
        $streamContext = Inspect $streamOpened.Id 'capture.context'
        $streamProducer = Inspect $streamOpened.Id 'producer.get' @{ key = 'io.structured' }
        $streamIo = Inspect $streamOpened.Id 'io.statistics'
        Assert-IoMode $streamContext $streamProducer $streamIo "$Label stream" $Mode
        Assert-Condition ([UInt64]$streamProducer.data.producer.counters.emitted -eq $result.producer_emitted) "$Label stream producer count mismatch"
        $result.stream_complete = $true
        $result.stream_fingerprint = [string]$streamOpened.Status.data.status.fingerprint
    }
    finally { Close-Trace $streamOpened.Id }
    return $result
}

function Median([double[]] $Values) {
    $ordered = @($Values | Sort-Object)
    return $ordered[[int][Math]::Floor($ordered.Count / 2)]
}

function Summarize($Runs) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns', 'frame_p50_ns', 'frame_p95_ns', 'frame_count',
        'stream_bytes', 'stream_bytes_per_frame')) {
        $result[$field + '_median'] = Median @($Runs | ForEach-Object { [double]$_[$field] })
    }
    return $result
}

function Compare-Metrics($Base, $Candidate) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns_median', 'frame_p50_ns_median', 'frame_p95_ns_median',
        'stream_bytes_median', 'stream_bytes_per_frame_median')) {
        $baseline = [double]$Base[$field]
        $result[$field + '_percent'] = [Math]::Round((([double]$Candidate[$field] / $baseline) - 1.0) * 100.0, 3)
    }
    return $result
}

try {
    Assert-Condition ($process.Start()) 'failed to start Query MCP server'
    $script:process = $process
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n13-file-io-performance'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $legacy = @(); $summary = @(); $pass = @(); $detail = @()
    for ($i = 0; $i -lt 3; ++$i) {
        $legacy += Measure-Trace $LegacySnapshots[$i] $LegacyStreams[$i] "Legacy-$($i + 1)" 'Legacy'
        $summary += Measure-Trace $SummarySnapshots[$i] $SummaryStreams[$i] "Summary-$($i + 1)" 'Summary'
        $pass += Measure-Trace $PassAndPhaseSnapshots[$i] $PassAndPhaseStreams[$i] "PassAndPhase-$($i + 1)" 'PassAndPhase'
        $detail += Measure-Trace $DetailSnapshots[$i] $DetailStreams[$i] "Detail8-$($i + 1)" 'Detail8'
    }
    $legacyMedian = Summarize $legacy
    $summaryMedian = Summarize $summary
    $passMedian = Summarize $pass
    $detailMedian = Summarize $detail
    $summaryVsLegacy = Compare-Metrics $legacyMedian $summaryMedian
    $passVsLegacy = Compare-Metrics $legacyMedian $passMedian
    $detailVsPass = Compare-Metrics $passMedian $detailMedian
    $passGate = [double]$passVsLegacy.frame_p95_ns_median_percent -le 3.0
    $result = [ordered]@{
        ok = $passGate
        schema_version = '1.12.0'
        sample_count_per_group = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; stable_frames = 300; warmup_seconds = 45; capture_seconds = 10 }
        gate = [ordered]@{ pass_and_phase_p95_limit_percent = 3.0; pass_and_phase_p95_passed = $passGate; detail8_is_bounded_diagnostic = $true }
        legacy = [ordered]@{ runs = $legacy; median = $legacyMedian }
        summary = [ordered]@{ runs = $summary; median = $summaryMedian }
        pass_and_phase = [ordered]@{ runs = $pass; median = $passMedian }
        detail8 = [ordered]@{ runs = $detail; median = $detailMedian }
        summary_vs_legacy = $summaryVsLegacy
        pass_and_phase_vs_legacy = $passVsLegacy
        detail8_vs_pass_and_phase = $detailVsPass
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 80) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Compress -Depth 16
    Assert-Condition $passGate "PassAndPhase P95 regression exceeds 3%: $($passVsLegacy.frame_p95_ns_median_percent)%"
}
finally {
    foreach ($traceId in @($script:opened)) {
        if ($process -and -not $process.HasExited) { try { [void](Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($process -and -not $process.HasExited) {
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $process.WaitForExit() }
    }
    if ($process) { $process.Dispose() }
}
