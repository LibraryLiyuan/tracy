[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$Trace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile,
    [string]$StreamTrace = '',
    [UInt64]$ExpectedLogicalResourceCount = 6209,
    [UInt64]$ExpectedGpuPassCount = 997075,
    [UInt32]$ExpectedOwnerRollupCount = 18
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }
foreach ($path in @($QueryExe, $Trace, $AllowRoot)) { Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path" }
if ($StreamTrace) { Assert-Condition (Test-Path -LiteralPath $StreamTrace) "stream trace does not exist: $StreamTrace" }

$script:NextRequestId = 1
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --indexed --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
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
function Invoke-ToolRaw([string]$Name, [hashtable]$Arguments = @{})
{
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    return $response.result
}
function Invoke-Tool([string]$Name, [hashtable]$Arguments = @{})
{
    $result = Invoke-ToolRaw $Name $Arguments
    if ([bool]$result.isError) { throw "$Name failed: $($result.structuredContent | ConvertTo-Json -Compress -Depth 40)" }
    Assert-Condition ([bool]$result.structuredContent.ok) "$Name omitted structuredContent.ok=true"
    return $result.structuredContent
}
function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    $arguments = @{ method = $Method; params = $Params }
    if ($TraceId) { $arguments.trace_id = $TraceId }
    return Invoke-Tool 'tracy_inspect' $arguments
}
function Measure-Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{})
{
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $response = Inspect $TraceId $Method $Params
    $watch.Stop()
    return [ordered]@{ elapsed_ms = $watch.ElapsedMilliseconds; response = $response }
}

if (-not $process.Start()) { throw 'failed to start tracy-query MCP server' }
$script:Process = $process
try
{
    $initialize = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; clientInfo = @{ name = 'jn-query-index-n16'; version = '1' }; capabilities = @{} }
    Assert-Condition ([string]$initialize.result.protocolVersion -eq '2025-11-25') 'MCP protocol version mismatch'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $openWatch = [Diagnostics.Stopwatch]::StartNew()
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Trace }
    $traceId = [string]$opened.data.trace_id
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    do
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $traceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { break }
        if ($state -in @('failed', 'closed')) { throw "indexed trace reached $state" }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    $openWatch.Stop()
    Assert-Condition ($state -eq 'ready') 'indexed trace did not become ready'
    Assert-Condition ($openWatch.ElapsedMilliseconds -le 5000) "indexed open exceeded 5 seconds: $($openWatch.ElapsedMilliseconds) ms"

    $overview = Measure-Inspect $traceId 'trace.overview'
    Assert-Condition ($overview.elapsed_ms -le 2000) "trace.overview exceeded 2 seconds: $($overview.elapsed_ms) ms"
    $capabilities = Measure-Inspect $traceId 'system.capabilities'
    Assert-Condition ($capabilities.elapsed_ms -le 2000) "system.capabilities exceeded 2 seconds: $($capabilities.elapsed_ms) ms"
    $cpuCapability = @($capabilities.response.data.domains | Where-Object { $_.domain -eq 'zone.cpu' })[0]
    $gpuCapability = @($capabilities.response.data.domains | Where-Object { $_.domain -eq 'zone.gpu' })[0]
    Assert-Condition ($null -ne $cpuCapability -and [bool]$cpuCapability.queryable -and [bool]$cpuCapability.indexed) 'CPU zone sidecar capability is not queryable/indexed'
    Assert-Condition ($null -ne $gpuCapability -and [bool]$gpuCapability.queryable -and [bool]$gpuCapability.indexed) 'GPU zone sidecar capability is not queryable/indexed'

    $first = Inspect $traceId 'zone.cpu.search' @{ limit = 100; max_scan_events = 64; fields = @('ref','name','start_ns','end_ns','self_time_ns','parent_ref') }
    Assert-Condition ([bool]$first.partial -and @($first.data.zones).Count -eq 64 -and $null -ne $first.page.next_cursor) 'bounded CPU page did not return resumable 64-record prefix'
    $second = Inspect $traceId 'zone.cpu.search' @{ limit = 100; max_scan_events = 256; cursor = [string]$first.page.next_cursor; fields = @('ref','name','start_ns','end_ns','self_time_ns','parent_ref') }
    Assert-Condition (-not [bool]$second.partial -and @($second.data.zones).Count -eq 100) 'CPU cursor continuation did not return a complete page'
    $refs = @($first.data.zones.ref) + @($second.data.zones.ref)
    Assert-Condition (($refs | Select-Object -Unique).Count -eq 164) 'CPU pagination duplicated or omitted stable refs'

    $gpu = Measure-Inspect $traceId 'zone.gpu.search' @{ limit = 1000; fields = @('ref','name','gpu_start_ns','gpu_end_ns','self_time_ns','parent_ref','query_id') }
    Assert-Condition ($gpu.elapsed_ms -le 2000 -and @($gpu.response.data.zones).Count -eq 1000) 'GPU zone domain query failed the 2-second/1000-record gate'

    $relations = Measure-Inspect $traceId 'relation.search' @{ limit = 1000 }
    Assert-Condition ($relations.elapsed_ms -le 5000) "relation.search exceeded 5 seconds: $($relations.elapsed_ms) ms"
    Assert-Condition (@($relations.response.data.relations).Count -gt 0) 'relation.search returned no relations'

    $gpuMemorySummary = Measure-Inspect $traceId 'memory.gpu.summary'
    Assert-Condition ($gpuMemorySummary.elapsed_ms -le 2000) "memory.gpu.summary exceeded 2 seconds: $($gpuMemorySummary.elapsed_ms) ms"
    Assert-Condition ([bool]$gpuMemorySummary.response.data.present -and [UInt64]$gpuMemorySummary.response.data.logical_resource_count -eq $ExpectedLogicalResourceCount) 'indexed GPU-memory summary is absent or has the wrong logical-resource count'
    $gpuRequestScopes = Measure-Inspect $traceId 'memory.gpu.request_scopes' @{ limit = 100 }
    Assert-Condition ($gpuRequestScopes.elapsed_ms -le 2000 -and @($gpuRequestScopes.response.data.scopes).Count -eq 100) 'GPU request-scope page failed the 2-second/100-record gate'
    $gpuAllocations = Measure-Inspect $traceId 'memory.gpu.allocations' @{ limit = 100 }
    Assert-Condition ($gpuAllocations.elapsed_ms -le 2000 -and @($gpuAllocations.response.data.allocations).Count -eq 100) 'GPU allocation page failed the 2-second/100-record gate'
    Assert-Condition ($null -ne $gpuAllocations.response.data.allocations[0].origin -and $null -ne $gpuAllocations.response.data.allocations[0].logical_resource) 'GPU allocation page omitted origin or logical-resource metadata'
    $gpuAttribution = Measure-Inspect $traceId 'memory.gpu.attribution' @{ limit = 100 }
    Assert-Condition ($gpuAttribution.elapsed_ms -le 5000 -and @($gpuAttribution.response.data.allocations).Count -eq 100) 'GPU attribution page failed the 5-second/100-record gate'
    Assert-Condition ([UInt64]$gpuAttribution.response.data.pass_count -eq $ExpectedGpuPassCount -and [UInt64]$gpuAttribution.response.data.logical_resource_count -eq $ExpectedLogicalResourceCount -and @($gpuAttribution.response.data.owner_rollups).Count -eq $ExpectedOwnerRollupCount) 'GPU attribution aggregate counts are inconsistent'

    $validation = Measure-Inspect $traceId 'validation.run' @{ max_scan_events = 100000000; max_cpu_ms = 60000 }
    Assert-Condition ($validation.elapsed_ms -le 60000) "validation.run exceeded 60 seconds: $($validation.elapsed_ms) ms"
    Assert-Condition (-not [bool]$validation.response.partial) 'validation.run exhausted its budget'

    $cancelWatch = [Diagnostics.Stopwatch]::StartNew()
    $submitted = Invoke-Tool 'tracy_validate' @{ trace_id = $traceId; async = $true; max_scan_events = 100000000; max_cpu_ms = 60000 }
    $jobId = [string]$submitted.data.job_id
    Assert-Condition (-not [string]::IsNullOrEmpty($jobId)) 'async validation did not return job_id'
    $cancelled = Invoke-Tool 'tracy_job' @{ job_id = $jobId; operation = 'cancel' }
    Assert-Condition ([bool]$cancelled.data.cancel_requested) 'cancellation request was not accepted'
    $cancelDeadline = [DateTime]::UtcNow.AddSeconds(2)
    do
    {
        $jobStatus = Invoke-Tool 'tracy_job' @{ job_id = $jobId; operation = 'status' }
        if ([bool]$jobStatus.data.done) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $cancelDeadline)
    $cancelWatch.Stop()
    Assert-Condition ([bool]$jobStatus.data.done -and [string]$jobStatus.data.state -eq 'cancelled') 'async validation was not cancelled within 2 seconds'
    Assert-Condition ($cancelWatch.ElapsedMilliseconds -le 2000) "cancellation exceeded 2 seconds: $($cancelWatch.ElapsedMilliseconds) ms"

    $responseBytes = [Text.Encoding]::UTF8.GetByteCount(($gpu.response | ConvertTo-Json -Compress -Depth 80))
    Assert-Condition ($responseBytes -le 16MB) "single MCP response exceeded 16 MiB: $responseBytes"
    $process.Refresh()
    $peakWorkingSetBytes = $process.PeakWorkingSet64
    $privateBytes = $process.PrivateMemorySize64
    Assert-Condition ($peakWorkingSetBytes -le 2GB) "MCP peak working set exceeded 2 GiB: $peakWorkingSetBytes"

    $streamResult = $null
    if ($StreamTrace)
    {
        [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId })
        $streamOpenWatch = [Diagnostics.Stopwatch]::StartNew()
        $streamOpened = Invoke-Tool 'tracy_trace_open' @{ path = $StreamTrace }
        $streamId = [string]$streamOpened.data.trace_id
        $streamDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do
        {
            $streamStatus = Invoke-Tool 'tracy_trace_status' @{ trace_id = $streamId }
            $streamState = [string]$streamStatus.data.status.state
            if ($streamState -eq 'ready') { break }
            if ($streamState -in @('failed', 'closed')) { throw "indexed stream reached $streamState" }
            Start-Sleep -Milliseconds 25
        } while ([DateTime]::UtcNow -lt $streamDeadline)
        $streamOpenWatch.Stop()
        Assert-Condition ($streamState -eq 'ready' -and $streamOpenWatch.ElapsedMilliseconds -le 5000) "cached indexed stream open exceeded 5 seconds: $($streamOpenWatch.ElapsedMilliseconds) ms"
        Assert-Condition ([string]$streamStatus.data.status.source_kind -eq 'segment' -and [UInt64]$streamStatus.data.status.revision -gt 0) 'stream source kind or committed revision is invalid'
        $streamSummary = Measure-Inspect $streamId 'memory.gpu.summary'
        Assert-Condition ($streamSummary.elapsed_ms -le 2000) "stream memory.gpu.summary exceeded 2 seconds: $($streamSummary.elapsed_ms) ms"
        Assert-Condition ([string]$streamSummary.response.data.logical_resource_count -eq [string]$gpuMemorySummary.response.data.logical_resource_count -and [string]$streamSummary.response.data.working_set_count -eq [string]$gpuMemorySummary.response.data.working_set_count) 'stream/snapshot GPU-memory counts differ'
        $streamResult = [ordered]@{ path = (Get-Item -LiteralPath $StreamTrace).FullName; trace_id = $streamId; revision = [string]$streamStatus.data.status.revision; open_ms = $streamOpenWatch.ElapsedMilliseconds; summary_ms = $streamSummary.elapsed_ms }
    }

    $result = [ordered]@{
        passed = $true
        trace = [ordered]@{ path = (Get-Item -LiteralPath $Trace).FullName; bytes = (Get-Item -LiteralPath $Trace).Length; sha256 = (Get-FileHash -LiteralPath $Trace -Algorithm SHA256).Hash.ToLowerInvariant(); trace_id = $traceId }
        timings_ms = [ordered]@{ indexed_open = $openWatch.ElapsedMilliseconds; overview = $overview.elapsed_ms; capabilities = $capabilities.elapsed_ms; gpu_domain_1000 = $gpu.elapsed_ms; relation_1000 = $relations.elapsed_ms; gpu_memory_summary = $gpuMemorySummary.elapsed_ms; gpu_request_scopes_100 = $gpuRequestScopes.elapsed_ms; gpu_allocations_100 = $gpuAllocations.elapsed_ms; gpu_attribution_100 = $gpuAttribution.elapsed_ms; validation = $validation.elapsed_ms; cancellation = $cancelWatch.ElapsedMilliseconds }
        counts = [ordered]@{ cpu_zones = [string]$overview.response.data.trace.counts.cpu_zones; gpu_zones = [string]$overview.response.data.trace.counts.gpu_zones; relations = [string]$overview.response.data.trace.counts.relations; io_requests = [string]$overview.response.data.trace.counts.io_requests }
        expected_gpu_memory_counts = [ordered]@{ logical_resources = [string]$ExpectedLogicalResourceCount; passes = [string]$ExpectedGpuPassCount; owner_rollups = [string]$ExpectedOwnerRollupCount }
        pagination = [ordered]@{ first = @($first.data.zones).Count; second = @($second.data.zones).Count; unique = ($refs | Select-Object -Unique).Count; first_partial = [bool]$first.partial; second_partial = [bool]$second.partial }
        cancellation = [ordered]@{ job_id = $jobId; state = [string]$jobStatus.data.state }
        response_bytes = $responseBytes
        memory = [ordered]@{ peak_working_set_bytes = $peakWorkingSetBytes; final_private_bytes = $privateBytes }
        capabilities = [ordered]@{ cpu = $cpuCapability; gpu = $gpuCapability }
        stream = $streamResult
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $result | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $OutputFile -Encoding UTF8
    $result | ConvertTo-Json -Compress -Depth 12
}
finally
{
    if ($script:Process -and -not $script:Process.HasExited)
    {
        try { $script:Process.StandardInput.Close() } catch {}
        if (-not $script:Process.WaitForExit(2000)) { $script:Process.Kill($true) }
    }
}
