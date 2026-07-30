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

    [switch] $RequireFrameImage
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

function Get-ExactPlot {
    param([string] $TraceId, [string] $Name)
    $result = Inspect -TraceId $TraceId -Method 'plot.list' -Params @{
        filter = @{ text = $Name; mode = 'exact' }
        limit = 20
    }
    $plots = @($result.data.plots)
    Assert-Condition ($plots.Count -eq 1) "plot missing or duplicated: $Name"
    return $plots[0]
}

function Get-Capability {
    param([object] $Capabilities, [string] $Domain)
    $matches = @($Capabilities.data.domains | Where-Object { [string]$_.domain -eq $Domain })
    Assert-Condition ($matches.Count -eq 1) "capability missing or duplicated: $Domain"
    return $matches[0]
}

function Get-Sha256Hex {
    param([byte[]] $Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return -join ($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') })
    }
    finally {
        $sha.Dispose()
    }
}

function Validate-SystemData {
    param([string] $TraceId)

    $counts = Inspect -TraceId $TraceId -Method 'trace.counts'
    $capabilities = Inspect -TraceId $TraceId -Method 'system.capabilities'
    $traceInfo = Inspect -TraceId $TraceId -Method 'trace.info'
    $appInfo = Inspect -TraceId $TraceId -Method 'trace.app_info'
    $validation = Inspect -TraceId $TraceId -Method 'validation.run'
    $poolsResult = Inspect -TraceId $TraceId -Method 'memory.pools' -Params @{ limit = 100 }
    $locksResult = Inspect -TraceId $TraceId -Method 'lock.list' -Params @{
        filter = @{ text = 'JN.Managed.Validation.ContendedLock'; mode = 'exact' }
        limit = 20
    }

    Assert-Condition ([bool]$validation.data.valid) 'trace validation failed'
    Assert-Condition ([UInt64]$validation.data.error_count -eq 0) 'trace validation reported errors'
    Assert-Condition ([UInt64]$counts.data.memory_events -gt 0) 'trace contains no memory events'
    Assert-Condition ([UInt64]$counts.data.memory_pools -gt 0) 'trace contains no memory pools'
    Assert-Condition ([UInt64]$counts.data.locks -eq 6) 'managed lock event count mismatch'

    $contextSwitch = Get-Capability -Capabilities $capabilities -Domain 'context_switch'
    $sample = Get-Capability -Capabilities $capabilities -Domain 'sample'
    Assert-Condition (-not [bool]$contextSwitch.present -and -not [bool]$contextSwitch.queryable) 'context switch capability must report unavailable when data is absent'
    Assert-Condition ([string]$contextSwitch.reason -eq 'data is absent from the persisted snapshot') 'context switch unavailable reason mismatch'
    Assert-Condition (-not [bool]$sample.present -and -not [bool]$sample.queryable) 'sample capability must report unavailable when data is absent'
    Assert-Condition ([UInt64]$traceInfo.data.sampling_period_ns -eq 0) 'sampling period must be zero when samples are absent'

    $engineIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JN Unity Tracy Target=*' })
    Assert-Condition ($engineIdentity.Count -eq 1) 'JN Unity Tracy AppInfo missing or duplicated'
    Assert-Condition ($engineIdentity[0] -match 'CPU.Memory=Selected\+Aggregate') 'CPU memory capture mode missing from AppInfo'
    Assert-Condition ($engineIdentity[0] -match 'IO=AsyncReadAggregate') 'I/O capture mode missing from AppInfo'
    Assert-Condition ($engineIdentity[0] -match 'Network=SocketAggregate') 'network capture mode missing from AppInfo'
    Assert-Condition ($engineIdentity[0] -match 'Sampling.ContextSwitch.Requested=TracyWindows Actual=QueryCapability') 'sampling/context-switch requested-vs-actual AppInfo mismatch'

    $pools = @($poolsResult.data.pools)
    $managedPools = @($pools | Where-Object { [string]$_.name -eq 'JN.Managed.Validation.NativeMemory' })
    Assert-Condition ($managedPools.Count -eq 1) 'managed validation memory pool missing or duplicated'
    $managedPool = $managedPools[0]
    Assert-Condition ([UInt64]$managedPool.event_count -eq 1) 'managed validation memory event count mismatch'
    Assert-Condition ([UInt64]$managedPool.free_count -eq 1) 'managed validation memory free count mismatch'
    Assert-Condition ([UInt64]$managedPool.active_count -eq 0) 'managed validation memory remained active'
    Assert-Condition ([UInt64]$managedPool.active_bytes -eq 0) 'managed validation memory active bytes are non-zero'

    $memoryEvents = Inspect -TraceId $TraceId -Method 'memory.events' -Params @{
        pool_ref = [string]$managedPool.ref
        limit = 20
    }
    $events = @($memoryEvents.data.events)
    Assert-Condition ($events.Count -eq 1) 'managed validation memory event missing or duplicated'
    Assert-Condition ([UInt64]$events[0].size_bytes -eq 1048576) 'managed validation memory size mismatch'
    Assert-Condition ([bool]$events[0].complete) 'managed validation allocation/free pair is incomplete'
    Assert-Condition ($null -ne $events[0].free_ns) 'managed validation free time is absent'

    $jobSchedulerPools = @($pools | Where-Object { [string]$_.name -eq 'JobScheduler' })
    $texturePools = @($pools | Where-Object { [string]$_.name -eq 'Texture' })
    Assert-Condition ($jobSchedulerPools.Count -eq 1 -and [UInt64]$jobSchedulerPools[0].event_count -gt 0) 'Unity JobScheduler allocator events missing'
    Assert-Condition ($texturePools.Count -eq 1 -and [UInt64]$texturePools[0].event_count -gt 0) 'Unity Texture allocator events missing'

    $locks = @($locksResult.data.locks)
    Assert-Condition ($locks.Count -eq 1) 'managed validation lock missing or duplicated'
    $lock = $locks[0]
    Assert-Condition ([bool]$lock.valid) 'managed validation lock is invalid'
    Assert-Condition ([bool]$lock.contended) 'managed validation lock did not record contention'
    Assert-Condition ([UInt64]$lock.event_count -eq 6) 'managed validation lock event count mismatch'
    Assert-Condition ([UInt64]$lock.thread_count -eq 2) 'managed validation lock thread count mismatch'

    $timeline = Inspect -TraceId $TraceId -Method 'lock.timeline' -Params @{
        lock_ref = [string]$lock.ref
        limit = 20
    }
    $lockEvents = @($timeline.data.events)
    Assert-Condition (@($lockEvents | Where-Object { $_.type -eq 'wait' }).Count -eq 2) 'lock wait count mismatch'
    Assert-Condition (@($lockEvents | Where-Object { $_.type -eq 'obtain' }).Count -eq 2) 'lock obtain count mismatch'
    Assert-Condition (@($lockEvents | Where-Object { $_.type -eq 'release' }).Count -eq 2) 'lock release count mismatch'

    $contention = Inspect -TraceId $TraceId -Method 'lock.contention_statistics' -Params @{ limit = 20 }
    $lockStats = @($contention.data.locks | Where-Object { [string]$_.lock_ref -eq [string]$lock.ref })
    Assert-Condition ($lockStats.Count -eq 1) 'lock contention statistics missing or duplicated'
    Assert-Condition ([UInt64]$lockStats[0].waits -eq 2) 'lock contention wait count mismatch'
    Assert-Condition ([UInt64]$lockStats[0].obtains -eq 2) 'lock contention obtain count mismatch'
    Assert-Condition ([UInt64]$lockStats[0].releases -eq 2) 'lock contention release count mismatch'
    Assert-Condition ([Int64]$lockStats[0].wait_time.max_ns -ge 50000000) 'lock contention did not preserve the deterministic 50 ms wait'

    $managedIo = Get-ExactPlot -TraceId $TraceId -Name 'JN.IO.Managed.Validation.Bytes'
    $managedSend = Get-ExactPlot -TraceId $TraceId -Name 'JN.Network.Managed.Validation.SendBytes'
    $managedReceive = Get-ExactPlot -TraceId $TraceId -Name 'JN.Network.Managed.Validation.ReceiveBytes'
    Assert-Condition ([double]$managedIo.max -eq 4096) 'managed I/O validation bytes mismatch'
    Assert-Condition ([double]$managedSend.max -eq 1536) 'managed network send validation bytes mismatch'
    Assert-Condition ([double]$managedReceive.max -eq 3072) 'managed network receive validation bytes mismatch'

    $cpuMemoryPlots = @(
        'Texture', 'Shader', 'GfxDevice', 'GfxThread', 'Physics', 'JobScheduler',
        'Mono', 'Resource', 'File', 'Network', 'SceneManager', 'GPUScene'
    )
    foreach ($label in $cpuMemoryPlots) {
        $plot = Get-ExactPlot -TraceId $TraceId -Name "JN.CPU.Memory.$label.ActiveBytes"
        Assert-Condition ([UInt64]$plot.point_count -gt 0) "CPU memory aggregate has no points: $label"
    }

    $ioBytes = Get-ExactPlot -TraceId $TraceId -Name 'JN.IO.Read.BytesPerFrame'
    $ioOperations = Get-ExactPlot -TraceId $TraceId -Name 'JN.IO.Read.OperationsPerFrame'
    $ioTotal = Get-ExactPlot -TraceId $TraceId -Name 'JN.IO.Read.TotalBytes'
    $ioQueue = Get-ExactPlot -TraceId $TraceId -Name 'JN.IO.QueueDepth'
    Assert-Condition ([double]$ioBytes.max -gt 0) 'Unity AsyncRead bytes were not captured'
    Assert-Condition ([double]$ioOperations.max -gt 0) 'Unity AsyncRead operations were not captured'
    Assert-Condition ([double]$ioTotal.max -ge [double]$ioBytes.max) 'Unity AsyncRead total bytes are inconsistent'
    Assert-Condition ([UInt64]$ioQueue.point_count -gt 0) 'Unity AsyncRead queue depth has no points'

    $networkReceive = Get-ExactPlot -TraceId $TraceId -Name 'JN.Network.Receive.BytesPerFrame'
    $networkReceiveOperations = Get-ExactPlot -TraceId $TraceId -Name 'JN.Network.Receive.OperationsPerFrame'
    $networkReceiveTotal = Get-ExactPlot -TraceId $TraceId -Name 'JN.Network.Receive.TotalBytes'
    Assert-Condition ([double]$networkReceive.max -gt 0) 'Unity socket receive bytes were not captured'
    Assert-Condition ([double]$networkReceiveOperations.max -gt 0) 'Unity socket receive operations were not captured'
    Assert-Condition ([double]$networkReceiveTotal.max -ge [double]$networkReceive.max) 'Unity socket receive total bytes are inconsistent'

    $frameImageWidth = '0'
    $frameImageHeight = '0'
    $frameImageFrameIndex = '0'
    $frameImageBc1Bytes = '0'
    $frameImagePngSha256 = ''
    if ($RequireFrameImage) {
        Assert-Condition ([UInt64]$counts.data.frame_images -eq 1) 'expected exactly one FrameImage'
        $frameImageCapability = Get-Capability -Capabilities $capabilities -Domain 'frame_image'
        Assert-Condition ([bool]$frameImageCapability.present -and [bool]$frameImageCapability.queryable) 'FrameImage capability is not queryable'

        $frameImages = Inspect -TraceId $TraceId -Method 'frame_image.list' -Params @{ limit = 20 }
        $images = @($frameImages.data.images)
        Assert-Condition ($images.Count -eq 1) 'FrameImage list missing or duplicated the validation image'
        $image = $images[0]
        Assert-Condition ([UInt64]$image.width -eq 64 -and [UInt64]$image.height -eq 32) 'FrameImage dimensions mismatch'
        Assert-Condition (-not [bool]$image.flipped) 'D3D12 FrameImage unexpectedly requested vertical flip'
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$image.frame_ref)) 'FrameImage is not bound to a main frame'
        Assert-Condition ([UInt64]$image.raw_bc1_bytes -eq 1024) 'FrameImage BC1 payload size mismatch'

        $metadata = Inspect -TraceId $TraceId -Method 'frame_image.metadata' -Params @{ ref = [string]$image.ref }
        Assert-Condition ([UInt64]$metadata.data.width -eq 64 -and [UInt64]$metadata.data.height -eq 32) 'FrameImage metadata dimensions mismatch'
        Assert-Condition ([string]$metadata.data.frame_ref -eq [string]$image.frame_ref) 'FrameImage list/metadata frame binding mismatch'

        $resourceRead = Send-Rpc -Method 'resources/read' -Params @{ uri = [string]$image.resource_uri } -TimeoutMilliseconds 60000
        $contents = @($resourceRead.result.contents)
        Assert-Condition ($contents.Count -eq 1) 'FrameImage MCP resource omitted content'
        Assert-Condition ([string]$contents[0].mimeType -eq 'image/png') 'FrameImage MCP resource MIME type mismatch'
        $png = [Convert]::FromBase64String([string]$contents[0].blob)
        Assert-Condition ($png.Length -gt 8) 'FrameImage MCP resource returned an empty PNG'
        Assert-Condition ($png[0] -eq 0x89 -and $png[1] -eq 0x50 -and $png[2] -eq 0x4E -and $png[3] -eq 0x47) 'FrameImage MCP resource PNG signature mismatch'

        $submitted = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.Submitted'
        $latency = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.ReadbackLatencyMs'
        $downsample = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.DownsampleSubmitCpuMs'
        Assert-Condition ([UInt64]$submitted.point_count -eq 1 -and [double]$submitted.max -eq 1) 'FrameImage submitted plot mismatch'
        Assert-Condition ([UInt64]$latency.point_count -eq 1 -and [double]$latency.max -gt 0) 'FrameImage readback latency plot mismatch'
        Assert-Condition ([UInt64]$downsample.point_count -eq 1 -and [double]$downsample.max -ge 0) 'FrameImage downsample submit plot mismatch'
        Assert-Condition ($engineIdentity[0] -match 'FrameImage=DefaultOff\+AsyncReadbackRing4\+Max320x180') 'FrameImage default-off native AppInfo mismatch'
        $frameImageIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JN FrameImage enabled=*' })
        Assert-Condition ($frameImageIdentity.Count -eq 1) 'FrameImage managed AppInfo missing or duplicated'
        Assert-Condition ($frameImageIdentity[0] -match 'size=64x32.*ring=4.*async=1.*gpu_downsample=1.*present_wait=0') 'FrameImage managed AppInfo mismatch'

        $frameImageWidth = [string]$image.width
        $frameImageHeight = [string]$image.height
        $frameImageFrameIndex = [string]$image.raw_frame_index
        $frameImageBc1Bytes = [string]$image.raw_bc1_bytes
        $frameImagePngSha256 = Get-Sha256Hex -Bytes $png
    }

    return [pscustomobject]@{
        memory_events = [string]$counts.data.memory_events
        memory_pools = [string]$counts.data.memory_pools
        lock_events = [string]$counts.data.locks
        managed_memory_size = [string]$events[0].size_bytes
        jobscheduler_events = [string]$jobSchedulerPools[0].event_count
        texture_events = [string]$texturePools[0].event_count
        lock_max_wait_ns = [string]$lockStats[0].wait_time.max_ns
        cpu_memory_plot_count = [string]$cpuMemoryPlots.Count
        io_bytes_per_frame = [string]$ioBytes.max
        io_operations_per_frame = [string]$ioOperations.max
        network_receive_bytes_per_frame = [string]$networkReceive.max
        network_receive_operations_per_frame = [string]$networkReceiveOperations.max
        context_switch_present = [bool]$contextSwitch.present
        sample_present = [bool]$sample.present
        sampling_period_ns = [string]$traceInfo.data.sampling_period_ns
        frame_image_width = $frameImageWidth
        frame_image_height = $frameImageHeight
        frame_image_frame_index = $frameImageFrameIndex
        frame_image_bc1_bytes = $frameImageBc1Bytes
        frame_image_png_sha256 = $frameImagePngSha256
        engine_app_info = [string]$engineIdentity[0]
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
        clientInfo = @{ name = 'jn-system-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Send-Notification -Method 'notifications/initialized'

    $snapshotOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $SnapshotTrace }
    $snapshotId = [string]$snapshotOpen.data.trace_id
    $streamOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $snapshotStatus = Wait-TraceReady -TraceId $snapshotId
    $streamStatus = Wait-TraceReady -TraceId $streamId

    $snapshot = Validate-SystemData -TraceId $snapshotId
    $stream = Validate-SystemData -TraceId $streamId
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($stream | ConvertTo-Json -Compress -Depth 20)) 'snapshot/stream system data mismatch'

    [pscustomobject]@{
        ok = $true
        snapshot_fingerprint = [string]$snapshotStatus.data.status.fingerprint
        stream_fingerprint = [string]$streamStatus.data.status.fingerprint
        memory_events = $snapshot.memory_events
        memory_pools = $snapshot.memory_pools
        lock_events = $snapshot.lock_events
        managed_memory_size = $snapshot.managed_memory_size
        jobscheduler_events = $snapshot.jobscheduler_events
        texture_events = $snapshot.texture_events
        lock_max_wait_ns = $snapshot.lock_max_wait_ns
        cpu_memory_plot_count = $snapshot.cpu_memory_plot_count
        io_bytes_per_frame = $snapshot.io_bytes_per_frame
        io_operations_per_frame = $snapshot.io_operations_per_frame
        network_receive_bytes_per_frame = $snapshot.network_receive_bytes_per_frame
        network_receive_operations_per_frame = $snapshot.network_receive_operations_per_frame
        context_switch_present = $snapshot.context_switch_present
        sample_present = $snapshot.sample_present
        sampling_period_ns = $snapshot.sampling_period_ns
        frame_image_width = $snapshot.frame_image_width
        frame_image_height = $snapshot.frame_image_height
        frame_image_frame_index = $snapshot.frame_image_frame_index
        frame_image_bc1_bytes = $snapshot.frame_image_bc1_bytes
        frame_image_png_sha256 = $snapshot.frame_image_png_sha256
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
