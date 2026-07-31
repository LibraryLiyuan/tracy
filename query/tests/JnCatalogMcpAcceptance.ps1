[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$ReplayTrace,
    [Parameter(Mandatory = $true)][string]$SecondSnapshotTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$LegacyTrace,
    [string[]]$RequiredKinds = @('source', 'pass', 'resource', 'metric', 'scene', 'camera', 'job', 'gpu_taxonomy', 'quality'),
    [int]$MinimumDefinitions = 9,
    [int]$MinimumEntities = 2
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $ReplayTrace, $SecondSnapshotTrace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}
if (-not [string]::IsNullOrEmpty($LegacyTrace))
{
    Assert-Condition (Test-Path -LiteralPath $LegacyTrace) "legacy trace does not exist: $LegacyTrace"
}

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
    $read = $script:Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    $line = $read.Result
    if ($null -eq $line) { throw "MCP stdout closed before response: $Method" }
    $message = $line | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) "unexpected MCP response id"
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
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
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

function Read-Catalog([string]$Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $script:OpenedIds += $traceId
    Wait-Ready $traceId
    $capabilities = Inspect $traceId 'system.capabilities'
    $kinds = Inspect $traceId 'catalog.kinds'
    $definitions = Inspect $traceId 'catalog.list' @{ limit = 500 }
    $entities = Inspect $traceId 'catalog.entities' @{ limit = 500 }
    $quality = Inspect $traceId 'catalog.quality'
    $appInfo = Inspect $traceId 'trace.app_info'
    $firstKey = [string]$definitions.data.definitions[0].definition_key
    $definition = Inspect $traceId 'catalog.get' @{ definition_key = $firstKey }
    Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId } | Out-Null
    $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $traceId })
    return [pscustomobject]@{
        Capabilities = $capabilities
        Kinds = $kinds
        Definitions = $definitions
        Entities = $entities
        Quality = $quality
        AppInfo = $appInfo
        Definition = $definition
    }
}

function Definition-KeySet($Catalog)
{
    return @($Catalog.Definitions.data.definitions | ForEach-Object { [string]$_.definition_key } | Sort-Object -Unique)
}

function Assert-Catalog($Catalog, [string]$Label)
{
    Assert-Condition ([bool]$Catalog.Kinds.ok) "$Label catalog.kinds failed"
    Assert-Condition ([string]$Catalog.Kinds.schema_version -eq '1.3.0') "$Label query schema is not 1.3.0"
    Assert-Condition ([bool]$Catalog.Kinds.data.present) "$Label catalog is absent"
    Assert-Condition ([bool]$Catalog.Kinds.data.complete) "$Label catalog is incomplete"
    $activeConnectionId = [string]$Catalog.Kinds.data.active_connection_id
    Assert-Condition (-not [string]::IsNullOrEmpty($activeConnectionId)) "$Label has no active catalog connection"
    Assert-Condition (@($Catalog.Kinds.data.connection_ids).Count -eq 1 -and
        [string]$Catalog.Kinds.data.connection_ids[0] -eq $activeConnectionId) "$Label exposed stale catalog connections"
    $kindNames = @($Catalog.Kinds.data.kinds | ForEach-Object { [string]$_.kind })
    foreach ($required in $RequiredKinds)
    {
        Assert-Condition ($required -in $kindNames) "$Label missing catalog kind $required"
    }
    Assert-Condition (@($Catalog.Definitions.data.definitions).Count -ge $MinimumDefinitions) "$Label has too few definitions"
    Assert-Condition (@($Catalog.Entities.data.entities).Count -ge $MinimumEntities) "$Label has too few entities"
    Assert-Condition ([int]$Catalog.Quality.data.quality.invalid_count -eq 0) "$Label has invalid catalog records"
    Assert-Condition ([int]$Catalog.Quality.data.quality.privacy_violation_count -eq 0) "$Label has privacy violations"
    Assert-Condition ([int]$Catalog.Quality.data.quality.unresolved_entity_count -eq 0) "$Label has unresolved entities"
    Assert-Condition ([bool]$Catalog.Definition.data.definition.definition_key.StartsWith('jn-def:v1:')) "$Label catalog.get returned invalid key"

    $entityIds = @($Catalog.Entities.data.entities | ForEach-Object { [string]$_.entity_id })
    Assert-Condition (@($entityIds | Sort-Object -Unique).Count -eq $entityIds.Count) "$Label EntityId is not capture-local unique"
    foreach ($entity in @($Catalog.Entities.data.entities))
    {
        Assert-Condition ([string]$entity.connection_id -eq $activeConnectionId) "$Label entity belongs to a stale connection"
        Assert-Condition ([int]$entity.connection_generation -gt 0) "$Label EntityId has no connection generation"
        Assert-Condition ($null -eq $entity.PSObject.Properties['name']) "$Label entity illegally carries a dynamic name"
    }
    foreach ($definition in @($Catalog.Definitions.data.definitions))
    {
        Assert-Condition ([string]$definition.canonical_name -ne 'Harness.NoCatalogMarker') "$Label NoCatalog source leaked into catalog"
        $fileId = [string]$definition.source.file_id
        Assert-Condition (-not $fileId.Contains('\')) "$Label SourceFileId contains a backslash"
        Assert-Condition (-not ($fileId.Length -ge 2 -and $fileId[1] -eq ':')) "$Label SourceFileId is absolute"
    }
    $rawAppInfo = @($Catalog.AppInfo.data.app_info) -join "`n"
    foreach ($forbidden in @('C:\Users\', 'payload=private', 'account=', 'username=', 'authorization:', 'bearer '))
    {
        Assert-Condition (-not $rawAppInfo.Contains($forbidden)) "$Label AppInfo leaked forbidden text: $forbidden"
    }
    $catalogCapability = @($Catalog.Capabilities.data.domains | Where-Object { [string]$_.domain -eq 'catalog' })
    Assert-Condition ($catalogCapability.Count -eq 1 -and [bool]$catalogCapability[0].present) "$Label catalog capability is unavailable"
}

$script:OpenedIds = @()
try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:Process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-catalog-acceptance'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Notify-Initialized

    $snapshot = Read-Catalog $SnapshotTrace
    $stream = Read-Catalog $StreamTrace
    $replay = Read-Catalog $ReplayTrace
    $second = Read-Catalog $SecondSnapshotTrace
    Assert-Catalog $snapshot 'snapshot'
    Assert-Catalog $stream 'stream'
    Assert-Catalog $replay 'replay'
    Assert-Catalog $second 'second snapshot'

    $snapshotKeys = Definition-KeySet $snapshot
    $streamKeys = Definition-KeySet $stream
    $replayKeys = Definition-KeySet $replay
    $secondKeys = Definition-KeySet $second
    Assert-Condition (($snapshotKeys -join "`n") -eq ($streamKeys -join "`n")) 'snapshot and stream DefinitionKeys differ'
    Assert-Condition (($snapshotKeys -join "`n") -eq ($replayKeys -join "`n")) 'snapshot and replay DefinitionKeys differ'
    Assert-Condition (($snapshotKeys -join "`n") -eq ($secondKeys -join "`n")) 'DefinitionKeys are not stable across captures'

    $legacyPresent = $null
    if (-not [string]::IsNullOrEmpty($LegacyTrace))
    {
        $opened = Invoke-Tool 'tracy_trace_open' @{ path = $LegacyTrace }
        $legacyId = [string]$opened.data.trace_id
        $script:OpenedIds += $legacyId
        Wait-Ready $legacyId
        $legacyKinds = Inspect $legacyId 'catalog.kinds'
        Assert-Condition (-not [bool]$legacyKinds.data.present) 'legacy trace fabricated catalog presence'
        Assert-Condition (-not [bool]$legacyKinds.data.complete) 'legacy trace fabricated complete catalog'
        $legacyPresent = [bool]$legacyKinds.data.present
        Invoke-Tool 'tracy_trace_close' @{ trace_id = $legacyId } | Out-Null
        $script:OpenedIds = @($script:OpenedIds | Where-Object { $_ -ne $legacyId })
    }

    [ordered]@{
        status = 'passed'
        query_schema = [string]$snapshot.Kinds.schema_version
        definition_count = @($snapshot.Definitions.data.definitions).Count
        entity_count = @($snapshot.Entities.data.entities).Count
        definition_keys_stable = $true
        snapshot_stream_replay_equal = $true
        privacy_violation_count = [int]$snapshot.Quality.data.quality.privacy_violation_count
        unresolved_entity_count = [int]$snapshot.Quality.data.quality.unresolved_entity_count
        legacy_present = $legacyPresent
    } | ConvertTo-Json -Depth 10
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
