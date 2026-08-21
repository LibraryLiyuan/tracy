[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $PrimaryTrace,
    [Parameter(Mandatory = $true)][string] $PrimaryFrameId,
    [Parameter(Mandatory = $true)][string] $ScriptTrace,
    [Parameter(Mandatory = $true)][string] $ScriptFrameId,
    [Parameter(Mandatory = $true)][string] $ElevatedTrace,
    [Parameter(Mandatory = $true)][string] $ElevatedFrameId,
    [Parameter(Mandatory = $true)][string] $AllowRoot
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $PrimaryTrace, $ScriptTrace, $ElevatedTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --indexed --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$queryProcess = [System.Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo
$script:NextRequestId = 1

function Send-Notification {
    param([string] $Method, [hashtable] $Params = @{})
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc {
    param([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 120000)
    $id = $script:NextRequestId++
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $read = $script:queryProcess.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    $stopwatch.Stop()
    if ($null -eq $read.Result) { throw "MCP stdout closed before response: $Method" }
    $message = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    $message | Add-Member -NotePropertyName wall_ms -NotePropertyValue $stopwatch.ElapsedMilliseconds
    $message | Add-Member -NotePropertyName response_bytes -NotePropertyValue ([Text.Encoding]::UTF8.GetByteCount($read.Result))
    return $message
}

function Invoke-Tool {
    param([string] $Name, [hashtable] $Arguments = @{})
    $response = Send-Rpc -Method 'tools/call' -Params @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) { throw "$Name failed: $($structured.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    $structured | Add-Member -NotePropertyName wall_ms -NotePropertyValue $response.wall_ms
    $structured | Add-Member -NotePropertyName response_bytes -NotePropertyValue $response.response_bytes
    return $structured
}

function Inspect {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    return Invoke-Tool -Name 'tracy_inspect' -Arguments @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Open-ReadyTrace {
    param([string] $Path)
    $opened = Invoke-Tool -Name 'tracy_trace_open' -Arguments @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-Tool -Name 'tracy_trace_status' -Arguments @{ trace_id = $traceId }
        if ([string]$status.data.status.state -eq 'ready') { return $traceId }
        if ([string]$status.data.status.state -in @('failed', 'closed')) { throw "trace entered $($status.data.status.state)" }
        Start-Sleep -Milliseconds 100
    }
    throw 'trace did not become ready'
}

function Close-Trace {
    param([string] $TraceId)
    [void](Invoke-Tool -Name 'tracy_trace_close' -Arguments @{ trace_id = $TraceId })
}

function Get-Capability {
    param([object] $Capabilities, [string] $Domain)
    return @($Capabilities.data.domains | Where-Object { [string]$_.domain -eq $Domain })[0]
}

$activeTrace = ''
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n16-evidence-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $activeTrace = Open-ReadyTrace $PrimaryTrace
    $capabilities = Inspect $activeTrace 'system.capabilities'
    $sampleCapability = Get-Capability $capabilities 'sample'
    $contextCapability = Get-Capability $capabilities 'context_switch'
    Assert-Condition (-not [bool]$sampleCapability.present) 'primary trace unexpectedly reports Sampling data'
    Assert-Condition (-not [bool]$contextCapability.present) 'primary trace unexpectedly reports Context Switch data'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$sampleCapability.reason)) 'missing Sampling absence reason'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$contextCapability.reason)) 'missing Context Switch absence reason'

    $evidenceLimits = @{ max_nodes = 50000; max_edges = 100000; max_scan_events = 5000000; max_cpu_ms = 60000 }
    $critical = Inspect $activeTrace 'frame.critical_path' (@{ frame_id = $PrimaryFrameId } + $evidenceLimits)
    $explain = Inspect $activeTrace 'frame.explain' (@{ frame_id = $PrimaryFrameId } + $evidenceLimits)
    Assert-Condition (-not [bool]$critical.partial -and [bool]$critical.data.complete) 'primary critical path is partial or incomplete'
    Assert-Condition (-not [bool]$critical.data.critical_path.has_cycle -and [bool]$critical.data.critical_path.valid_contribution) 'primary critical path is invalid'
    Assert-Condition (-not [bool]$explain.partial -and [bool]$explain.data.complete) 'primary frame explain is partial or incomplete'
    Assert-Condition ($critical.wall_ms -le 5000) 'primary critical path exceeded 5 seconds'
    Assert-Condition ($explain.wall_ms -le 5000) 'primary frame explain exceeded 5 seconds'

    $resource = Inspect $activeTrace 'evidence.graph' @{
        frame_id = $PrimaryFrameId; domains = @('gpu', 'resource', 'submission');
        max_nodes = 5000; max_edges = 10000; max_scan_events = 5000000; max_cpu_ms = 60000
    }
    Assert-Condition (-not [bool]$resource.partial) 'focused resource evidence graph is partial'
    Assert-Condition ($resource.wall_ms -le 5000) 'focused resource evidence graph exceeded 5 seconds'
    Assert-Condition ($resource.response_bytes -le 16MB) 'focused resource evidence response exceeded 16 MiB'
    $resourceNodes = @($resource.data.nodes | Where-Object { [string]$_.domain -eq 'resource' })
    $resourceEdges = @($resource.data.edges)
    $logicalCount = @($resourceNodes | Where-Object { [string]$_.kind -eq 'gpu_logical_resource' }).Count
    $physicalCount = @($resourceNodes | Where-Object { [string]$_.kind -eq 'gpu_physical_allocation' }).Count
    $ownerCount = @($resourceNodes | Where-Object { [string]$_.kind -eq 'gpu_primary_owner' }).Count
    $referenceCount = @($resourceEdges | Where-Object { [string]$_.relation -eq 'references_resource' }).Count
    $backedByCount = @($resourceEdges | Where-Object { [string]$_.relation -eq 'backed_by' }).Count
    $ownedByCount = @($resourceEdges | Where-Object { [string]$_.relation -eq 'owned_by' }).Count
    Assert-Condition ($logicalCount -gt 0 -and $physicalCount -gt 0 -and $ownerCount -gt 0) 'GPU logical/physical/owner evidence is incomplete'
    Assert-Condition ($referenceCount -gt 0 -and $backedByCount -gt 0 -and $ownedByCount -gt 0) 'GPU resource evidence relations are incomplete'
    $primaryResult = [ordered]@{
        fingerprint = [string]$critical.trace.fingerprint
        frame_id = $PrimaryFrameId
        critical_wall_ms = $critical.wall_ms
        explain_wall_ms = $explain.wall_ms
        explain_domains = @($explain.data.domain_coverage | Where-Object { [bool]$_.present } | ForEach-Object { [string]$_.domain })
        missing_domains = @($explain.data.missing_evidence | ForEach-Object { [string]$_.domain })
        sampling = @{ present = [bool]$sampleCapability.present; reason = [string]$sampleCapability.reason }
        context_switch = @{ present = [bool]$contextCapability.present; reason = [string]$contextCapability.reason }
        resource = @{ wall_ms = $resource.wall_ms; response_bytes = $resource.response_bytes; logical = $logicalCount; physical = $physicalCount; owners = $ownerCount; references = $referenceCount; backed_by = $backedByCount; owned_by = $ownedByCount; quality_codes = @($resource.data.quality_findings | ForEach-Object { [string]$_.code }) }
    }
    Close-Trace $activeTrace
    $activeTrace = ''

    $activeTrace = Open-ReadyTrace $ScriptTrace
    $scriptSummary = Inspect $activeTrace 'runtime.script.summary'
    $scriptGraph = Inspect $activeTrace 'evidence.graph' @{
        frame_id = $ScriptFrameId; domains = @('script'); max_nodes = 2000; max_edges = 4000;
        max_scan_events = 5000000; max_cpu_ms = 60000
    }
    Assert-Condition ([bool]$scriptSummary.data.present -and [bool]$scriptSummary.data.complete) 'script summary is absent or incomplete'
    Assert-Condition ([UInt64]$scriptSummary.data.counts.zones -gt 0 -and [UInt64]$scriptSummary.data.counts.complete_zones -eq [UInt64]$scriptSummary.data.counts.zones) 'script zones are absent or unpaired'
    Assert-Condition (-not [bool]$scriptGraph.partial -and [bool]$scriptGraph.data.complete) 'script evidence graph is partial or incomplete'
    Assert-Condition ($scriptGraph.wall_ms -le 5000) 'script evidence graph exceeded 5 seconds'
    $scriptNodes = @($scriptGraph.data.nodes | Where-Object { [string]$_.domain -eq 'script' })
    $scriptEdges = @($scriptGraph.data.edges)
    $managedZones = @($scriptNodes | Where-Object { [string]$_.kind -eq 'managed_zone' }).Count
    $luaZones = @($scriptNodes | Where-Object { [string]$_.kind -eq 'lua_zone' }).Count
    $sourceStacks = @($scriptNodes | Where-Object { [string]$_.kind -eq 'source_stack' }).Count
    $sourceFrames = @($scriptNodes | Where-Object { [string]$_.kind -eq 'source_frame' }).Count
    Assert-Condition ($managedZones -gt 0 -and $luaZones -gt 0 -and $sourceStacks -gt 0 -and $sourceFrames -gt 0) 'managed/Lua source-stack evidence is incomplete'
    Assert-Condition (@($scriptEdges | Where-Object { [string]$_.relation -eq 'captures_source_stack' -and [string]$_.evidence_kind -eq 'exact' }).Count -gt 0) 'script zone-to-stack exact relation is missing'
    Assert-Condition (@($scriptEdges | Where-Object { [string]$_.relation -eq 'contains_source_frame' -and [string]$_.evidence_kind -eq 'exact' }).Count -gt 0) 'script stack-to-frame exact relation is missing'
    foreach ($frame in @($scriptNodes | Where-Object { [string]$_.kind -eq 'source_frame' })) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$frame.details.file) -and [UInt64]$frame.details.line -gt 0) 'script source frame omitted file or line'
    }
    $scriptResult = [ordered]@{
        fingerprint = [string]$scriptGraph.trace.fingerprint
        frame_id = $ScriptFrameId
        wall_ms = $scriptGraph.wall_ms
        managed_zones = $managedZones
        lua_zones = $luaZones
        source_stacks = $sourceStacks
        source_frames = $sourceFrames
        files = @($scriptNodes | Where-Object { [string]$_.kind -eq 'source_frame' } | ForEach-Object { [string]$_.details.file } | Sort-Object -Unique)
    }
    Close-Trace $activeTrace
    $activeTrace = ''

    $activeTrace = Open-ReadyTrace $ElevatedTrace
    $elevatedCapabilities = Inspect $activeTrace 'system.capabilities'
    $elevatedSample = Get-Capability $elevatedCapabilities 'sample'
    $elevatedContext = Get-Capability $elevatedCapabilities 'context_switch'
    Assert-Condition ([bool]$elevatedSample.present -and [bool]$elevatedSample.queryable) 'elevated Sampling capability is absent or unqueryable'
    Assert-Condition ([bool]$elevatedContext.present -and [bool]$elevatedContext.queryable) 'elevated Context Switch capability is absent or unqueryable'
    $traceInfo = Inspect $activeTrace 'trace.info'
    $traceCounts = Inspect $activeTrace 'trace.counts'
    $sampleProbe = Inspect $activeTrace 'sample.list' @{ limit = 10 }
    Assert-Condition ([UInt64]$traceInfo.data.sampling_period_ns -gt 0 -and [UInt64]$traceCounts.data.samples -gt 0) 'elevated Sampling data is empty'
    Assert-Condition (@($sampleProbe.data.samples).Count -gt 0) 'sample.list returned no records'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]@($sampleProbe.data.samples)[0].callstack_ref)) 'sample record omitted callstack_ref'
    $frame = Inspect $activeTrace 'frame.identity' @{ frame_id = $ElevatedFrameId }
    $contextProbe = Inspect $activeTrace 'context_switch.range' @{
        start_ns = [string]$frame.data.identity.begin_ns; end_ns = [string]$frame.data.identity.end_ns; limit = 10
    }
    Assert-Condition (@($contextProbe.data.context_switches).Count -gt 0) 'context_switch.range returned no records for the selected frame'
    $contextGraph = Inspect $activeTrace 'evidence.graph' @{
        frame_id = $ElevatedFrameId; domains = @('context_switch'); max_nodes = 5000; max_edges = 10000;
        max_scan_events = 5000000; max_cpu_ms = 60000
    }
    Assert-Condition (-not [bool]$contextGraph.partial -and $contextGraph.wall_ms -le 5000) 'context-switch evidence is partial or exceeded 5 seconds'
    $contextNodes = @($contextGraph.data.nodes | Where-Object { [string]$_.domain -eq 'context_switch' })
    $runNodes = @($contextNodes | Where-Object { [string]$_.kind -eq 'context_switch_run' }).Count
    $waitNodes = @($contextNodes | Where-Object { [string]$_.kind -eq 'context_switch_wait' }).Count
    Assert-Condition ($runNodes -gt 0 -and $waitNodes -gt 0) 'context-switch run/wait evidence is incomplete'
    $elevatedResult = [ordered]@{
        fingerprint = [string]$contextGraph.trace.fingerprint
        frame_id = $ElevatedFrameId
        sampling_period_ns = [string]$traceInfo.data.sampling_period_ns
        samples = [string]$traceCounts.data.samples
        context_switch_samples = [string]$traceCounts.data.context_switch_samples
        sample_probe = @($sampleProbe.data.samples).Count
        context_probe = @($contextProbe.data.context_switches).Count
        graph_wall_ms = $contextGraph.wall_ms
        run_nodes = $runNodes
        wait_nodes = $waitNodes
    }
    Close-Trace $activeTrace
    $activeTrace = ''

    [ordered]@{
        ok = $true
        schema_version = '1.30.0'
        sidecar_schema = 3
        primary = $primaryResult
        script = $scriptResult
        elevated = $elevatedResult
    } | ConvertTo-Json -Compress -Depth 60
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($activeTrace) -and $queryProcess -and -not $queryProcess.HasExited) {
        try { Close-Trace $activeTrace } catch {}
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) { $queryProcess.Kill($true); $queryProcess.WaitForExit() }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
