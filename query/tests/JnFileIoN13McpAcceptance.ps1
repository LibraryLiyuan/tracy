[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [switch] $RealCapture
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$script:NextRequestId = 1
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$queryProcess = [System.Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo

function Send-Notification {
    param([string] $Method, [hashtable] $Params = @{})
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc {
    param([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 240000)
    $id = $script:NextRequestId++
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $read = $script:queryProcess.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    if ($null -eq $read.Result) { throw "MCP stdout closed before response: $Method" }
    $message = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    return $message
}

function Invoke-Tool {
    param([string] $Name, [hashtable] $Arguments = @{})
    $response = Send-Rpc -Method 'tools/call' -Params @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) { throw "$Name failed: $($structured.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Inspect {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    return Invoke-Tool -Name 'tracy_inspect' -Arguments @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Wait-Ready {
    param([string] $TraceId)
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-Tool -Name 'tracy_trace_status' -Arguments @{ trace_id = $TraceId }
        if ([string]$status.data.status.state -eq 'ready') { return $status }
        if ([string]$status.data.status.state -in @('failed', 'closed')) { throw "trace entered $($status.data.status.state)" }
        Start-Sleep -Milliseconds 250
    }
    throw 'trace did not become ready'
}

function Get-IoRequests {
    param([string] $TraceId, [bool] $BoundedSample)
    if ($BoundedSample) {
        $response = Inspect $TraceId 'io.search' @{ offset = 0; limit = 1000 }
        return @($response.data.requests)
    }
    $values = [System.Collections.Generic.List[object]]::new()
    for ($offset = 0; $offset -lt 100000; $offset += 1000) {
        $response = Inspect $TraceId 'io.search' @{ offset = $offset; limit = 1000 }
        $page = @($response.data.requests)
        foreach ($request in $page) { $values.Add($request) }
        if ($page.Count -lt 1000) { break }
    }
    return @($values)
}

function Resolve-RequestCallstack {
    param([string] $TraceId, [string] $CallstackRef, [string] $Label)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($CallstackRef)) "$Label has no callstack ref"
    $resolved = Inspect $TraceId 'callstack.frames' @{ callstack = $CallstackRef; max_depth = 64 }
    $frames = @($resolved.data.frames)
    Assert-Condition ($frames.Count -gt 0) "$Label callstack did not resolve"
    Assert-Condition ($frames.Count -le 62) "$Label callstack exceeded the Windows safe maximum"
    $sourceFrames = @($frames | Where-Object {
        -not [string]::IsNullOrWhiteSpace([string]$_.file) -and [int]$_.line -gt 0
    })
    Assert-Condition ($sourceFrames.Count -gt 0) "$Label callstack has no source file and line"
    return [ordered]@{
        depth = $frames.Count
        source_frames = @($sourceFrames | ForEach-Object {
            [ordered]@{ function = [string]$_.function; file = [string]$_.file; line = [int]$_.line }
        })
    }
}

function Validate-N13 {
    param([string] $TraceId)
    $capabilities = Inspect $TraceId 'system.capabilities'
    $ioCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'io' })
    Assert-Condition ($ioCapability.Count -eq 1 -and [bool]$ioCapability[0].present) 'I/O capability unavailable'
    foreach ($method in @('io.search', 'io.get', 'io.statistics', 'io.chain')) {
        Assert-Condition (@($ioCapability[0].methods) -contains $method) "I/O capability is missing $method"
    }

    $network = Inspect $TraceId 'network.capabilities'
    Assert-Condition (-not [bool]$network.data.present) 'Network must remain absent in N13'
    Assert-Condition ([string]$network.data.status -eq 'DeferredByUser') 'Network status is not DeferredByUser'
    Assert-Condition ([string]$network.data.reason -eq 'deferred_by_user') 'Network reason mismatch'

    $context = Inspect $TraceId 'capture.context'
    $statistics = Inspect $TraceId 'io.statistics'
    $validation = Inspect $TraceId 'validation.run'
    $requests = Get-IoRequests $TraceId ([bool]$RealCapture)
    $stats = $statistics.data

    Assert-Condition ([bool]$stats.present) 'io.statistics is not present'
    Assert-Condition ([int]$stats.io_schema_version -eq 1) 'I/O schema 1 was not detected'
    Assert-Condition ([string]$stats.lifecycle_contract.connection_boundary -eq 'active_requests_only') 'I/O active snapshot contract mismatch'
    Assert-Condition ([string]$stats.lifecycle_contract.completed_before_connection -eq 'not_replayed') 'I/O completed-request replay contract mismatch'
    Assert-Condition ([UInt64]$stats.counts.requests -gt 0) 'structured I/O requests are empty'
    Assert-Condition ([UInt64]$stats.counts.completed -gt 0) 'completed I/O requests are empty'
    Assert-Condition ([UInt64]$stats.counts.request_callstacks -gt 0) 'I/O request callstacks are empty'
    Assert-Condition ([bool]$stats.producer_quality.present) 'io.structured producer quality is missing'
    Assert-Condition ([bool]$stats.producer_quality.complete) 'io.structured producer lost core lifecycle events'
    foreach ($counter in @('dropped', 'overflow', 'mismatch', 'unresolved', 'tail_truncated')) {
        Assert-Condition ([UInt64]$stats.producer_quality.counters.$counter -eq 0) "io.structured producer counter is non-zero: $counter"
    }
    Assert-Condition ([UInt64]$stats.quality.unexplained_missing_start -eq 0) 'I/O request has an unexplained missing Start'
    Assert-Condition ([UInt64]$stats.quality.unexplained_missing_terminal -eq 0) 'I/O request has an unexplained missing terminal stage'
    Assert-Condition ([UInt64]$stats.quality.unexplained_truncated -eq 0) 'I/O request has unexplained truncation'
    Assert-Condition ([UInt64]$stats.quality.duplicate_terminal -eq 0) 'I/O request has duplicate terminal stages'
    Assert-Condition ([UInt64]$stats.quality.invalid_order -eq 0) 'I/O stage order is invalid'
    Assert-Condition ([UInt64]$stats.quality.unresolved_parent -eq 0) 'I/O parent is unresolved'
    Assert-Condition ([UInt64]$stats.quality.bytes_overflow -eq 0) 'I/O transferred bytes overflow requested bytes'
    Assert-Condition ([UInt64]$stats.quality.orphan -eq 0) 'I/O request is orphaned'
    Assert-Condition ([bool]$stats.complete) 'I/O quality gate is incomplete'
    Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) 'trace validation failed'
    if (-not $RealCapture) {
        Assert-Condition ($requests.Count -eq [int][UInt64]$stats.counts.requests) 'io.search count differs from io.statistics'
        Assert-Condition ([UInt64]$stats.quality.missing_start -eq 0) 'synthetic I/O request is missing Start'
        Assert-Condition ([UInt64]$stats.quality.missing_terminal -eq 0) 'synthetic I/O request is missing a terminal stage'
        Assert-Condition ([UInt64]$stats.quality.right_censored -eq 0) 'synthetic I/O request was unexpectedly right-censored'
    }
    else {
        Assert-Condition ([UInt64]$stats.quality.missing_terminal -eq [UInt64]$stats.quality.right_censored) 'real open I/O requests are not fully explained by capture-end censoring'
        Assert-Condition ([UInt64]$stats.quality.missing_start -eq [UInt64]$stats.quality.right_censored_queued) 'real queued requests do not explain every missing Start'
    }

    $operations = @($stats.counts.operations.PSObject.Properties | Where-Object {
        [UInt64]$_.Value -gt 0
    } | ForEach-Object { [string]$_.Name } | Sort-Object -Unique)
    $sources = @($requests | ForEach-Object { [string]$_.source } | Sort-Object -Unique)
    $callstackRequest = @($requests | Where-Object { $null -ne $_.request_callstack_ref } | Select-Object -First 1)
    Assert-Condition ($callstackRequest.Count -eq 1) 'could not select an I/O request with a callstack'
    $callstack = Resolve-RequestCallstack $TraceId ([string]$callstackRequest[0].request_callstack_ref) 'I/O request'

    $root = if ($RealCapture) {
        @((Inspect $TraceId 'io.search' @{ operation = 'resource_load'; offset = 0; limit = 1 }).data.requests)
    } else {
        @($requests | Where-Object { [string]$_.operation -eq 'resource_load' } | Select-Object -First 1)
    }
    $root = @($root)
    if ($root.Count -eq 0) { $root = @($requests | Select-Object -First 1) }
    Assert-Condition ($root.Count -eq 1) 'could not select an I/O chain root'
    $detail = Inspect $TraceId 'io.get' @{ ref = [string]$root[0].ref }
    Assert-Condition ($null -ne $detail.data.request) 'io.get omitted request detail'
    Assert-Condition (@($detail.data.request.stages).Count -gt 0) 'io.get omitted request stages'
    $chain = Inspect $TraceId 'io.chain' @{ ref = [string]$root[0].ref; max_nodes = 10000 }
    $chainNodes = @($chain.data.nodes)
    $chainEdges = @($chain.data.edges)
    Assert-Condition ($chainNodes.Count -gt 0) 'io.chain returned no nodes'
    Assert-Condition (-not [bool]$chain.data.truncated) 'io.chain was truncated'
    Assert-Condition (@($chainEdges | Where-Object { [string]$_.evidence_kind -ne 'exact' }).Count -eq 0) 'io.chain contains a non-exact edge'
    $chainCoverage = if ($chainEdges.Count -gt 0) {
        'exact_parent_edges'
    } elseif ($RealCapture -and [string]$context.data.context.runtime.target_kind -eq 'editor' -and
        -not ($operations -contains 'jnfs_load')) {
        'editor_load_proxy_no_native_child'
    } else {
        'missing_expected_parent_edge'
    }

    if (-not $RealCapture) {
        Assert-Condition ([UInt64]$stats.counts.requests -eq 6) 'synthetic request count mismatch'
        Assert-Condition ([UInt64]$stats.counts.completed -eq 6) 'synthetic completed request count mismatch'
        Assert-Condition ([UInt64]$stats.counts.failed -eq 0 -and [UInt64]$stats.counts.cancelled -eq 0) 'synthetic terminal status mismatch'
        Assert-Condition ([UInt64]$stats.counts.requeue_stages -eq 1) 'synthetic requeue count mismatch'
        Assert-Condition ([UInt64]$stats.counts.request_callstacks -eq 5) 'synthetic callstack count mismatch'
        Assert-Condition ([UInt64]$stats.counts.connection_snapshots -eq 1) 'synthetic active connection snapshot mismatch'
        Assert-Condition ([UInt64]$stats.quality.capture_boundary -eq 1) 'synthetic capture-boundary request mismatch'
        Assert-Condition (($operations -join ',') -eq 'decompress,integrate,jnfs_load,read,resource_load') 'synthetic operation set mismatch'
        Assert-Condition ($chainNodes.Count -eq 5 -and $chainEdges.Count -eq 4) 'synthetic exact chain shape mismatch'
        Assert-Condition ([string]$context.data.context.workload.scenario -eq 'n13-file-io') 'synthetic capture context mismatch'
        $harnessFrames = @($callstack.source_frames | Where-Object {
            [string]$_.file -like '*JNTracyHarness.cpp' -and [int]$_.line -gt 0
        })
        Assert-Condition ($harnessFrames.Count -gt 0) 'synthetic I/O stack did not resolve JNTracyHarness.cpp with a line number'
    }
    else {
        foreach ($required in @('read', 'resource_load')) {
            Assert-Condition ($operations -contains $required) "real capture is missing required operation: $required"
        }
        if ($operations -contains 'jnfs_load') {
            Assert-Condition ($chainNodes.Count -gt 1 -and $chainEdges.Count -gt 0) 'real JNFS ResourceLoad has no exact native I/O child edge'
        } else {
            Assert-Condition ($chainCoverage -eq 'editor_load_proxy_no_native_child') 'missing exact edge has no valid EditorLoadProxy coverage reason'
        }
    }

    return [ordered]@{
        requests = [string]$stats.counts.requests
        completed = [string]$stats.counts.completed
        failed = [string]$stats.counts.failed
        cancelled = [string]$stats.counts.cancelled
        requeue_stages = [string]$stats.counts.requeue_stages
        request_callstacks = [string]$stats.counts.request_callstacks
        connection_snapshots = [string]$stats.counts.connection_snapshots
        operations = $operations
        sources = $sources
        quality = [ordered]@{
            missing_start = [string]$stats.quality.missing_start
            missing_terminal = [string]$stats.quality.missing_terminal
            duplicate_terminal = [string]$stats.quality.duplicate_terminal
            invalid_order = [string]$stats.quality.invalid_order
            unresolved_parent = [string]$stats.quality.unresolved_parent
            bytes_overflow = [string]$stats.quality.bytes_overflow
            orphan = [string]$stats.quality.orphan
            truncated = [string]$stats.quality.truncated
            capture_boundary = [string]$stats.quality.capture_boundary
            right_censored = [string]$stats.quality.right_censored
            right_censored_queued = [string]$stats.quality.right_censored_queued
            right_censored_running = [string]$stats.quality.right_censored_running
            unexplained_missing_start = [string]$stats.quality.unexplained_missing_start
            unexplained_missing_terminal = [string]$stats.quality.unexplained_missing_terminal
            unexplained_truncated = [string]$stats.quality.unexplained_truncated
        }
        producer_quality = $stats.producer_quality
        chain_nodes = $chainNodes.Count
        chain_edges = $chainEdges.Count
        chain_evidence = @($chainEdges | ForEach-Object { [string]$_.evidence_kind } | Sort-Object -Unique)
        chain_coverage = $chainCoverage
        callstack = $callstack
        network_status = [string]$network.data.status
        validation_errors = [string]$validation.data.error_count
    }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n13-file-io-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $results = [ordered]@{}
    foreach ($entry in @(
        @{ label = 'snapshot'; path = $SnapshotTrace },
        @{ label = 'stream'; path = $StreamTrace },
        @{ label = 'replay'; path = $ReplayTrace }
    )) {
        $opened = Invoke-Tool 'tracy_trace_open' @{ path = $entry.path }
        $traceId = [string]$opened.data.trace_id
        $traceIds += $traceId
        $status = Wait-Ready $traceId
        $results[$entry.label] = [ordered]@{
            fingerprint = [string]$status.data.status.fingerprint
            semantics = Validate-N13 $traceId
        }
        [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $traceIds = @($traceIds | Where-Object { $_ -ne $traceId })
    }

    $snapshotSemantics = $results.snapshot.semantics | ConvertTo-Json -Compress -Depth 50
    Assert-Condition (($results.stream.semantics | ConvertTo-Json -Compress -Depth 50) -eq $snapshotSemantics) 'snapshot/stream N13 semantic mismatch'
    Assert-Condition (($results.replay.semantics | ConvertTo-Json -Compress -Depth 50) -eq $snapshotSemantics) 'snapshot/replay N13 semantic mismatch'
    [ordered]@{ ok = $true; schema_version = '1.24.0'; traces = $results } | ConvertTo-Json -Compress -Depth 60
}
finally {
    foreach ($traceId in $traceIds) {
        if (-not $queryProcess.HasExited) { try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) { $queryProcess.Kill($true); $queryProcess.WaitForExit() }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
