[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $ReplayTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [switch] $RealCapture,
    [switch] $RequireContextSwitch,
    [switch] $RequireSampling,
    [string] $CheckpointDirectory = '',
    [string] $OutputPath = '',
    [switch] $UseIndexed
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}
if (-not [string]::IsNullOrWhiteSpace($CheckpointDirectory)) {
    Assert-Condition (Test-Path -LiteralPath $CheckpointDirectory) "checkpoint directory does not exist: $CheckpointDirectory"
}

$script:NextRequestId = 1
$script:RequiredFrameId = $null
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp$(if ($UseIndexed) { ' --indexed' } else { '' }) --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
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

function Get-CompleteFrames {
    param([string] $TraceId)
    $frames = [System.Collections.Generic.List[object]]::new()
    for ($offset = 0; $offset -lt 10000; $offset += 1000) {
        $response = Inspect $TraceId 'frame.identity' @{ offset = $offset; limit = 1000 }
        $page = @($response.data.identities)
        foreach ($frame in $page) {
            if ([bool]($frame.complete) -and [Int64]($frame.duration_ns) -gt 0) { $frames.Add($frame) }
        }
        if ($page.Count -lt 1000) { break }
    }
    return @($frames)
}

function Get-ComparableN14Semantics {
    param([object] $Semantics)
    $normalized = $Semantics | ConvertTo-Json -Depth 70 | ConvertFrom-Json
    if ($null -ne $normalized.context_switch_capability) {
        $normalized.context_switch_capability.reason = '<source-specific-capability-provenance>'
    }
    if ($null -ne $normalized.sampling_capability) {
        $normalized.sampling_capability.reason = '<source-specific-capability-provenance>'
    }
    return $normalized | ConvertTo-Json -Compress -Depth 70
}

function Validate-N14 {
    param([string] $TraceId)

    $capabilities = Inspect $TraceId 'system.capabilities'
    $evidenceCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'evidence' })
    Assert-Condition ($evidenceCapability.Count -eq 1 -and [bool]$evidenceCapability[0].present) 'evidence capability unavailable'
    foreach ($method in @('evidence.graph', 'frame.critical_path', 'frame.explain')) {
        Assert-Condition (@($evidenceCapability[0].methods) -contains $method) "evidence capability is missing $method"
    }
    $contextCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'context_switch' })
    Assert-Condition ($contextCapability.Count -eq 1) 'context_switch capability entry is missing'
    $contextSwitchAvailable = [bool]$contextCapability[0].present
    if ($RequireContextSwitch) {
        Assert-Condition $contextSwitchAvailable 'context_switch capability is required but unavailable'
    }
    $sampleCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'sample' })
    Assert-Condition ($sampleCapability.Count -eq 1) 'sample capability entry is missing'
    $samplingAvailable = [bool]$sampleCapability[0].present
    if ($RequireSampling) {
        Assert-Condition $samplingAvailable 'sample capability is required but unavailable'
        Assert-Condition ([bool]$sampleCapability[0].queryable) 'sample capability is present but not queryable'
    }
    $traceInfo = Inspect $TraceId 'trace.info'
    $traceCounts = Inspect $TraceId 'trace.counts'
    $sampleCount = [UInt64]$traceCounts.data.samples
    $contextSwitchSampleCount = [UInt64]$traceCounts.data.context_switch_samples
    $samplingPeriodNs = [UInt64]$traceInfo.data.sampling_period_ns
    $sampleProbeCount = 0
    if ($RequireSampling) {
        Assert-Condition ($samplingPeriodNs -gt 0) 'required sampling period is zero'
        Assert-Condition (($sampleCount + $contextSwitchSampleCount) -gt 0) 'required persisted sample count is zero'
        $sampleProbe = Inspect $TraceId 'sample.list' @{ limit = 1 }
        $sampleProbeCount = @($sampleProbe.data.samples).Count
        Assert-Condition ($sampleProbeCount -gt 0) 'sample.list returned no persisted sample entity'
    }

    $frames = @(Get-CompleteFrames $TraceId)
    Assert-Condition ($frames.Count -gt 0) 'no complete FrameIdentity with positive duration'
    $orderedFrames = @($frames | Sort-Object { [UInt64]($_.frame_id) })
    $frame = @($orderedFrames | Select-Object -Last 1)[0]
    $contextSwitchProbeCount = 0
    if ($RequireContextSwitch) {
        if (-not [string]::IsNullOrWhiteSpace([string]$script:RequiredFrameId)) {
            $matchingFrames = @($orderedFrames | Where-Object { [string]$_.frame_id -eq [string]$script:RequiredFrameId })
            Assert-Condition ($matchingFrames.Count -eq 1) "required cross-media frame is missing: $script:RequiredFrameId"
            $frame = $matchingFrames[0]
            $probe = Inspect $TraceId 'context_switch.range' @{
                start_ns = [string]$frame.begin_ns
                end_ns = [string]$frame.end_ns
                limit = 1
            }
            $contextSwitchProbeCount = @($probe.data.context_switches).Count
        }
        else {
            $frame = $null
            foreach ($candidate in @($orderedFrames | Sort-Object { [UInt64]($_.frame_id) } -Descending)) {
                $probe = Inspect $TraceId 'context_switch.range' @{
                    start_ns = [string]$candidate.begin_ns
                    end_ns = [string]$candidate.end_ns
                    limit = 1
                }
                $contextSwitchProbeCount = @($probe.data.context_switches).Count
                if ($contextSwitchProbeCount -gt 0) {
                    $frame = $candidate
                    break
                }
            }
            Assert-Condition ($null -ne $frame) 'no complete frame overlaps a persisted context_switch event'
            $script:RequiredFrameId = [string]$frame.frame_id
        }
        Assert-Condition ($contextSwitchProbeCount -gt 0) "required frame has no persisted context_switch event: $($frame.frame_id)"
    }
    $frameRef = [string]$frame.ref
    $limits = @{ max_scan_events = 5000000; max_cpu_ms = 60000; max_nodes = 50000; max_edges = 100000 }
    $graphLimits = if ($RealCapture) {
        @{ max_scan_events = 5000000; max_cpu_ms = 60000; max_nodes = 2000; max_edges = 5000 }
    }
    else { $limits }

    $graphResponse = Inspect $TraceId 'evidence.graph' (@{ ref = $frameRef } + $graphLimits)
    if ($RealCapture) {
        Assert-Condition ([bool]$graphResponse.partial) 'bounded real evidence.graph did not report partial'
        Assert-Condition ([bool]$graphResponse.data.truncated) 'bounded real evidence.graph did not report truncation'
    }
    else {
        Assert-Condition (-not [bool]$graphResponse.partial) 'unbounded synthetic evidence.graph unexpectedly returned partial'
    }
    $graph = $graphResponse.data
    Assert-Condition ([bool]$graph.present) 'evidence.graph is not present'
    if (-not $RealCapture) { Assert-Condition (-not [bool]$graph.truncated) 'synthetic evidence.graph was truncated' }
    Assert-Condition (-not [bool]$graph.heuristic_enabled) 'heuristic evidence is enabled by default'
    Assert-Condition (-not [bool]$graph.heuristic_used_by_default_path) 'default path used heuristic evidence'
    Assert-Condition ([UInt64]$graph.evidence_counts.exact -gt 0) 'evidence graph contains no exact evidence'
    Assert-Condition ([UInt64]$graph.evidence_counts.heuristic -eq 0) 'evidence graph contains heuristic evidence while disabled'

    $nodes = @($graph.nodes)
    $edges = @($graph.edges)
    Assert-Condition ($nodes.Count -gt 0 -and $edges.Count -gt 0) 'evidence graph is empty'
    $nodeRefs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($node in $nodes) { [void]$nodeRefs.Add([string]$node.ref) }
    foreach ($edge in $edges) {
        Assert-Condition ($nodeRefs.Contains([string]$edge.source_ref)) "edge source is unresolved: $($edge.ref)"
        Assert-Condition ($nodeRefs.Contains([string]$edge.target_ref)) "edge target is unresolved: $($edge.ref)"
        Assert-Condition ([string]$edge.evidence_kind -in @('exact', 'derived')) "unexpected evidence kind: $($edge.evidence_kind)"
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$edge.rule_id)) "edge omitted rule_id: $($edge.ref)"
    }

    $criticalResponse = Inspect $TraceId 'frame.critical_path' (@{ ref = $frameRef } + $limits)
    Assert-Condition (-not [bool]$criticalResponse.partial) 'frame.critical_path unexpectedly returned partial'
    $critical = $criticalResponse.data
    Assert-Condition ([string]$critical.algorithm -eq 'causal_dag_incremental_wall_clock_v1') 'critical-path algorithm mismatch'
    Assert-Condition (-not [bool]$critical.critical_path.has_cycle) 'critical path contains a cycle'
    Assert-Condition ([bool]$critical.critical_path.valid_contribution) 'critical path has invalid wall-clock contribution'
    Assert-Condition ([Int64]$critical.critical_path.total_wall_clock_contribution_ns -le [Int64]$critical.critical_path.frame_duration_ns) 'critical path exceeds frame duration'
    Assert-Condition ([string]$critical.critical_path.overlap_accounting -eq 'incremental_wall_clock_union_v1') 'critical-path overlap accounting mismatch'

    $explainResponse = Inspect $TraceId 'frame.explain' (@{ ref = $frameRef } + $limits)
    Assert-Condition (-not [bool]$explainResponse.partial) 'frame.explain unexpectedly returned partial'
    $explain = $explainResponse.data
    Assert-Condition ([string]$explain.conclusion_contract -eq 'all conclusions must cite returned node/edge refs; absent domains are not real zero') 'frame explain conclusion contract mismatch'
    Assert-Condition ([string]$explain.analysis_confidence -in @('high', 'medium')) 'frame explain confidence is unexpectedly low or partial'

    $edgeBounded = Inspect $TraceId 'evidence.graph' @{
        ref = $frameRef; max_scan_events = 5000000; max_cpu_ms = 60000;
        max_nodes = $(if ($RealCapture) { 2000 } else { 50000 }); max_edges = 1
    }
    Assert-Condition ([bool]$edgeBounded.partial) 'max_edges did not set top-level partial'
    Assert-Condition ([bool]$edgeBounded.data.truncated) 'max_edges did not mark graph truncated'
    Assert-Condition ([UInt64]$edgeBounded.data.omitted_edges -gt 0) 'max_edges did not count omitted edges'
    Assert-Condition (@($edgeBounded.budget.exhausted_by) -contains 'max_edges') 'max_edges was not recorded as the exhausted budget'

    $coverageSource = if ($RealCapture) { $explain.domain_coverage } else { $graph.domain_coverage }
    $coverage = @($coverageSource | Sort-Object domain | ForEach-Object {
        [ordered]@{
            domain = [string]$_.domain
            present = [bool]$_.present
            node_count = [string]$_.node_count
            status = [string]$_.status
            capability_domain = [string]$_.capability_domain
            capability_available = [bool]$_.capability_available
            reason = [string]$_.reason
        }
    })
    $presentDomains = @($coverage | Where-Object { $_.present } | ForEach-Object { $_.domain })
    $contextSwitchRunCount = 0
    $contextSwitchWaitCount = 0
    if ($RequireContextSwitch) {
        $contextCoverage = @($coverage | Where-Object { $_.domain -eq 'context_switch' })
        Assert-Condition ($contextCoverage.Count -eq 1) 'required context_switch coverage entry is missing'
        Assert-Condition ([bool]$contextCoverage[0].capability_available) 'required context_switch coverage reports capability unavailable'
        Assert-Condition ([bool]$contextCoverage[0].present) 'required context_switch evidence is not present'
        Assert-Condition ([UInt64]$contextCoverage[0].node_count -gt 0) 'required context_switch evidence has no nodes'
        Assert-Condition (@($explain.missing_evidence | Where-Object { [string]$_.domain -eq 'context_switch' }).Count -eq 0) 'frame explain reports required context_switch evidence as missing'

        $contextGraphResponse = Inspect $TraceId 'evidence.graph' @{
            ref = $frameRef
            domains = @('context_switch')
            max_scan_events = 5000000
            max_cpu_ms = 60000
            max_nodes = 10000
            max_edges = 20000
        }
        Assert-Condition (-not [bool]$contextGraphResponse.partial) 'focused context_switch evidence graph is partial'
        $contextNodes = @($contextGraphResponse.data.nodes | Where-Object { [string]$_.domain -eq 'context_switch' })
        $contextSwitchRunCount = @($contextNodes | Where-Object { [string]$_.kind -eq 'context_switch_run' }).Count
        $contextSwitchWaitCount = @($contextNodes | Where-Object { [string]$_.kind -eq 'context_switch_wait' }).Count
        Assert-Condition ($contextSwitchRunCount -gt 0) 'required context_switch_run evidence is absent'
        Assert-Condition ($contextSwitchWaitCount -gt 0) 'required context_switch_wait evidence is absent'
    }
    if (-not $RealCapture) {
        foreach ($required in @('cpu', 'job', 'wait', 'lock', 'io', 'submission', 'gpu', 'resource')) {
            Assert-Condition ($presentDomains -contains $required) "synthetic evidence graph is missing domain: $required"
        }
        if ($contextSwitchAvailable) {
            Assert-Condition ($presentDomains -contains 'context_switch') 'synthetic evidence graph is missing available context_switch data'
        }
        else {
            Assert-Condition (-not ($presentDomains -contains 'context_switch')) 'context_switch evidence exists while capability reports absent'
            Assert-Condition (@($explain.missing_evidence | Where-Object { [string]$_.domain -eq 'context_switch' }).Count -eq 1) 'frame explain did not report unavailable context_switch evidence'
        }
        Assert-Condition ([bool]$graph.complete) 'synthetic evidence graph is incomplete'
        $unexpectedMissing = @($explain.missing_evidence | Where-Object {
            [string]$_.domain -ne 'context_switch' -or $contextSwitchAvailable
        })
        Assert-Condition ($unexpectedMissing.Count -eq 0) 'synthetic frame explain reports unexpected missing evidence'
    }
    else {
        Assert-Condition ($presentDomains -contains 'cpu') 'real evidence graph is missing CPU evidence'
        Assert-Condition ($presentDomains -contains 'gpu') 'real evidence graph is missing GPU evidence'
        Assert-Condition ($presentDomains.Count -ge 3) 'real evidence graph has insufficient cross-domain coverage'
    }

    return [ordered]@{
        frame_id = [string]$frame.frame_id
        frame_duration_ns = [string]$frame.duration_ns
        frame_domains = @($frame.domains | Sort-Object)
        complete = [bool]$graph.complete
        node_count = [string]$graph.node_count
        edge_count = [string]$graph.edge_count
        evidence_counts = [ordered]@{
            exact = [string]$graph.evidence_counts.exact
            derived = [string]$graph.evidence_counts.derived
            heuristic = [string]$graph.evidence_counts.heuristic
        }
        domain_coverage = $coverage
        critical_path = [ordered]@{
            total_wall_clock_contribution_ns = [string]$critical.critical_path.total_wall_clock_contribution_ns
            frame_duration_ns = [string]$critical.critical_path.frame_duration_ns
            node_kinds = @($critical.critical_path.nodes | ForEach-Object { [string]$_.kind })
            node_domains = @($critical.critical_path.nodes | ForEach-Object { [string]$_.domain })
            has_cycle = [bool]$critical.critical_path.has_cycle
            valid_contribution = [bool]$critical.critical_path.valid_contribution
        }
        explain_confidence = [string]$explain.analysis_confidence
        missing_domains = @($explain.missing_evidence | ForEach-Object { [string]$_.domain } | Sort-Object)
        context_switch_capability = [ordered]@{
            present = $contextSwitchAvailable
            required = [bool]$RequireContextSwitch
            selected_frame_probe_count = $contextSwitchProbeCount
            run_node_count = $contextSwitchRunCount
            wait_node_count = $contextSwitchWaitCount
            reason = [string]$contextCapability[0].reason
        }
        sampling_capability = [ordered]@{
            present = $samplingAvailable
            required = [bool]$RequireSampling
            sampling_period_ns = [string]$samplingPeriodNs
            samples = [string]$sampleCount
            context_switch_samples = [string]$contextSwitchSampleCount
            selected_probe_count = $sampleProbeCount
            reason = [string]$sampleCapability[0].reason
        }
        edge_budget_partial = [bool]$edgeBounded.partial
    }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n14-evidence-acceptance'; version = '1.0' } }
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
            semantics = Validate-N14 $traceId
        }
        if (-not [string]::IsNullOrWhiteSpace($CheckpointDirectory)) {
            $checkpointPath = Join-Path $CheckpointDirectory "N14-elevated-$($entry.label)-semantics.json"
            $results[$entry.label].semantics | ConvertTo-Json -Depth 70 |
                Set-Content -LiteralPath $checkpointPath -Encoding utf8
        }
        [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $traceIds = @($traceIds | Where-Object { $_ -ne $traceId })
    }

    $snapshotSemantics = Get-ComparableN14Semantics $results.snapshot.semantics
    Assert-Condition ((Get-ComparableN14Semantics $results.stream.semantics) -eq $snapshotSemantics) 'snapshot/stream N14 semantic mismatch'
    Assert-Condition ((Get-ComparableN14Semantics $results.replay.semantics) -eq $snapshotSemantics) 'snapshot/replay N14 semantic mismatch'
    $document = [ordered]@{ ok = $true; schema_version = '1.28.0'; indexed = [bool]$UseIndexed; traces = $results }
    $json = $document | ConvertTo-Json -Depth 70
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $outputDirectory = Split-Path -Parent $OutputPath
        if ($outputDirectory) { [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null }
        [IO.File]::WriteAllText($OutputPath, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    $json
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
