[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string[]] $LegacySnapshots,
    [Parameter(Mandatory = $true)][string[]] $LegacyStreams,
    [Parameter(Mandatory = $true)][string[]] $SummarySnapshots,
    [Parameter(Mandatory = $true)][string[]] $SummaryStreams,
    [Parameter(Mandatory = $true)][string[]] $DetailSnapshots,
    [Parameter(Mandatory = $true)][string[]] $DetailStreams,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][string] $OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool] $Condition, [string] $Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }

foreach ($group in @($LegacySnapshots, $LegacyStreams, $SummarySnapshots, $SummaryStreams,
    $DetailSnapshots, $DetailStreams)) {
    Assert-Condition ($group.Count -eq 3) 'each N12 performance group requires exactly three captures'
}
foreach ($path in @($QueryExe, $AllowRoot) + $LegacySnapshots + $LegacyStreams +
    $SummarySnapshots + $SummaryStreams + $DetailSnapshots + $DetailStreams) {
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

function Assert-JobMode($Stats, [string] $Label, [string] $Mode) {
    Assert-Condition ([bool]$Stats.present) "$Label Job statistics absent"
    Assert-Condition ([UInt64]$Stats.counts.jobs -gt 0) "$Label Jobs absent"
    Assert-Condition ([UInt64]$Stats.counts.completed -gt 0) "$Label completed Jobs absent"
    Assert-Condition ([UInt64]$Stats.counts.managed -gt 0) "$Label Managed Jobs absent"
    Assert-Condition ([UInt64]$Stats.counts.burst -gt 0) "$Label Burst Jobs absent"
    if ($Mode -eq 'Legacy') {
        Assert-Condition ([int]$Stats.job_schema_version -eq 1) "$Label unexpectedly contains Job v2 stages"
        Assert-Condition ([UInt64]$Stats.counts.v2 -eq 0) "$Label Legacy contains Job v2 jobs"
        Assert-Condition ([UInt64]$Stats.counts.schedule_callstacks -eq 0) "$Label Legacy contains schedule callstacks"
        Assert-Condition ([UInt64]$Stats.counts.wait_callstacks -eq 0) "$Label Legacy contains wait callstacks"
    }
    else {
        Assert-Condition ([int]$Stats.job_schema_version -eq 2) "$Label Job schema 2 absent"
        Assert-Condition ([UInt64]$Stats.counts.v2 -gt 0) "$Label Job v2 stages absent"
        Assert-Condition ([UInt64]$Stats.counts.scheduler_steals -gt 0) "$Label scheduler steals absent"
        Assert-Condition ([UInt64]$Stats.counts.range_steal_slices -gt 0) "$Label range steals absent"
        Assert-Condition ([UInt64]$Stats.counts.wait_jobs -gt 0) "$Label Job waits absent"
        Assert-Condition (@($Stats.lanes).Count -gt 0) "$Label lanes absent"
        Assert-Condition ([bool]$Stats.quality.complete) "$Label Job v2 quality incomplete"
        Assert-Condition ([UInt64]$Stats.quality.missing_ready -eq 0) "$Label missing Ready"
        Assert-Condition ([UInt64]$Stats.quality.missing_queue -eq 0) "$Label missing QueueEnter"
        Assert-Condition ([UInt64]$Stats.quality.invalid_order -eq 0) "$Label invalid Job order"
        Assert-Condition ([UInt64]$Stats.latency.schedule_to_ready.count -gt 0) "$Label schedule-to-ready latency absent"
        Assert-Condition ([UInt64]$Stats.latency.queue_to_first_run.count -gt 0) "$Label queue-to-run latency absent"
        if ($Mode -eq 'Summary') {
            Assert-Condition ([UInt64]$Stats.counts.schedule_callstacks -eq 0) "$Label Summary contains schedule callstacks"
            Assert-Condition ([UInt64]$Stats.counts.wait_callstacks -eq 0) "$Label Summary contains wait callstacks"
        }
        else {
            Assert-Condition ([UInt64]$Stats.counts.schedule_callstacks -gt 0) "$Label Detail schedule callstacks absent"
            Assert-Condition ([UInt64]$Stats.counts.wait_callstacks -gt 0) "$Label Detail wait callstacks absent"
        }
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
        Assert-Condition ([int]$context.data.context.capture_config.job_schema -eq 2) "$Label capture context Job schema mismatch"
        Assert-Condition ([string]$context.data.context.capture_config.job_source -eq 'native-hooks-job-v2') "$Label Job source mismatch"
        $expectedMode = switch ($Mode) { 'Legacy' { 'legacy' } 'Summary' { 'summary' } 'Detail8' { 'detail' } }
        $expectedDepth = if ($Mode -eq 'Detail8') { 8 } else { 0 }
        Assert-Condition ([string]$context.data.context.capture_config.n12_job_mode -eq $expectedMode) "$Label requested N12 mode mismatch"
        Assert-Condition ([int]$context.data.context.capture_config.job_callstack_depth_requested -eq $expectedDepth) "$Label requested Job stack depth mismatch"
        Assert-Condition ([int]$context.data.context.capture_config.callstack.domains.job.effective -eq $expectedDepth) "$Label effective Job stack depth mismatch"

        $overview = Inspect $traceId 'trace.overview'
        $frames = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $frames -and [UInt64]$frames.count -gt 0) "$Label frame statistics absent"
        $stats = (Inspect $traceId 'job.statistics').data
        Assert-JobMode $stats $Label $Mode
        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Label validation failed"

        $snapshotResult = [ordered]@{
            label = $Label
            mode = $Mode
            frame_count = [UInt64]$frames.count
            frame_mean_ns = [double]$frames.mean_ns
            frame_p50_ns = [double]$frames.p50_ns
            frame_p95_ns = [double]$frames.p95_ns
            jobs = [UInt64]$stats.counts.jobs
            v2_jobs = [UInt64]$stats.counts.v2
            managed_jobs = [UInt64]$stats.counts.managed
            burst_jobs = [UInt64]$stats.counts.burst
            scheduler_steals = [UInt64]$stats.counts.scheduler_steals
            range_steal_slices = [UInt64]$stats.counts.range_steal_slices
            wait_jobs = [UInt64]$stats.counts.wait_jobs
            schedule_callstacks = [UInt64]$stats.counts.schedule_callstacks
            wait_callstacks = [UInt64]$stats.counts.wait_callstacks
            lane_count = @($stats.lanes).Count
            boundary_orphan = [UInt64]$stats.quality.capture_boundary_orphan
            boundary_truncated = [UInt64]$stats.quality.capture_boundary_truncated
            stream_bytes = [UInt64](Get-Item -LiteralPath $Stream).Length
            stream_bytes_per_frame = [Math]::Round((Get-Item -LiteralPath $Stream).Length / [double]$frames.count, 3)
            validation_errors = [UInt64]$validation.data.error_count
        }
    }
    finally { Close-Trace $opened.Id }

    $streamOpened = Open-Trace $Stream
    try {
        Assert-Condition ([bool]$streamOpened.Status.data.status.complete) "$Label stream is not complete"
        $streamStats = (Inspect $streamOpened.Id 'job.statistics').data
        Assert-JobMode $streamStats "$Label stream" $Mode
        Assert-Condition ([UInt64]$streamStats.counts.jobs -eq $snapshotResult.jobs) "$Label stream Job count mismatch"
        Assert-Condition ([UInt64]$streamStats.counts.v2 -eq $snapshotResult.v2_jobs) "$Label stream Job v2 count mismatch"
        Assert-Condition ([UInt64]$streamStats.counts.scheduler_steals -eq $snapshotResult.scheduler_steals) "$Label stream steal count mismatch"
        $snapshotResult.stream_complete = $true
        $snapshotResult.stream_fingerprint = [string]$streamOpened.Status.data.status.fingerprint
    }
    finally { Close-Trace $streamOpened.Id }
    return $snapshotResult
}

function Median([double[]] $Values) {
    $ordered = @($Values | Sort-Object)
    return $ordered[[int][Math]::Floor($ordered.Count / 2)]
}

function Summarize($Runs) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns','frame_p50_ns','frame_p95_ns','frame_count','jobs','v2_jobs',
        'managed_jobs','burst_jobs','scheduler_steals','range_steal_slices','wait_jobs','schedule_callstacks',
        'wait_callstacks','lane_count','stream_bytes','stream_bytes_per_frame')) {
        $result[$field + '_median'] = Median @($Runs | ForEach-Object { [double]$_[$field] })
    }
    return $result
}

function Compare-Metrics($Base, $Candidate) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns_median','frame_p50_ns_median','frame_p95_ns_median','stream_bytes_median','stream_bytes_per_frame_median')) {
        $baseline = [double]$Base[$field]
        $result[$field + '_percent'] = [Math]::Round((([double]$Candidate[$field] / $baseline) - 1.0) * 100.0, 3)
    }
    return $result
}

try {
    Assert-Condition ($process.Start()) 'failed to start Query MCP server'
    $script:process = $process
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n12-job-performance'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $legacy = @(); $summary = @(); $detail = @()
    for ($i = 0; $i -lt 3; ++$i) {
        $legacy += Measure-Trace $LegacySnapshots[$i] $LegacyStreams[$i] "Legacy-$($i + 1)" 'Legacy'
        $summary += Measure-Trace $SummarySnapshots[$i] $SummaryStreams[$i] "Summary-$($i + 1)" 'Summary'
        $detail += Measure-Trace $DetailSnapshots[$i] $DetailStreams[$i] "Detail8-$($i + 1)" 'Detail8'
    }
    $legacyMedian = Summarize $legacy
    $summaryMedian = Summarize $summary
    $detailMedian = Summarize $detail
    $summaryComparison = Compare-Metrics $legacyMedian $summaryMedian
    $detailComparison = Compare-Metrics $summaryMedian $detailMedian
    $summaryGatePassed = [double]$summaryComparison.frame_p95_ns_median_percent -le 3.0
    $result = [ordered]@{
        ok = $summaryGatePassed
        schema_version = '1.11.0'
        sample_count_per_group = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; stable_frames = 300; warmup_seconds = 45; capture_seconds = 10 }
        gate = [ordered]@{ summary_p95_limit_percent = 3.0; summary_p95_passed = $summaryGatePassed; detail8_is_bounded_diagnostic = $true }
        legacy = [ordered]@{ runs = $legacy; median = $legacyMedian }
        summary = [ordered]@{ runs = $summary; median = $summaryMedian }
        detail8 = [ordered]@{ runs = $detail; median = $detailMedian }
        summary_vs_legacy = $summaryComparison
        detail8_vs_summary = $detailComparison
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 80) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Compress -Depth 15
    Assert-Condition $summaryGatePassed "Summary P95 regression exceeds 3%: $($summaryComparison.frame_p95_ns_median_percent)%"
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
