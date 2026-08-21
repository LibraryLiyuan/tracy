[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [switch] $RealCapture,
    [ValidateSet(2, 3)][int] $ExpectedJobSchema = 2,
    [string] $ExpectedSourceMode = '',
    [switch] $ExpectContinuation,
    [ValidateRange(1, 60000)][int] $ValidationCpuMilliseconds = 5000,
    [switch] $RequireCompleteValidation,
    [string] $OutputPath = ''
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
$startInfo.Arguments = "--mcp --indexed --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
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
        if ([string]$status.data.status.state -eq 'ready' -and [bool]$status.data.status.complete) { return $status }
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
        $response = Inspect $TraceId 'job.search' @{ offset = $offset; limit = 1000; max_scan_events = 100000000; max_cpu_ms = 60000 }
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
    $statistics = Inspect $TraceId 'job.statistics' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
    $validation = Inspect $TraceId 'validation.run' @{ max_scan_events = 100000000; max_cpu_ms = $ValidationCpuMilliseconds }
    $jobs = Get-JobPage $TraceId
    $stats = $statistics.data

    Assert-Condition ([bool]$stats.present) 'job.statistics is not present'
    $sourceMode = if ([string]::IsNullOrWhiteSpace($ExpectedSourceMode)) { "native-hooks-job-v$ExpectedJobSchema" } else { $ExpectedSourceMode }
    Assert-Condition ([int]$stats.job_schema_version -eq $ExpectedJobSchema) "Job schema $ExpectedJobSchema was not detected"
    Assert-Condition ([string]$stats.source_mode -eq $sourceMode) 'unexpected Job source mode'
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
    if ($ExpectContinuation) {
        Assert-Condition ([UInt64]$stats.counts.v3 -gt 0) 'Job schema 3 events are empty'
        Assert-Condition ([UInt64]$stats.counts.wait_ends -gt 0) 'Job WaitEnd is missing'
        Assert-Condition ([UInt64]$stats.counts.continuations -gt 0) 'Job Continuation is missing'
        Assert-Condition ([UInt64]$stats.quality.missing_continuation -eq 0) 'normal WaitEnd is missing Continuation'
        Assert-Condition ([UInt64]$stats.quality.continuation_without_wait -eq 0) 'Continuation has no matching WaitEnd'
        Assert-Condition ([UInt64]$stats.quality.continuation_before_completion -eq 0) 'Continuation precedes Job completion'
    }
    Assert-Condition (-not [bool]$stats.quality.cancelled_supported) 'cancelled_supported must remain false for this uJobs branch'
    Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) 'trace validation failed'
    if ($RequireCompleteValidation) {
        Assert-Condition ([bool]$validation.data.complete) 'trace validation was partial'
    }

    if (-not $RealCapture) {
        $expectedSyntheticJobs = if ($ExpectContinuation) { 3 } else { 2 }
        Assert-Condition ([UInt64]$stats.counts.jobs -eq $expectedSyntheticJobs -and [UInt64]$stats.counts.v2 -eq $expectedSyntheticJobs) 'synthetic Job count mismatch'
        Assert-Condition ([UInt64]$stats.counts.completed -eq $expectedSyntheticJobs) 'synthetic completed Job count mismatch'
        Assert-Condition ([UInt64]$stats.counts.managed -eq 1 -and [UInt64]$stats.counts.burst -eq 1) 'synthetic kind count mismatch'
        Assert-Condition ([UInt64]$stats.counts.scheduler_steals -eq 1) 'synthetic scheduler steal count mismatch'
        Assert-Condition ([UInt64]$stats.counts.range_steal_slices -eq 1) 'synthetic range steal count mismatch'
        Assert-Condition ([UInt64]$stats.counts.wait_jobs -eq 1) 'synthetic wait count mismatch'
        Assert-Condition ([UInt64]$stats.counts.schedule_callstacks -eq 2) 'synthetic schedule callstack count mismatch'
        Assert-Condition ([UInt64]$stats.counts.wait_callstacks -eq 1) 'synthetic wait callstack count mismatch'
        Assert-Condition ([string]$context.data.context.runtime.target_kind -eq 'native-harness' -and
            [string]$context.data.context.workload.scene -eq 'synthetic') 'synthetic capture context mismatch'
        if ($ExpectContinuation) {
            Assert-Condition ([UInt64]$stats.counts.jobs -eq 3 -and [UInt64]$stats.counts.completed -eq 3) 'synthetic Job v3/boundary count mismatch'
            Assert-Condition ([UInt64]$stats.counts.v3 -eq 3) 'synthetic Job v3 count mismatch'
            Assert-Condition ([UInt64]$stats.counts.wait_ends -eq 1) 'synthetic WaitEnd count mismatch'
            Assert-Condition ([UInt64]$stats.counts.continuations -eq 1) 'synthetic Continuation count mismatch'
            Assert-Condition ([UInt64]$stats.counts.jobs_without_waiter -eq 2) 'synthetic no-waiter Job count mismatch'
            Assert-Condition ([UInt64]$stats.quality.capture_boundary_orphan -eq 1) 'synthetic capture-boundary Job missing'
        }
    }

    $scheduleJob = @($jobs | Where-Object { $null -ne $_.schedule_callstack_ref } | Select-Object -First 1)
    $waitCandidates = @($jobs | Where-Object {
        [UInt64]$_.wait.callstack_count -gt 0 -and
        (-not $ExpectContinuation -or (
            -not [bool]$_.capture_boundary -and
            $null -ne $_.origin_frame_ref -and
            [UInt64]$_.wait.end_count -gt 0 -and
            [UInt64]$_.wait.continuation_count -gt 0))
    })
    Assert-Condition ($scheduleJob.Count -eq 1) 'could not select a Job with schedule callstack'
    Assert-Condition ($waitCandidates.Count -gt 0) 'could not select a non-boundary Job with wait/continuation evidence'
    $waitJob = @($waitCandidates | Select-Object -First 1)
    $scheduleDepth = Resolve-Depth $TraceId ([string]$scheduleJob[0].schedule_callstack_ref) 'Job schedule'
    $detail = Inspect $TraceId 'job.get' @{ ref = [string]$waitJob[0].ref }
    $waitCallstacks = @($detail.data.wait_callstacks)
    Assert-Condition ($waitCallstacks.Count -gt 0) 'job.get omitted wait callstacks'
    Assert-Condition ([string]$waitCallstacks[0].callstack_kind -eq 'native') 'wait callstack kind mismatch'
    $waitDepth = Resolve-Depth $TraceId ([string]$waitCallstacks[0].callstack_ref) 'Job wait'

    $chainNodeCount = 0
    $continuationRelationCount = 0
    if ($ExpectContinuation) {
        $chain = Inspect $TraceId 'correlation.chain' @{
            ref = [string]$waitJob[0].origin_frame_ref
            max_nodes = 10000
            max_cpu_ms = 60000
        }
        $relations = @($chain.data.relations)
        $chainNodeCount = @($chain.data.nodes).Count
        $continuationRelationCount = @($relations | Where-Object { [string]$_.relation -eq 'continues_on_waiter' }).Count
        $chainRelationNames = @($relations | ForEach-Object { [string]$_.relation } | Sort-Object -Unique)
        Assert-Condition ($chainNodeCount -gt 2) ("Frame correlation chain did not reach Job stages; " +
            "job=$([string]$waitJob[0].job_id) frame=$([string]$waitJob[0].origin_frame_ref) " +
            "job_stage_count=$([string]$waitJob[0].stage_count) chain_nodes=$chainNodeCount " +
            "chain_relations=$($relations.Count) relation_names=$($chainRelationNames -join ',')")
        Assert-Condition ($continuationRelationCount -gt 0) 'Frame correlation chain omitted WaitEnd -> Continuation'
        Assert-Condition (@($relations | Where-Object { [string]$_.relation -eq 'completion_releases' }).Count -gt 0) 'Frame correlation chain omitted Complete -> Continuation'
        Assert-Condition ([string]$chain.data.evidence_kind -eq 'exact') 'Job continuation chain is not exact evidence'
        if ([bool]$chain.partial) {
            $exhaustedBy = @($chain.budget.exhausted_by | ForEach-Object { [string]$_ })
            Assert-Condition ([bool]$chain.data.truncated -and $chainNodeCount -eq 10000) `
                'partial Frame correlation chain did not stop at its explicit node budget'
            Assert-Condition ($exhaustedBy.Count -eq 1 -and $exhaustedBy[0] -eq 'max_nodes') `
                "Frame correlation chain exhausted an unexpected budget: $($exhaustedBy -join ',')"
        }
    }

    $stageNames = @($detail.data.stages | ForEach-Object { [string]$_.stage } | Sort-Object -Unique)
    $requiredStages = @('ready', 'queue_enter', 'dispatch')
    if ($ExpectContinuation) { $requiredStages += 'continuation' }
    foreach ($required in $requiredStages) {
        Assert-Condition ($stageNames -contains $required) "job.get is missing stage: $required"
    }
    Assert-Condition (($stageNames -contains 'wait_callstack') -or ($stageNames -contains 'wait_callsite')) `
        'job.get is missing both legacy wait_callstack and SiteReuse wait_callsite'
    if ($stageNames -contains 'wait_callsite') {
        $waitSite = @($detail.data.stages | Where-Object { [string]$_.stage -eq 'wait_callsite' } | Select-Object -First 1)
        Assert-Condition ($waitSite.Count -eq 1 -and $null -ne $waitSite[0].callsite_id) 'Wait SiteReuse callsite ID is missing'
        Assert-Condition ([string]$waitSite[0].provenance -eq 'SiteReused') 'Wait SiteReuse provenance is missing'
        Assert-Condition ($null -ne $waitSite[0].stack_ref) 'Wait SiteReuse stack reference is missing'
    }
    $ready = @($detail.data.stages | Where-Object { [string]$_.stage -eq 'ready' } | Select-Object -First 1)
    Assert-Condition ($ready.Count -eq 1 -and [string]$ready[0].reason -ne 'unknown') 'Ready reason is not decoded'
    $stealJob = @($jobs | Where-Object { [UInt64]$_.scheduler_steal_count -gt 0 } | Select-Object -First 1)
    Assert-Condition ($stealJob.Count -eq 1) 'could not select a Job with scheduler steal evidence'
    $stealDetail = Inspect $TraceId 'job.get' @{ ref = [string]$stealJob[0].ref }
    $steal = @($stealDetail.data.stages | Where-Object { [string]$_.stage -eq 'steal' } | Select-Object -First 1)
    Assert-Condition ($steal.Count -eq 1 -and $null -ne $steal[0].thief_lane -and $null -ne $steal[0].victim_lane) 'Steal lanes are not decoded'

    return [ordered]@{
        jobs = [string]$stats.counts.jobs
        v2 = [string]$stats.counts.v2
        v3 = if ($null -eq $stats.counts.v3) { '0' } else { [string]$stats.counts.v3 }
        completed = [string]$stats.counts.completed
        managed = [string]$stats.counts.managed
        burst = [string]$stats.counts.burst
        scheduler_steals = [string]$stats.counts.scheduler_steals
        range_steal_slices = [string]$stats.counts.range_steal_slices
        wait_jobs = [string]$stats.counts.wait_jobs
        schedule_callstacks = [string]$stats.counts.schedule_callstacks
        wait_callstacks = [string]$stats.counts.wait_callstacks
        wait_ends = if ($null -eq $stats.counts.wait_ends) { '0' } else { [string]$stats.counts.wait_ends }
        continuations = if ($null -eq $stats.counts.continuations) { '0' } else { [string]$stats.counts.continuations }
        jobs_without_waiter = if ($null -eq $stats.counts.jobs_without_waiter) { '0' } else { [string]$stats.counts.jobs_without_waiter }
        correlation_chain_nodes = $chainNodeCount
        continuation_relations = $continuationRelationCount
        lane_count = @($stats.lanes).Count
        schedule_callstack_depth = $scheduleDepth
        wait_callstack_depth = $waitDepth
        selected_stage_names = $stageNames
        boundary_orphan = [string]$stats.quality.capture_boundary_orphan
        boundary_truncated = [string]$stats.quality.capture_boundary_truncated
        validation_errors = [string]$validation.data.error_count
        validation_complete = [bool]$validation.data.complete
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
    $resultJson = [ordered]@{ ok = $true; schema_version = '1.30.0'; expected_job_schema = $ExpectedJobSchema; traces = $results } | ConvertTo-Json -Compress -Depth 40
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) { [void](New-Item -ItemType Directory -Force -Path $parent) }
        [IO.File]::WriteAllText($OutputPath, $resultJson, [Text.UTF8Encoding]::new($false))
    }
    $resultJson
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
