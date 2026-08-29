[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$Trace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [string]$OutputJson,
    [string]$ExpectedFrame
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $Trace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
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
$startInfo.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo

function Send-Rpc([string]$Method, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 30000)
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
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    if ($null -ne $message.error) { throw "MCP protocol error: $($message.error | ConvertTo-Json -Compress -Depth 20)" }
    return $message
}

function Invoke-Tool([string]$Name, [hashtable]$Arguments = @{}, [int]$TimeoutMilliseconds = 30000)
{
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments } $TimeoutMilliseconds
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    if ([bool]$response.result.isError)
    {
        throw "$Name failed: $($response.result.structuredContent | ConvertTo-Json -Compress -Depth 30)"
    }
    return $response.result.structuredContent
}

function Inspect([string]$TraceId, [string]$Method, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 30000)
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params } $TimeoutMilliseconds
}

function Measure-Call([scriptblock]$Call)
{
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $value = & $Call
    $watch.Stop()
    return [pscustomobject]@{ Value = $value; Milliseconds = $watch.Elapsed.TotalMilliseconds }
}

$script:Process = $process
$script:TraceId = $null
$result = $null

try
{
    Assert-Condition $process.Start() 'failed to start tracy-query MCP process'
    $initialized = Send-Rpc 'initialize' @{
        protocolVersion = '2025-11-25'
        clientInfo = @{ name = 'n29-mcp-acceptance'; version = '1' }
        capabilities = @{}
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize version mismatch'
    $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}')
    $process.StandardInput.Flush()

    $opened = Measure-Call { Invoke-Tool 'tracy_trace_open' @{ path = $Trace } }
    $script:TraceId = [string]$opened.Value.data.trace_id
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($script:TraceId)) 'trace.open returned no trace id'

    $readyWatch = [Diagnostics.Stopwatch]::StartNew()
    do
    {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $script:TraceId }
        $state = [string]$status.data.status.state
        if ($state -in @('failed', 'closed')) { throw "trace reached $state" }
        if ($state -ne 'ready') { Start-Sleep -Milliseconds 50 }
    } while ($state -ne 'ready' -and $readyWatch.Elapsed.TotalSeconds -lt 10)
    $readyWatch.Stop()
    Assert-Condition ($state -eq 'ready') 'sidecar trace did not become ready within 10 seconds'

    $catalog = Measure-Call { Inspect $script:TraceId 'gpu.catalog.status' }
    Assert-Condition ([bool]$catalog.Value.ok) 'gpu.catalog.status failed'
    Assert-Condition ([string]$catalog.Value.data.status -in @('complete', 'partial')) 'GPU analysis sidecar status is not usable'
    Assert-Condition ([UInt64]$catalog.Value.data.counts.resources -gt 0) 'sidecar contains no GPU resources'
    Assert-Condition ([UInt64]$catalog.Value.data.counts.allocations -gt 0) 'sidecar contains no GPU allocations'

    $search = Measure-Call { Inspect $script:TraceId 'gpu.resource.search' @{ offset = 0; limit = 1 } }
    Assert-Condition ([bool]$search.Value.ok) 'gpu.resource.search failed'
    Assert-Condition ($search.Value.data.resources.Count -eq 1) 'gpu.resource.search returned no first resource'
    $resourceId = [UInt64]$search.Value.data.resources[0].resource_id

    $explain = Measure-Call { Inspect $script:TraceId 'gpu.resource.explain' @{ resource_id = $resourceId; offset = 0; limit = 64 } }
    Assert-Condition ([bool]$explain.Value.ok) 'gpu.resource.explain failed'
    Assert-Condition ([UInt64]$explain.Value.data.resource_id -eq $resourceId) 'resource explain id mismatch'
    Assert-Condition ($null -ne $explain.Value.data.exactness) 'resource explain omitted exactness'

    $frameLookup = $null
    $passLookup = $null
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFrame))
    {
        [UInt64]$expectedFrameId = 0
        Assert-Condition ([UInt64]::TryParse($ExpectedFrame, [ref]$expectedFrameId)) 'ExpectedFrame must be an unsigned 64-bit integer'
        $frameLookup = Measure-Call { Inspect $script:TraceId 'gpu.pass.by_frame' @{ frame_id = $ExpectedFrame; offset = 0; limit = 64 } }
        Assert-Condition ([bool]$frameLookup.Value.ok) 'gpu.pass.by_frame failed'
        Assert-Condition ($frameLookup.Value.data.passes.Count -gt 0) 'gpu.pass.by_frame returned no passes'
        Assert-Condition ([UInt64]$frameLookup.Value.data.passes[0].frame_id -eq $expectedFrameId) 'frame-to-pass result returned the wrong sparse frame id'
        $resourcePass = $frameLookup.Value.data.passes | Where-Object { [UInt64]$_.direct_resource_count -gt 0 } | Select-Object -First 1
        Assert-Condition ($null -ne $resourcePass) 'frame contains no pass with direct resources in the returned page'
        $passId = [UInt64]$resourcePass.pass_id
        $passLookup = Measure-Call { Inspect $script:TraceId 'gpu.pass.resources' @{ pass_id = $passId; offset = 0; limit = 64 } }
        Assert-Condition ([bool]$passLookup.Value.ok) 'gpu.pass.resources failed after frame discovery'
        Assert-Condition ([UInt64]$passLookup.Value.data.resource_count -gt 0) 'discovered pass has no resources'
        Assert-Condition ($frameLookup.Milliseconds -le 2000) 'gpu.pass.by_frame exceeded 2 seconds'
        Assert-Condition ($passLookup.Milliseconds -le 2000) 'gpu.pass.resources exceeded 2 seconds'
    }

    $process.Refresh()
    $privateBytes = [UInt64]$process.PrivateMemorySize64
    $workingSetBytes = [UInt64]$process.WorkingSet64
    Assert-Condition ($privateBytes -lt 4GB) 'MCP GPU-only fast path exceeded 4 GiB private bytes and likely loaded the full Worker'
    Assert-Condition ($catalog.Milliseconds -le 2000) 'gpu.catalog.status exceeded 2 seconds'
    Assert-Condition ($explain.Milliseconds -le 2000) 'gpu.resource.explain exceeded 2 seconds'

    $result = [ordered]@{
        gate = 'N29-MCP-Sidecar-FastPath'
        status = 'Passed'
        trace = $Trace
        trace_id = $script:TraceId
        timings_ms = [ordered]@{
            trace_open = [Math]::Round($opened.Milliseconds, 3)
            trace_ready = [Math]::Round($readyWatch.Elapsed.TotalMilliseconds, 3)
            catalog_status = [Math]::Round($catalog.Milliseconds, 3)
            resource_search = [Math]::Round($search.Milliseconds, 3)
            resource_explain = [Math]::Round($explain.Milliseconds, 3)
            pass_by_frame = if ($frameLookup) { [Math]::Round($frameLookup.Milliseconds, 3) } else { $null }
            pass_resources = if ($passLookup) { [Math]::Round($passLookup.Milliseconds, 3) } else { $null }
        }
        process_memory = [ordered]@{ private_bytes = $privateBytes; working_set_bytes = $workingSetBytes }
        summary = $catalog.Value.data.analysis_sidecar.summary
        counts = $catalog.Value.data.counts
        sample_resource_id = $resourceId
        sparse_frame_id = if ($frameLookup) { $ExpectedFrame } else { $null }
        sample_pass_id = if ($frameLookup) { [string]$resourcePass.pass_id } else { $null }
    }
}
finally
{
    if ($process -and -not $process.HasExited)
    {
        if ($script:TraceId)
        {
            try { Invoke-Tool 'tracy_trace_close' @{ trace_id = $script:TraceId } | Out-Null } catch {}
        }
        try { $process.StandardInput.Close() } catch {}
        if (-not $process.WaitForExit(2000)) { $process.Kill($true) }
    }
}

if (-not [string]::IsNullOrWhiteSpace($OutputJson))
{
    $parent = Split-Path -Parent $OutputJson
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $OutputJson -Encoding utf8
}
$result | ConvertTo-Json -Depth 30
