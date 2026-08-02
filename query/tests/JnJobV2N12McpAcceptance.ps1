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

function Resolve-Depth {
    param([string] $TraceId, [string] $CallstackRef, [string] $Label)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($CallstackRef)) "$Label has no callstack ref"
    $resolved = Inspect $TraceId 'callstack.frames' @{ callstack = $CallstackRef; max_depth = 64 }
    $frames = @($resolved.data.frames)
    Assert-Condition ($frames.Count -gt 0) "$Label callstack did not resolve"
    Assert-Condition ($frames.Count -le 62) "$Label callstack exceeded the Windows safe maximum"
    return $frames.Count
}

function Get-JobPage {
    param([string] $TraceId)
    $values = [System.Collections.Generic.List[object]]::new()
    for ($offset = 0; $offset -lt 10000; $offset += 1000) {
        $response = Inspect $TraceId 'job.search' @{ offset = $offset; limit = 1000 }
        $page = @($response.data.jobs)
        foreach ($job in $page) { $values.Add($job) }
        if ($page.Count -lt 1000) { break }
    }
    return @($values)
}

function Validate-N12 {
    param([string] $TraceId)
    $capabilities = Inspect $TraceId 'system.capabilities'
    $jobCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'job' })
    Assert-Condition ($jobCapability.Count -eq 1 -and [bool]$jobCapability[0].present) 'job capability unavailable'
    Assert-Condition (@($jobCapability[0].methods) -contains 'job.statistics') 'job.statistics capability missing'

    $context = Inspect $TraceId 'capture.context'
    $statistics = Inspect $TraceId 'job.statistics'
    $validation = Inspect $TraceId 'validation.run'
    $jobs = Get-JobPage $TraceId
    $stats = $statistics.data

    Assert-Condition ([bool]$stats.present) 'job.statistics is not present'
    Assert-Condition ([int]$stats.job_schema_version -eq 2) 'Job schema 2 was not detected'
    Assert-Condition ([string]$stats.source_mode -eq 'native-hooks-job-v2') 'unexpected Job source mode'
    Assert-Condition ([string]$stats.callstack_kind -eq 'native') 'Job callstack kind is not native'
    Assert-Condition ([UInt64]$stats.counts.jobs -gt 0 -and [UInt64]$stats.counts.v2 -gt 0) 'Job v2 events are empty'
    Assert-Condition ([UInt64]$stats.counts.completed -gt 0) 'no completed Job was captured'
    Assert-Condition ([UInt64]$stats.counts.managed -gt 0) 'Managed Job type binding is missing'
    Assert-Condition ([UInt64]$stats.counts.burst -gt 0) 'Burst Job type binding is missing'
    Assert-Condition ([UInt64]$stats.counts.scheduler_steals -gt 0) 'scheduler lane steal evidence is missing'
    Assert-Condition ([UInt64]$stats.counts.range_steal_slices -gt 0) 'range partition steal evidence is missing'
    Assert-Condition ([UInt64]$stats.counts.wait_jobs -gt 0) 'Job wait decomposition is missing'
    Assert-Condition ([UInt64]$stats.counts.schedule_callstacks -gt 0) 'Job schedule native callstack is missing'
    Assert-Condition ([UInt64]$stats.counts.wait_callstacks -gt 0) 'Job wait native callstack is missing'
    Assert-Condition (@($stats.lanes).Count -gt 0) 'execution lane data is missing'
    Assert-Condition ([bool]$stats.quality.complete) 'Job v2 quality gate is incomplete'
    Assert-Condition ([UInt64]$stats.quality.missing_ready -eq 0) 'non-boundary Job is missing Ready'
    Assert-Condition ([UInt64]$stats.quality.missing_queue -eq 0) 'non-boundary dispatched Job is missing QueueEnter'
    Assert-Condition ([UInt64]$stats.quality.invalid_order -eq 0) 'Job stage timestamp order is invalid'
    Assert-Condition (-not [bool]$stats.quality.cancelled_supported) 'cancelled_supported must remain false for this uJobs branch'
    Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) 'trace validation failed'

    if (-not $RealCapture) {
        Assert-Condition ([UInt64]$stats.counts.jobs -eq 2 -and [UInt64]$stats.counts.v2 -eq 2) 'synthetic Job count mismatch'
        Assert-Condition ([UInt64]$stats.counts.completed -eq 2) 'synthetic completed Job count mismatch'
        Assert-Condition ([UInt64]$stats.counts.managed -eq 1 -and [UInt64]$stats.counts.burst -eq 1) 'synthetic kind count mismatch'
        Assert-Condition ([UInt64]$stats.counts.scheduler_steals -eq 1) 'synthetic scheduler steal count mismatch'
        Assert-Condition ([UInt64]$stats.counts.range_steal_slices -eq 1) 'synthetic range steal count mismatch'
        Assert-Condition ([UInt64]$stats.counts.wait_jobs -eq 1) 'synthetic wait count mismatch'
        Assert-Condition ([UInt64]$stats.counts.schedule_callstacks -eq 2) 'synthetic schedule callstack count mismatch'
        Assert-Condition ([UInt64]$stats.counts.wait_callstacks -eq 1) 'synthetic wait callstack count mismatch'
        Assert-Condition ([string]$context.data.context.workload.scenario -eq 'n12-job-v2') 'synthetic capture context mismatch'
    }

    $scheduleJob = @($jobs | Where-Object { $null -ne $_.schedule_callstack_ref } | Select-Object -First 1)
    $waitJob = @($jobs | Where-Object { [UInt64]$_.wait.callstack_count -gt 0 } | Select-Object -First 1)
    Assert-Condition ($scheduleJob.Count -eq 1) 'could not select a Job with schedule callstack'
    Assert-Condition ($waitJob.Count -eq 1) 'could not select a Job with wait callstack'
    $scheduleDepth = Resolve-Depth $TraceId ([string]$scheduleJob[0].schedule_callstack_ref) 'Job schedule'
    $detail = Inspect $TraceId 'job.get' @{ ref = [string]$waitJob[0].ref }
    $waitCallstacks = @($detail.data.wait_callstacks)
    Assert-Condition ($waitCallstacks.Count -gt 0) 'job.get omitted wait callstacks'
    Assert-Condition ([string]$waitCallstacks[0].callstack_kind -eq 'native') 'wait callstack kind mismatch'
    $waitDepth = Resolve-Depth $TraceId ([string]$waitCallstacks[0].callstack_ref) 'Job wait'

    $stageNames = @($detail.data.stages | ForEach-Object { [string]$_.stage } | Sort-Object -Unique)
    foreach ($required in @('ready', 'queue_enter', 'dispatch', 'steal', 'wait_callstack')) {
        Assert-Condition ($stageNames -contains $required) "job.get is missing stage: $required"
    }
    $ready = @($detail.data.stages | Where-Object { [string]$_.stage -eq 'ready' } | Select-Object -First 1)
    $steal = @($detail.data.stages | Where-Object { [string]$_.stage -eq 'steal' } | Select-Object -First 1)
    Assert-Condition ($ready.Count -eq 1 -and [string]$ready[0].reason -ne 'unknown') 'Ready reason is not decoded'
    Assert-Condition ($steal.Count -eq 1 -and $null -ne $steal[0].thief_lane -and $null -ne $steal[0].victim_lane) 'Steal lanes are not decoded'

    return [ordered]@{
        jobs = [string]$stats.counts.jobs
        v2 = [string]$stats.counts.v2
        completed = [string]$stats.counts.completed
        managed = [string]$stats.counts.managed
        burst = [string]$stats.counts.burst
        scheduler_steals = [string]$stats.counts.scheduler_steals
        range_steal_slices = [string]$stats.counts.range_steal_slices
        wait_jobs = [string]$stats.counts.wait_jobs
        schedule_callstacks = [string]$stats.counts.schedule_callstacks
        wait_callstacks = [string]$stats.counts.wait_callstacks
        lane_count = @($stats.lanes).Count
        schedule_callstack_depth = $scheduleDepth
        wait_callstack_depth = $waitDepth
        selected_stage_names = $stageNames
        boundary_orphan = [string]$stats.quality.capture_boundary_orphan
        boundary_truncated = [string]$stats.quality.capture_boundary_truncated
        validation_errors = [string]$validation.data.error_count
    }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n12-job-v2-acceptance'; version = '1.0' } }
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
            semantics = Validate-N12 $traceId
        }
        [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $traceIds = @($traceIds | Where-Object { $_ -ne $traceId })
    }

    $snapshotSemantics = $results.snapshot.semantics | ConvertTo-Json -Compress -Depth 30
    Assert-Condition (($results.stream.semantics | ConvertTo-Json -Compress -Depth 30) -eq $snapshotSemantics) 'snapshot/stream N12 semantic mismatch'
    Assert-Condition (($results.replay.semantics | ConvertTo-Json -Compress -Depth 30) -eq $snapshotSemantics) 'snapshot/replay N12 semantic mismatch'
    [ordered]@{ ok = $true; schema_version = '1.11.0'; traces = $results } | ConvertTo-Json -Compress -Depth 40
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
