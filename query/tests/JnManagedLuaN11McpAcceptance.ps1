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
    Assert-Condition ([string]$message.id -eq [string]$id) "unexpected MCP response id"
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

function Validate-N11 {
    param([string] $TraceId)
    $capabilities = Inspect $TraceId 'system.capabilities'
    $scriptCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'runtime.script' })
    $gcCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'memory.gc' })
    Assert-Condition ($scriptCapability.Count -eq 1 -and [bool]$scriptCapability[0].present) 'runtime.script capability unavailable'
    Assert-Condition ($gcCapability.Count -eq 1 -and [bool]$gcCapability[0].present) 'memory.gc capability unavailable'

    $script = Inspect $TraceId 'runtime.script.summary'
    $managedFrames = Inspect $TraceId 'runtime.script.frames' @{ runtime = 'managed'; limit = 100 }
    $luaFrames = Inspect $TraceId 'runtime.script.frames' @{ runtime = 'lua'; limit = 100 }
    $managedStacks = Inspect $TraceId 'runtime.script.stacks' @{ runtime = 'managed'; limit = 100 }
    $luaStacks = Inspect $TraceId 'runtime.script.stacks' @{ runtime = 'lua'; limit = 100 }
    $zones = Inspect $TraceId 'runtime.script.zones' @{ limit = 100 }
    $gc = Inspect $TraceId 'memory.gc.summary'
    $gcEvents = Inspect $TraceId 'memory.gc.events' @{ limit = 100 }
    $validation = Inspect $TraceId 'validation.run'

    Assert-Condition ([bool]$script.data.present -and [bool]$script.data.complete) 'script summary incomplete'
    if ($RealCapture) {
        Assert-Condition ([UInt64]$script.data.counts.frames -ge 2) 'real script frame coverage missing'
        Assert-Condition ([UInt64]$script.data.counts.stacks -ge 2) 'real script stack coverage missing'
        Assert-Condition ([UInt64]$script.data.counts.markers -ge 2) 'real script marker coverage missing'
        Assert-Condition ([UInt64]$script.data.counts.zones -ge 2 -and
            [UInt64]$script.data.counts.complete_zones -eq [UInt64]$script.data.counts.zones) 'real script zone pairing mismatch'
    }
    else {
        Assert-Condition ([UInt64]$script.data.counts.frames -eq 2) 'script frame count mismatch'
        Assert-Condition ([UInt64]$script.data.counts.stacks -eq 2) 'script stack count mismatch'
        Assert-Condition ([UInt64]$script.data.counts.markers -eq 2) 'script marker count mismatch'
        Assert-Condition ([UInt64]$script.data.counts.zones -eq 2 -and [UInt64]$script.data.counts.complete_zones -eq 2) 'script zone pairing mismatch'
    }
    Assert-Condition ([UInt64]$script.data.quality.invalid_records -eq 0) 'invalid script records present'
    Assert-Condition ([UInt64]$script.data.quality.unresolved_references -eq 0) 'unresolved script references present'

    $managedFrame = @($managedFrames.data.frames)[0]
    $luaFrame = @($luaFrames.data.frames)[0]
    if ($RealCapture) {
        $managedCandidates = @($managedFrames.data.frames | Where-Object {
            [string]$_.function -like '*JNTracyN11Validation*'
        })
        Assert-Condition ($managedCandidates.Count -gt 0) 'real managed validation frame missing'
        $managedFrame = $managedCandidates[0]
        Assert-Condition ([string]$managedFrame.file -like '*com.jngame.tracy/editor/jntracyn11validation.cs') 'real managed source normalization mismatch'
        Assert-Condition (@($luaFrames.data.frames).Count -gt 0) 'real Lua debug frames missing'
        Assert-Condition ([string]$luaFrame.file -like '*jntracy.n11.validation*') 'real Lua source normalization mismatch'
        Assert-Condition (@($managedStacks.data.stacks | Where-Object { [bool]$_.complete }).Count -gt 0) 'real managed stack expansion failed'
        Assert-Condition (@($luaStacks.data.stacks | Where-Object { [bool]$_.complete }).Count -gt 0) 'real Lua stack expansion failed'
    }
    else {
        Assert-Condition (@($managedFrames.data.frames).Count -eq 1 -and [string]$managedFrame.function -eq 'Harness.Managed.Caller') 'managed frame mismatch'
        Assert-Condition ([string]$managedFrame.file -eq 'package/com.jngame.tracy/runtime/harnessmanaged.cs') 'managed source normalization mismatch'
        Assert-Condition (@($luaFrames.data.frames).Count -eq 1 -and [string]$luaFrame.function -eq 'HarnessLuaUpdate') 'Lua frame mismatch'
        Assert-Condition ([string]$luaFrame.file -eq 'project/lua/harness.lua') 'Lua source normalization mismatch'
        Assert-Condition (@($managedStacks.data.stacks).Count -eq 1 -and [bool]$managedStacks.data.stacks[0].complete) 'managed stack expansion failed'
        Assert-Condition (@($luaStacks.data.stacks).Count -eq 1 -and [bool]$luaStacks.data.stacks[0].complete) 'Lua stack expansion failed'
    }
    foreach ($zone in @($zones.data.zones)) {
        Assert-Condition ([bool]$zone.complete -and [Int64]$zone.duration_ns -gt 0) 'script zone is incomplete or has zero duration'
        Assert-Condition ($null -ne $zone.marker -and $null -ne $zone.stack) 'script zone relation is unresolved'
    }

    Assert-Condition ([bool]$gc.data.present -and [bool]$gc.data.complete) 'GC summary incomplete'
    if ($RealCapture) {
        Assert-Condition ([UInt64]$gc.data.counts.events -gt 0) 'real GC events missing'
        Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -gt 0) 'real managed heap sample missing'
        Assert-Condition ([UInt64]$gc.data.latest.lua_heap_used_bytes -gt 0) 'real Lua heap sample missing'
        Assert-Condition (@($gcEvents.data.events).Count -gt 0) 'real GC event pagination empty'
    }
    else {
        Assert-Condition ([UInt64]$gc.data.counts.events -eq 4) 'GC event count mismatch'
        Assert-Condition ([UInt64]$gc.data.counts.paired_intervals -eq 1) 'GC interval pairing mismatch'
        Assert-Condition ([UInt64]$gc.data.latest.managed_heap_used_bytes -eq 1048576) 'managed heap sample mismatch'
        Assert-Condition ([UInt64]$gc.data.latest.lua_heap_used_bytes -eq 65536) 'Lua heap sample mismatch'
        Assert-Condition (@($gcEvents.data.events).Count -eq 4) 'GC event pagination mismatch'
    }
    Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) 'trace validation failed'

    return [ordered]@{
        script = [ordered]@{
            frames = [string]$script.data.counts.frames
            stacks = [string]$script.data.counts.stacks
            markers = [string]$script.data.counts.markers
            zones = [string]$script.data.counts.zones
            managed_function = [string]$managedFrame.function
            managed_file = [string]$managedFrame.file
            lua_function = [string]$luaFrame.function
            lua_file = [string]$luaFrame.file
        }
        gc = [ordered]@{
            events = [string]$gc.data.counts.events
            paired_intervals = [string]$gc.data.counts.paired_intervals
            managed_heap_used_bytes = [string]$gc.data.latest.managed_heap_used_bytes
            lua_heap_used_bytes = [string]$gc.data.latest.lua_heap_used_bytes
        }
        validation_errors = [string]$validation.data.error_count
    }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n11-acceptance'; version = '1.0' } }
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
            semantics = Validate-N11 $traceId
        }
        [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $traceIds = @($traceIds | Where-Object { $_ -ne $traceId })
    }

    $snapshotSemantics = $results.snapshot.semantics | ConvertTo-Json -Compress -Depth 30
    Assert-Condition (($results.stream.semantics | ConvertTo-Json -Compress -Depth 30) -eq $snapshotSemantics) 'snapshot/stream N11 semantic mismatch'
    Assert-Condition (($results.replay.semantics | ConvertTo-Json -Compress -Depth 30) -eq $snapshotSemantics) 'snapshot/replay N11 semantic mismatch'
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
