[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$SecondSnapshotTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$LegacyTrace,
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $SecondSnapshotTrace, $AllowRoot, $LegacyTrace))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$script:NextRequestId = 1
$script:OpenedIds = @()
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`""
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
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 60))
    $script:Process.StandardInput.Flush()
    $read = $script:Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    $line = $read.Result
    if ($null -eq $line) { throw "MCP stdout closed before response: $Method" }
    $message = $line | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    return $message
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
        throw "$Name failed: $($response.result.structuredContent | ConvertTo-Json -Compress -Depth 30)"
    }
    return $response.result.structuredContent
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

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Read-FrameCorrelation([string]$Path, [string]$Label)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $script:OpenedIds += $traceId
    Wait-Ready $traceId

    $capabilities = Inspect $traceId 'system.capabilities'
    $frames = Inspect $traceId 'frame.identity' @{ limit = 1000 }
    Assert-Condition ([string]$frames.schema_version -eq '1.15.0') "$Label query schema is not 1.15.0"
    Assert-Condition ([bool]$frames.data.present) "$Label FrameIdentity is absent"
    Assert-Condition (@($frames.data.identities).Count -gt 0) "$Label has no FrameIdentity"

    $frameCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'frame' })
    $correlationCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'correlation' })
    Assert-Condition ($frameCapability.Count -eq 1 -and [bool]$frameCapability[0].present) "$Label frame capability is unavailable"
    Assert-Condition ($correlationCapability.Count -eq 1 -and [bool]$correlationCapability[0].present) "$Label correlation capability is unavailable"

    $completeIds = @()
    $incompleteIds = @()
    $nestedIdentityCount = 0
    $identityList = @($frames.data.identities)
    foreach ($identity in $identityList)
    {
        Assert-Condition ([string]$identity.evidence_kind -eq 'exact') "$Label used inferred FrameIdentity evidence"
        if ([bool]$identity.complete) { $completeIds += [string]$identity.frame_id }
        else { $incompleteIds += [string]$identity.frame_id }

        $events = @($identity.events)
        $editorBegin = @($events | Where-Object { $_.domain -eq 'editor' -and $_.phase -eq 'begin' -and [bool]$_.canonical })
        $editorEnd = @($events | Where-Object { $_.domain -eq 'editor' -and $_.phase -eq 'end' -and [bool]$_.canonical })
        Assert-Condition ($editorBegin.Count -le 1) "$Label FrameIdentity has duplicate canonical Editor begin"
        Assert-Condition ($editorEnd.Count -le 1) "$Label FrameIdentity has duplicate canonical Editor end"
        if ([bool]$identity.complete -and ($editorBegin.Count -gt 0 -or $editorEnd.Count -gt 0))
        {
            Assert-Condition ($editorBegin.Count -eq 1) "$Label FrameIdentity has duplicate canonical Editor begin"
            Assert-Condition ($editorEnd.Count -eq 1) "$Label FrameIdentity has no unique canonical Editor end"
        }

        $playerEvents = @($events | Where-Object { $_.domain -eq 'player' })
        if ($editorBegin.Count -eq 1 -and $editorEnd.Count -eq 1 -and $playerEvents.Count -gt 0)
        {
            $nestedIdentityCount++
            Assert-Condition (@($playerEvents | Where-Object { $_.phase -eq 'begin' -and [bool]$_.alias }).Count -eq 1) "$Label nested Player begin is not one alias"
            Assert-Condition (@($playerEvents | Where-Object { $_.phase -eq 'end' -and [bool]$_.alias }).Count -eq 1) "$Label nested Player end is not one alias"
            Assert-Condition (@($playerEvents | Where-Object { [bool]$_.canonical }).Count -eq 0) "$Label nested Player event is incorrectly canonical"
            $gpuMemory = @($events | Where-Object { $_.domain -eq 'gpu_memory' -and $_.phase -eq 'boundary' })
            Assert-Condition ($gpuMemory.Count -eq 1) "$Label nested Editor/Player frame advanced GPU memory $($gpuMemory.Count) times"
        }
    }
    Assert-Condition ($completeIds.Count -gt 0) "$Label has no complete FrameIdentity"
    Assert-Condition ($nestedIdentityCount -gt 0) "$Label did not capture nested Editor/Player frames"
    Assert-Condition ($incompleteIds.Count -le 2) "$Label has more than two capture-edge incomplete FrameIdentities"
    if ($incompleteIds.Count -gt 0)
    {
        $edgeIds = @([string]$identityList[0].frame_id, [string]$identityList[-1].frame_id)
        foreach ($incompleteId in $incompleteIds)
        {
            Assert-Condition ($incompleteId -in $edgeIds) "$Label has an incomplete FrameIdentity away from a capture edge"
        }
    }

    $jobs = Inspect $traceId 'job.search' @{ limit = 1000 }
    $originJob = @($jobs.data.jobs | Where-Object { $null -ne $_.origin_frame_ref -and -not [string]::IsNullOrEmpty([string]$_.origin_frame_ref) } | Select-Object -First 1)
    Assert-Condition ($originJob.Count -eq 1) "$Label has no Job with exact origin FrameIdentity"
    $frameRef = [string]$originJob[0].origin_frame_ref
    $identity = Inspect $traceId 'frame.identity' @{ ref = $frameRef }
    $related = Inspect $traceId 'entity.related' @{ ref = $frameRef }
    $chainNodeBudget = 4096
    $chain = Inspect $traceId 'correlation.chain' @{ ref = $frameRef; max_nodes = $chainNodeBudget }
    $slice = Inspect $traceId 'timeline.correlated_slice' @{ ref = $frameRef; max_nodes = $chainNodeBudget }
    Assert-Condition ([string]$identity.data.identity.ref -eq $frameRef) "$Label frame.identity(ref) returned the wrong entity"
    Assert-Condition ([string]$related.data.evidence_kind -eq 'exact') "$Label entity.related used inferred evidence"
    Assert-Condition (@($related.data.relations | Where-Object { $_.relation -eq 'schedules' -and $_.source_ref -eq $frameRef }).Count -gt 0) "$Label FrameIdentity has no schedules relation"
    Assert-Condition ([string]$chain.data.evidence_kind -eq 'exact') "$Label correlation chain used inferred evidence"
    Assert-Condition (@($chain.data.nodes).Count -gt 1) "$Label correlation chain did not leave the frame root"
    if ([bool]$chain.data.truncated)
    {
        Assert-Condition (@($chain.data.nodes).Count -eq $chainNodeBudget) "$Label correlation chain truncated before its explicit node budget"
    }
    Assert-Condition ([string]$slice.data.evidence_kind -eq 'exact') "$Label correlated slice used inferred evidence"
    Assert-Condition (-not [bool]$slice.data.window_inference_used) "$Label correlated slice used a time-window heuristic"
    Assert-Condition ([bool]$slice.data.truncated -eq [bool]$chain.data.truncated) "$Label correlated slice did not preserve chain truncation semantics"
    Assert-Condition (@($slice.data.jobs).Count -gt 0) "$Label correlated slice has no jobs"

    Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $traceId })
    return [pscustomobject]@{
        FrameIds = @($frames.data.identities | ForEach-Object { [string]$_.frame_id })
        CompleteIds = @($completeIds)
        Generations = @($frames.data.identities | ForEach-Object { [int]$_.connection_generation } | Sort-Object -Unique)
        IdentityCount = @($frames.data.identities).Count
        IncompleteIdentityCount = $incompleteIds.Count
        NestedIdentityCount = $nestedIdentityCount
        SampleFrameId = [string]$identity.data.identity.frame_id
        SampleFrameRef = $frameRef
        RelatedCount = @($related.data.relations).Count
        ChainNodeCount = @($chain.data.nodes).Count
        ChainTruncated = [bool]$chain.data.truncated
        CorrelatedJobCount = @($slice.data.jobs).Count
    }
}

try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-frame-correlation-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $snapshot = Read-FrameCorrelation $SnapshotTrace 'snapshot'
    $stream = Read-FrameCorrelation $StreamTrace 'stream'
    $replay = Read-FrameCorrelation $ReplayTrace 'replay'
    $second = Read-FrameCorrelation $SecondSnapshotTrace 'second snapshot'

    Assert-Condition (($snapshot.FrameIds -join "`n") -eq ($stream.FrameIds -join "`n")) 'snapshot and stream FrameIdentity sets differ'
    Assert-Condition (($snapshot.FrameIds -join "`n") -eq ($replay.FrameIds -join "`n")) 'snapshot and replay FrameIdentity sets differ'
    Assert-Condition (($snapshot.Generations -join ',') -ne ($second.Generations -join ',')) 'reconnect did not advance the FrameIdentity connection generation'

    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $LegacyTrace }
    $legacyId = [string]$opened.data.trace_id
    $script:OpenedIds += $legacyId
    Wait-Ready $legacyId
    $legacy = Inspect $legacyId 'frame.identity' @{ limit = 10 }
    Assert-Condition (-not [bool]$legacy.data.present) 'legacy trace fabricated FrameIdentity presence'
    Invoke-Tool 'tracy_trace_close' @{ trace_id = $legacyId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $legacyId })

    $result = [ordered]@{
        status = 'passed'
        query_schema = '1.15.0'
        snapshot_identity_count = $snapshot.IdentityCount
        nested_editor_player_identity_count = $snapshot.NestedIdentityCount
        capture_edge_incomplete_identity_count = $snapshot.IncompleteIdentityCount
        sample_frame_id = $snapshot.SampleFrameId
        related_relation_count = $snapshot.RelatedCount
        correlation_chain_node_count = $snapshot.ChainNodeCount
        correlation_chain_truncated = $snapshot.ChainTruncated
        correlated_job_count = $snapshot.CorrelatedJobCount
        snapshot_stream_replay_equal = $true
        reconnect_generation_advanced = $true
        legacy_present = [bool]$legacy.data.present
        evidence_kind = 'exact'
        window_inference_used = $false
    }
    $json = $result | ConvertTo-Json -Depth 10
    if (-not [string]::IsNullOrEmpty($OutputFile))
    {
        $parent = Split-Path -Parent $OutputFile
        if (-not [string]::IsNullOrEmpty($parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        [IO.File]::WriteAllText($OutputFile, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    $json
}
finally
{
    foreach ($traceId in @($script:OpenedIds))
    {
        try { Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null } catch {}
    }
    if ($null -ne $script:Process -and -not $script:Process.HasExited)
    {
        $script:Process.StandardInput.Close()
        if (-not $script:Process.WaitForExit(3000)) { $script:Process.Kill() }
    }
}
