[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $ReconnectTrace,
    [Parameter(Mandatory = $true)][string] $TruncatedStream,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [string] $OutputPath = ''
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool] $Condition, [string] $Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $ReconnectTrace, $TruncatedStream, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$script:NextRequestId = 1
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$queryProcess = [Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo

function Send-Notification([string] $Method, [hashtable] $Params = @{})
{
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 240000)
{
    $id = $script:NextRequestId++
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $read = $script:queryProcess.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw "MCP response timed out: $Method" }
    if ($null -eq $read.Result) { throw "MCP stdout closed before response: $Method" }
    $message = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$message.id -eq [string]$id) 'unexpected MCP response id'
    return $message
}

function Invoke-Tool([string] $Name, [hashtable] $Arguments = @{})
{
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) { throw "$Name failed: $($structured.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
{
    return Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Open-Trace([string] $Path)
{
    $opened = Invoke-Tool 'tracy_trace_open' @{ path = $Path }
    $traceId = [string]$opened.data.trace_id
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-Tool 'tracy_trace_status' @{ trace_id = $traceId }
        if ([string]$status.data.status.state -eq 'ready') {
            return [ordered]@{ trace_id = $traceId; status = $status.data.status }
        }
        if ([string]$status.data.status.state -in @('failed', 'closed')) { throw "trace entered $($status.data.status.state)" }
        Start-Sleep -Milliseconds 250
    }
    throw 'trace did not become ready'
}

$openTraceIds = New-Object 'System.Collections.Generic.List[string]'
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n15-exception-gate'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $reconnect = Open-Trace $ReconnectTrace
    $openTraceIds.Add([string]$reconnect.trace_id)
    $catalog = (Inspect $reconnect.trace_id 'catalog.kinds').data
    Assert-Condition ([bool]$catalog.present -and [bool]$catalog.complete) 'reconnect catalog is incomplete'
    Assert-Condition ([UInt64]$catalog.active_connection_id -ge 2) 'reconnect did not advance the active connection ID'
    Assert-Condition (@($catalog.connection_ids).Count -eq 1) 'stale connection leaked into the active catalog'
    Assert-Condition ([string]$catalog.connection_ids[0] -eq [string]$catalog.active_connection_id) 'catalog exposes a non-active connection'
    $reconnectValidation = (Inspect $reconnect.trace_id 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }).data
    Assert-Condition ([bool]$reconnectValidation.valid -and [UInt64]$reconnectValidation.error_count -eq 0) 'reconnect trace validation failed'
    [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $reconnect.trace_id })
    [void]$openTraceIds.Remove([string]$reconnect.trace_id)

    $truncated = Open-Trace $TruncatedStream
    $openTraceIds.Add([string]$truncated.trace_id)
    Assert-Condition (-not [bool]$truncated.status.complete) 'truncated stream is marked complete'
    # The complete source trace already passed an unbounded production
    # validation.  Recovery validation is intentionally bounded: the gate is
    # that a large interrupted journal remains queryable and reports its tail
    # explicitly, not that the same 253 MiB prefix is rescanned a second time.
    $truncatedValidation = Inspect $truncated.trace_id 'validation.run' @{ max_cpu_ms = 5000; max_scan_events = 5000000 }
    Assert-Condition (-not [bool]$truncatedValidation.trace.complete) 'truncated trace envelope is marked complete'
    Assert-Condition (-not [bool]$truncatedValidation.data.complete) 'truncated validation is marked complete'
    $tailFindings = @($truncatedValidation.data.findings | Where-Object { [string]$_.code -eq 'TRUNCATED_STREAM_TAIL' })
    Assert-Condition ($tailFindings.Count -eq 1) 'truncated stream tail diagnostic is absent or duplicated'
    $exhaustedBy = @($truncatedValidation.budget.exhausted_by | ForEach-Object { [string]$_ })
    if ([bool]$truncatedValidation.partial) {
        Assert-Condition ($exhaustedBy.Count -gt 0) 'truncated validation is silently partial without an exhausted budget'
    }

    $report = [ordered]@{
        ok = $true
        schema_version = 1
        stage = 'N15'
        gate = 'exception-reconnect'
        reconnect = [ordered]@{
            trace = (Resolve-Path -LiteralPath $ReconnectTrace).Path
            active_connection_id = [string]$catalog.active_connection_id
            connection_ids = @($catalog.connection_ids)
            catalog_complete = [bool]$catalog.complete
            validation_errors = [string]$reconnectValidation.error_count
        }
        truncated_stream = [ordered]@{
            trace = (Resolve-Path -LiteralPath $TruncatedStream).Path
            complete = [bool]$truncated.status.complete
            tail_finding_count = $tailFindings.Count
            validation_complete = [bool]$truncatedValidation.data.complete
            validation_partial = [bool]$truncatedValidation.partial
            validation_exhausted_by = $exhaustedBy
            validation_omitted_count = $truncatedValidation.omitted_count
        }
    }
    $json = $report | ConvertTo-Json -Depth 30
    if (-not [string]::IsNullOrEmpty($OutputPath)) {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrEmpty($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        [IO.File]::WriteAllText($OutputPath, $json, [Text.UTF8Encoding]::new($false))
    }
    $json
}
finally {
    foreach ($traceId in $openTraceIds.ToArray()) {
        if ($queryProcess -and -not $queryProcess.HasExited) { try { [void](Invoke-Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) { $queryProcess.Kill($true); $queryProcess.WaitForExit() }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
