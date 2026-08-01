[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$OffSnapshot,
    [Parameter(Mandatory = $true)][string]$OffStream,
    [Parameter(Mandatory = $true)][string]$OffReplay,
    [Parameter(Mandatory = $true)][string]$DetailSnapshot,
    [Parameter(Mandatory = $true)][string]$DetailStream,
    [Parameter(Mandatory = $true)][string]$DetailReplay,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $OffSnapshot, $OffStream, $OffReplay,
    $DetailSnapshot, $DetailStream, $DetailReplay, $AllowRoot))
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
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 60))
    $script:Process.StandardInput.Flush()
    $read = $script:Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    $line = $read.Result
    if ($null -eq $line) { throw "MCP stdout closed before response: $Method" }
    $message = $line | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    return $message
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
        throw "$Name failed: $($response.result.structuredContent | ConvertTo-Json -Compress -Depth 30)"
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

function Assert-Domain($Domains, [string]$Name, [int]$Effective, [string]$Source, [bool]$Inherited)
{
    $domain = $Domains.PSObject.Properties[$Name].Value
    Assert-Condition ($null -ne $domain) "callstack domain is absent: $Name"
    Assert-Condition ([int]$domain.effective -eq $Effective) "$Name effective depth mismatch"
    Assert-Condition ([string]$domain.source -eq $Source) "$Name source mismatch"
    Assert-Condition ([bool]$domain.inherited -eq $Inherited) "$Name inherited flag mismatch"
    Assert-Condition (-not [bool]$domain.invalid -and -not [bool]$domain.clamped) "$Name is invalid or clamped"
}

function Measure-Trace([string]$Path, [string]$Profile)
{
    $traceId = Open-Trace $Path
    $context = Inspect $traceId 'capture.context'
    Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Profile context is incomplete"
    Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Profile context has invalid records"
    Assert-Condition ([string]$context.data.context.workload.scene -eq 'taijibase_constructedarmor_main_01') "$Profile scene mismatch"
    Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Profile graphics API mismatch"
    Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Profile gfx-jobs mismatch"

    $callstack = $context.data.context.capture_config.callstack
    Assert-Condition ([int]$callstack.schema_version -eq 1 -and [int]$callstack.maximum_depth -eq 62) "$Profile callstack schema mismatch"
    $domains = $callstack.domains
    if ($Profile -eq 'Off')
    {
        Assert-Domain $domains 'global' 0 'command_line_global' $false
        foreach ($name in @('csharp', 'unity_marker', 'lua', 'job', 'gpu_zone', 'cpu_alloc', 'gpu_alloc'))
        {
            Assert-Domain $domains $name 0 'command_line_domain' $false
        }
    }
    else
    {
        Assert-Domain $domains 'global' 1 'command_line_global' $false
        Assert-Domain $domains 'csharp' 8 'command_line_domain' $false
        Assert-Domain $domains 'unity_marker' 1 'command_line_global' $true
        Assert-Domain $domains 'lua' 8 'command_line_csharp' $true
        Assert-Domain $domains 'job' 8 'command_line_domain' $false
        Assert-Domain $domains 'gpu_zone' 8 'command_line_domain' $false
        Assert-Domain $domains 'cpu_alloc' 8 'command_line_domain' $false
        Assert-Domain $domains 'gpu_alloc' 8 'command_line_domain' $false
    }

    $overview = Inspect $traceId 'trace.overview'
    $statistics = $overview.data.primary_frame_statistics
    Assert-Condition ($null -ne $statistics -and [UInt64]$statistics.count -gt 0) "$Profile frame statistics are absent"
    $counts = Inspect $traceId 'trace.counts'
    Assert-Condition ([UInt64]$counts.data.cpu_zones -gt 0 -and [UInt64]$counts.data.gpu_zones -gt 0 -and [UInt64]$counts.data.jobs -gt 0) "$Profile core data is absent"

    $gpu = Inspect $traceId 'zone.gpu.search' @{ limit = 1000 }
    $gpuZones = @($gpu.data.zones)
    $gpuWithStack = @($gpuZones | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.callstack_ref) })
    $jobs = Inspect $traceId 'job.search' @{ filter = @{ text = 'Unity.Job.NativeSingle'; mode = 'exact' }; limit = 1000 }
    $jobValues = @($jobs.data.jobs)
    $jobsWithStack = @($jobValues | Where-Object { [UInt64]$_.schedule_callstack -ne 0 })
    if ($Profile -eq 'Off')
    {
        Assert-Condition ($gpuWithStack.Count -eq 0) 'Off GPU zones unexpectedly contain callstacks'
        Assert-Condition ($jobsWithStack.Count -eq 0) 'Off Jobs unexpectedly contain schedule callstacks'
    }
    else
    {
        Assert-Condition ($gpuWithStack.Count -gt 0) 'Detail GPU zones contain no callstacks'
        Assert-Condition ($jobsWithStack.Count -gt 0) 'Detail Jobs contain no schedule callstacks'
    }

    $coverage = Inspect $traceId 'capture.coverage'
    $dropped = [UInt64]0
    $overflow = [UInt64]0
    $mismatch = [UInt64]0
    $unresolved = [UInt64]0
    foreach ($producer in @($coverage.data.producers))
    {
        $dropped += [UInt64]$producer.counters.dropped
        $overflow += [UInt64]$producer.counters.overflow
        $mismatch += [UInt64]$producer.counters.mismatch
        $unresolved += [UInt64]$producer.counters.unresolved
    }
    Assert-Condition ($dropped -eq 0 -and $overflow -eq 0) "$Profile has dropped/overflow producer events"

    $validation = Inspect $traceId 'validation.run'
    Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$Profile validation failed"
    $first = [Int64]$overview.data.trace.first_time_ns
    $last = [Int64]$overview.data.trace.last_time_ns
    Assert-Condition ($last -gt $first) "$Profile trace span is invalid"

    Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $traceId })
    return [ordered]@{
        config_generation = [string]$callstack.config_generation
        frame_count = [string]$statistics.count
        frame_mean_ns = [double]$statistics.mean_ns
        frame_p50_ns = [double]$statistics.p50_ns
        frame_p95_ns = [double]$statistics.p95_ns
        trace_span_ns = [string]($last - $first)
        cpu_zones = [string]$counts.data.cpu_zones
        gpu_zones = [string]$counts.data.gpu_zones
        jobs = [string]$counts.data.jobs
        callstack_payloads = [string]$counts.data.callstack_payloads
        gpu_scanned = $gpuZones.Count
        gpu_with_stack = $gpuWithStack.Count
        jobs_scanned = $jobValues.Count
        jobs_with_stack = $jobsWithStack.Count
        producer_dropped = [string]$dropped
        producer_overflow = [string]$overflow
        producer_mismatch = [string]$mismatch
        producer_unresolved = [string]$unresolved
        validation_errors = [string]$validation.data.error_count
    }
}

function Assert-Replay-Equal($Snapshot, $Stream, $Replay, [string]$Label)
{
    $a = $Snapshot | ConvertTo-Json -Compress -Depth 20
    $b = $Stream | ConvertTo-Json -Compress -Depth 20
    $c = $Replay | ConvertTo-Json -Compress -Depth 20
    if ($a -ne $b) { throw "$Label snapshot/stream metrics differ`nSNAPSHOT=$a`nSTREAM=$b" }
    if ($a -ne $c) { throw "$Label snapshot/replay metrics differ`nSNAPSHOT=$a`nREPLAY=$c" }
}

try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-callstack-performance-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $offMetrics = Measure-Trace $OffSnapshot 'Off'
    $offStreamMetrics = Measure-Trace $OffStream 'Off'
    $offReplayMetrics = Measure-Trace $OffReplay 'Off'
    Assert-Replay-Equal $offMetrics $offStreamMetrics $offReplayMetrics 'Off'

    $detailMetrics = Measure-Trace $DetailSnapshot 'Detail'
    $detailStreamMetrics = Measure-Trace $DetailStream 'Detail'
    $detailReplayMetrics = Measure-Trace $DetailReplay 'Detail'
    Assert-Replay-Equal $detailMetrics $detailStreamMetrics $detailReplayMetrics 'Detail'

    Assert-Condition ([UInt64]$detailMetrics.callstack_payloads -gt [UInt64]$offMetrics.callstack_payloads) 'Detail did not increase callstack payload coverage'
    $offBytes = (Get-Item -LiteralPath $OffStream).Length
    $detailBytes = (Get-Item -LiteralPath $DetailStream).Length
    $offSeconds = [double]$offMetrics.trace_span_ns / 1e9
    $detailSeconds = [double]$detailMetrics.trace_span_ns / 1e9
    $result = [ordered]@{
        status = 'passed'
        query_schema = '1.4.0'
        profile_gate = 'Detail is a bounded diagnostic profile; no production P95 threshold is claimed'
        off = $offMetrics
        detail = $detailMetrics
        comparison = [ordered]@{
            frame_p50_percent = [Math]::Round((($detailMetrics.frame_p50_ns / $offMetrics.frame_p50_ns) - 1.0) * 100.0, 3)
            frame_p95_percent = [Math]::Round((($detailMetrics.frame_p95_ns / $offMetrics.frame_p95_ns) - 1.0) * 100.0, 3)
            frame_mean_percent = [Math]::Round((($detailMetrics.frame_mean_ns / $offMetrics.frame_mean_ns) - 1.0) * 100.0, 3)
            stream_bytes_percent = [Math]::Round((($detailBytes / $offBytes) - 1.0) * 100.0, 3)
            off_stream_bytes_per_second = [Math]::Round($offBytes / $offSeconds, 1)
            detail_stream_bytes_per_second = [Math]::Round($detailBytes / $detailSeconds, 1)
            off_stream_bytes_per_frame = [Math]::Round($offBytes / [double]$offMetrics.frame_count, 1)
            detail_stream_bytes_per_frame = [Math]::Round($detailBytes / [double]$detailMetrics.frame_count, 1)
        }
        snapshot_stream_replay_equal = $true
        detail_short_window_gate = $true
    }
    $json = $result | ConvertTo-Json -Depth 20
    $parent = Split-Path -Parent $OutputFile
    if (-not [string]::IsNullOrEmpty($parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [IO.File]::WriteAllText($OutputFile, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    $json
}
finally
{
    foreach ($traceId in @($script:OpenedIds))
    {
        try { Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null } catch {}
    }
    if ($null -ne $script:Process -and -not $script:Process.HasExited)
    {
        $script:Process.StandardInput.Close()
        if (-not $script:Process.WaitForExit(3000)) { $script:Process.Kill() }
    }
}
