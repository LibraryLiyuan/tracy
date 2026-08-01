[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string[]]$OffSnapshots,
    [Parameter(Mandatory = $true)][string[]]$OffStreams,
    [Parameter(Mandatory = $true)][string[]]$OnSnapshots,
    [Parameter(Mandatory = $true)][string[]]$OnStreams,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

Assert-Condition ($OffSnapshots.Count -eq 3) 'exactly three Off snapshots are required'
Assert-Condition ($OffStreams.Count -eq 3) 'exactly three Off streams are required'
Assert-Condition ($OnSnapshots.Count -eq 3) 'exactly three On snapshots are required'
Assert-Condition ($OnStreams.Count -eq 3) 'exactly three On streams are required'
foreach ($path in @($QueryExe) + $OffSnapshots + $OffStreams + $OnSnapshots + $OnStreams + @($AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$script:NextRequestId = 1
$script:OpenedIds = @()
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo

function Send-Rpc([string]$Method, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 600000)
{
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 80))
    $script:Process.StandardInput.Flush()
    while ($true)
    {
        $read = $script:Process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
        $line = $read.Result
        if ($null -eq $line) { throw "MCP stdout closed before response: $Method" }
        $message = $line | ConvertFrom-Json
        if ($null -ne $message.PSObject.Properties['id'] -and [string]$message.id -eq [string]$id) { return $message }
    }
}

function Notify-Initialized()
{
    $payload = [ordered]@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} }
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 10))
    $script:Process.StandardInput.Flush()
}

function Invoke-Tool([string]$Name, [hashtable]$Arguments = @{})
{
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    if ([bool]$response.result.isError)
    {
        throw "$Name failed: $($response.result.structuredContent | ConvertTo-Json -Compress -Depth 40)"
    }
    Assert-Condition ([bool]$response.result.structuredContent.ok) "$Name omitted structuredContent.ok=true"
    return $response.result.structuredContent
}

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Wait-Ready([string]$TraceId)
{
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached $state" }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready"
}

function Open-Trace([string]$Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $script:OpenedIds += $traceId
    Wait-Ready $traceId
    return $traceId
}

function Close-Trace([string]$TraceId)
{
    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $TraceId })
}

function Measure-Trace([string]$Snapshot, [string]$Stream, [string]$Label, [bool]$TaxonomyEnabled)
{
    $traceId = Open-Trace $Snapshot
    try
    {
        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Label capture context is incomplete"
        Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context is invalid"
        Assert-Condition ([string]$context.data.context.workload.scene -eq 'taijibase_constructedarmor_main_01') "$Label scene mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Label graphics API mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Label gfx-jobs mismatch"
        Assert-Condition ([string]$context.data.context.capture_config.profile -eq 'PassAndPhase') "$Label capture profile mismatch"

        $overview = Inspect $traceId 'trace.overview'
        $statistics = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $statistics -and [UInt64]$statistics.count -gt 0) "$Label frame statistics are absent"
        $counts = Inspect $traceId 'trace.counts'
        Assert-Condition ([UInt64]$counts.data.cpu_zones -gt 0 -and [UInt64]$counts.data.jobs -gt 0) "$Label core CPU/Job data is absent"

        $producerCoverage = Inspect $traceId 'capture.coverage'
        Assert-Condition ([bool]$producerCoverage.data.present -and [bool]$producerCoverage.data.complete) "$Label producer coverage is incomplete"
        Assert-Condition (@($producerCoverage.data.invalid_records).Count -eq 0) "$Label producer coverage is invalid"
        $producerMatches = @($producerCoverage.data.producers | Where-Object { $_.key -eq 'gpu.taxonomy.fallback' })
        Assert-Condition ($producerMatches.Count -eq 1) "$Label taxonomy producer is missing or duplicated"
        $producer = $producerMatches[0]
        Assert-Condition ([UInt64]$producer.counters.dropped -eq 0 -and [UInt64]$producer.counters.overflow -eq 0) "$Label taxonomy producer dropped or overflowed events"

        $tree = Inspect $traceId 'gpu.taxonomy.tree' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
        $taxonomyCoverage = Inspect $traceId 'gpu.taxonomy.coverage' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
        if ($TaxonomyEnabled)
        {
            Assert-Condition ([string]$producer.state -in @('covered', 'filtered', 'degraded')) "$Label taxonomy producer is not enabled"
            Assert-Condition ([UInt64]$producer.counters.emitted -gt 0) "$Label taxonomy producer emitted no events"
            Assert-Condition ([bool]$tree.data.present -and [bool]$tree.data.complete -and [bool]$tree.data.scan_complete) "$Label taxonomy tree is incomplete"
            Assert-Condition ([int]$tree.data.definition_count -eq 34 -and [int]$tree.data.quality.missing_part_count -eq 0) "$Label taxonomy catalog is incomplete"
            Assert-Condition ([UInt64]$tree.data.unknown_taxonomy_zone_count -eq 0) "$Label has unknown taxonomy zones"
            Assert-Condition ([bool]$tree.data.hierarchy_gate.observed_l0_has_multiple_l1 -and [bool]$tree.data.hierarchy_gate.observed_l1_has_multiple_l2) "$Label observed hierarchy gate failed"
            Assert-Condition ([bool]$taxonomyCoverage.data.present -and [bool]$taxonomyCoverage.data.complete) "$Label taxonomy coverage is incomplete"
            Assert-Condition ([UInt64]$taxonomyCoverage.data.statuses.executed.count -gt 0 -and [UInt64]$taxonomyCoverage.data.statuses.fallback.count -gt 0) "$Label taxonomy execution/fallback coverage is empty"
        }
        else
        {
            Assert-Condition ([string]$producer.state -eq 'disabled') "$Label taxonomy producer is not disabled"
            Assert-Condition ([UInt64]$producer.counters.emitted -eq 0) "$Label disabled taxonomy producer emitted events"
            Assert-Condition (-not [bool]$tree.data.present -and -not [bool]$taxonomyCoverage.data.present) "$Label disabled capture fabricated taxonomy data"
        }

        $validation = Inspect $traceId 'validation.run'
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Label trace validation failed"
        $first = [Int64]$overview.data.trace.first_time_ns
        $last = [Int64]$overview.data.trace.last_time_ns
        Assert-Condition ($last -gt $first) "$Label trace span is invalid"
        $snapshotFile = Get-Item -LiteralPath $Snapshot
        $streamFile = Get-Item -LiteralPath $Stream
        return [ordered]@{
            label = $Label
            capture_profile = [string]$context.data.context.capture_config.profile
            frame_count = [string]$statistics.count
            frame_mean_ns = [double]$statistics.mean_ns
            frame_p50_ns = [double]$statistics.p50_ns
            frame_p95_ns = [double]$statistics.p95_ns
            trace_span_ns = [string]($last - $first)
            cpu_zones = [string]$counts.data.cpu_zones
            gpu_zones = [string]$counts.data.gpu_zones
            jobs = [string]$counts.data.jobs
            callstack_payloads = [string]$counts.data.callstack_payloads
            taxonomy_present = [bool]$tree.data.present
            taxonomy_zone_count = if ($TaxonomyEnabled) { [string]$tree.data.taxonomy_zone_count } else { '0' }
            producer_state = [string]$producer.state
            producer_observed = [string]$producer.counters.observed
            producer_emitted = [string]$producer.counters.emitted
            producer_filtered = [string]$producer.counters.filtered
            producer_mismatch = [string]$producer.counters.mismatch
            producer_dropped = [string]$producer.counters.dropped
            producer_overflow = [string]$producer.counters.overflow
            producer_unresolved = [string]$producer.counters.unresolved
            validation_errors = [string]$validation.data.error_count
            snapshot_bytes = [string]$snapshotFile.Length
            stream_bytes = [string]$streamFile.Length
            stream_bytes_per_frame = [Math]::Round($streamFile.Length / [double]$statistics.count, 3)
            snapshot_sha256 = (Get-FileHash -LiteralPath $Snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
            stream_sha256 = (Get-FileHash -LiteralPath $Stream -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    finally
    {
        Close-Trace $traceId
    }
}

function Get-Median([double[]]$Values)
{
    $ordered = @($Values | Sort-Object)
    return $ordered[[int][Math]::Floor($ordered.Count / 2)]
}

function Summarize($Runs)
{
    return [ordered]@{
        frame_mean_ns_median = Get-Median @($Runs | ForEach-Object { [double]$_.frame_mean_ns })
        frame_p50_ns_median = Get-Median @($Runs | ForEach-Object { [double]$_.frame_p50_ns })
        frame_p95_ns_median = Get-Median @($Runs | ForEach-Object { [double]$_.frame_p95_ns })
        frame_count_median = Get-Median @($Runs | ForEach-Object { [double]$_.frame_count })
        cpu_zones_median = Get-Median @($Runs | ForEach-Object { [double]$_.cpu_zones })
        gpu_zones_median = Get-Median @($Runs | ForEach-Object { [double]$_.gpu_zones })
        jobs_median = Get-Median @($Runs | ForEach-Object { [double]$_.jobs })
        callstack_payloads_median = Get-Median @($Runs | ForEach-Object { [double]$_.callstack_payloads })
        stream_bytes_median = Get-Median @($Runs | ForEach-Object { [double]$_.stream_bytes })
        stream_bytes_per_frame_median = Get-Median @($Runs | ForEach-Object { [double]$_.stream_bytes_per_frame })
        taxonomy_zone_count_median = Get-Median @($Runs | ForEach-Object { [double]$_.taxonomy_zone_count })
        producer_mismatch_median = Get-Median @($Runs | ForEach-Object { [double]$_.producer_mismatch })
    }
}

Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
$script:Process = $process
try
{
    $initialize = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-gpu-taxonomy-performance-acceptance'; version = '1' } }
    Assert-Condition ([string]$initialize.result.protocolVersion -eq '2025-11-25') 'MCP protocol version mismatch'
    Notify-Initialized

    $offRuns = @()
    $onRuns = @()
    for ($index = 0; $index -lt 3; ++$index)
    {
        $offRuns += Measure-Trace $OffSnapshots[$index] $OffStreams[$index] "Off-$($index + 1)" $false
        $onRuns += Measure-Trace $OnSnapshots[$index] $OnStreams[$index] "On-$($index + 1)" $true
    }
    $offSummary = Summarize $offRuns
    $onSummary = Summarize $onRuns
    $comparison = [ordered]@{}
    foreach ($field in @('frame_mean_ns_median', 'frame_p50_ns_median', 'frame_p95_ns_median', 'stream_bytes_median', 'stream_bytes_per_frame_median'))
    {
        $baseline = [double]$offSummary[$field]
        $comparison[$field + '_percent'] = [Math]::Round((([double]$onSummary[$field] / $baseline) - 1.0) * 100.0, 3)
    }
    $result = [ordered]@{
        schema_version = '1.6.0'
        result = 'PASS'
        sample_count_per_group = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; capture_profile = 'PassAndPhase'; capture_seconds = 10 }
        gate = 'N07 correctness and measured-overhead gate; final production thresholds remain N15'
        off = [ordered]@{ runs = $offRuns; median = $offSummary }
        on = [ordered]@{ runs = $onRuns; median = $onSummary }
        comparison = $comparison
        build_settings_sha256 = 'e2df8f252b04e33eaa5408001c5d89bb1225e17f7cf8739eb32f30e4da1f308b'
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 80) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Depth 12
}
finally
{
    foreach ($traceId in @($script:OpenedIds))
    {
        try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {}
    }
    try { $process.StandardInput.Close() } catch {}
    if (-not $process.WaitForExit(5000)) { $process.Kill() }
    $process.Dispose()
}
