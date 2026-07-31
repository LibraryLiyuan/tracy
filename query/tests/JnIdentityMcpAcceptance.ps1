[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$LegacyTrace,
    [switch]$CheckContextQuality,
    [switch]$SkipSyntheticProducerChecks,
    [string]$ExpectedDegradedProducer = 'test.degraded',
    [string]$ExpectedRealZeroProducer = 'test.real-zero',
    [string]$ExpectedDisabledProducer = 'test.disabled',
    [string]$ExpectedUnsupportedProducer = 'test.unsupported',
    [string]$ExpectedZoneDomainProducer = 'test.zone-domain',
    [string]$ExpectedGpuMemoryProducer = 'memory.gpu.registry',
    [string]$ExpectedWorkloadScene,
    [string[]]$RequiredProducerKeys = @(),
    [string[]]$ExpectedDisabledProducerKeys = @(),
    [switch]$RequireAnyEmittedProducer
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }
foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $AllowRoot)) { Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path" }
if (-not [string]::IsNullOrEmpty($LegacyTrace)) { Assert-Condition (Test-Path -LiteralPath $LegacyTrace) "legacy trace does not exist: $LegacyTrace" }

$script:NextRequestId = 1
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

function Send-Rpc([string]$Method, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 240000)
{
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:Process.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:Process.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $script:Process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) { throw "MCP response timed out: $Method" }
        $line = $read.Result
        if ($null -eq $line) { throw "MCP stdout closed before response: $Method" }
        $message = $line | ConvertFrom-Json
        if ($null -ne $message.PSObject.Properties['id'])
        {
            Assert-Condition ([string]$message.id -eq [string]$id) "unexpected MCP response id"
            return $message
        }
    }
    throw "MCP response deadline expired: $Method"
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
    if ([bool]$response.result.isError) { throw "$Name failed: $($response.result.structuredContent | ConvertTo-Json -Compress -Depth 20)" }
    return $response.result.structuredContent
}

function Wait-Ready([string]$TraceId)
{
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
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

function Open-Trace([string]$Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $id = [string]$opened.data.trace_id
    $status = Wait-Ready $id
    return [pscustomobject]@{ Id = $id; Status = $status }
}

function Inspect-Identity([string]$TraceId)
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = 'trace.identity'; params = @{} }
}

function Read-IdentityTrace([string]$Path, [bool]$RunGeneralChecks = $false)
{
    $opened = Open-Trace $Path
    $script:OpenedIds += $opened.Id
    try
    {
        $identity = Inspect-Identity $opened.Id
        $context = $null
        $coverage = $null
        $producerList = $null
        if ($CheckContextQuality)
        {
            $context = Invoke-Tool 'tracy_inspect' @{ trace_id = $opened.Id; method = 'capture.context'; params = @{} }
            $coverage = Invoke-Tool 'tracy_inspect' @{ trace_id = $opened.Id; method = 'capture.coverage'; params = @{} }
            $producerList = Invoke-Tool 'tracy_inspect' @{ trace_id = $opened.Id; method = 'producer.list'; params = @{} }
        }
        $overview = $null
        $validation = $null
        if ($RunGeneralChecks)
        {
            $overview = Invoke-Tool 'tracy_overview' @{ trace_id = $opened.Id }
            $validation = Invoke-Tool 'tracy_validate' @{ trace_id = $opened.Id; async = $false }
            Assert-Condition ([bool]$overview.ok) 'tracy_overview failed'
            Assert-Condition ([bool]$validation.ok) 'tracy_validate failed'
        }
        Invoke-Tool 'tracy_trace_close' @{ trace_id = $opened.Id } | Out-Null
        $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $opened.Id })
        return [pscustomobject]@{ Status = $opened.Status; Identity = $identity; Context = $context; Coverage = $coverage; ProducerList = $producerList; Overview = $overview; Validation = $validation }
    }
    catch
    {
        throw
    }
}

$script:OpenedIds = @()
try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-identity-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $tools = Send-Rpc 'tools/list'
    Assert-Condition (@($tools.result.tools).Count -eq 12) 'model-visible MCP tool count changed'
    $described = Invoke-Tool 'tracy_describe' @{}
    Assert-Condition ([bool]$described.ok) 'tracy_describe failed'

    $snapshot = Read-IdentityTrace $SnapshotTrace $true
    $stream = Read-IdentityTrace $StreamTrace
    $replay = Read-IdentityTrace $ReplayTrace
    $snapshotIdentity = $snapshot.Identity
    $streamIdentity = $stream.Identity
    $replayIdentity = $replay.Identity

    foreach ($result in @($snapshotIdentity, $streamIdentity, $replayIdentity))
    {
        Assert-Condition ([bool]$result.ok) 'trace.identity query failed'
        Assert-Condition ([string]$result.schema_version -eq '1.2.0') 'unexpected Query schema version'
        Assert-Condition ([bool]$result.data.present) 'capture identity is absent'
        Assert-Condition ([bool]$result.data.complete) 'capture identity is incomplete'
        Assert-Condition (@($result.data.missing_required).Count -eq 0) 'required identity fields are missing'
        Assert-Condition (@($result.data.conflicts).Count -eq 0) 'identity contains conflicts'
        Assert-Condition (@($result.data.invalid_records).Count -eq 0) 'identity contains invalid records'
    }
    $snapshotCanonical = $snapshotIdentity.data.identity | ConvertTo-Json -Compress -Depth 30
    $streamCanonical = $streamIdentity.data.identity | ConvertTo-Json -Compress -Depth 30
    $replayCanonical = $replayIdentity.data.identity | ConvertTo-Json -Compress -Depth 30
    Assert-Condition ($snapshotCanonical -eq $streamCanonical) 'snapshot and committed stream identity differ'
    Assert-Condition ($snapshotCanonical -eq $replayCanonical) 'snapshot and replay identity differ'
    Assert-Condition ([string]$snapshotIdentity.data.canonical_fingerprint -eq [string]$streamIdentity.data.canonical_fingerprint) 'identity fingerprints differ'
    Assert-Condition ([string]$snapshotIdentity.data.canonical_fingerprint -eq [string]$replayIdentity.data.canonical_fingerprint) 'snapshot and replay identity fingerprints differ'
    Assert-Condition (-not [string]::IsNullOrEmpty([string]$snapshotIdentity.data.identity.build.artifacts.jn_client.loaded_path)) 'actual JNTracyClient loaded path is missing'

    $contextGeneration = $null
    $producerCount = $null
    $degradedState = $null
    $realZeroState = $null
    if ($CheckContextQuality)
    {
        foreach ($result in @($snapshot.Context, $stream.Context, $replay.Context))
        {
            Assert-Condition ([bool]$result.ok) 'capture.context query failed'
            Assert-Condition ([bool]$result.data.present) 'capture context is absent'
            Assert-Condition ([bool]$result.data.complete) 'capture context is incomplete'
            Assert-Condition (@($result.data.missing_layers).Count -eq 0) 'capture context layers are missing'
            Assert-Condition (@($result.data.invalid_records).Count -eq 0) 'capture context has invalid records'
        }
        foreach ($result in @($snapshot.Coverage, $stream.Coverage, $replay.Coverage))
        {
            Assert-Condition ([bool]$result.ok) 'capture.coverage query failed'
            Assert-Condition ([bool]$result.data.present) 'producer coverage is absent'
            Assert-Condition ([bool]$result.data.complete) 'producer coverage is incomplete'
            Assert-Condition (@($result.data.invalid_records).Count -eq 0) 'producer coverage has invalid records'
        }
        foreach ($result in @($snapshot.ProducerList, $stream.ProducerList, $replay.ProducerList))
        {
            Assert-Condition ([bool]$result.ok) 'producer.list query failed'
            Assert-Condition ([bool]$result.data.present) 'producer list is absent'
            Assert-Condition ([bool]$result.data.complete) 'producer list is incomplete'
            Assert-Condition (@($result.data.invalid_records).Count -eq 0) 'producer list has invalid records'
        }
        $snapshotContext = $snapshot.Context.data.context | ConvertTo-Json -Compress -Depth 30
        Assert-Condition ($snapshotContext -eq ($stream.Context.data.context | ConvertTo-Json -Compress -Depth 30)) 'snapshot and stream context differ'
        Assert-Condition ($snapshotContext -eq ($replay.Context.data.context | ConvertTo-Json -Compress -Depth 30)) 'snapshot and replay context differ'
        $snapshotProducers = $snapshot.Coverage.data.producers | ConvertTo-Json -Compress -Depth 30
        Assert-Condition ($snapshotProducers -eq ($stream.Coverage.data.producers | ConvertTo-Json -Compress -Depth 30)) 'snapshot and stream producer coverage differ'
        Assert-Condition ($snapshotProducers -eq ($replay.Coverage.data.producers | ConvertTo-Json -Compress -Depth 30)) 'snapshot and replay producer coverage differ'
        Assert-Condition ($snapshotProducers -eq ($snapshot.ProducerList.data.producers | ConvertTo-Json -Compress -Depth 30)) 'producer.list and capture.coverage differ'
        if (-not [string]::IsNullOrEmpty($ExpectedWorkloadScene))
        {
            Assert-Condition ([string]$snapshot.Context.data.context.workload.scene -eq $ExpectedWorkloadScene) 'unexpected workload scene'
        }
        foreach ($producerKey in $RequiredProducerKeys)
        {
            $required = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $producerKey })
            Assert-Condition ($required.Count -eq 1) "required producer is missing or duplicated: $producerKey"
        }
        foreach ($producerKey in $ExpectedDisabledProducerKeys)
        {
            $disabledRequired = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $producerKey })
            Assert-Condition ($disabledRequired.Count -eq 1 -and [string]$disabledRequired[0].state -eq 'disabled') "producer is not disabled: $producerKey"
        }
        if ($RequireAnyEmittedProducer)
        {
            $emitted = @($snapshot.Coverage.data.producers | Where-Object { [uint64]$_.counters.emitted -gt 0 })
            Assert-Condition ($emitted.Count -gt 0) 'no producer emitted an event in the capture window'
        }
        if (-not $SkipSyntheticProducerChecks)
        {
            $degraded = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedDegradedProducer })
            $realZero = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedRealZeroProducer })
            $disabled = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedDisabledProducer })
            $unsupported = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedUnsupportedProducer })
            $zoneDomain = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedZoneDomainProducer })
            $gpuMemory = @($snapshot.Coverage.data.producers | Where-Object { $_.key -eq $ExpectedGpuMemoryProducer })
            Assert-Condition ($degraded.Count -eq 1 -and [string]$degraded[0].state -eq 'degraded') 'degraded producer state was not preserved'
            Assert-Condition ([uint64]$degraded[0].counters.filtered -gt 0 -and [uint64]$degraded[0].counters.overflow -gt 0) 'filter/overflow counters were not preserved'
            Assert-Condition ($realZero.Count -eq 1 -and [string]$realZero[0].state -eq 'real_zero') 'real-zero producer was not distinguished'
            Assert-Condition ($disabled.Count -eq 1 -and [string]$disabled[0].state -eq 'disabled') 'disabled producer was not distinguished'
            Assert-Condition ($unsupported.Count -eq 1 -and [string]$unsupported[0].state -eq 'unsupported') 'unsupported producer was not distinguished'
            Assert-Condition ($zoneDomain.Count -eq 1 -and [string]$zoneDomain[0].state -eq 'covered') 'attributed Zone producer was not covered'
            Assert-Condition ([uint64]$zoneDomain[0].counters.observed -gt 0 -and [uint64]$zoneDomain[0].counters.emitted -gt 0) 'attributed Zone counters were not preserved'
            Assert-Condition ($gpuMemory.Count -eq 1 -and [bool]$gpuMemory[0].effective) 'GPU memory producer was not effective'
            Assert-Condition ([uint64]$gpuMemory[0].counters.observed -gt 0 -and [uint64]$gpuMemory[0].counters.emitted -gt 0) 'GPU memory producer counters were not preserved'
            $degradedState = [string]$degraded[0].state
            $realZeroState = [string]$realZero[0].state
        }
        $probe = Open-Trace $SnapshotTrace
        $script:OpenedIds += $probe.Id
        $probeKey = if ($RequiredProducerKeys.Count -gt 0) { $RequiredProducerKeys[0] } else { $ExpectedDegradedProducer }
        $producerGet = Invoke-Tool 'tracy_inspect' @{ trace_id = $probe.Id; method = 'producer.get'; params = @{ key = $probeKey } }
        Assert-Condition ([string]$producerGet.data.producer.key -eq $probeKey) 'producer.get did not return the requested producer'
        Invoke-Tool 'tracy_trace_close' @{ trace_id = $probe.Id } | Out-Null
        $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $probe.Id })
        $contextGeneration = [string]$snapshot.Context.data.generation
        $producerCount = @($snapshot.Coverage.data.producers).Count
    }

    $legacyPresent = $null
    if (-not [string]::IsNullOrEmpty($LegacyTrace))
    {
        $legacy = Read-IdentityTrace $LegacyTrace
        $legacyIdentity = $legacy.Identity
        Assert-Condition (-not [bool]$legacyIdentity.data.present) 'legacy trace fabricated a capture identity'
        Assert-Condition ($null -eq $legacyIdentity.data.identity) 'legacy identity must be null'
        $legacyPresent = [bool]$legacyIdentity.data.present
        if ($CheckContextQuality)
        {
            Assert-Condition (-not [bool]$legacy.Context.data.present) 'legacy trace fabricated Capture Context'
            Assert-Condition (-not [bool]$legacy.Coverage.data.present) 'legacy trace fabricated Producer Quality'
        }
    }

    [pscustomobject]@{
        ok = $true
        query_schema = [string]$snapshotIdentity.schema_version
        snapshot_fingerprint = [string]$snapshot.Status.data.status.fingerprint
        stream_fingerprint = [string]$stream.Status.data.status.fingerprint
        replay_fingerprint = [string]$replay.Status.data.status.fingerprint
        identity_fingerprint = [string]$snapshotIdentity.data.canonical_fingerprint
        identity_records = [string]$snapshotIdentity.data.records.valid
        identity_bytes = [string]$snapshotIdentity.data.records.envelope_bytes
        build_id = [string]$snapshotIdentity.data.identity.build.build_id
        engine_revision = [string]$snapshotIdentity.data.identity.build.repositories.engine.revision
        package_revision = [string]$snapshotIdentity.data.identity.build.repositories.package.revision
        tracy_revision = [string]$snapshotIdentity.data.identity.build.repositories.tracy.revision
        connection_id = [string]$snapshotIdentity.data.identity.connection.id
        instance_cookie = [string]$snapshotIdentity.data.identity.connection.instance_cookie
        jn_client_loaded_path = [string]$snapshotIdentity.data.identity.build.artifacts.jn_client.loaded_path
        overview_ok = [bool]$snapshot.Overview.ok
        validation_ok = [bool]$snapshot.Validation.ok
        legacy_present = $legacyPresent
        context_generation = $contextGeneration
        producer_count = $producerCount
        degraded_state = $degradedState
        real_zero_state = $realZeroState
    } | ConvertTo-Json -Depth 10
}
finally
{
    foreach ($id in $script:OpenedIds)
    {
        try { Invoke-Tool 'tracy_trace_close' @{ trace_id = $id } | Out-Null } catch {}
    }
    if (-not $process.HasExited)
    {
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
    $process.Dispose()
}
