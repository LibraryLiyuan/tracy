[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $BaselineTrace,
    [Parameter(Mandatory = $true)][string] $CandidateTrace,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][string] $ReportJson,
    [switch] $AllowSyntheticIncompleteContext
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $BaselineTrace, $CandidateTrace, $AllowRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
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
    $script:queryProcess.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 50))
    $script:queryProcess.StandardInput.Flush()
}

function Send-Rpc {
    param([string] $Method, [hashtable] $Params = @{}, [int] $TimeoutMilliseconds = 300000)
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

function Invoke-Tool {
    param([string] $Name, [hashtable] $Arguments = @{})
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    Assert-Condition ($null -ne $response.result) "$Name omitted result"
    return $response.result
}

function Invoke-ToolChecked {
    param([string] $Name, [hashtable] $Arguments = @{})
    $result = Invoke-Tool $Name $Arguments
    if ([bool]$result.isError) { throw "$Name failed: $($result.structuredContent.error | ConvertTo-Json -Compress -Depth 20)" }
    Assert-Condition ([bool]$result.structuredContent.ok) "$Name omitted structuredContent.ok=true"
    return $result.structuredContent
}

function Wait-Ready {
    param([string] $TraceId)
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Invoke-ToolChecked 'tracy_trace_status' @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return $status }
        if ($state -in @('failed', 'closed')) { throw "trace entered $state" }
        Start-Sleep -Milliseconds 250
    }
    throw 'trace did not become ready'
}

function Inspect-Raw {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    $result = Invoke-Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
    return $result.structuredContent
}

function ConvertTo-CanonicalValue {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [string]) {
        $normalized = [regex]::Replace($Value, 'tracy:v1:[0-9a-fA-F]{16}:', 'tracy:v1:<trace>:')
        return [regex]::Replace($normalized, 'tracy://trace/[^/]+/', 'tracy://trace/<trace>/')
    }
    if ($Value -is [System.Collections.IDictionary]) {
        $ordered = [ordered]@{}
        foreach ($key in @($Value.Keys | Sort-Object)) {
            if ([string]$key -in @('fingerprint', 'trace_id', 'load_time_ns')) { continue }
            $ordered[[string]$key] = ConvertTo-CanonicalValue $Value[$key]
        }
        return $ordered
    }
    if ($Value -is [pscustomobject]) {
        $ordered = [ordered]@{}
        foreach ($property in @($Value.PSObject.Properties | Sort-Object Name)) {
            if ($property.Name -in @('fingerprint', 'trace_id', 'load_time_ns')) { continue }
            $ordered[$property.Name] = ConvertTo-CanonicalValue $property.Value
        }
        return $ordered
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        return @($Value | ForEach-Object { ConvertTo-CanonicalValue $_ })
    }
    return $Value
}

function Get-Sha256Text {
    param([string] $Text)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([Convert]::ToHexString($sha.ComputeHash($bytes))) }
    finally { $sha.Dispose() }
}

function Get-DomainSnapshot {
    param([string] $TraceId, [string] $Method, [hashtable] $Params = @{})
    $response = Inspect-Raw $TraceId $Method $Params
    if (-not [bool]$response.ok) {
        $error = ConvertTo-CanonicalValue $response.error
        return [ordered]@{ available = $false; error = $error }
    }
    return [ordered]@{ available = $true; data = ConvertTo-CanonicalValue $response.data }
}

$traceIds = @()
try {
    Assert-Condition ($queryProcess.Start()) 'failed to start tracy-query MCP server'
    $script:queryProcess = $queryProcess
    $initialized = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-stream-convert-compare'; version = '1.0' } }
    Assert-Condition ([string]$initialized.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    Send-Notification 'notifications/initialized'

    $opened = @{}
    foreach ($entry in @(
        @{ label = 'baseline'; path = $BaselineTrace },
        @{ label = 'candidate'; path = $CandidateTrace }
    )) {
        $open = Invoke-ToolChecked 'tracy_trace_open' @{ path = $entry.path }
        $traceId = [string]$open.data.trace_id
        $traceIds += $traceId
        [void](Wait-Ready $traceId)
        $opened[$entry.label] = $traceId
    }

    $methods = [ordered]@{
        overview = @{ method = 'trace.overview'; params = @{} }
        capture_context = @{ method = 'capture.context'; params = @{} }
        capture_coverage = @{ method = 'capture.coverage'; params = @{} }
        gpu_memory = @{ method = 'memory.gpu.summary'; params = @{} }
        gpu_catalog_status = @{ method = 'gpu.catalog.status'; params = @{} }
        gpu_catalog_validation = @{ method = 'gpu.catalog.validation'; params = @{} }
        gpu_resource_search = @{ method = 'gpu.resource.search'; params = @{ limit = 1000 } }
        resource_graph = @{ method = 'resource.summary'; params = @{} }
        jobs = @{ method = 'job.statistics'; params = @{ max_cpu_ms = 60000 } }
        io = @{ method = 'io.statistics'; params = @{} }
        script = @{ method = 'runtime.script.summary'; params = @{} }
        context_switch = @{ method = 'context_switch.statistics'; params = @{} }
        frame_images = @{ method = 'frame_image.list'; params = @{ limit = 1000 } }
        validation = @{ method = 'validation.run'; params = @{ max_cpu_ms = 60000; max_scan_events = 250000000 } }
    }

    $domainReport = [ordered]@{}
    $allEqual = $true
    $snapshots = @{ baseline = @{}; candidate = @{} }
    foreach ($domain in $methods.Keys) {
        $definition = $methods[$domain]
        foreach ($label in @('baseline', 'candidate')) {
            $snapshot = Get-DomainSnapshot $opened[$label] $definition.method $definition.params
            $json = $snapshot | ConvertTo-Json -Compress -Depth 100
            $snapshots[$label][$domain] = @{ value = $snapshot; json = $json; hash = Get-Sha256Text $json }
        }
        $equal = $snapshots.baseline[$domain].json -ceq $snapshots.candidate[$domain].json
        if (-not $equal) {
            $allEqual = $false
            Set-Content -LiteralPath "$ReportJson.$domain.baseline.json" -Value $snapshots.baseline[$domain].json -Encoding utf8NoBOM
            Set-Content -LiteralPath "$ReportJson.$domain.candidate.json" -Value $snapshots.candidate[$domain].json -Encoding utf8NoBOM
        }
        $domainReport[$domain] = [ordered]@{
            equal = $equal
            baseline_sha256 = $snapshots.baseline[$domain].hash
            candidate_sha256 = $snapshots.candidate[$domain].hash
            baseline_available = [bool]$snapshots.baseline[$domain].value.available
            candidate_available = [bool]$snapshots.candidate[$domain].value.available
        }
    }

    $frameCount = [Math]::Min(
        [int64]$snapshots.baseline.overview.value.data.primary_frame_statistics.count,
        [int64]$snapshots.candidate.overview.value.data.primary_frame_statistics.count)
    $normalized = Get-DomainSnapshot $opened.candidate 'compare.normalized' @{
        baseline_trace_id = $opened.baseline
        comparison_mode = 'performance'
        frame_set = 'Frames'
        warmup_frames = 0
        window_frames = [Math]::Max(1, $frameCount - 1)
        allow_warnings = $true
        max_scan_events = 100000000
        max_cpu_ms = 60000
        limit = 100
    }
    $normalizedPerformed = [bool]$normalized.available -and [bool]$normalized.data.performed
    $normalizedStatus = 'performed'
    if ($normalizedPerformed) {
        $normalizedOk = [int64]$normalized.data.cpu.unmatched_count -eq 0 -and
            [int64]$normalized.data.gpu.unmatched_count -eq 0 -and
            [int64]$normalized.data.jobs.unmatched_count -eq 0 -and
            [double]$normalized.data.frames.delta.mean_ns -eq 0.0
    }
    else {
        $hardFailures = @($normalized.data.compatibility.checks | Where-Object { -not [bool]$_.matched -and [string]$_.severity -eq 'hard' })
        $onlyPreexistingIdentityGap = $hardFailures.Count -eq 1 -and [string]$hardFailures[0].id -eq 'identity.complete' -and
            -not [bool]$hardFailures[0].baseline -and -not [bool]$hardFailures[0].candidate
        $symmetricSyntheticGaps = $AllowSyntheticIncompleteContext -and $hardFailures.Count -gt 0 -and
            @($hardFailures | Where-Object {
                $_.baseline -ne $_.candidate -or
                ($null -ne $_.baseline -and [bool]$_.baseline)
            }).Count -eq 0
        $normalizedOk = $onlyPreexistingIdentityGap -or $symmetricSyntheticGaps
        $normalizedStatus = if ($onlyPreexistingIdentityGap) {
            'skipped_preexisting_capture_identity_incomplete'
        }
        elseif ($symmetricSyntheticGaps) {
            'skipped_synthetic_context_incomplete'
        }
        else {
            'refused'
        }
    }
    if (-not $normalizedOk) { $allEqual = $false }

    $report = [ordered]@{
        schema = 1
        equal = $allEqual
        comparison = 'MCP canonical domain data plus complete-frame normalized comparison'
        baseline = $BaselineTrace
        candidate = $CandidateTrace
        frame_window = $frameCount
        normalized_compare_passed = $normalizedOk
        normalized_compare_status = $normalizedStatus
        normalized_compare = $normalized
        domains = $domainReport
    }
    $temporary = "$ReportJson.tmp"
    $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
    Move-Item -LiteralPath $temporary -Destination $ReportJson -Force
    if (-not $allEqual) { throw "semantic comparison failed; see $ReportJson" }
    Write-Host "Semantic comparison passed: $ReportJson"
}
finally {
    foreach ($traceId in $traceIds) {
        try { [void](Invoke-ToolChecked 'tracy_trace_close' @{ trace_id = $traceId }) } catch {}
    }
    if ($null -ne $queryProcess -and -not $queryProcess.HasExited) {
        try { $queryProcess.StandardInput.Close() } catch {}
        if (-not $queryProcess.WaitForExit(2000)) { $queryProcess.Kill($true) }
    }
    $queryProcess.Dispose()
}
