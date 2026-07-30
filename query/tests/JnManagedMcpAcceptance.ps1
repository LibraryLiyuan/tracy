[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $QueryExe,

    [Parameter(Mandatory = $true)]
    [string] $SnapshotTrace,

    [Parameter(Mandatory = $true)]
    [string] $StreamTrace,

    [Parameter(Mandatory = $true)]
    [string] $AllowRoot
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

foreach ($requiredPath in @($QueryExe, $SnapshotTrace, $StreamTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
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
    $payload = [ordered]@{ jsonrpc = '2.0'; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc {
    param(
        [string] $Method,
        [hashtable] $Params = @{},
        [int] $TimeoutMilliseconds = 240000
    )
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $script:queryProcess.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) {
            throw "MCP response timed out: $Method (id=$id)"
        }
        $line = $read.Result
        if ($null -eq $line) {
            throw "MCP stdout closed before response: $Method (id=$id)"
        }
        $message = $line | ConvertFrom-Json
        if ($null -ne $message.PSObject.Properties['id']) {
            Assert-Condition ([string]$message.id -eq [string]$id) "unexpected response id $($message.id), expected $id"
            return $message
        }
    }
    throw "MCP response deadline expired: $Method (id=$id)"
}

function Invoke-McpTool {
    param([string] $Name, [hashtable] $Arguments = @{})
    $response = Send-Rpc -Method 'tools/call' -Params @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError) {
        throw "$Name returned an error: $($structured.error | ConvertTo-Json -Compress -Depth 20)"
    }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Wait-TraceReady {
    param([string] $TraceId)
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-McpTool -Name 'tracy_trace_status' -Arguments @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') {
            return $status
        }
        if ($state -in @('failed', 'closed')) {
            throw "trace $TraceId reached terminal state $state"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready"
}

function Inspect {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    return Invoke-McpTool -Name 'tracy_inspect' -Arguments @{
        trace_id = $TraceId
        method = $Method
        params = $Params
    }
}

function Zone-Statistics {
    param([string] $TraceId, [string] $Name)
    $result = Inspect -TraceId $TraceId -Method 'zone.cpu.statistics' -Params @{
        filter = @{ text = $Name; mode = 'exact' }
        limit = 20
    }
    Assert-Condition ([UInt64]$result.data.group_count -eq 1) "expected one source-location group for $Name"
    return @($result.data.groups)[0]
}

function Validate-ManagedData {
    param([string] $TraceId)

    $counts = Inspect -TraceId $TraceId -Method 'trace.counts'
    $appInfo = Inspect -TraceId $TraceId -Method 'trace.app_info'
    $direct = Zone-Statistics -TraceId $TraceId -Name 'JN.Direct/Managed.Validation'
    $callback = Zone-Statistics -TraceId $TraceId -Name 'JN.Managed.Callback.Validation'
    $dedupe = Zone-Statistics -TraceId $TraceId -Name 'JN.Direct/Managed.DuplicateFilter'
    $sources = Inspect -TraceId $TraceId -Method 'source.locations' -Params @{
        filter = @{ text = 'JN.Direct/Managed'; mode = 'prefix' }
        limit = 20
    }
    $allocationPlot = Inspect -TraceId $TraceId -Method 'plot.list' -Params @{
        filter = @{ text = 'JN.Managed.DirectHotPathAllocatedBytes'; mode = 'exact' }
        limit = 20
    }
    $heartbeat = Inspect -TraceId $TraceId -Method 'plot.list' -Params @{
        filter = @{ text = 'JN.Managed.Validation.Heartbeat'; mode = 'exact' }
        limit = 20
    }
    $messages = Inspect -TraceId $TraceId -Method 'message.search' -Params @{
        filter = @{ text = 'JN managed callback+direct validation active'; mode = 'exact' }
        limit = 20
    }
    $validation = Inspect -TraceId $TraceId -Method 'validation.run'

    Assert-Condition ([bool]$validation.data.valid) 'trace validation failed'
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) 'trace validation reported errors'
    Assert-Condition ([UInt64]$counts.data.cpu_zones -gt 0) 'trace contains no CPU zones'
    Assert-Condition ([UInt64]$direct.inclusive.count -eq 4460) 'direct zone count must cover warmup + allocation probe + bootstrap'
    Assert-Condition ([UInt64]$callback.inclusive.count -eq 300) 'callback marker count mismatch'
    Assert-Condition ([UInt64]$dedupe.inclusive.count -eq 300) 'DirectOnly filter failed or duplicated direct zones'

    Assert-Condition ([string]$direct.file -like '*PackageRepo\com.jngame.tracy\Editor\JNTracyManagedValidation.cs') 'direct source file is not the managed validation file'
    Assert-Condition ([string]$callback.file -eq 'UnityProfilerMarker') 'callback marker did not use the native profiler bridge source'

    $sourceValues = @($sources.data.source_locations)
    Assert-Condition ($sourceValues.Count -eq 2) 'expected exactly two managed direct source locations'
    foreach ($source in $sourceValues) {
        Assert-Condition (-not [bool]$source.dynamic) "managed source location unexpectedly became dynamic: $($source.name)"
        Assert-Condition ([string]$source.function -eq 'Run') "managed source function mismatch: $($source.name)"
        Assert-Condition ([UInt64]$source.line -gt 0) "managed source line is absent: $($source.name)"
    }

    $allocationPlots = @($allocationPlot.data.plots)
    Assert-Condition ($allocationPlots.Count -eq 1) 'managed allocation plot missing or duplicated'
    Assert-Condition ([UInt64]$allocationPlots[0].point_count -eq 1) 'managed allocation plot point count mismatch'
    Assert-Condition ([double]$allocationPlots[0].min -eq 0 -and [double]$allocationPlots[0].max -eq 0) 'managed direct hot path allocated bytes are non-zero'

    $heartbeatPlots = @($heartbeat.data.plots)
    Assert-Condition ($heartbeatPlots.Count -eq 1) 'managed heartbeat plot missing or duplicated'
    Assert-Condition ([UInt64]$heartbeatPlots[0].point_count -eq 4460) 'managed heartbeat point count mismatch'
    Assert-Condition (@($messages.data.messages).Count -eq 1) 'managed validation message missing or duplicated'

    $managedIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JN managed bridge ABI=*' })
    $validationIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JN managed validation source=*' })
    Assert-Condition ($managedIdentity.Count -eq 1) 'managed bridge AppInfo missing or duplicated'
    Assert-Condition ($validationIdentity.Count -eq 1) 'managed validation AppInfo missing or duplicated'
    Assert-Condition ($managedIdentity[0] -match 'ABI=0x00010000') 'managed AppInfo ABI mismatch'
    Assert-Condition ($managedIdentity[0] -match 'Config=0x8DAF4C01004D0005') 'managed AppInfo config mismatch'
    Assert-Condition ($managedIdentity[0] -match 'JNTracyClient.dll') 'managed AppInfo omitted native DLL path'

    return [pscustomobject]@{
        cpu_zones = [string]$counts.data.cpu_zones
        direct_count = [string]$direct.inclusive.count
        callback_count = [string]$callback.inclusive.count
        duplicate_filter_count = [string]$dedupe.inclusive.count
        direct_source = $sourceValues | Select-Object name, function, file, line, dynamic
        allocation_plot = $allocationPlots | Select-Object name, point_count, min, max
        heartbeat_plot = $heartbeatPlots | Select-Object name, point_count, min, max
        managed_app_info = [string]$managedIdentity[0]
        validation_app_info = [string]$validationIdentity[0]
        message_count = [string]@($messages.data.messages).Count
        validation_errors = [string]$validation.data.error_count
    }
}

$snapshotId = ''
$streamId = ''
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc -Method 'initialize' -Params @{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'jn-managed-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Send-Notification -Method 'notifications/initialized'

    $snapshotOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $SnapshotTrace }
    $snapshotId = [string]$snapshotOpen.data.trace_id
    $streamOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $snapshotStatus = Wait-TraceReady -TraceId $snapshotId
    $streamStatus = Wait-TraceReady -TraceId $streamId

    $snapshot = Validate-ManagedData -TraceId $snapshotId
    $stream = Validate-ManagedData -TraceId $streamId
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($stream | ConvertTo-Json -Compress -Depth 20)) 'snapshot/stream managed data mismatch'

    [pscustomobject]@{
        ok = $true
        snapshot_fingerprint = [string]$snapshotStatus.data.status.fingerprint
        stream_fingerprint = [string]$streamStatus.data.status.fingerprint
        cpu_zones = $snapshot.cpu_zones
        direct_count = $snapshot.direct_count
        callback_count = $snapshot.callback_count
        duplicate_filter_count = $snapshot.duplicate_filter_count
        managed_source_count = @($snapshot.direct_source).Count
        allocation_bytes = [string]@($snapshot.allocation_plot)[0].max
        heartbeat_points = [string]@($snapshot.heartbeat_plot)[0].point_count
        message_count = $snapshot.message_count
        validation_errors = $snapshot.validation_errors
        managed_app_info = $snapshot.managed_app_info
    } | ConvertTo-Json -Compress -Depth 20
}
finally {
    foreach ($traceId in @($snapshotId, $streamId)) {
        if (-not [string]::IsNullOrWhiteSpace($traceId) -and -not $queryProcess.HasExited) {
            try { [void](Invoke-McpTool -Name 'tracy_trace_close' -Arguments @{ trace_id = $traceId }) } catch {}
        }
    }
    if ($queryProcess -and -not $queryProcess.HasExited) {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000)) {
            $queryProcess.Kill($true)
            $queryProcess.WaitForExit()
        }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
