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

foreach ($group in @($OffSnapshots, $OffStreams, $SummarySnapshots, $SummaryStreams, $DetailSnapshots, $DetailStreams)) {
    Assert-Condition ($group.Count -eq 3) 'each performance group requires exactly three captures'
}
foreach ($path in @($QueryExe, $AllowRoot) + $OffSnapshots + $OffStreams + $SummarySnapshots + $SummaryStreams + $DetailSnapshots + $DetailStreams) {
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
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 60
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

function Measure-Trace([string] $Snapshot, [string] $Stream, [string] $Label, [string] $Mode) {
    $traceId = Open-Trace $Snapshot
    try {
        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present) "$Label capture context absent"
        Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context invalid"
        if (-not [bool]$context.data.complete) {
            $missingLayers = @($context.data.missing_layers)
            Assert-Condition ($missingLayers.Count -eq 1 -and [string]$missingLayers[0] -eq 'workload') "$Label has an unexpected incomplete context"
        }
        Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Label graphics API mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Label gfx-jobs mismatch"
        $expectedGpuAllocDepth = if ($Mode -eq 'Detail') { 8 } else { 0 }
        Assert-Condition ([int]$context.data.context.capture_config.callstack.domains.gpu_alloc.effective -eq $expectedGpuAllocDepth) "$Label GPU allocation callstack depth mismatch"

        $overview = Inspect $traceId 'trace.overview'
        $statistics = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $statistics -and [UInt64]$statistics.count -gt 0) "$Label frame statistics absent"
        $counts = Inspect $traceId 'trace.counts'
        Assert-Condition ([UInt64]$counts.data.cpu_zones -gt 0) "$Label CPU zones absent"
        $summary = $null
        $originAvailable = 0
        $allocationCount = 0
        if ($Mode -eq 'Off') {
            $gpuCapability = @($overview.data.capabilities | Where-Object { [string]$_.domain -eq 'memory.gpu' })
            Assert-Condition ($gpuCapability.Count -eq 1) "$Label memory.gpu capability missing"
            Assert-Condition (-not [bool]$gpuCapability[0].present -and -not [bool]$gpuCapability[0].queryable) "$Label fabricated GPU memory capability while disabled"
        }
        else {
            $summary = Inspect $traceId 'memory.gpu.summary'
            Assert-Condition ([bool]$summary.data.present) "$Label GTMEM2 summary absent"
            Assert-Condition ([string]$summary.data.protocol -eq 'GTMEM2') "$Label GTMEM2 protocol mismatch"
            Assert-Condition ([UInt64]$summary.data.physical.active_bytes -gt 0) "$Label physical registry empty"
            Assert-Condition ([UInt64]$summary.data.logical_resource_count -gt 0) "$Label logical registry empty"
            $residency = Inspect $traceId 'memory.gpu.residency' @{ limit = 1 }
            $fragmentation = Inspect $traceId 'memory.gpu.fragmentation' @{ limit = 1 }
            Assert-Condition ([bool]$residency.data.present) "$Label residency absent"
            Assert-Condition ([bool]$fragmentation.data.present) "$Label fragmentation absent"
            $allocations = Inspect $traceId 'memory.gpu.allocations' @{ limit = 100 }
            $values = @($allocations.data.allocations)
            $allocationCount = $values.Count
            $originAvailable = @($values | Where-Object { [string]$_.origin.callstack_availability -eq 'available' }).Count
        }

        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Label validation failed"
        $streamInfo = Get-Item -LiteralPath $Stream
        return [ordered]@{
            label = $Label
            mode = $Mode
            capture_context_complete = [bool]$context.data.complete
            capture_context_missing_layers = @($context.data.missing_layers)
            gpu_alloc_callstack_depth = $expectedGpuAllocDepth
            frame_count = [UInt64]$statistics.count
            frame_mean_ns = [double]$statistics.mean_ns
            frame_p50_ns = [double]$statistics.p50_ns
            frame_p95_ns = [double]$statistics.p95_ns
            cpu_zones = [UInt64]$counts.data.cpu_zones
            gpu_zones = [UInt64]$counts.data.gpu_zones
            callstack_payloads = [UInt64]$counts.data.callstack_payloads
            gpu_memory_present = if ($Mode -eq 'Off') { $false } else { [bool]$summary.data.present }
            physical_active_bytes = if ($Mode -eq 'Off') { 0 } else { [UInt64]$summary.data.physical.active_bytes }
            logical_resource_count = if ($Mode -eq 'Off') { 0 } else { [UInt64]$summary.data.logical_resource_count }
            allocation_sample_count = $allocationCount
            allocation_callstack_available = $originAvailable
            stream_bytes = [UInt64]$streamInfo.Length
            stream_bytes_per_frame = [Math]::Round($streamInfo.Length / [double]$statistics.count, 3)
            validation_errors = [UInt64]$validation.data.error_count
        }
    }
    finally { Close-Trace $traceId }
}

function Median([double[]] $Values) { $ordered = @($Values | Sort-Object); return $ordered[[int][Math]::Floor($ordered.Count / 2)] }
function Summarize($Runs) {
    $result = [ordered]@{}
    foreach ($field in @('frame_mean_ns','frame_p50_ns','frame_p95_ns','frame_count','cpu_zones','gpu_zones','callstack_payloads','stream_bytes','stream_bytes_per_frame','allocation_callstack_available')) {
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
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n10-memory-performance'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $off = @(); $summary = @(); $detail = @()
    for ($i = 0; $i -lt 3; ++$i) {
        $off += Measure-Trace $OffSnapshots[$i] $OffStreams[$i] "Off-$($i + 1)" 'Off'
        $summary += Measure-Trace $SummarySnapshots[$i] $SummaryStreams[$i] "Summary-$($i + 1)" 'Summary'
        $detail += Measure-Trace $DetailSnapshots[$i] $DetailStreams[$i] "Detail-$($i + 1)" 'Detail'
    }
    Assert-Condition ((@($detail | ForEach-Object { $_.allocation_callstack_available } | Measure-Object -Sum).Sum) -gt 0) 'Detail captures contain no GPU allocation callstack evidence'
    $offMedian = Summarize $off; $summaryMedian = Summarize $summary; $detailMedian = Summarize $detail
    $result = [ordered]@{
        ok = $true
        schema_version = '1.9.0'
        sample_count_per_group = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; capture_seconds = 10 }
        gate = 'Summary is the daily profile; Detail allocation stacks are a bounded diagnostic profile'
        off = [ordered]@{ runs = $off; median = $offMedian }
        summary = [ordered]@{ runs = $summary; median = $summaryMedian }
        detail = [ordered]@{ runs = $detail; median = $detailMedian }
        summary_vs_off = (Compare-Metrics $offMedian $summaryMedian)
        detail_vs_summary = (Compare-Metrics $summaryMedian $detailMedian)
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 80) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Compress -Depth 12
}
finally {
    foreach ($traceId in @($script:opened)) { if ($process -and -not $process.HasExited) { try { [void](Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} } }
    if ($process -and -not $process.HasExited) {
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $process.WaitForExit() }
    }
    if ($process) { $process.Dispose() }
}
