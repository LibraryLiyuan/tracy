[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}
foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $AllowRoot))
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

function Resolve-Depth([string]$TraceId, [string]$CallstackRef, [string]$Label)
{
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($CallstackRef)) "$Label has no callstack ref"
    $resolved = Inspect $TraceId 'callstack.frames' @{ callstack = $CallstackRef; max_depth = 64 }
    $frames = @($resolved.data.frames)
    Assert-Condition ($frames.Count -gt 0) "$Label callstack did not resolve"
    Assert-Condition ($frames.Count -le 62) "$Label callstack exceeded the Windows safe maximum"
    return $frames.Count
}

function Assert-EffectiveDomain($Domains, [string]$Name, [int]$Effective, [string]$Source, [bool]$Inherited)
{
    $domain = $Domains.PSObject.Properties[$Name].Value
    Assert-Condition ($null -ne $domain) "callstack domain is absent: $Name"
    Assert-Condition ([int]$domain.effective -eq $Effective) "$Name effective depth mismatch"
    Assert-Condition ([string]$domain.source -eq $Source) "$Name source mismatch"
    Assert-Condition ([bool]$domain.inherited -eq $Inherited) "$Name inherited flag mismatch"
    Assert-Condition (-not [bool]$domain.invalid -and -not [bool]$domain.clamped) "$Name config is invalid or clamped"
}

function Validate-SceneTrace([string]$Path, [string]$Label)
{
    $opened = Open-Trace $Path
    $traceId = $opened.Id
    $context = Inspect $traceId 'capture.context'
    Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$Label capture context is incomplete"
    Assert-Condition (@($context.data.missing_layers).Count -eq 0) "$Label capture context has missing layers"
    Assert-Condition (@($context.data.invalid_records).Count -eq 0) "$Label capture context has invalid records"
    Assert-Condition ([string]$context.data.context.workload.scene -eq 'taijibase_constructedarmor_main_01') "$Label workload scene mismatch"
    Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$Label graphics API mismatch"
    Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$Label Graphics Jobs mode mismatch"

    $callstack = $context.data.context.capture_config.callstack
    Assert-Condition ($null -ne $callstack -and [int]$callstack.maximum_depth -eq 62) "$Label callstack config is absent or invalid"
    Assert-Condition ([bool]$callstack.frozen_at_startup) "$Label callstack config was not frozen"
    $domains = $callstack.domains
    Assert-EffectiveDomain $domains 'csharp' 8 'command_line_domain' $false
    Assert-EffectiveDomain $domains 'unity_marker' 1 'command_line_global' $true
    Assert-EffectiveDomain $domains 'lua' 8 'command_line_csharp' $true
    Assert-EffectiveDomain $domains 'job' 8 'command_line_domain' $false
    Assert-EffectiveDomain $domains 'gpu_zone' 8 'command_line_domain' $false
    Assert-EffectiveDomain $domains 'cpu_alloc' 8 'command_line_domain' $false
    Assert-EffectiveDomain $domains 'gpu_alloc' 8 'command_line_domain' $false

    $counts = Inspect $traceId 'trace.counts'
    Assert-Condition ([UInt64]$counts.data.gpu_zones -gt 0) "$Label has no GPU zones"
    Assert-Condition ([UInt64]$counts.data.jobs -gt 0) "$Label has no Jobs"
    Assert-Condition ([UInt64]$counts.data.callstack_payloads -gt 0) "$Label has no callstack payloads"

    $gpuContexts = Inspect $traceId 'zone.gpu.contexts' @{ limit = 20 }
    $contextValues = @($gpuContexts.data.contexts)
    $direct = @($contextValues | Where-Object { [string]$_.name -eq 'GPU Direct' })
    $copy = @($contextValues | Where-Object { [string]$_.name -eq 'GPU Copy' })
    Assert-Condition ($direct.Count -eq 1 -and [UInt64]$direct[0].zone_count -gt 0) "$Label GPU Direct context is absent"
    Assert-Condition ($copy.Count -eq 1 -and [UInt64]$copy[0].zone_count -gt 0) "$Label GPU Copy context is absent"

    $gpu = Inspect $traceId 'zone.gpu.search' @{ limit = 1000 }
    $gpuZones = @($gpu.data.zones)
    $gpuWithStack = @($gpuZones | Where-Object { $null -ne $_.callstack_ref -and -not [string]::IsNullOrEmpty([string]$_.callstack_ref) })
    Assert-Condition ($gpuZones.Count -gt 0 -and $gpuWithStack.Count -gt 0) "$Label GPU zones have no command-recording callstack"
    $gpuDepth = Resolve-Depth $traceId ([string]$gpuWithStack[0].callstack_ref) 'GPU zone'

    $jobs = Inspect $traceId 'job.search' @{
        filter = @{ text = 'Unity.Job.NativeSingle'; mode = 'exact' }
        limit = 1000
    }
    $jobValues = @($jobs.data.jobs)
    $jobsWithStack = @($jobValues | Where-Object { [UInt64]$_.schedule_callstack -ne 0 })
    Assert-Condition ($jobValues.Count -gt 0 -and $jobsWithStack.Count -gt 0) "$Label real Job schedules have no callstack"
    $jobDepth = Resolve-Depth $traceId ([string]$jobsWithStack[0].schedule_callstack_ref) 'Job schedule'

    $validation = Inspect $traceId 'validation.run'
    Assert-Condition ([bool]$validation.data.valid) "$Label trace validation failed"
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) "$Label trace validation reported errors"

    Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $traceId })
    return [ordered]@{
        config_generation = [string]$callstack.config_generation
        scene = [string]$context.data.context.workload.scene
        cpu_zones = [string]$counts.data.cpu_zones
        gpu_zones = [string]$counts.data.gpu_zones
        jobs = [string]$counts.data.jobs
        callstack_payloads = [string]$counts.data.callstack_payloads
        gpu_context_count = $contextValues.Count
        gpu_direct_zones = [string]$direct[0].zone_count
        gpu_copy_zones = [string]$copy[0].zone_count
        gpu_scanned = $gpuZones.Count
        gpu_with_stack = $gpuWithStack.Count
        gpu_stack_depth = $gpuDepth
        jobs_scanned = $jobValues.Count
        jobs_with_stack = $jobsWithStack.Count
        job_stack_depth = $jobDepth
        validation_errors = [string]$validation.data.error_count
    }
}

try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-scene-callstack-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $snapshot = Validate-SceneTrace $SnapshotTrace 'snapshot'
    $stream = Validate-SceneTrace $StreamTrace 'stream'
    $replay = Validate-SceneTrace $ReplayTrace 'replay'
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($stream | ConvertTo-Json -Compress -Depth 20)) 'snapshot/stream scene callstack semantics differ'
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($replay | ConvertTo-Json -Compress -Depth 20)) 'snapshot/replay scene callstack semantics differ'

    $result = [ordered]@{
        status = 'passed'
        query_schema = '1.14.0'
        scene = $snapshot.scene
        config_generation = $snapshot.config_generation
        cpu_zones = $snapshot.cpu_zones
        gpu_zones = $snapshot.gpu_zones
        jobs = $snapshot.jobs
        callstack_payloads = $snapshot.callstack_payloads
        gpu_context_count = $snapshot.gpu_context_count
        gpu_direct_zones = $snapshot.gpu_direct_zones
        gpu_copy_zones = $snapshot.gpu_copy_zones
        gpu_zones_with_stack = $snapshot.gpu_with_stack
        gpu_stack_depth = $snapshot.gpu_stack_depth
        jobs_with_schedule_stack = $snapshot.jobs_with_stack
        job_stack_depth = $snapshot.job_stack_depth
        snapshot_stream_replay_equal = $true
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
