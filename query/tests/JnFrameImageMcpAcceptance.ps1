[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SnapshotTrace,
    [Parameter(Mandatory = $true)][string] $StreamTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][ValidateRange(4, 65532)][int] $ExpectedWidth,
    [Parameter(Mandatory = $true)][ValidateRange(4, 65532)][int] $ExpectedHeight,
    [ValidateSet('HighEvidence', 'Triggered')][string] $ExpectedKind = 'HighEvidence',
    [bool] $ExpectedFlipped = $false,
    [ValidateRange(1, 10000)][int] $MinimumMatchingImages = 1,
    [ValidateRange(0, 10000)][int] $ExpectedExactTotal = 0,
    [ValidateRange(0, 10000)][int] $ExpectedIntervalFrames = 0,
    [string] $OutputPngPath = '',
    [string] $OutputReportPath = '',
    [switch] $Indexed,
    [switch] $SkipValidation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Condition([bool] $Condition, [string] $Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($requiredPath in @($QueryExe, $SnapshotTrace, $StreamTrace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $requiredPath) "required path does not exist: $requiredPath"
}

Assert-Condition (($ExpectedWidth -band 3) -eq 0 -and ($ExpectedHeight -band 3) -eq 0) `
    'FrameImage dimensions must be multiples of four'

$script:NextRequestId = 1
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $QueryExe
$indexedArgument = if ($Indexed) { ' --indexed' } else { '' }
$startInfo.Arguments = "--mcp$indexedArgument --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$startInfo.WorkingDirectory = $AllowRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$queryProcess = [Diagnostics.Process]::new()
$queryProcess.StartInfo = $startInfo

function Send-Notification([string] $Method, [hashtable] $Params = @{})
{
    $payload = [ordered]@{ jsonrpc = '2.0'; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 240000)
{
    $id = $script:NextRequestId++
    $payload = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params }
    $script:queryProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $remaining = [Math]::Max(1, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $read = $script:queryProcess.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) { throw "MCP response timed out: $Method (id=$id)" }
        $line = $read.Result
        if ($null -eq $line) { throw "MCP stdout closed before response: $Method (id=$id)" }
        $message = $line | ConvertFrom-Json
        if ($null -ne $message.PSObject.Properties['id'])
        {
            Assert-Condition ([string]$message.id -eq [string]$id) "unexpected response id $($message.id), expected $id"
            return $message
        }
    }
    throw "MCP response deadline expired: $Method (id=$id)"
}

function Invoke-McpTool([string] $Name, [hashtable] $Arguments = @{})
{
    $response = Send-Rpc -Method 'tools/call' -Params @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    $structured = $response.result.structuredContent
    if ([bool]$response.result.isError)
    {
        throw "$Name returned an error: $($structured.error | ConvertTo-Json -Compress -Depth 20)"
    }
    Assert-Condition ([bool]$structured.ok) "$Name omitted structuredContent.ok=true"
    return $structured
}

function Wait-TraceReady([string] $TraceId)
{
    $deadline = [DateTime]::UtcNow.AddMinutes(4)
    while ([DateTime]::UtcNow -lt $deadline)
    {
        $status = Invoke-McpTool -Name 'tracy_trace_status' -Arguments @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return $status }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached terminal state $state" }
        Start-Sleep -Milliseconds 250
    }
    throw "trace $TraceId did not become ready"
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
{
    return Invoke-McpTool -Name 'tracy_inspect' -Arguments @{
        trace_id = $TraceId
        method = $Method
        params = $Params
    }
}

function Get-ExactPlot([string] $TraceId, [string] $Name)
{
    $result = Inspect -TraceId $TraceId -Method 'plot.list' -Params @{
        filter = @{ text = $Name; mode = 'exact' }
        limit = 20
    }
    $plots = @($result.data.plots)
    Assert-Condition ($plots.Count -eq 1) "plot missing or duplicated: $Name"
    return $plots[0]
}

function Get-Sha256Hex([byte[]] $Bytes)
{
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
}

function ConvertFrom-Base64Url([string] $Value)
{
    $base64 = $Value.Replace('-', '+').Replace('_', '/')
    switch ($base64.Length % 4)
    {
        2 { $base64 += '==' }
        3 { $base64 += '=' }
    }
    return [Convert]::FromBase64String($base64)
}

function Get-Capability([object] $Capabilities, [string] $Domain)
{
    $matches = @($Capabilities.data.domains | Where-Object { [string]$_.domain -eq $Domain })
    Assert-Condition ($matches.Count -eq 1) "capability missing or duplicated: $Domain"
    return $matches[0]
}

function Validate-FrameImage([string] $TraceId, [bool] $ExportPng)
{
    $counts = Inspect -TraceId $TraceId -Method 'trace.counts'
    $capabilities = Inspect -TraceId $TraceId -Method 'system.capabilities'
    $appInfo = Inspect -TraceId $TraceId -Method 'trace.app_info'
    $validation = if ($SkipValidation) { $null } else {
        Inspect -TraceId $TraceId -Method 'validation.run' -Params @{
            max_scan_events = 100000000
            max_cpu_ms = 60000
        }
    }
    # Continue collecting FrameImage evidence when whole-trace validation is
    # partial. The report must survive a failed global gate so image transport,
    # dimensions and frame binding can be diagnosed independently. The caller
    # still fails after writing the structured report.

    $capability = Get-Capability -Capabilities $capabilities -Domain 'frame_image'
    Assert-Condition ([bool]$capability.present -and [bool]$capability.queryable) 'FrameImage capability is not queryable'
    if ($ExpectedExactTotal -gt 0)
    {
        Assert-Condition ([UInt64]$counts.data.frame_images -eq [UInt64]$ExpectedExactTotal) 'FrameImage exact total mismatch'
    }

    $list = Inspect -TraceId $TraceId -Method 'frame_image.list' -Params @{ limit = 1000 }
    $allImages = @($list.data.images)
    # Do not use `$matches`: PowerShell's automatic `$Matches` variable is
    # case-insensitive and is overwritten by the AppInfo regex checks below.
    $matchingImages = @($allImages | Where-Object {
        [int]$_.width -eq $ExpectedWidth -and [int]$_.height -eq $ExpectedHeight
    })
    Assert-Condition ($matchingImages.Count -ge $MinimumMatchingImages) 'required FrameImage dimensions are missing'
    if ($ExpectedIntervalFrames -gt 0 -and $matchingImages.Count -gt 1)
    {
        $orderedFrameIndexes = @($matchingImages | ForEach-Object { [UInt64]$_.raw_frame_index } | Sort-Object)
        for ($index = 1; $index -lt $orderedFrameIndexes.Count; ++$index)
        {
            Assert-Condition (($orderedFrameIndexes[$index] - $orderedFrameIndexes[$index - 1]) -eq [UInt64]$ExpectedIntervalFrames) `
                "FrameImage interval mismatch at matching image $index"
        }
    }
    $image = $matchingImages[0]
    Assert-Condition ([bool]$image.flipped -eq $ExpectedFlipped) 'FrameImage flip metadata mismatch'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$image.frame_ref)) 'FrameImage is not bound to a main frame'
    $expectedBc1Bytes = [UInt64]($ExpectedWidth * $ExpectedHeight / 2)
    Assert-Condition ([UInt64]$image.raw_bc1_bytes -eq $expectedBc1Bytes) 'FrameImage BC1 byte count mismatch'

    $metadata = Inspect -TraceId $TraceId -Method 'frame_image.metadata' -Params @{ ref = [string]$image.ref }
    Assert-Condition ([int]$metadata.data.width -eq $ExpectedWidth -and [int]$metadata.data.height -eq $ExpectedHeight) 'FrameImage metadata dimensions mismatch'
    Assert-Condition ([string]$metadata.data.frame_ref -eq [string]$image.frame_ref) 'FrameImage list/metadata frame binding mismatch'

    $raw = Inspect -TraceId $TraceId -Method 'frame_image.raw' -Params @{
        ref = [string]$image.ref
        offset_bytes = 0
        max_bytes = 1048576
    }
    Assert-Condition ([string]$raw.data.format -eq 'bc1_dxt1') 'FrameImage raw format mismatch'
    Assert-Condition ([bool]$raw.data.eof) 'FrameImage raw payload was unexpectedly paginated'
    Assert-Condition ([UInt64]$raw.data.returned_bytes -eq $expectedBc1Bytes -and [UInt64]$raw.data.total_bytes -eq $expectedBc1Bytes) 'FrameImage raw payload size mismatch'
    $rawBytes = ConvertFrom-Base64Url ([string]$raw.data.data_base64url)
    Assert-Condition ([UInt64]$rawBytes.Length -eq $expectedBc1Bytes) 'decoded FrameImage raw payload size mismatch'

    $resourceRead = Send-Rpc -Method 'resources/read' -Params @{ uri = [string]$image.resource_uri } -TimeoutMilliseconds 60000
    $contents = @($resourceRead.result.contents)
    Assert-Condition ($contents.Count -eq 1 -and [string]$contents[0].mimeType -eq 'image/png') 'FrameImage MCP PNG resource mismatch'
    $png = [Convert]::FromBase64String([string]$contents[0].blob)
    Assert-Condition ($png.Length -gt 8 -and $png[0] -eq 0x89 -and $png[1] -eq 0x50 -and $png[2] -eq 0x4E -and $png[3] -eq 0x47) 'FrameImage PNG signature mismatch'
    if ($ExportPng -and -not [string]::IsNullOrWhiteSpace($OutputPngPath))
    {
        $outputDirectory = Split-Path -Parent $OutputPngPath
        if (-not [string]::IsNullOrWhiteSpace($outputDirectory))
        {
            [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
        }
        [IO.File]::WriteAllBytes($OutputPngPath, $png)
    }

    $engineIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JN Unity Tracy Target=*' })
    Assert-Condition ($engineIdentity.Count -eq 1 -and $engineIdentity[0] -match 'FrameImage=Daily960x540Every60\+Triggered1280x720\+FinalColorOnly\+AsyncReadbackRing4') 'native FrameImage AppInfo mismatch'
    $managedIdentity = @($appInfo.data.app_info | Where-Object { $_ -like 'JNFI1|*' })
    Assert-Condition ($managedIdentity.Count -ge 1) 'managed FrameImage AppInfo missing'
    # Tracy on-demand replays process AppInfo after reconnect. The managed
    # bridge also publishes for the new connection, so physical duplicates are
    # valid only when they collapse to one semantic payload.
    $uniqueManagedIdentity = @($managedIdentity | Sort-Object -Unique)
    Assert-Condition ($uniqueManagedIdentity.Count -eq 1) 'managed FrameImage AppInfo payloads disagree'
    $config = ([string]$uniqueManagedIdentity[0]).Substring(6) | ConvertFrom-Json
    Assert-Condition ([UInt64]$config.schema_version -eq 1 -and [string]$config.source -eq 'final-color-only') 'managed FrameImage schema/source mismatch'
    Assert-Condition ([UInt64]$config.ring -eq 4 -and [bool]$config.async -and -not [bool]$config.present_wait) 'managed FrameImage async policy mismatch'
    if ($ExpectedKind -eq 'HighEvidence')
    {
        Assert-Condition ([int]$config.high_evidence.width -eq $ExpectedWidth -and [int]$config.high_evidence.height -eq $ExpectedHeight) 'HighEvidence config dimensions mismatch'
        $kindSubmitted = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.Submitted.HighEvidence'
    }
    else
    {
        Assert-Condition ([int]$config.triggered.width -eq $ExpectedWidth -and [int]$config.triggered.height -eq $ExpectedHeight) 'Triggered config dimensions mismatch'
        $kindSubmitted = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.Submitted.Triggered'
    }

    $submitted = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.Submitted'
    $latency = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.ReadbackLatencyMs'
    $captureSubmit = Get-ExactPlot -TraceId $TraceId -Name 'JN.FrameImage.CaptureSubmitCpuMs'
    Assert-Condition ([UInt64]$submitted.point_count -ge [UInt64]$MinimumMatchingImages) 'FrameImage submitted plot count mismatch'
    Assert-Condition ([UInt64]$kindSubmitted.point_count -ge 1) 'FrameImage kind-specific submitted plot is empty'
    Assert-Condition ([UInt64]$latency.point_count -ge 1 -and [double]$latency.max -gt 0) 'FrameImage readback latency plot mismatch'
    Assert-Condition ([UInt64]$captureSubmit.point_count -ge 1 -and [double]$captureSubmit.max -ge 0) 'FrameImage capture submit plot mismatch'

    return [ordered]@{
        total_images = [string]$allImages.Count
        matching_images = [string]$matchingImages.Count
        width = [string]$ExpectedWidth
        height = [string]$ExpectedHeight
        kind = $ExpectedKind
        raw_frame_index = [string]$image.raw_frame_index
        expected_interval_frames = [string]$ExpectedIntervalFrames
        raw_bc1_bytes = [string]$expectedBc1Bytes
        raw_sha256 = Get-Sha256Hex $rawBytes
        png_sha256 = Get-Sha256Hex $png
        validation_skipped = [bool]$SkipValidation
        validation_complete = $(if ($null -ne $validation) { [bool]$validation.data.complete } else { $null })
        validation_valid = $(if ($null -ne $validation) { [bool]$validation.data.valid } else { $null })
        validation_partial = $(if ($null -ne $validation) { [bool]$validation.partial } else { $null })
        validation_errors = $(if ($null -ne $validation) { [string]$validation.data.error_count } else { $null })
        capture_submit_cpu_ms = [ordered]@{
            point_count = [string]$captureSubmit.point_count
            min = [string]$captureSubmit.min
            max = [string]$captureSubmit.max
            mean = [string]([double]$captureSubmit.sum / [Math]::Max(1, [double]$captureSubmit.point_count))
        }
        readback_latency_ms = [ordered]@{
            point_count = [string]$latency.point_count
            min = [string]$latency.min
            max = [string]$latency.max
            mean = [string]([double]$latency.sum / [Math]::Max(1, [double]$latency.point_count))
        }
    }
}

$snapshotId = ''
$streamId = ''
try
{
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc -Method 'initialize' -Params @{
        protocolVersion = '2025-11-25'
        capabilities = @{}
        clientInfo = @{ name = 'jn-frame-image-acceptance'; version = '1.0' }
    }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialization failed'
    Send-Notification -Method 'notifications/initialized'

    $snapshotOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $SnapshotTrace }
    $snapshotId = [string]$snapshotOpen.data.trace_id
    $streamOpen = Invoke-McpTool -Name 'tracy_trace_open' -Arguments @{ path = $StreamTrace }
    $streamId = [string]$streamOpen.data.trace_id
    $snapshotStatus = Wait-TraceReady -TraceId $snapshotId
    $streamStatus = Wait-TraceReady -TraceId $streamId
    $snapshotWatch = [Diagnostics.Stopwatch]::StartNew()
    $snapshot = Validate-FrameImage -TraceId $snapshotId -ExportPng $true
    $snapshotWatch.Stop()
    $streamWatch = [Diagnostics.Stopwatch]::StartNew()
    $stream = Validate-FrameImage -TraceId $streamId -ExportPng $false
    $streamWatch.Stop()
    Assert-Condition (($snapshot | ConvertTo-Json -Compress -Depth 20) -eq ($stream | ConvertTo-Json -Compress -Depth 20)) 'snapshot/stream FrameImage data mismatch'
    $queryProcess.Refresh()

    $result = [ordered]@{
        ok = ([bool]$snapshot.validation_skipped -or
            ([bool]$snapshot.validation_complete -and [bool]$snapshot.validation_valid -and
                -not [bool]$snapshot.validation_partial -and [UInt64]$snapshot.validation_errors -eq 0))
        snapshot_fingerprint = [string]$snapshotStatus.data.status.fingerprint
        stream_fingerprint = [string]$streamStatus.data.status.fingerprint
        frame_image = $snapshot
        query_pressure = [ordered]@{
            snapshot_validation_and_read_ms = $snapshotWatch.ElapsedMilliseconds
            stream_validation_and_read_ms = $streamWatch.ElapsedMilliseconds
            peak_working_set_bytes = $queryProcess.PeakWorkingSet64
            private_bytes = $queryProcess.PrivateMemorySize64
        }
    }
    $resultJson = $result | ConvertTo-Json -Compress -Depth 20
    if (-not [string]::IsNullOrWhiteSpace($OutputReportPath))
    {
        $reportDirectory = Split-Path -Parent $OutputReportPath
        if (-not [string]::IsNullOrWhiteSpace($reportDirectory))
        {
            [IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
        }
        [IO.File]::WriteAllText($OutputReportPath, $resultJson, [Text.UTF8Encoding]::new($false))
    }
    if (-not $SkipValidation)
    {
        Assert-Condition ([bool]$result.ok) 'trace validation is incomplete or invalid'
    }
    $resultJson
}
finally
{
    foreach ($traceId in @($snapshotId, $streamId))
    {
        if (-not [string]::IsNullOrWhiteSpace($traceId) -and -not $queryProcess.HasExited)
        {
            try { [void](Invoke-McpTool -Name 'tracy_trace_close' -Arguments @{ trace_id = $traceId }) } catch {}
        }
    }
    if ($queryProcess -and -not $queryProcess.HasExited)
    {
        $queryProcess.StandardInput.Close()
        if (-not $queryProcess.WaitForExit(5000))
        {
            $queryProcess.Kill($true)
            $queryProcess.WaitForExit()
        }
    }
    if ($queryProcess) { $queryProcess.Dispose() }
}
