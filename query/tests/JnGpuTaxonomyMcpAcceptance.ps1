[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$Snapshot,
    [Parameter(Mandatory = $true)][string]$Stream,
    [Parameter(Mandatory = $true)][string]$Replay,
    [Parameter(Mandatory = $true)][string]$LegacyTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $Snapshot, $Stream, $Replay, $LegacyTrace, $AllowRoot))
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

function Invoke-Tool([string]$Name, [hashtable]$Arguments = @{})
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

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
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

function Find-Node($Nodes, [UInt64]$Id)
{
    return @($Nodes | Where-Object { [UInt64]([string]$_.taxonomy_id) -eq $Id })
}

function Assert-RequiredNode($Nodes, [UInt64]$Id, [UInt64]$ParentId, [int]$Level, [string]$Name, [string]$Label)
{
    $matches = @(Find-Node $Nodes $Id)
    Assert-Condition ($matches.Count -eq 1) "$Label expected one taxonomy node $Id"
    $node = $matches[0]
    Assert-Condition ([UInt64]([string]$node.parent_id) -eq $ParentId) "$Label taxonomy $Id parent mismatch"
    Assert-Condition ([int]$node.level -eq $Level) "$Label taxonomy $Id level mismatch"
    Assert-Condition ([string]$node.canonical_name -eq $Name) "$Label taxonomy $Id name mismatch"
    Assert-Condition (-not [string]::IsNullOrEmpty([string]$node.catalog_definition_key)) "$Label taxonomy $Id has no catalog link"
}

function Get-Semantics([string]$Path, [string]$Label)
{
    $traceId = Open-Trace $Path
    try
    {
        $tree = Inspect $traceId 'gpu.taxonomy.tree' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
        $coverage = Inspect $traceId 'gpu.taxonomy.coverage' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
        $catalog = Inspect $traceId 'catalog.list' @{ kind = 'gpu_taxonomy'; limit = 500 }

        Assert-Condition ([string]$tree.schema_version -eq '1.15.0') "$Label query schema mismatch"
        Assert-Condition ([bool]$tree.data.present -and [bool]$tree.data.complete) "$Label taxonomy tree absent or incomplete"
        Assert-Condition ([int]$tree.data.schema_version -eq 2) "$Label taxonomy schema mismatch"
        Assert-Condition ([bool]$tree.data.scan_complete) "$Label taxonomy scan exhausted its budget"
        Assert-Condition ([int]$tree.data.definition_count -eq 34) "$Label definition count is not 34"
        Assert-Condition (@($tree.data.nodes).Count -eq 34) "$Label node count is not 34"
        Assert-Condition (@($catalog.data.definitions).Count -eq 34) "$Label generic catalog count is not 34"
        Assert-Condition ([int]$tree.data.quality.missing_part_count -eq 0) "$Label taxonomy snapshot is missing one or more parts"
        Assert-Condition ([UInt64]([string]$tree.data.unknown_taxonomy_zone_count) -eq 0) "$Label contains unknown taxonomy GPU zones"
        Assert-Condition ([bool]$tree.data.hierarchy_gate.static_l0_has_multiple_l1) "$Label static L0/L1 gate failed"
        Assert-Condition ([bool]$tree.data.hierarchy_gate.static_l1_has_multiple_l2) "$Label static L1/L2 gate failed"
        Assert-Condition ([bool]$tree.data.hierarchy_gate.observed_l0_has_multiple_l1) "$Label observed L0/L1 gate failed"
        Assert-Condition ([bool]$tree.data.hierarchy_gate.observed_l1_has_multiple_l2) "$Label observed L1/L2 gate failed"

        $nodes = @($tree.data.nodes)
        Assert-RequiredNode $nodes 65537 0 0 'GPU.Frame.Direct' $Label
        Assert-RequiredNode $nodes 65538 0 0 'GPU.Frame.Compute' $Label
        Assert-RequiredNode $nodes 65539 0 0 'GPU.Frame.Copy' $Label
        Assert-RequiredNode $nodes 268500992 65537 1 'GPU.L1.FrameSetup' $Label
        Assert-RequiredNode $nodes 268566528 65537 1 'GPU.L1.Visibility' $Label
        Assert-RequiredNode $nodes 268632064 65537 1 'GPU.L1.Shadows' $Label
        Assert-RequiredNode $nodes 268697600 65537 1 'GPU.L1.VirtualGeometry' $Label
        Assert-RequiredNode $nodes 268763136 65537 1 'GPU.L1.Lighting' $Label
        Assert-RequiredNode $nodes 268828672 65537 1 'GPU.L1.PostProcessing' $Label
        Assert-RequiredNode $nodes 268894208 65539 1 'GPU.L1.Transfers' $Label
        Assert-RequiredNode $nodes 537067522 268632064 2 'GPU.L2.Shadows.VSM' $Label
        Assert-RequiredNode $nodes 537133057 268697600 2 'GPU.L2.VirtualGeometry.Culling' $Label
        Assert-RequiredNode $nodes 537133058 268697600 2 'GPU.L2.VirtualGeometry.Raster' $Label
        Assert-RequiredNode $nodes 537198593 268763136 2 'GPU.L2.Lighting.ScreenProbe' $Label
        Assert-RequiredNode $nodes 537264129 268828672 2 'GPU.L2.PostProcessing.TSR' $Label

        Assert-Condition ([bool]$coverage.data.present -and [bool]$coverage.data.complete) "$Label taxonomy coverage absent or incomplete"
        Assert-Condition ([bool]$coverage.data.statuses.executed.available) "$Label executed status unavailable"
        Assert-Condition ([UInt64]([string]$coverage.data.statuses.executed.count) -gt 0) "$Label contains no executed taxonomy GPU zones"
        Assert-Condition ([bool]$coverage.data.statuses.fallback.available) "$Label fallback status unavailable"
        Assert-Condition ([UInt64]([string]$coverage.data.statuses.fallback.count) -gt 0) "$Label contains no fallback GPU zones"
        Assert-Condition ([UInt64]([string]$coverage.data.statuses.fallback.classified_marker_count) -gt 0) "$Label classifier emitted no markers"
        Assert-Condition ([bool]$coverage.data.statuses.unclassified.available) "$Label unclassified status unavailable"
        Assert-Condition (-not [bool]$coverage.data.statuses.culled.available) "$Label fabricated culled status availability"
        Assert-Condition (-not [string]::IsNullOrEmpty([string]$coverage.data.statuses.culled.reason)) "$Label culled status has no absence reason"
        Assert-Condition (-not [bool]$coverage.data.statuses.disabled.available) "$Label fabricated disabled status availability"
        Assert-Condition (-not [string]::IsNullOrEmpty([string]$coverage.data.statuses.disabled.reason)) "$Label disabled status has no absence reason"

        $projection = @($nodes | Sort-Object { [UInt64]([string]$_.taxonomy_id) } | ForEach-Object {
            [ordered]@{
                taxonomy_id = [string]$_.taxonomy_id
                parent_id = [string]$_.parent_id
                level = [int]$_.level
                canonical_name = [string]$_.canonical_name
                zone_count = [string]$_.execution.zone_count
                complete_zone_count = [string]$_.execution.complete_zone_count
                total_gpu_ns = [string]$_.execution.total_gpu_ns
            }
        })
        return [ordered]@{
            label = $Label
            source_kind = [string]$tree.trace.source_kind
            active_connection_id = [string]$tree.data.active_connection_id
            source_mode = [string]$tree.data.source_mode
            definition_count = [int]$tree.data.definition_count
            taxonomy_zone_count = [string]$tree.data.taxonomy_zone_count
            logical_edges = @($tree.data.logical_edges | ForEach-Object { "$(($_.parent_id)):$(($_.child_id))" } | Sort-Object)
            observed_edges = @($tree.data.observed_edges | ForEach-Object { "$(($_.parent_id)):$(($_.child_id)):$(($_.zone_pair_count))" } | Sort-Object)
            nodes = $projection
            statuses = $coverage.data.statuses
            classifier_quality = $coverage.data.classifier_quality
            hierarchy_gate = $tree.data.hierarchy_gate
        }
    }
    finally
    {
        Close-Trace $traceId
    }
}

function Assert-SemanticEqual($Left, $Right, [string]$Label)
{
    foreach ($field in @('definition_count', 'taxonomy_zone_count', 'source_mode'))
    {
        Assert-Condition ([string]$Left.$field -eq [string]$Right.$field) "$Label differs for $field"
    }
    foreach ($field in @('logical_edges', 'observed_edges', 'nodes', 'statuses', 'hierarchy_gate'))
    {
        Assert-Condition (($Left.$field | ConvertTo-Json -Compress -Depth 60) -eq ($Right.$field | ConvertTo-Json -Compress -Depth 60)) "$Label differs for $field"
    }
}

Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
$script:Process = $process
try
{
    $initialize = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-gpu-taxonomy-acceptance'; version = '1' } }
    Assert-Condition ([string]$initialize.result.protocolVersion -eq '2025-11-25') 'MCP protocol version mismatch'
    Notify-Initialized

    $snapshotResult = Get-Semantics $Snapshot 'snapshot'
    $streamResult = Get-Semantics $Stream 'committed_stream'
    $replayResult = Get-Semantics $Replay 'stream_replay'
    Assert-Condition ($snapshotResult.source_kind -eq 'snapshot') 'snapshot source kind mismatch'
    Assert-Condition ($streamResult.source_kind -eq 'segment') 'stream source kind mismatch'
    Assert-Condition ($replayResult.source_kind -eq 'snapshot') 'replay source kind mismatch'
    Assert-SemanticEqual $snapshotResult $streamResult 'snapshot/stream'
    Assert-SemanticEqual $snapshotResult $replayResult 'snapshot/replay'

    $legacyId = Open-Trace $LegacyTrace
    try
    {
        $legacyTree = Inspect $legacyId 'gpu.taxonomy.tree'
        $legacyCoverage = Inspect $legacyId 'gpu.taxonomy.coverage'
        Assert-Condition (-not [bool]$legacyTree.data.present) 'legacy trace fabricated taxonomy tree presence'
        Assert-Condition (-not [bool]$legacyCoverage.data.present) 'legacy trace fabricated taxonomy coverage presence'
        Assert-Condition (-not [string]::IsNullOrEmpty([string]$legacyTree.data.reason)) 'legacy taxonomy tree omitted absence reason'
        Assert-Condition (-not [string]::IsNullOrEmpty([string]$legacyCoverage.data.reason)) 'legacy taxonomy coverage omitted absence reason'
    }
    finally
    {
        Close-Trace $legacyId
    }

    $result = [ordered]@{
        schema_version = '1.15.0'
        taxonomy_schema_version = 2
        result = 'PASS'
        snapshot = $snapshotResult
        committed_stream = $streamResult
        stream_replay = $replayResult
        legacy = [ordered]@{ tree_present = [bool]$legacyTree.data.present; coverage_present = [bool]$legacyCoverage.data.present; reason = [string]$legacyTree.data.reason }
        files = [ordered]@{}
    }
    foreach ($item in ([ordered]@{ snapshot = $Snapshot; stream = $Stream; replay = $Replay; legacy = $LegacyTrace }).GetEnumerator())
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
    foreach ($traceId in @($script:OpenedIds))
    {
        try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {}
    }
    try { $process.StandardInput.Close() } catch {}
    if (-not $process.WaitForExit(5000)) { $process.Kill() }
    $process.Dispose()
}
