[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $QueryExe,

    [Parameter(Mandatory = $true)]
    [string] $SnapshotTrace,

    [Parameter(Mandatory = $true)]
    [string] $StreamTrace,

    [Parameter(Mandatory = $true)]
    [string] $AllowRoot,

    [switch] $RequireLogical
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
$startInfo.Arguments = "--mcp --allow-root `"$AllowRoot`""
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

$expectedPlots = @(
    'GPU.VRAM.EngineKnownPhysical.LocalBytes',
    'GPU.VRAM.EngineKnownPhysical.NonLocalBytes',
    'GPU.VRAM.EngineKnownPhysical.LiveAllocations',
    'GPU.VRAM.Diagnostics.DuplicateAllocations',
    'GPU.VRAM.Diagnostics.UnknownFrees',
    'GPU.DXGI.Local.UsageBytes',
    'GPU.DXGI.Local.BudgetBytes',
    'GPU.DXGI.Local.ReservationBytes',
    'GPU.DXGI.Local.AvailableForReservationBytes',
    'GPU.DXGI.NonLocal.UsageBytes',
    'GPU.DXGI.NonLocal.BudgetBytes',
    'GPU.DXGI.NonLocal.ReservationBytes',
    'GPU.DXGI.NonLocal.AvailableForReservationBytes'
)

$expectedPools = @(
    'GPU D3D12 Physical Local Committed',
    'GPU D3D12 Physical Local Heap',
    'GPU D3D12 Physical NonLocal Committed',
    'GPU D3D12 Physical NonLocal Heap'
)

function Validate-GpuMemoryData {
    param([string] $TraceId)

    $counts = Inspect -TraceId $TraceId -Method 'trace.counts'
    $appInfo = Inspect -TraceId $TraceId -Method 'trace.app_info'
    $plots = Inspect -TraceId $TraceId -Method 'plot.list' -Params @{
        filter = @{ text = 'GPU.'; mode = 'prefix' }
        limit = 100
    }
    $pools = Inspect -TraceId $TraceId -Method 'memory.gpu.pools' -Params @{ limit = 100 }
    $validation = Inspect -TraceId $TraceId -Method 'validation.run'
    $logical = $null

    Assert-Condition ([UInt64]$counts.data.memory_events -gt 0) 'no GPU memory events'
    Assert-Condition ([bool]$validation.data.valid) 'trace validation failed'
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) 'trace validation reported errors'

    $dxgiInfo = @($appInfo.data.app_info | Where-Object { $_ -like 'JNTracy.DXGI*' })
    Assert-Condition ($dxgiInfo.Count -eq 1) 'expected exactly one JNTracy.DXGI AppInfo record'
    Assert-Condition ($dxgiInfo[0] -match 'local_hr=0x00000000') 'DXGI local query failed'
    Assert-Condition ($dxgiInfo[0] -match 'nonlocal_hr=0x00000000') 'DXGI non-local query failed'

    $actualPlots = @($plots.data.plots | Sort-Object name)
    Assert-Condition ($actualPlots.Count -ge $expectedPlots.Count) "expected at least $($expectedPlots.Count) GPU plots, got $($actualPlots.Count)"
    foreach ($name in $expectedPlots) {
        $plot = @($actualPlots | Where-Object { $_.name -eq $name })
        Assert-Condition ($plot.Count -eq 1) "missing or duplicate plot: $name"
        Assert-Condition ([UInt64]$plot[0].point_count -gt 0) "plot has no points: $name"
    }
    foreach ($name in @('GPU.VRAM.Diagnostics.DuplicateAllocations', 'GPU.VRAM.Diagnostics.UnknownFrees')) {
        $plot = @($actualPlots | Where-Object { $_.name -eq $name })[0]
        Assert-Condition ([double]$plot.max -eq 0) "$name is non-zero"
    }

    $actualPools = @($pools.data.pools | Sort-Object name)
    Assert-Condition ($actualPools.Count -ge $expectedPools.Count) "expected at least $($expectedPools.Count) GPU pools, got $($actualPools.Count)"
    foreach ($name in $expectedPools) {
        $pool = @($actualPools | Where-Object { $_.name -eq $name })
        Assert-Condition ($pool.Count -eq 1) "missing or duplicate pool: $name"
        Assert-Condition ([UInt64]$pool[0].event_count -gt 0) "pool has no events: $name"
    }

    if ($RequireLogical) {
        $attribution = Inspect -TraceId $TraceId -Method 'memory.gpu.attribution' -Params @{ limit = 20 }
        $ownedAndUsed = Inspect -TraceId $TraceId -Method 'memory.gpu.allocations' -Params @{
            relation_state = 'request_and_uses'
            limit = 100
        }
        $passes = Inspect -TraceId $TraceId -Method 'memory.gpu.pass_uses' -Params @{ limit = 100 }

        Assert-Condition ([bool]$attribution.data.protocol_present) 'GTMEM1 protocol is absent'
        Assert-Condition ([bool]$attribution.data.complete) 'GPU memory attribution is incomplete'
        Assert-Condition (@($attribution.data.warnings).Count -eq 0) 'GPU memory attribution reported warnings'
        Assert-Condition ([UInt64]$attribution.data.logical_resource_count -gt 0) 'no logical GPU resources'
        Assert-Condition ([UInt64]$attribution.data.pass_count -gt 0) 'no GPU memory reference passes'
        Assert-Condition (@($attribution.data.owner_rollups).Count -gt 0) 'no GPU owner rollups'
        Assert-Condition (@($attribution.data.working_sets).Count -gt 0) 'no GPU working sets'

        $vsmTaxonomyId = [UInt64]537067522
        $vsmOwner = @($attribution.data.owner_rollups | Where-Object { [UInt64]$_.taxonomy_id -eq $vsmTaxonomyId })
        Assert-Condition ($vsmOwner.Count -eq 1) 'expected one VSM owner rollup'
        Assert-Condition ([UInt64]$vsmOwner[0].owned_physical_bytes -gt 0) 'VSM owner has no physical bytes'
        Assert-Condition ([UInt64]$vsmOwner[0].physical_allocation_count -gt 0) 'VSM owner has no physical allocations'

        $workingSets = @($attribution.data.working_sets)
        Assert-Condition (@($workingSets | Where-Object { [UInt64]$_.taxonomy_id -eq $vsmTaxonomyId }).Count -gt 0) 'VSM has no referenced working set'
        Assert-Condition (@($workingSets | Where-Object { $id = [UInt64]$_.taxonomy_id; $id -ge 0x10000000 -and $id -lt 0x20000000 }).Count -gt 1) 'expected multiple L1 working sets'
        Assert-Condition (@($workingSets | Where-Object { $id = [UInt64]$_.taxonomy_id; $id -ge 0x20000000 -and $id -lt 0x30000000 }).Count -gt 1) 'expected multiple L2 working sets'

        $vsmAllocations = @($ownedAndUsed.data.allocations | Where-Object {
            $_.logical_resource.name -eq 'JN Tracy VSM Physical Pages Validation Target' -and
            [UInt64]$_.logical_resource.primary_owner_id -eq $vsmTaxonomyId
        })
        Assert-Condition ($vsmAllocations.Count -eq 1) 'expected one owned-and-used VSM validation resource'
        Assert-Condition ([UInt64]$vsmAllocations[0].pass_ref_count -gt 0) 'VSM validation resource has no pass references'

        $passValues = @($passes.data.passes)
        Assert-Condition ($passValues.Count -gt 0) 'GPU pass query returned no rows'
        foreach ($pass in $passValues) {
            Assert-Condition ([bool]$pass.complete) "incomplete GPU memory pass: $($pass.name)"
            Assert-Condition ([string]$pass.gpu_pairing -eq 'exact') "non-exact GPU zone pairing: $($pass.name)"
            Assert-Condition (-not [bool]$pass.truncated) "truncated GPU memory pass: $($pass.name)"
            Assert-Condition ([UInt64]$pass.dropped_uses -eq 0) "dropped uses in GPU memory pass: $($pass.name)"
        }

        $logical = [pscustomobject]@{
            logical_resource_count = [string]$attribution.data.logical_resource_count
            pass_count = [string]$attribution.data.pass_count
            owner_rollups = @($attribution.data.owner_rollups)
            working_sets = $workingSets
            owned_and_used = @($vsmAllocations | Select-Object allocation_id, request_label_id, pass_ref_count, logical_resource)
            pass_preview = @($passValues | Select-Object name, taxonomy_id, frame, complete, gpu_pairing, total_use_count, dropped_uses, truncated)
        }
    }

    return [pscustomobject]@{
        memory_events = [string]$counts.data.memory_events
        persisted_plot_events = [string]$counts.data.plots
        dxgi_app_info = [string]$dxgiInfo[0]
        plots = @($actualPlots | Select-Object name, point_count, min, max)
        pools = @($actualPools | Select-Object name, event_count, active_count, active_bytes, free_count)
        validation_errors = [string]$validation.data.error_count
        logical = $logical
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
        clientInfo = @{ name = 'jn-gpu-memory-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Send-Notification -Method 'notifications/initialized'

    $snapshotOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $SnapshotTrace }
    $snapshotId = [string]$snapshotOpen.data.trace_id
    $streamOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $snapshotStatus = Wait-TraceReady -TraceId $snapshotId
    $streamStatus = Wait-TraceReady -TraceId $streamId

    $snapshot = Validate-GpuMemoryData -TraceId $snapshotId
    $stream = Validate-GpuMemoryData -TraceId $streamId
    Assert-Condition (($snapshot.plots | ConvertTo-Json -Compress -Depth 10) -eq ($stream.plots | ConvertTo-Json -Compress -Depth 10)) 'snapshot/stream plot data mismatch'
    Assert-Condition (($snapshot.pools | ConvertTo-Json -Compress -Depth 10) -eq ($stream.pools | ConvertTo-Json -Compress -Depth 10)) 'snapshot/stream pool data mismatch'
    Assert-Condition ($snapshot.memory_events -eq $stream.memory_events) 'snapshot/stream memory event count mismatch'
    Assert-Condition ($snapshot.persisted_plot_events -eq $stream.persisted_plot_events) 'snapshot/stream persisted plot event count mismatch'
    Assert-Condition ($snapshot.dxgi_app_info -eq $stream.dxgi_app_info) 'snapshot/stream DXGI AppInfo mismatch'
    if ($RequireLogical) {
        Assert-Condition (($snapshot.logical | ConvertTo-Json -Compress -Depth 30) -eq ($stream.logical | ConvertTo-Json -Compress -Depth 30)) 'snapshot/stream logical GPU attribution mismatch'
    }

    [pscustomobject]@{
        ok = $true
        snapshot_fingerprint = [string]$snapshotStatus.data.status.fingerprint
        stream_fingerprint = [string]$streamStatus.data.status.fingerprint
        memory_events = $snapshot.memory_events
        persisted_plot_events = $snapshot.persisted_plot_events
        gpu_plot_count = $snapshot.plots.Count
        gpu_pool_count = $snapshot.pools.Count
        logical_resource_count = if ($RequireLogical) { $snapshot.logical.logical_resource_count } else { $null }
        gpu_memory_pass_count = if ($RequireLogical) { $snapshot.logical.pass_count } else { $null }
        owner_rollup_count = if ($RequireLogical) { $snapshot.logical.owner_rollups.Count } else { $null }
        working_set_count = if ($RequireLogical) { $snapshot.logical.working_sets.Count } else { $null }
        dxgi_app_info = $snapshot.dxgi_app_info
        validation_errors = $snapshot.validation_errors
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
