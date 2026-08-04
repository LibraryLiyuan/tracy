[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$LegacyTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $LegacyTrace, $AllowRoot))
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
    Assert-Condition ([bool]$response.result.structuredContent.ok) "$Name omitted structuredContent.ok=true"
    return $response.result.structuredContent
}

function Wait-Ready([string]$TraceId)
{
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return $status }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached $state" }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready"
}

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Open-Trace([string]$Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $script:OpenedIds += $traceId
    $status = Wait-Ready $traceId
    return [pscustomobject]@{ Id = $traceId; Status = $status }
}

function Resolve-Callstack([string]$TraceId, [string]$CallstackRef, [string]$Label)
{
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($CallstackRef)) "$Label has no callstack ref"
    $resolved = Inspect $TraceId 'callstack.frames' @{ callstack = $CallstackRef; max_depth = 64 }
    $frames = @($resolved.data.frames)
    Assert-Condition ($frames.Count -gt 0) "$Label callstack did not resolve"
    Assert-Condition ($frames.Count -le 62) "$Label callstack exceeded the Windows safe maximum"
    return [pscustomobject]@{
        Depth = $frames.Count
        Path = @($frames | ForEach-Object { if ([string]::IsNullOrEmpty([string]$_.name)) { [string]$_.address } else { [string]$_.name } })
    }
}

function Zone-Callstack([string]$TraceId, [string]$Name, [bool]$ExpectStack)
{
    $result = Inspect $TraceId 'zone.cpu.search' @{
        filter = @{ text = $Name; mode = 'exact' }
        limit = 1000
    }
    $zones = @($result.data.zones)
    Assert-Condition ($zones.Count -gt 0) "CPU zone is absent: $Name"
    $withStack = @($zones | Where-Object { $null -ne $_.callstack_ref -and -not [string]::IsNullOrEmpty([string]$_.callstack_ref) })
    if ($ExpectStack)
    {
        Assert-Condition ($withStack.Count -gt 0) "CPU zone has no callstack: $Name"
        $resolved = Resolve-Callstack $TraceId ([string]$withStack[0].callstack_ref) $Name
        return [pscustomobject]@{ Count = $zones.Count; WithStack = $withStack.Count; Depth = $resolved.Depth; Path = $resolved.Path }
    }
    Assert-Condition ($withStack.Count -eq 0) "disabled CPU zone unexpectedly has a callstack: $Name"
    return [pscustomobject]@{ Count = $zones.Count; WithStack = 0; Depth = 0; Path = @() }
}

function Assert-Domain($Domains, [string]$Name, [int]$Requested, [int]$Effective,
    [string]$Source, [bool]$Inherited)
{
    $domain = $Domains.PSObject.Properties[$Name].Value
    Assert-Condition ($null -ne $domain) "callstack domain is absent: $Name"
    Assert-Condition ([int]$domain.requested -eq $Requested) "$Name requested depth mismatch"
    Assert-Condition ([int]$domain.effective -eq $Effective) "$Name effective depth mismatch"
    Assert-Condition ([string]$domain.source -eq $Source) "$Name source mismatch"
    Assert-Condition ([bool]$domain.inherited -eq $Inherited) "$Name inherited flag mismatch"
    Assert-Condition (-not [bool]$domain.invalid) "$Name is unexpectedly invalid"
    Assert-Condition (-not [bool]$domain.clamped) "$Name is unexpectedly clamped"
}

function Validate-Trace([string]$Path, [string]$Label)
{
    $opened = Open-Trace $Path
    $traceId = $opened.Id
    $context = Inspect $traceId 'capture.context'
    Assert-Condition ([bool]$context.data.present) "$Label capture context is absent"
    Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context has invalid records"
    Assert-Condition ([bool]$context.data.layers.capture_config) "$Label capture config layer is absent"
    Assert-Condition ([bool]$context.data.layers.runtime) "$Label runtime context layer is absent"
    $callstack = $context.data.context.capture_config.callstack
    Assert-Condition ($null -ne $callstack) "$Label unified callstack config is absent"
    Assert-Condition ([int]$callstack.schema_version -eq 1) "$Label callstack schema mismatch"
    Assert-Condition ([int]$callstack.maximum_depth -eq 62) "$Label callstack maximum mismatch"
    Assert-Condition ([bool]$callstack.frozen_at_startup) "$Label callstack config was not frozen at startup"
    $domains = $callstack.domains
    Assert-Domain $domains 'global' 1 1 'command_line_global' $false
    Assert-Domain $domains 'csharp' 8 8 'command_line_domain' $false
    Assert-Domain $domains 'unity_marker' 1 1 'command_line_global' $true
    Assert-Domain $domains 'lua' 8 8 'command_line_csharp' $true
    Assert-Domain $domains 'job' 8 8 'command_line_domain' $false
    Assert-Domain $domains 'gpu_zone' 8 8 'command_line_domain' $false
    Assert-Domain $domains 'cpu_alloc' 8 8 'command_line_domain' $false
    Assert-Domain $domains 'gpu_alloc' 8 8 'command_line_domain' $false

    $direct = Zone-Callstack $traceId 'JN.Direct/Managed.Validation' $true
    $disabled = Zone-Callstack $traceId 'JN.Direct/Managed.Callstack.Disabled' $false
    $clamped = Zone-Callstack $traceId 'JN.Direct/Managed.Callstack.Clamped62' $true
    $lua = Zone-Callstack $traceId 'JN.Direct/Lua.Callstack.Validation' $true
    $callback = Zone-Callstack $traceId 'JN.Managed.Callback.Validation' $true

    $messages = Inspect $traceId 'message.search' @{
        filter = @{ text = 'JN managed callback+direct validation active'; mode = 'exact' }
        limit = 20
    }
    $messageValues = @($messages.data.messages)
    Assert-Condition ($messageValues.Count -eq 1) "$Label managed validation message is missing or duplicated"
    $messageStack = Resolve-Callstack $traceId ([string]$messageValues[0].callstack_ref) 'managed message'

    $jobs = Inspect $traceId 'job.search' @{ limit = 1000 }
    $jobValues = @($jobs.data.jobs)
    $jobsWithStack = @($jobValues | Where-Object { [UInt64]$_.schedule_callstack -ne 0 })
    $jobStackDepth = 0
    if ($jobValues.Count -gt 0)
    {
        Assert-Condition ($jobsWithStack.Count -gt 0) "$Label Job domain is present but has no schedule callstack"
        $jobStackDepth = (Resolve-Callstack $traceId ([string]$jobsWithStack[0].schedule_callstack_ref) 'Job schedule').Depth
    }

    $capabilities = Inspect $traceId 'system.capabilities'
    $gpuCapability = @($capabilities.data.domains | Where-Object { [string]$_.domain -eq 'zone.gpu' })
    $gpuZones = @()
    $gpuWithStack = @()
    $gpuStackDepth = 0
    if ($gpuCapability.Count -eq 1 -and [bool]$gpuCapability[0].present)
    {
        $gpu = Inspect $traceId 'zone.gpu.search' @{ limit = 1000 }
        $gpuZones = @($gpu.data.zones)
        Assert-Condition ($gpuZones.Count -gt 0) "$Label GPU capability is present but contains no zones"
        $gpuWithStack = @($gpuZones | Where-Object { $null -ne $_.callstack_ref -and -not [string]::IsNullOrEmpty([string]$_.callstack_ref) })
        Assert-Condition ($gpuWithStack.Count -gt 0) "$Label has no GPU command-recording callstack"
        $gpuStackDepth = (Resolve-Callstack $traceId ([string]$gpuWithStack[0].callstack_ref) 'GPU zone').Depth
    }

    $pools = Inspect $traceId 'memory.pools' @{ limit = 1000 }
    $poolValues = @($pools.data.pools | Where-Object { [string]$_.name -eq 'JN.Managed.Validation.NativeMemory' })
    Assert-Condition ($poolValues.Count -eq 1) "$Label selected managed memory pool is absent or duplicated"
    $memory = Inspect $traceId 'memory.events' @{ pool_ref = [string]$poolValues[0].ref; limit = 20 }
    $memoryEvents = @($memory.data.events)
    Assert-Condition ($memoryEvents.Count -eq 1) "$Label selected managed allocation is absent or duplicated"
    $memoryStack = Resolve-Callstack $traceId ([string]$memoryEvents[0].allocation_callstack_ref) 'CPU allocation'

    $validation = Inspect $traceId 'validation.run'
    Assert-Condition ([bool]$validation.data.valid) "$Label trace validation failed"
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) "$Label trace validation reported errors"

    Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $traceId })
    return [ordered]@{
        config_generation = [string]$callstack.config_generation
        direct = @{ count = $direct.Count; with_stack = $direct.WithStack; depth = $direct.Depth }
        disabled = @{ count = $disabled.Count; with_stack = $disabled.WithStack }
        clamped = @{ count = $clamped.Count; with_stack = $clamped.WithStack; depth = $clamped.Depth }
        lua = @{ count = $lua.Count; with_stack = $lua.WithStack; depth = $lua.Depth }
        callback = @{ count = $callback.Count; with_stack = $callback.WithStack; depth = $callback.Depth }
        message_depth = $messageStack.Depth
        jobs_scanned = $jobValues.Count
        jobs_with_schedule_stack = $jobsWithStack.Count
        job_stack_depth = $jobStackDepth
        gpu_zones_scanned = $gpuZones.Count
        gpu_zones_with_stack = $gpuWithStack.Count
        gpu_stack_depth = $gpuStackDepth
        cpu_allocation_stack_depth = $memoryStack.Depth
        validation_errors = [string]$validation.data.error_count
    }
}

try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-callstack-config-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $snapshot = Validate-Trace $SnapshotTrace 'snapshot'
    $stream = Validate-Trace $StreamTrace 'stream'
    $replay = Validate-Trace $ReplayTrace 'replay'
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($stream | ConvertTo-Json -Compress -Depth 20)) 'snapshot/stream callstack semantics differ'
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($replay | ConvertTo-Json -Compress -Depth 20)) 'snapshot/replay callstack semantics differ'

    $legacyOpened = Open-Trace $LegacyTrace
    $legacyContext = Inspect $legacyOpened.Id 'capture.context'
    Assert-Condition ($null -eq $legacyContext.data.context.capture_config.callstack) 'legacy trace fabricated unified callstack config'
    Invoke-Tool 'tracy_trace_close' @{ trace_id = $legacyOpened.Id } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $legacyOpened.Id })

    $result = [ordered]@{
        status = 'passed'
        query_schema = '1.14.0'
        maximum_callstack_depth = 62
        config_generation = $snapshot.config_generation
        direct_zone_count = $snapshot.direct.count
        disabled_zone_stack_count = $snapshot.disabled.with_stack
        lua_zone_stack_count = $snapshot.lua.with_stack
        callback_zone_stack_count = $snapshot.callback.with_stack
        jobs_with_schedule_stack = $snapshot.jobs_with_schedule_stack
        job_event_present = ($snapshot.jobs_scanned -gt 0)
        gpu_zones_with_stack = $snapshot.gpu_zones_with_stack
        gpu_event_present = ($snapshot.gpu_zones_scanned -gt 0)
        cpu_allocation_stack_depth = $snapshot.cpu_allocation_stack_depth
        snapshot_stream_replay_equal = $true
        legacy_callstack_config_present = $false
        validation_errors = $snapshot.validation_errors
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
