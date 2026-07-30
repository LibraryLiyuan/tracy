[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $QueryExe,

    [Parameter(Mandatory = $true)]
    [string] $SnapshotTrace,

    [Parameter(Mandatory = $true)]
    [string] $StreamTrace,

    [Parameter(Mandatory = $true)]
    [string] $AllowRoot
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

foreach ($requiredPath in @($QueryExe, $SnapshotTrace, $StreamTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
}

$script:NextRequestId = 1
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true

$queryProcess = [System.Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo

function Send-Notification {
    param([string] $Method, [hashtable] $Params = @{})
    $payload = [ordered]@{ jsonrpc = '2.0'; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc {
    param(
        [string] $Method,
        [hashtable] $Params = @{},
        [int] $TimeoutMilliseconds = 240000
    )
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $script:queryProcess.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) {
            throw "MCP response timed out: $Method (id=$id)"
        }
        $line = $read.Result
        if ($null -eq $line) {
            throw "MCP stdout closed before response: $Method (id=$id)"
        }
        $message = $line | ConvertFrom-Json
        if ($null -ne $message.PSObject.Properties['id']) {
            Assert-Condition ([string]$message.id -eq [string]$id) "unexpected response id $($message.id), expected $id"
            return $message
        }
    }
    throw "MCP response deadline expired: $Method (id=$id)"
}

function Invoke-McpTool {
    param([string] $Name, [hashtable] $Arguments = @{})
    $response = Send-Rpc -Method 'tools/call' -Params @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) {
        throw "$Name returned an error: $($structured.error | ConvertTo-Json -Compress -Depth 20)"
    }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Wait-TraceReady {
    param([string] $TraceId)
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-McpTool -Name 'tracy_trace_status' -Arguments @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') {
            return $status
        }
        if ($state -in @('failed', 'closed')) {
            throw "trace $TraceId reached terminal state $state"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready"
}

function Inspect {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    return Invoke-McpTool -Name 'tracy_inspect' -Arguments @{
        trace_id = $TraceId
        method = $Method
        params = $Params
    }
}

function Validate-GfxData {
    param([string] $TraceId)
    $stats = Inspect -TraceId $TraceId -Method 'job.gfx.statistics'
    Assert-Condition ([UInt64]$stats.data.counts.dispatches -gt 0) 'no Gfx dispatches'
    Assert-Condition ([UInt64]$stats.data.counts.entities -gt 0) 'no Gfx entities'
    Assert-Condition ([UInt64]$stats.data.counts.links -gt 0) 'no Gfx links'
    foreach ($relation in @('dispatches', 'executes', 'produces', 'submits')) {
        Assert-Condition ([UInt64]$stats.data.links_by_relation.$relation -gt 0) "missing $relation links"
    }
    foreach ($field in @('dangling_parent_entities', 'dangling_link_sources', 'dangling_link_targets', 'uncaptured_execute_links')) {
        Assert-Condition ([UInt64]$stats.data.integrity.$field -eq 0) "$field is non-zero"
    }
    Assert-Condition ([UInt64]$stats.data.integrity.captured_execute_links -eq [UInt64]$stats.data.links_by_relation.executes) 'execute links do not resolve to captured jobs'

    $jobRef = [string]$stats.data.samples.linked_job_ref
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($jobRef)) 'statistics omitted linked_job_ref'
    $chain = Inspect -TraceId $TraceId -Method 'job.gfx_chain' -Params @{ ref = $jobRef; max_nodes = 200 }
    Assert-Condition (-not [bool]$chain.data.truncated) 'representative Gfx chain was truncated'
    Assert-Condition (@($chain.data.dispatches).Count -eq 1) 'representative chain did not contain exactly one dispatch'
    Assert-Condition (@($chain.data.jobs).Count -gt 0) 'representative chain omitted jobs'
    Assert-Condition (@($chain.data.entities).Count -gt 0) 'representative chain omitted entities'
    $chainRelations = @($chain.data.links.relation | Sort-Object -Unique)
    foreach ($relation in @('dispatches', 'executes', 'produces', 'submits')) {
        Assert-Condition ($relation -in $chainRelations) "representative chain omitted $relation"
    }

    $job = Inspect -TraceId $TraceId -Method 'job.get' -Params @{ ref = $jobRef }
    $threads = Inspect -TraceId $TraceId -Method 'thread.list' -Params @{ limit = 100 }
    $threadByRef = @{}
    foreach ($thread in @($threads.data.threads)) {
        $threadByRef[[string]$thread.ref] = [string]$thread.name
    }
    $dispatchThread = [string]$chain.data.dispatches[0].thread_ref
    $submissionThread = [string](@($chain.data.entities | Where-Object { $_.kind -eq 'submission' })[0].thread_ref)
    $workerStage = @($job.data.stages | Where-Object { $_.stage -eq 'worker_slice_begin' })[0]
    Assert-Condition ($threadByRef[$dispatchThread] -eq 'Render Thread') "dispatch thread is $($threadByRef[$dispatchThread])"
    Assert-Condition ($threadByRef[$submissionThread] -eq 'D3D12 Task.Worker') "submission thread is $($threadByRef[$submissionThread])"
    Assert-Condition ($threadByRef[[string]$workerStage.thread_ref] -like 'Job.Worker *') "worker slice thread is $($threadByRef[[string]$workerStage.thread_ref])"

    return [pscustomobject]@{
        stats = $stats.data
        chain = [pscustomobject]@{
            job_ref = $jobRef
            jobs = @($chain.data.jobs).Count
            dispatches = @($chain.data.dispatches).Count
            entities = @($chain.data.entities).Count
            links = @($chain.data.links).Count
            relations = $chainRelations
            dispatch_thread = $threadByRef[$dispatchThread]
            worker_thread = $threadByRef[[string]$workerStage.thread_ref]
            submission_thread = $threadByRef[$submissionThread]
        }
    }
}

$snapshotId = ''
$streamId = ''
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc -Method 'initialize' -Params @{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'jn-gfx-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Send-Notification -Method 'notifications/initialized'

    $snapshotOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $SnapshotTrace }
    $snapshotId = [string]$snapshotOpen.data.trace_id
    $streamOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $snapshotStatus = Wait-TraceReady -TraceId $snapshotId
    $streamStatus = Wait-TraceReady -TraceId $streamId

    $snapshot = Validate-GfxData -TraceId $snapshotId
    $stream = Validate-GfxData -TraceId $streamId
    foreach ($field in @('dispatches', 'entities', 'links', 'jobs')) {
        Assert-Condition ([string]$snapshot.stats.counts.$field -eq [string]$stream.stats.counts.$field) "snapshot/stream $field mismatch"
    }
    foreach ($relation in @('dispatches', 'executes', 'produces', 'submits')) {
        Assert-Condition ([string]$snapshot.stats.links_by_relation.$relation -eq [string]$stream.stats.links_by_relation.$relation) "snapshot/stream $relation mismatch"
    }

    [pscustomobject]@{
        ok = $true
        snapshot_fingerprint = [string]$snapshotStatus.data.status.fingerprint
        stream_fingerprint = [string]$streamStatus.data.status.fingerprint
        counts = $snapshot.stats.counts
        entities_by_kind = $snapshot.stats.entities_by_kind
        links_by_relation = $snapshot.stats.links_by_relation
        integrity = $snapshot.stats.integrity
        representative_chain = $snapshot.chain
    } | ConvertTo-Json -Compress -Depth 20
}
finally {
    foreach ($traceId in @($snapshotId, $streamId)) {
        if (-not [string]::IsNullOrWhiteSpace($traceId) -and -not $queryProcess.HasExited) {
            try { [void](Invoke-McpTool -Name 'tracy_trace_close' -Arguments @{ trace_id = $traceId }) } catch {}
        }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) {
            $queryProcess.Kill($true)
            $queryProcess.WaitForExit()
        }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
