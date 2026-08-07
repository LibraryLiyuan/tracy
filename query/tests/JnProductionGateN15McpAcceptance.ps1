[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [switch] $RequireForcedDegrade
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
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
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
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) { throw "$Name failed: $($structured.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Inspect {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Wait-Ready {
    param([string] $TraceId)
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        if ([string]$status.data.status.state -eq 'ready') { return $status }
        if ([string]$status.data.status.state -in @('failed', 'closed')) { throw "trace entered $($status.data.status.state)" }
        Start-Sleep -Milliseconds 250
    }
    throw 'trace did not become ready'
}

function Validate-N15Trace {
    param([string] $TraceId, [string] $Label)
    $context = (Inspect $TraceId 'capture.context').data
    Assert-Condition ([bool]$context.present -and [bool]$context.complete) "$Label capture.context is incomplete"
    Assert-Condition ([UInt64]$context.context.runtime_policy.schema_version -eq 1) "$Label runtime policy schema is missing"
    Assert-Condition ([string]$context.context.capture_config.profile_requested -ne '') "$Label requested profile is missing"
    Assert-Condition ([string]$context.context.capture_config.profile_effective -ne '') "$Label effective profile is missing"
    $fixedOrder = @($context.context.runtime_policy.fixed_order | ForEach-Object { [string]$_ })
    Assert-Condition (($fixedOrder -join ',') -eq 'callstack,resource_reference,job_full_slice,gpu_l2_l3') "$Label fixed degradation order drifted"
    $preserved = @($context.context.runtime_policy.preserved | ForEach-Object { [string]$_ })
    foreach ($required in @('frame', 'gpu_l0', 'capture_identity', 'capture_context', 'producer_quality')) {
        Assert-Condition ($preserved -contains $required) "$Label does not preserve $required"
    }
    if ($RequireForcedDegrade) {
        Assert-Condition ([UInt64]$context.context.runtime_policy.degrade_count -ge 4) "$Label did not record four forced degradation steps"
        Assert-Condition ([UInt64]$context.context.runtime_policy.degrade_level -eq 0) "$Label did not recover to level zero"
    }

    $coverage = (Inspect $TraceId 'capture.coverage').data
    Assert-Condition ([bool]$coverage.present -and [bool]$coverage.complete) "$Label capture.coverage is incomplete"
    $producers = @($coverage.producers)
    Assert-Condition ($producers.Count -gt 0) "$Label has no producer quality records"
    foreach ($producer in $producers) {
        Assert-Condition ($null -ne $producer.runtime_policy) "$Label producer $($producer.key) omitted runtime_policy"
        Assert-Condition ([UInt64]$producer.runtime_policy.queue_high_watermark -gt 0) "$Label producer $($producer.key) omitted queue watermark"
        Assert-Condition ($null -ne $producer.runtime_policy.PSObject.Properties['max_events_per_frame']) "$Label producer $($producer.key) omitted event budget"
        Assert-Condition ($null -ne $producer.runtime_policy.PSObject.Properties['max_bytes_per_frame']) "$Label producer $($producer.key) omitted byte budget"
        Assert-Condition ($null -ne $producer.runtime_policy.PSObject.Properties['max_callstacks_per_frame']) "$Label producer $($producer.key) omitted callstack budget"
    }

    $validation = (Inspect $TraceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }).data
    Assert-Condition ([bool]$validation.valid -and [UInt64]$validation.error_count -eq 0) "$Label validation.run failed"

    return [ordered]@{
        profile_requested = [string]$context.context.capture_config.profile_requested
        profile_effective = [string]$context.context.capture_config.profile_effective
        degrade_level = [string]$context.context.runtime_policy.degrade_level
        degrade_count = [string]$context.context.runtime_policy.degrade_count
        fixed_order = $fixedOrder
        preserved = $preserved
        producer_policies = @($producers | Sort-Object key | ForEach-Object {
            [ordered]@{ key = [string]$_.key; runtime_policy = $_.runtime_policy }
        })
        validation_errors = [string]$validation.error_count
    }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n15-production-gate'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $opened = [ordered]@{}
    # MCP deliberately caps active sessions at two. Keep snapshot+replay open
    # for the pair comparison, then close replay before validating the stream.
    foreach ($entry in @(
        @{ label = 'snapshot'; path = $SnapshotTrace },
        @{ label = 'replay'; path = $ReplayTrace }
    )) {
        $result = Invoke-Tool 'tracy_trace_open' @{ path = $entry.path }
        $traceId = [string]$result.data.trace_id
        $traceIds += $traceId
        $status = Wait-Ready $traceId
        $opened[$entry.label] = [ordered]@{
            trace_id = $traceId
            fingerprint = [string]$status.data.status.fingerprint
            semantics = Validate-N15Trace $traceId $entry.label
        }
    }

    $compareParams = @{
        baseline_trace_id = $opened.replay.trace_id
        comparison_mode = 'performance'
        warmup_frames = 0
        window_frames = 1
    }
    $compatibility = (Inspect $opened.snapshot.trace_id 'compare.compatibility' $compareParams).data
    Assert-Condition ([UInt64]$compatibility.hard_failure_count -eq 0) "snapshot/replay compatibility has hard failures: $($compatibility.checks | ConvertTo-Json -Compress -Depth 20)"
    Assert-Condition ([bool]$compatibility.performance_comparable) 'snapshot/replay pair is not performance comparable'
    Assert-Condition ([bool]$compatibility.frame_window.valid) 'snapshot/replay common frame window is invalid'

    $normalizedParams = @{} + $compareParams
    $normalizedParams.allow_warnings = $true
    $normalizedParams.limit = 100
    $normalized = (Inspect $opened.snapshot.trace_id 'compare.normalized' $normalizedParams).data
    Assert-Condition ([bool]$normalized.performed) 'normalized comparison was refused'
    Assert-Condition ([string]$normalized.normalization.frame_alignment -eq 'complete-frame ordinal') 'normalized frame alignment drifted'

    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $opened.replay.trace_id })
    $traceIds = @($traceIds | Where-Object { $_ -ne $opened.replay.trace_id })
    $streamOpen = Invoke-Tool 'tracy_trace_open' @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $traceIds += $streamId
    $streamStatus = Wait-Ready $streamId
    $opened.stream = [ordered]@{
        trace_id = $streamId
        fingerprint = [string]$streamStatus.data.status.fingerprint
        semantics = Validate-N15Trace $streamId 'stream'
    }

    $canonical = $opened.snapshot.semantics | ConvertTo-Json -Compress -Depth 50
    Assert-Condition (($opened.stream.semantics | ConvertTo-Json -Compress -Depth 50) -eq $canonical) 'snapshot/stream N15 semantics differ'
    Assert-Condition (($opened.replay.semantics | ConvertTo-Json -Compress -Depth 50) -eq $canonical) 'snapshot/replay N15 semantics differ'

    $compatibilitySummary = [ordered]@{
        verdict = [string]$compatibility.verdict
        performance_comparable = [bool]$compatibility.performance_comparable
        hard_failure_count = [string]$compatibility.hard_failure_count
        warning_count = [string]$compatibility.warning_count
        frame_window = $compatibility.frame_window
    }
    $normalizedSummary = [ordered]@{
        performed = [bool]$normalized.performed
        normalization = $normalized.normalization
        frames = $normalized.frames
        cpu_count = @($normalized.cpu).Count
        gpu_count = @($normalized.gpu).Count
        job_count = @($normalized.jobs).Count
        producer_count = @($normalized.producers).Count
    }

    [ordered]@{
        ok = $true
        schema_version = '1.15.0'
        traces = $opened
        compatibility = $compatibilitySummary
        normalized = $normalizedSummary
    } | ConvertTo-Json -Compress -Depth 70
}
finally {
    foreach ($traceId in $traceIds) {
        if ($queryProcess -and -not $queryProcess.HasExited) { try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) { $queryProcess.Kill($true); $queryProcess.WaitForExit() }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
