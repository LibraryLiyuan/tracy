[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $Trace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [string] $StreamTrace = '',
    [string] $OldTrace = '',
    [string] $PhysicalRef = '',
    [string] $Output = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Condition([bool] $Condition, [string] $Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $Trace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}
foreach ($path in @($StreamTrace, $OldTrace))
{
    if (-not [string]::IsNullOrWhiteSpace($path)) { Assert-Condition (Test-Path -LiteralPath $path) "optional trace does not exist: $path" }
}

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $QueryExe
$start.Arguments = "--mcp --indexed --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$start.WorkingDirectory = $AllowRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$script:NextId = 1

function Send-Notification([string] $Method, [hashtable] $Params = @{})
{
    $script:process.StandardInput.WriteLine((@{ jsonrpc='2.0'; method=$Method; params=$Params } | ConvertTo-Json -Compress -Depth 40))
    $script:process.StandardInput.Flush()
}

function Send-Rpc([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 120000)
{
    $requestId = $script:NextId++
    $script:process.StandardInput.WriteLine((@{ jsonrpc='2.0'; id=$requestId; method=$Method; params=$Params } | ConvertTo-Json -Compress -Depth 40))
    $script:process.StandardInput.Flush()
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $read = $script:process.StandardOutput.ReadLineAsync()
    Assert-Condition ($read.Wait($TimeoutMilliseconds)) "MCP response timed out: $Method"
    $watch.Stop()
    Assert-Condition ($null -ne $read.Result) "MCP stdout closed: $Method"
    $message = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$requestId) 'unexpected MCP response id'
    $message | Add-Member wall_ms $watch.ElapsedMilliseconds
    $message | Add-Member response_bytes ([Text.Encoding]::UTF8.GetByteCount($read.Result))
    return $message
}

function Invoke-Tool([string] $Name, [hashtable] $Arguments = @{}, [int] $TimeoutMilliseconds = 120000)
{
    $response = Send-Rpc 'tools/call' @{ name=$Name; arguments=$Arguments } $TimeoutMilliseconds
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) { throw "$Name failed: $($structured.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$structured.ok) "$Name omitted ok=true"
    $structured | Add-Member wall_ms $response.wall_ms
    $structured | Add-Member response_bytes $response.response_bytes
    return $structured
}

function Open-Trace([string] $Path)
{
    $openWatch = [Diagnostics.Stopwatch]::StartNew()
    $opened = Invoke-Tool 'tracy_trace_open' @{ path=$Path } 300000
    $traceId = [string]$opened.data.trace_id
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id=$traceId } 300000
        if ([string]$status.data.status.state -eq 'ready') { $openWatch.Stop(); return @{ id=$traceId; wall_ms=$openWatch.ElapsedMilliseconds } }
        if ([string]$status.data.status.state -in @('failed','closed')) { throw "trace entered $($status.data.status.state)" }
        Start-Sleep -Milliseconds 100
    }
    throw 'trace did not become ready'
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id=$TraceId; method=$Method; params=$Params } 300000
}

function Close-Trace([string] $TraceId)
{
    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id=$TraceId })
}

function Validate-ResourceTrace([string] $Path)
{
    $opened = Open-Trace $Path
    $traceId = [string]$opened.id
    try
    {
        $capability = Inspect $traceId 'resource.capability'
        Assert-Condition ([bool]$capability.data.present) 'resource capability is absent'
        $summary = Inspect $traceId 'resource.summary'
        Assert-Condition ([bool]$summary.data.complete) 'resource summary is incomplete'
        Assert-Condition ([UInt64]$summary.data.counts.objects -gt 0) 'resource summary has no objects'
        Assert-Condition ([UInt64]$summary.data.counts.assets -gt 0) 'resource summary has no assets'
        Assert-Condition ([UInt64]$summary.data.counts.dropped -eq 0) 'resource producer dropped data'

        $objects = Inspect $traceId 'object.search' @{ limit=100 }
        $assets = Inspect $traceId 'asset.search' @{ limit=1 }
        Assert-Condition (@($objects.data.objects).Count -gt 0) 'object.search pagination failed'
        Assert-Condition (@($assets.data.assets).Count -eq 1) 'asset.search pagination failed'
        $objectRef = [string]@($objects.data.objects)[0].ref
        $assetRef = [string]@($assets.data.assets)[0].ref
        Assert-Condition ($objectRef.StartsWith('object:')) 'object ref is not stable-id based'
        Assert-Condition ($assetRef.StartsWith('asset:')) 'asset ref is not stable-id based'

        $meshObject = @($objects.data.objects | Where-Object {
            @($_.parts).Count -gt 0 -or @($_.metadata | Where-Object { [int]$_.key -eq 72 -and [UInt64]$_.value -eq 1 }).Count -gt 0
        } | Select-Object -First 1)
        $textureObject = @($objects.data.objects | Where-Object {
            @($_.metadata | Where-Object { [int]$_.key -eq 72 -and [UInt64]$_.value -in @(2,4) }).Count -gt 0
        } | Select-Object -First 1)
        $probes = @(
            @{ method='object.get'; ref=$objectRef }, @{ method='asset.get'; ref=$assetRef },
            @{ method='resource.lifecycle'; ref=$objectRef }, @{ method='resource.relations'; ref=$objectRef },
            @{ method='resource.memory_impact'; ref=$objectRef }, @{ method='resource.gpu_usage'; ref=$objectRef; limit=100 },
            @{ method='resource.io_history'; ref=$objectRef }, @{ method='resource.evidence_path'; ref=$objectRef }
        )
        if ($meshObject.Count -gt 0) { $probes += @{ method='mesh.get'; ref=[string]$meshObject[0].ref } }
        if ($textureObject.Count -gt 0) { $probes += @{ method='texture.get'; ref=[string]$textureObject[0].ref } }

        $results = [ordered]@{}
        foreach ($item in $probes)
        {
            $params = @{ ref=$item.ref; max_nodes=2048; max_edges=4096; max_cpu_ms=60000; max_scan_events=5000000 }
            if ($item.ContainsKey('limit')) { $params.limit = [int]$item.limit }
            $value = Inspect $traceId $item.method $params
            Assert-Condition ([bool]$value.data.present) "$($item.method) returned present=false"
            Assert-Condition ($value.response_bytes -le 16MB) "$($item.method) exceeded 16 MiB"
            Assert-Condition ($value.wall_ms -le 5000) "$($item.method) exceeded 5 seconds"
            $results[$item.method] = @{ wall_ms=$value.wall_ms; response_bytes=$value.response_bytes; complete=[bool]$value.data.complete }
        }
        $physicalProbe = $null
        if (-not [string]::IsNullOrWhiteSpace($PhysicalRef))
        {
            $occupants = Inspect $traceId 'resource.physical.occupants' @{ ref=$PhysicalRef; limit=100; max_cpu_ms=60000 }
            Assert-Condition ([bool]$occupants.data.present) 'resource.physical.occupants returned present=false'
            Assert-Condition ([UInt64]$occupants.data.matched_count -gt 0) 'physical allocation has no logical occupants'
            Assert-Condition (@($occupants.data.occupants).Count -gt 0) 'physical occupants page is empty'
            Assert-Condition ($null -ne $occupants.page.next_cursor) 'high-fanout physical occupants omitted pagination cursor'
            $evidence = Inspect $traceId 'resource.evidence_path' @{ ref=$PhysicalRef; max_nodes=4096; max_edges=8192; max_cpu_ms=60000 }
            Assert-Condition ([bool]$evidence.data.complete) 'physical reverse evidence path is incomplete'
            Assert-Condition (-not [bool]$evidence.data.truncated) 'physical reverse evidence path is truncated'
            $assetCount = @($evidence.data.nodes | Where-Object { [int]$_.kind -eq 1 }).Count
            $objectCount = @($evidence.data.nodes | Where-Object { [int]$_.kind -eq 2 }).Count
            Assert-Condition ($assetCount -gt 0) 'physical reverse evidence path has no Asset origin'
            Assert-Condition ($objectCount -gt 0) 'physical reverse evidence path has no UnityObject origin'
            $physicalProbe = @{
                ref=$PhysicalRef; matched_occupants=[string]$occupants.data.matched_count;
                returned_occupants=@($occupants.data.occupants).Count; next_cursor_present=($null -ne $occupants.page.next_cursor);
                evidence_nodes=@($evidence.data.nodes).Count; evidence_edges=@($evidence.data.edges).Count;
                asset_nodes=$assetCount; object_nodes=$objectCount; complete=[bool]$evidence.data.complete
            }
        }
        $validation = Inspect $traceId 'resource.validation'
        Assert-Condition ([bool]$validation.data.complete) 'resource.validation failed'
        Assert-Condition ([UInt64]$validation.data.counts.unresolved_edges -eq 0) 'unresolved resource relation endpoint'
        Assert-Condition ([UInt64]$validation.data.counts.unresolved_ranges -eq 0) 'unresolved ResourcePart range target'
        return [ordered]@{
            path=$Path; fingerprint=[string]$summary.trace.fingerprint; open_wall_ms=$opened.wall_ms;
            counts=$summary.data.counts; object_ref=$objectRef; asset_ref=$assetRef; methods=$results;
            physical_probe=$physicalProbe; validation=$validation.data.counts
        }
    }
    finally { Close-Trace $traceId }
}

$active = ''
try
{
    Assert-Condition ($process.Start()) 'failed to start tracy-query MCP server'
    $script:process = $process
    $initialized = Send-Rpc 'initialize' @{ protocolVersion='2025-11-25'; capabilities=@{}; clientInfo=@{name='jn-n26-resource-acceptance';version='1.0'} }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $snapshot = Validate-ResourceTrace $Trace
    $stream = $null
    if (-not [string]::IsNullOrWhiteSpace($StreamTrace))
    {
        $stream = Validate-ResourceTrace $StreamTrace
        foreach ($field in @('assets','objects','parts','ranges','relations','dropped'))
        {
            Assert-Condition ([string]$snapshot.counts.$field -eq [string]$stream.counts.$field) "stream/snapshot count mismatch: $field"
        }
    }

    $old = $null
    if (-not [string]::IsNullOrWhiteSpace($OldTrace))
    {
        $opened = Open-Trace $OldTrace
        try
        {
            $oldCapability = Inspect $opened.id 'resource.capability'
            Assert-Condition (-not [bool]$oldCapability.data.present) 'old trace unexpectedly reports resource graph'
            Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$oldCapability.data.reason)) 'old trace omitted absence reason'
            $old = @{ path=$OldTrace; reason=[string]$oldCapability.data.reason }
        }
        finally { Close-Trace $opened.id }
    }

    $report = [ordered]@{ schema_version=1; stage='N26.WorkC'; passed=$true; generated_utc=[DateTime]::UtcNow.ToString('o'); snapshot=$snapshot; stream=$stream; old_trace=$old }
    $json = $report | ConvertTo-Json -Depth 30
    if (-not [string]::IsNullOrWhiteSpace($Output)) { [IO.File]::WriteAllText($Output,$json+[Environment]::NewLine,[Text.UTF8Encoding]::new($false)) }
    $json
}
finally
{
    if ($process -and -not $process.HasExited)
    {
        try { $process.StandardInput.Close() } catch {}
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    if ($process) { $process.Dispose() }
}
