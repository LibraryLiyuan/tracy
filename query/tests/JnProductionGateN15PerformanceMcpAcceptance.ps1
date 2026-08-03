[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string[]] $TracePaths,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [string] $OutputPath = ''
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

Assert-Condition ($TracePaths.Count -ge 3) 'at least three independent traces are required'
foreach ($path in @($QueryExe, $AllowRoot) + $TracePaths) {
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

function Open-And-Validate {
    param([string] $Path, [int] $Index)
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $status = Wait-Ready $traceId

    $context = (Inspect $traceId 'capture.context').data
    Assert-Condition ([bool]$context.present -and [bool]$context.complete) "run $Index capture.context is incomplete"
    Assert-Condition (@($context.missing_layers).Count -eq 0) "run $Index capture.context has missing layers"
    Assert-Condition (@($context.invalid_records).Count -eq 0) "run $Index capture.context has invalid records"
    Assert-Condition ([string]$context.context.capture_config.profile_requested -eq 'Summary') "run $Index requested profile is not Summary"
    Assert-Condition ([string]$context.context.capture_config.profile_effective -eq 'Summary') "run $Index effective profile is not Summary"
    $fixedOrder = @($context.context.runtime_policy.fixed_order | ForEach-Object { [string]$_ })
    Assert-Condition (($fixedOrder -join ',') -eq 'callstack,resource_reference,job_full_slice,gpu_l2_l3') "run $Index fixed degradation order drifted"
    $preserved = @($context.context.runtime_policy.preserved | ForEach-Object { [string]$_ })
    foreach ($required in @('frame', 'gpu_l0', 'capture_identity', 'capture_context', 'producer_quality')) {
        Assert-Condition ($preserved -contains $required) "run $Index does not preserve $required"
    }

    $coverage = (Inspect $traceId 'capture.coverage').data
    Assert-Condition ([bool]$coverage.present -and [bool]$coverage.complete) "run $Index capture.coverage is incomplete"
    $producers = @($coverage.producers)
    Assert-Condition ($producers.Count -gt 0) "run $Index has no producer quality records"
    foreach ($producer in $producers) {
        Assert-Condition ($null -ne $producer.runtime_policy) "run $Index producer $($producer.key) omitted runtime_policy"
    }

    $validation = (Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }).data
    Assert-Condition ([bool]$validation.valid -and [UInt64]$validation.error_count -eq 0) "run $Index validation.run failed"

    return [ordered]@{
        trace_id = $traceId
        path = (Resolve-Path -LiteralPath $Path).Path
        fingerprint = [string]$status.data.status.fingerprint
        profile_requested = [string]$context.context.capture_config.profile_requested
        profile_effective = [string]$context.context.capture_config.profile_effective
        degrade_level = [UInt64]$context.context.runtime_policy.degrade_level
        degrade_count = [UInt64]$context.context.runtime_policy.degrade_count
        producer_count = [UInt64]$producers.Count
        validation_errors = [UInt64]$validation.error_count
    }
}

function Compare-Traces {
    param([System.Collections.IDictionary] $Baseline, [System.Collections.IDictionary] $Candidate, [string] $Label)
    $parameters = @{
        baseline_trace_id = $Baseline.trace_id
        comparison_mode = 'performance'
        warmup_frames = 30
        window_frames = 120
    }
    $compatibility = (Inspect $Candidate.trace_id 'compare.compatibility' $parameters).data
    Assert-Condition ([UInt64]$compatibility.hard_failure_count -eq 0) "$Label compatibility has hard failures"
    Assert-Condition ([bool]$compatibility.performance_comparable) "$Label is not performance comparable"
    Assert-Condition ([bool]$compatibility.frame_window.valid) "$Label common frame window is invalid"

    $normalizedParameters = @{} + $parameters
    $normalizedParameters.allow_warnings = $true
    $normalizedParameters.limit = 100
    $normalized = (Inspect $Candidate.trace_id 'compare.normalized' $normalizedParameters).data
    Assert-Condition ([bool]$normalized.performed) "$Label normalized comparison was refused"
    Assert-Condition ([string]$normalized.normalization.frame_alignment -eq 'complete-frame ordinal') "$Label frame alignment drifted"

    return [ordered]@{
        label = $Label
        verdict = [string]$compatibility.verdict
        hard_failure_count = [UInt64]$compatibility.hard_failure_count
        warning_count = [UInt64]$compatibility.warning_count
        performance_comparable = [bool]$compatibility.performance_comparable
        frame_window = $compatibility.frame_window
        normalized_performed = [bool]$normalized.performed
        frame_alignment = [string]$normalized.normalization.frame_alignment
    }
}

$openTraceIds = New-Object 'System.Collections.Generic.List[string]'
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n15-performance-gate'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $results = New-Object 'System.Collections.Generic.List[object]'
    $comparisons = New-Object 'System.Collections.Generic.List[object]'
    $previous = $null
    for ($index = 0; $index -lt $TracePaths.Count; ++$index) {
        $current = Open-And-Validate $TracePaths[$index] ($index + 1)
        $openTraceIds.Add([string]$current.trace_id)
        $results.Add($current)
        if ($null -ne $previous) {
            $comparisons.Add((Compare-Traces $previous $current "run-$index-to-run-$($index + 1)"))
            [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $previous.trace_id })
            [void]$openTraceIds.Remove([string]$previous.trace_id)
        }
        $previous = $current
    }

    $report = [ordered]@{
        ok = $true
        schema_version = 1
        stage = 'N15'
        gate = 'performance-mcp'
        traces = $results.ToArray()
        comparisons = $comparisons.ToArray()
    }
    $json = $report | ConvertTo-Json -Depth 50
    if (-not [string]::IsNullOrEmpty($OutputPath)) {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrEmpty($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        [IO.File]::WriteAllText($OutputPath, $json, [Text.UTF8Encoding]::new($false))
    }
    $json
}
finally {
    foreach ($traceId in $openTraceIds.ToArray()) {
        if ($queryProcess -and -not $queryProcess.HasExited) { try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) { $queryProcess.Kill($true); $queryProcess.WaitForExit() }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
