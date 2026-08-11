[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [string] $OutputFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Condition([bool] $Condition, [string] $Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$script:NextRequestId = 1
$script:OpenedIds = @()
$script:Process = $null
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

function Send-Rpc([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 600000)
{
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 80))
    $script:Process.StandardInput.Flush()
    $read = $script:Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    if ($null -eq $read.Result) { throw "MCP stdout closed before response: $Method" }
    $message = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    return $message
}

function Notify-Initialized()
{
    $payload = [ordered]@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} }
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 10))
    $script:Process.StandardInput.Flush()
}

function Invoke-Tool([string] $Name, [hashtable] $Arguments = @{})
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

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Open-Trace([string] $Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $script:OpenedIds += $traceId
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $traceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready')
        {
            return [pscustomobject]@{ Id = $traceId; Fingerprint = [string]$status.data.status.fingerprint }
        }
        if ($state -in @('failed', 'closed')) { throw "trace reached state $state" }
        Start-Sleep -Milliseconds 250
    }
    throw "trace did not become ready: $Path"
}

function Close-Trace([string] $TraceId)
{
    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $TraceId })
}

function Get-Capability($Capabilities, [string] $Domain)
{
    $values = @($Capabilities.data.domains | Where-Object { [string]$_.domain -eq $Domain })
    Assert-Condition ($values.Count -eq 1) "capability is absent or duplicated: $Domain"
    return $values[0]
}

function Validate-Trace([string] $Path, [string] $Label)
{
    $opened = Open-Trace $Path
    $traceId = $opened.Id
    try
    {
        $capabilities = Inspect $traceId 'system.capabilities'
        foreach ($domain in @('memory', 'memory.gc', 'io', 'sample', 'context_switch'))
        {
            $capability = Get-Capability $capabilities $domain
            Assert-Condition ([bool]$capability.present -and [bool]$capability.queryable) `
                "$Label capability unavailable: $domain reason=$($capability.reason)"
        }

        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Label capture context incomplete"
        Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context has invalid records"
        $config = $context.data.context.capture_config
        Assert-Condition ([bool]$config.managed_gc_sampling) "$Label managed GC sampling was not enabled"
        Assert-Condition ([int]$config.callstack.domains.cpu_alloc.effective -eq 8) "$Label CPU allocation stack depth mismatch"
        Assert-Condition ([int]$config.callstack.domains.io.effective -eq 8) "$Label I/O stack depth mismatch"
        $actual = $config.actual_capabilities
        Assert-Condition ([bool]$actual.sampling.present -and [UInt64]$actual.sampling.sample_count -gt 0) `
            "$Label Sampling requested but no samples were captured"
        Assert-Condition ([bool]$actual.context_switch.present -and [UInt64]$actual.context_switch.event_count -gt 0) `
            "$Label Context Switch requested but no events were captured"

        $poolsResult = Inspect $traceId 'memory.pools' @{ limit = 1000 }
        $cpuPools = @($poolsResult.data.pools | Where-Object {
            -not [bool]$_.gpu_d3d12 -and [string]$_.name -ne 'Default allocator' -and [UInt64]$_.event_count -gt 0
        })
        Assert-Condition ($cpuPools.Count -gt 0) "$Label contains no CPU allocator pools"
        $poolSummary = @($cpuPools | Sort-Object name | ForEach-Object {
            [ordered]@{
                name = [string]$_.name
                event_count = [string]$_.event_count
                free_count = [string]$_.free_count
                active_count = [string]$_.active_count
                active_bytes = [string]$_.active_bytes
            }
        })
        [UInt64]$totalEvents = 0
        [UInt64]$totalFrees = 0
        foreach ($pool in $cpuPools)
        {
            $totalEvents += [UInt64]$pool.event_count
            $totalFrees += [UInt64]$pool.free_count
        }
        Assert-Condition ($totalEvents -gt 0 -and $totalFrees -gt 0) "$Label CPU allocation/free lifecycle is empty"

        $selectedPool = @($cpuPools | Sort-Object @{ Expression = { [UInt64]$_.event_count }; Descending = $true })[0]
        $eventsResult = Inspect $traceId 'memory.events' @{
            pool_ref = [string]$selectedPool.ref
            limit = 1000
            max_scan_events = 100000000
            max_cpu_ms = 60000
        }
        $events = @($eventsResult.data.events)
        Assert-Condition ($events.Count -gt 0) "$Label selected CPU pool has no events"
        Assert-Condition (@($events | Where-Object { [bool]$_.complete }).Count -gt 0) `
            "$Label selected CPU pool has no completed allocation/free pair"
        $withStack = @($events | Where-Object {
            $property = $_.PSObject.Properties['allocation_callstack_ref']
            $null -ne $property -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)
        })
        Assert-Condition ($withStack.Count -gt 0) "$Label CPU allocation callstacks are absent"
        $stack = Inspect $traceId 'callstack.frames' @{
            callstack = [string]$withStack[0].allocation_callstack_ref
            max_depth = 64
        }
        $stackFrames = @($stack.data.frames)
        Assert-Condition ($stackFrames.Count -gt 0) "$Label CPU allocation callstack did not resolve"
        Assert-Condition (@($stackFrames | Where-Object {
            $property = $_.PSObject.Properties['line']
            $null -ne $property -and [int]$property.Value -gt 0
        }).Count -gt 0) `
            "$Label CPU allocation callstack has no source line"

        $gcLimits = @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        $gc = Inspect $traceId 'memory.gc.summary' $gcLimits
        Assert-Condition ([bool]$gc.data.present -and [bool]$gc.data.complete) "$Label managed GC summary incomplete"
        Assert-Condition ([UInt64]$gc.data.counts.events -gt 0) "$Label managed GC events are absent"
        Assert-Condition ([UInt64]$gc.data.counts.paired_intervals -gt 0) "$Label managed GC intervals are absent"
        Assert-Condition ([UInt64]$gc.data.counts.sampled_allocation_bytes -gt 0) `
            "$Label managed allocation byte samples are absent"
        Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -gt 0) "$Label managed heap-used sample is absent"
        Assert-Condition ([UInt64]$gc.data.latest.managed_heap_reserved_bytes -gt 0) `
            "$Label managed heap-reserved sample is absent"
        Assert-Condition ([UInt64]$gc.data.quality.invalid_records -eq 0 -and
            [UInt64]$gc.data.quality.open_intervals -eq 0 -and
            [UInt64]$gc.data.quality.orphan_ends -eq 0) "$Label managed GC quality is invalid"

        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) `
            "$Label trace validation failed"

        return [ordered]@{
            fingerprint = $opened.Fingerprint
            validation = [ordered]@{
                complete = [bool]$validation.data.complete
                partial = [bool]$validation.partial
                errors = [string]$validation.data.error_count
            }
            semantics = [ordered]@{
                cpu_pool_count = $cpuPools.Count
                cpu_events = [string]$totalEvents
                cpu_frees = [string]$totalFrees
                cpu_pools = $poolSummary
                selected_pool = [string]$selectedPool.name
                selected_pool_events = $events.Count
                selected_pool_complete = @($events | Where-Object { [bool]$_.complete }).Count
                allocation_stack_depth = $stackFrames.Count
                allocation_stack_source_lines = @($stackFrames | Where-Object {
                    $property = $_.PSObject.Properties['line']
                    $null -ne $property -and [int]$property.Value -gt 0
                }).Count
                gc_events = [string]$gc.data.counts.events
                gc_paired_intervals = [string]$gc.data.counts.paired_intervals
                gc_sampled_allocation_bytes = [string]$gc.data.counts.sampled_allocation_bytes
                managed_heap_used_bytes = [string]$gc.data.latest.managed_heap_used_bytes
                managed_heap_reserved_bytes = [string]$gc.data.latest.managed_heap_reserved_bytes
                sampling_events = [string]$actual.sampling.sample_count
                context_switch_events = [string]$actual.context_switch.event_count
                validation_errors = [string]$validation.data.error_count
            }
        }
    }
    finally
    {
        Close-Trace $traceId
    }
}

try
{
    Assert-Condition $process.Start() 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'jn-n17-startup-io-memory-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $results = [ordered]@{
        snapshot = Validate-Trace $SnapshotTrace 'snapshot'
        stream = Validate-Trace $StreamTrace 'stream'
        replay = Validate-Trace $ReplayTrace 'replay'
    }
    $baseline = $results.snapshot.semantics | ConvertTo-Json -Compress -Depth 30
    $streamMatches = ($results.stream.semantics | ConvertTo-Json -Compress -Depth 30) -eq $baseline
    $replayMatches = ($results.replay.semantics | ConvertTo-Json -Compress -Depth 30) -eq $baseline
    $result = [ordered]@{
        ok = $streamMatches -and $replayMatches
        schema_version = '1.26.0'
        comparison = [ordered]@{
            snapshot_stream_equal = $streamMatches
            snapshot_replay_equal = $replayMatches
            validation_all_complete = [bool]$results.snapshot.validation.complete -and
                [bool]$results.stream.validation.complete -and [bool]$results.replay.validation.complete
        }
        traces = $results
    }
    $json = $result | ConvertTo-Json -Depth 60
    if (-not [string]::IsNullOrWhiteSpace($OutputFile))
    {
        $parent = Split-Path -Parent $OutputFile
        if (-not [string]::IsNullOrWhiteSpace($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        [IO.File]::WriteAllText($OutputFile, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    Assert-Condition $streamMatches 'snapshot/stream Startup I/O/Memory semantics differ'
    Assert-Condition $replayMatches 'snapshot/replay Startup I/O/Memory semantics differ'
    $json
}
finally
{
    foreach ($traceId in @($script:OpenedIds))
    {
        try { Close-Trace $traceId } catch {}
    }
    if ($null -ne $script:Process -and -not $script:Process.HasExited)
    {
        $script:Process.StandardInput.Close()
        if (-not $script:Process.WaitForExit(5000))
        {
            $script:Process.Kill($true)
            [void]$script:Process.WaitForExit()
        }
    }
    if ($null -ne $script:Process) { $script:Process.Dispose() }
}
