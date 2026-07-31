[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$LegacyTrace
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
        return [pscustomobject]@{ Status = $opened.Status; Identity = $identity; Overview = $overview; Validation = $validation }
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
        Assert-Condition ([string]$result.schema_version -eq '1.1.0') 'unexpected Query schema version'
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

    $legacyPresent = $null
    if (-not [string]::IsNullOrEmpty($LegacyTrace))
    {
        $legacy = Read-IdentityTrace $LegacyTrace
        $legacyIdentity = $legacy.Identity
        Assert-Condition (-not [bool]$legacyIdentity.data.present) 'legacy trace fabricated a capture identity'
        Assert-Condition ($null -eq $legacyIdentity.data.identity) 'legacy identity must be null'
        $legacyPresent = [bool]$legacyIdentity.data.present
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
