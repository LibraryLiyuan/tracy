[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$Snapshot,
    [Parameter(Mandatory = $true)][string]$Stream,
    [Parameter(Mandatory = $true)][string]$Replay,
    [Parameter(Mandatory = $true)][string]$TruncatedStream,
    [Parameter(Mandatory = $true)][string]$LegacyTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $Snapshot, $Stream, $Replay, $TruncatedStream, $LegacyTrace, $AllowRoot))
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

function Invoke-ToolRaw([string]$Name, [hashtable]$Arguments = @{})
{
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    return $response.result
}

function Invoke-Tool([string]$Name, [hashtable]$Arguments = @{})
{
    $result = Invoke-ToolRaw $Name $Arguments
    if ([bool]$result.isError)
    {
        throw "$Name failed: $($result.structuredContent | ConvertTo-Json -Compress -Depth 40)"
    }
    Assert-Condition ([bool]$result.structuredContent.ok) "$Name omitted structuredContent.ok=true"
    return $result.structuredContent
}

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    $arguments = @{ method = $Method; params = $Params }
    if ($TraceId) { $arguments.trace_id = $TraceId }
    return Invoke-Tool 'tracy_inspect' $arguments
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
    if (-not $TraceId) { return }
    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $TraceId })
}

function Get-Semantics([string]$TraceId, [string]$Label)
{
    $counts = Inspect $TraceId 'trace.counts'
    $context = Inspect $TraceId 'capture.context'
    Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Label capture context is incomplete"
    $zones = Inspect $TraceId 'zone.cpu.search' @{ limit = 3; fields = @('name', 'start_ns', 'end_ns', 'duration_ns') }
    Assert-Condition (@($zones.data.zones).Count -eq 3) "$Label did not return three CPU zones"
    return [ordered]@{
        label = $Label
        source_kind = [string]$counts.trace.source_kind
        trace_complete = [bool]$counts.trace.complete
        counts = $counts.data
        scene = [string]$context.data.context.workload.scene
        graphics_api = [string]$context.data.context.runtime.graphics_api
        graphics_jobs = [string]$context.data.context.runtime.graphics_jobs_effective
        zone_values = @($zones.data.zones | ForEach-Object {
            [ordered]@{ name = [string]$_.name; start_ns = [string]$_.start_ns; end_ns = [string]$_.end_ns; duration_ns = [string]$_.duration_ns }
        })
    }
}

function Assert-SemanticEqual($Left, $Right, [string]$Label)
{
    foreach ($field in @('frames', 'frame_sets', 'cpu_zones', 'gpu_zones', 'threads', 'memory_events', 'jobs', 'gfx_dispatches', 'gfx_entities', 'gfx_links'))
    {
        Assert-Condition ([string]$Left.counts.$field -eq [string]$Right.counts.$field) "$Label count differs for $field"
    }
    Assert-Condition ($Left.scene -eq $Right.scene) "$Label scene differs"
    Assert-Condition ($Left.graphics_api -eq $Right.graphics_api) "$Label graphics API differs"
    Assert-Condition ($Left.graphics_jobs -eq $Right.graphics_jobs) "$Label graphics-jobs mode differs"
    Assert-Condition (($Left.zone_values | ConvertTo-Json -Compress -Depth 10) -eq ($Right.zone_values | ConvertTo-Json -Compress -Depth 10)) "$Label projected CPU zones differ"
}

function Invoke-BudgetCursorGate([string]$TraceId)
{
    $unbounded = Inspect $TraceId 'zone.cpu.search' @{ limit = 164; fields = @('name', 'start_ns', 'end_ns') }
    Assert-Condition (-not [bool]$unbounded.partial) 'unbounded reference page is partial'
    Assert-Condition (@($unbounded.data.zones).Count -eq 164) 'unbounded reference page size mismatch'

    $first = Inspect $TraceId 'zone.cpu.search' @{ limit = 100; max_scan_events = 64; fields = @('name', 'start_ns', 'end_ns') }
    Assert-Condition ([bool]$first.partial) 'small scan budget did not produce partial=true'
    Assert-Condition ($null -eq $first.omitted_count) 'partial result fabricated an omitted count'
    Assert-Condition ([bool]$first.page.partial) 'partial page flag is false'
    Assert-Condition (-not [bool]$first.page.omitted_count_exact) 'partial page claims an exact omitted count'
    Assert-Condition (@($first.data.zones).Count -eq 64) 'small scan budget did not return the expected bounded prefix'
    Assert-Condition ([string]$first.budget.exhausted_by[0] -eq 'max_scan_events') 'scan budget exhaustion reason mismatch'
    Assert-Condition ($null -ne $first.page.next_cursor) 'partial scan omitted next_cursor'

    $second = Inspect $TraceId 'zone.cpu.search' @{
        limit = 100
        max_scan_events = 256
        fields = @('name', 'start_ns', 'end_ns')
        cursor = [string]$first.page.next_cursor
    }
    Assert-Condition (-not [bool]$second.partial) 'resumed full page is incorrectly partial'
    Assert-Condition (@($second.data.zones).Count -eq 100) 'resumed page size mismatch'

    $expected = @($unbounded.data.zones | ForEach-Object { [string]$_.ref })
    $actual = @($first.data.zones | ForEach-Object { [string]$_.ref }) + @($second.data.zones | ForEach-Object { [string]$_.ref })
    Assert-Condition (($actual | Select-Object -Unique).Count -eq 164) 'budget cursor produced duplicate refs'
    Assert-Condition (($actual -join '|') -eq ($expected -join '|')) 'budget cursor omitted or reordered refs'

    $groupBudget = Inspect $TraceId 'zone.cpu.flamegraph' @{ limit = 20; max_groups = 1; max_scan_events = 100000; max_cpu_ms = 60000 }
    Assert-Condition ([bool]$groupBudget.partial) 'max_groups did not produce a partial result'
    Assert-Condition (@($groupBudget.budget.exhausted_by) -contains 'max_groups') 'max_groups exhaustion reason is absent'

    $frameIdentity = Inspect $TraceId 'frame.identity' @{ limit = 1 }
    $frameRef = [string]$frameIdentity.data.identities[0].ref
    Assert-Condition (-not [string]::IsNullOrEmpty($frameRef)) 'frame.identity did not provide a correlation root'
    $nodeBudget = Inspect $TraceId 'correlation.chain' @{ ref = $frameRef; max_nodes = 1; max_cpu_ms = 60000 }
    Assert-Condition ([bool]$nodeBudget.partial) 'max_nodes did not produce a partial result'
    Assert-Condition ([bool]$nodeBudget.data.truncated) 'max_nodes result is not marked truncated'
    Assert-Condition (@($nodeBudget.budget.exhausted_by) -contains 'max_nodes') 'max_nodes exhaustion reason is absent'

    return [ordered]@{
        first_returned = @($first.data.zones).Count
        second_returned = @($second.data.zones).Count
        unique_refs = ($actual | Select-Object -Unique).Count
        first_budget = $first.budget
        second_budget = $second.budget
        group_budget = $groupBudget.budget
        node_budget = $nodeBudget.budget
    }
}

function Invoke-CancellationGate([string]$TraceId)
{
    $submitted = Invoke-Tool 'tracy_validate' @{ trace_id = $TraceId; async = $true; max_scan_events = 100000000; max_cpu_ms = 60000 }
    $jobId = [string]$submitted.data.job_id
    Assert-Condition (-not [string]::IsNullOrEmpty($jobId)) 'async validation did not return a job_id'
    $cancelled = Invoke-Tool 'tracy_job' @{ job_id = $jobId; operation = 'cancel' }
    Assert-Condition ([bool]$cancelled.data.cancel_requested) 'job cancel request was not accepted'
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    do
    {
        $status = Invoke-Tool 'tracy_job' @{ job_id = $jobId; operation = 'status' }
        if ([bool]$status.data.done) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    Assert-Condition ([string]$status.data.state -eq 'cancelled') "async validation ended as $($status.data.state), not cancelled"
    $result = Invoke-ToolRaw 'tracy_job' @{ job_id = $jobId; operation = 'result' }
    Assert-Condition ([bool]$result.isError) 'cancelled job result is not an error'
    Assert-Condition ([string]$result.structuredContent.error.code -eq 'CANCELLED') 'cancelled job result code mismatch'
    return [ordered]@{ job_id = $jobId; final_state = [string]$status.data.state; result_code = [string]$result.structuredContent.error.code }
}

if (-not $process.Start()) { throw 'failed to start tracy-query MCP server' }
$script:Process = $process
try
{
    $initialize = Send-Rpc 'initialize' @{
        protocolVersion = '2025-11-25'
        clientInfo = @{ name = 'jn-query-contract-acceptance'; version = '1' }
        capabilities = @{}
    }
    Assert-Condition ([string]$initialize.result.protocolVersion -eq '2025-11-25') 'MCP protocol version mismatch'
    Notify-Initialized

    $tools = Send-Rpc 'tools/list'
    $inspectTool = @($tools.result.tools | Where-Object { $_.name -eq 'tracy_inspect' })[0]
    Assert-Condition ($null -ne $inspectTool) 'tracy_inspect tool is absent'
    $operationSchemas = @($inspectTool.inputSchema.PSObject.Properties['x-tracy-operationSchemas'].Value)
    $methodEnum = @($inspectTool.inputSchema.properties.method.enum)
    Assert-Condition ($operationSchemas.Count -eq $methodEnum.Count) 'MCP method enum and operation schema registry differ'
    Assert-Condition ([string]$inspectTool.outputSchema.properties.schema_version.const -eq '1.5.0') 'MCP output schema version mismatch'

    $schema = Inspect '' 'system.schema'
    $describe = Inspect '' 'system.describe'
    Assert-Condition ([string]$schema.schema_version -eq '1.5.0') 'Query schema version mismatch'
    Assert-Condition (@($schema.data.operations).Count -eq $operationSchemas.Count) 'system.schema registry count differs from MCP'
    Assert-Condition (@($describe.data.operations).Count -eq $operationSchemas.Count) 'system.describe registry count differs from MCP'

    $snapshotId = Open-Trace $Snapshot
    $streamId = Open-Trace $Stream
    $snapshotSemantics = Get-Semantics $snapshotId 'snapshot'
    $streamSemantics = Get-Semantics $streamId 'committed_stream'
    Assert-Condition ($snapshotSemantics.source_kind -eq 'snapshot') 'snapshot source kind mismatch'
    Assert-Condition ($streamSemantics.source_kind -eq 'segment') 'committed stream source kind mismatch'
    Assert-Condition ($snapshotSemantics.trace_complete -and $streamSemantics.trace_complete) 'snapshot or committed stream is marked incomplete'
    Assert-SemanticEqual $snapshotSemantics $streamSemantics 'snapshot/stream'
    Close-Trace $streamId

    $replayId = Open-Trace $Replay
    $replaySemantics = Get-Semantics $replayId 'stream_replay'
    Assert-Condition ($replaySemantics.source_kind -eq 'snapshot') 'stream replay source kind mismatch'
    Assert-Condition ($replaySemantics.trace_complete) 'stream replay is marked incomplete'
    Assert-SemanticEqual $snapshotSemantics $replaySemantics 'snapshot/replay'

    $budgetGate = Invoke-BudgetCursorGate $snapshotId
    $cancellationGate = Invoke-CancellationGate $snapshotId
    Close-Trace $replayId
    Close-Trace $snapshotId

    $truncatedId = Open-Trace $TruncatedStream
    $truncatedValidation = Inspect $truncatedId 'validation.run' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
    Assert-Condition (-not [bool]$truncatedValidation.trace.complete) 'truncated stream is marked complete'
    Assert-Condition (-not [bool]$truncatedValidation.data.complete) 'truncated validation is marked complete'
    $tailFinding = @($truncatedValidation.data.findings | Where-Object { $_.code -eq 'TRUNCATED_STREAM_TAIL' })
    Assert-Condition ($tailFinding.Count -eq 1) 'truncated stream diagnostic is absent or duplicated'
    Assert-Condition (-not [bool]$truncatedValidation.partial) 'truncated validation unexpectedly exhausted query budget'
    Close-Trace $truncatedId

    $legacyId = Open-Trace $LegacyTrace
    $legacyContext = Inspect $legacyId 'capture.context'
    $legacyCatalog = Inspect $legacyId 'catalog.kinds'
    Assert-Condition (-not [bool]$legacyContext.data.present) 'legacy trace fabricated capture context'
    Assert-Condition (-not [bool]$legacyCatalog.data.present) 'legacy trace fabricated catalog data'
    Assert-Condition (-not [string]::IsNullOrEmpty([string]$legacyContext.data.reason)) 'legacy capture context omitted absence reason'
    Assert-Condition (-not [string]::IsNullOrEmpty([string]$legacyCatalog.data.reason)) 'legacy catalog omitted absence reason'
    Close-Trace $legacyId

    $result = [ordered]@{
        schema_version = '1.5.0'
        registry = [ordered]@{ methods = $methodEnum.Count; describe = @($describe.data.operations).Count; system_schema = @($schema.data.operations).Count; mcp = $operationSchemas.Count }
        semantics = [ordered]@{ snapshot = $snapshotSemantics; committed_stream = $streamSemantics; stream_replay = $replaySemantics }
        budget_cursor = $budgetGate
        cancellation = $cancellationGate
        truncated_stream = [ordered]@{
            source_kind = [string]$truncatedValidation.trace.source_kind
            trace_complete = [bool]$truncatedValidation.trace.complete
            validation_complete = [bool]$truncatedValidation.data.complete
            finding = $tailFinding[0]
        }
        legacy = [ordered]@{
            capture_context_present = [bool]$legacyContext.data.present
            capture_context_reason = [string]$legacyContext.data.reason
            catalog_present = [bool]$legacyCatalog.data.present
            catalog_reason = [string]$legacyCatalog.data.reason
        }
        files = [ordered]@{}
    }
    foreach ($item in ([ordered]@{ snapshot = $Snapshot; stream = $Stream; replay = $Replay; truncated_stream = $TruncatedStream; legacy = $LegacyTrace }).GetEnumerator())
    {
        $file = Get-Item -LiteralPath $item.Value
        $result.files[$item.Key] = [ordered]@{ path = $file.FullName; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $result | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $OutputFile -Encoding UTF8
    $result | ConvertTo-Json -Depth 12
}
finally
{
    foreach ($traceId in $script:OpenedIds)
    {
        try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {}
    }
    try { $process.StandardInput.Close() } catch {}
    if (-not $process.WaitForExit(5000)) { $process.Kill() }
    $process.Dispose()
}
