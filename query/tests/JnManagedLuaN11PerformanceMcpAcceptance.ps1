[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string[]] $OffSnapshots,
    [Parameter(Mandatory = $true)][string[]] $OffStreams,
    [Parameter(Mandatory = $true)][string[]] $SummarySnapshots,
    [Parameter(Mandatory = $true)][string[]] $SummaryStreams,
    [Parameter(Mandatory = $true)][string[]] $DetailSnapshots,
    [Parameter(Mandatory = $true)][string[]] $DetailStreams,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][string] $OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool] $Condition, [string] $Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }

foreach ($group in @($OffSnapshots, $OffStreams, $SummarySnapshots, $SummaryStreams,
    $DetailSnapshots, $DetailStreams)) {
    Assert-Condition ($group.Count -eq 3) 'each N11 performance group requires exactly three captures'
}
foreach ($path in @($QueryExe, $AllowRoot) + $OffSnapshots + $OffStreams +
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
        if ($state -eq 'ready') { return }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached $state" }
        Start-Sleep -Milliseconds 100
    }
    throw "trace not ready: $TraceId"
}

function Open-Trace([string] $Path) {
    $opened = Tool 'tracy_trace_open' @{ path = $Path }
    $id = [string]$opened.data.trace_id
    $script:opened += $id
    Wait-Ready $id
    return $id
}

function Close-Trace([string] $TraceId) {
    [void](Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:opened = @($script:opened | Where-Object { $_ -ne $TraceId })
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{}) {
    return Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Measure-Trace([string] $Snapshot, [string] $Stream, [string] $Label, [string] $Profile) {
    $traceId = Open-Trace $Snapshot
    try {
        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present) "$Label capture context absent"
        Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context invalid"
        Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Label graphics API mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Label gfx-jobs mismatch"
        Assert-Condition ([string]$context.data.context.capture_config.n11_profile -eq $Profile) "$Label N11 profile mismatch"
        $detail = $Profile -eq 'Detail'
        $sampling = $Profile -ne 'Off'
        Assert-Condition ([bool]$context.data.context.capture_config.managed_gc_sampling -eq $sampling) "$Label managed GC sampling mismatch"
        Assert-Condition ([bool]$context.data.context.capture_config.managed_stack_explicit -eq $detail) "$Label managed stack mode mismatch"
        Assert-Condition ([bool]$context.data.context.capture_config.lua_stack_explicit -eq $detail) "$Label Lua stack mode mismatch"

        $overview = Inspect $traceId 'trace.overview'
        $statistics = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $statistics -and [UInt64]$statistics.count -gt 0) "$Label frame statistics absent"
        $counts = Inspect $traceId 'trace.counts'
        $script = Inspect $traceId 'runtime.script.summary'
        $gc = Inspect $traceId 'memory.gc.summary'
        Assert-Condition ([bool]$script.data.present -and [bool]$script.data.complete) "$Label script summary incomplete"
        Assert-Condition ([UInt64]$script.data.quality.invalid_records -eq 0) "$Label invalid script records"
        Assert-Condition ([UInt64]$script.data.quality.unresolved_references -eq 0) "$Label unresolved script references"
        Assert-Condition ([bool]$gc.data.present -and [bool]$gc.data.complete) "$Label GC summary incomplete"

        if ($detail) {
            Assert-Condition ([UInt64]$script.data.counts.frames -gt 0) "$Label script frames absent"
            Assert-Condition ([UInt64]$script.data.counts.stacks -ge 2) "$Label Managed/Lua stacks absent"
            Assert-Condition ([UInt64]$script.data.counts.zones -gt 0 -and
                [UInt64]$script.data.counts.complete_zones -eq [UInt64]$script.data.counts.zones) "$Label script zones incomplete"
            Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -gt 0) "$Label managed heap absent"
            Assert-Condition ([UInt64]$gc.data.latest.lua_heap_used_bytes -gt 0) "$Label Lua heap absent"
        }
        else {
            Assert-Condition ([UInt64]$script.data.counts.stacks -eq 0) "$Label unexpectedly captured explicit stacks"
            Assert-Condition ([UInt64]$script.data.counts.zones -eq 0) "$Label unexpectedly captured explicit script zones"
            if ($Profile -eq 'Summary') {
                Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -gt 0) "$Label managed heap sample absent"
                Assert-Condition ([UInt64]$gc.data.latest.lua_heap_used_bytes -eq 0) "$Label unexpectedly sampled Lua heap"
            }
            else {
                Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -eq 0) "$Label unexpectedly sampled managed heap"
                Assert-Condition ([UInt64]$gc.data.latest.lua_heap_used_bytes -eq 0) "$Label unexpectedly sampled Lua heap"
            }
        }

        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Label validation failed"
        $streamInfo = Get-Item -LiteralPath $Stream
        return [ordered]@{
            label = $Label
            profile = $Profile
            frame_count = [UInt64]$statistics.count
            frame_mean_ns = [double]$statistics.mean_ns
            frame_p50_ns = [double]$statistics.p50_ns
            frame_p95_ns = [double]$statistics.p95_ns
            cpu_zones = [UInt64]$counts.data.cpu_zones
            gpu_zones = [UInt64]$counts.data.gpu_zones
            script_frames = [UInt64]$script.data.counts.frames
            script_stacks = [UInt64]$script.data.counts.stacks
            script_zones = [UInt64]$script.data.counts.zones
            gc_events = [UInt64]$gc.data.counts.events
            gc_paired_intervals = [UInt64]$gc.data.counts.paired_intervals
            managed_heap_used_bytes = [UInt64]$gc.data.latest.managed_heap_used_bytes
            lua_heap_used_bytes = [UInt64]$gc.data.latest.lua_heap_used_bytes
            stream_bytes = [UInt64]$streamInfo.Length
            stream_bytes_per_frame = [Math]::Round($streamInfo.Length / [double]$statistics.count, 3)
            validation_errors = [UInt64]$validation.data.error_count
        }
    }
    finally { Close-Trace $traceId }
}

function Median([double[]] $Values) {
    $ordered = @($Values | Sort-Object)
    return $ordered[[int][Math]::Floor($ordered.Count / 2)]
}

function Summarize($Runs) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns','frame_p50_ns','frame_p95_ns','frame_count','cpu_zones','gpu_zones',
        'script_frames','script_stacks','script_zones','gc_events','gc_paired_intervals','stream_bytes','stream_bytes_per_frame')) {
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
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n11-performance'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $off = @(); $summary = @(); $detail = @()
    for ($i = 0; $i -lt 3; ++$i) {
        $off += Measure-Trace $OffSnapshots[$i] $OffStreams[$i] "Off-$($i + 1)" 'Off'
        $summary += Measure-Trace $SummarySnapshots[$i] $SummaryStreams[$i] "Summary-$($i + 1)" 'Summary'
        $detail += Measure-Trace $DetailSnapshots[$i] $DetailStreams[$i] "Detail-$($i + 1)" 'Detail'
    }
    $offMedian = Summarize $off
    $summaryMedian = Summarize $summary
    $detailMedian = Summarize $detail
    $summaryComparison = Compare-Metrics $offMedian $summaryMedian
    $detailComparison = Compare-Metrics $summaryMedian $detailMedian
    $summaryGatePassed = [double]$summaryComparison.frame_p95_ns_median_percent -le 3.0
    $result = [ordered]@{
        ok = $summaryGatePassed
        schema_version = '1.10.0'
        sample_count_per_group = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; stable_frames = 300; warmup_seconds = 45; capture_seconds = 10 }
        gate = [ordered]@{ summary_p95_limit_percent = 3.0; summary_p95_passed = $summaryGatePassed; detail_is_bounded_diagnostic = $true }
        off = [ordered]@{ runs = $off; median = $offMedian }
        summary = [ordered]@{ runs = $summary; median = $summaryMedian }
        detail = [ordered]@{ runs = $detail; median = $detailMedian }
        summary_vs_off = $summaryComparison
        detail_vs_summary = $detailComparison
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
