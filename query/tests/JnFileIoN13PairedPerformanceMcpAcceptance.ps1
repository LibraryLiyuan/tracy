[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string[]] $LegacySessionDirectories,
    [Parameter(Mandatory = $true)][string[]] $PassAndPhaseSessionDirectories,
    [Parameter(Mandatory = $true)][string] $AllowRoot,
    [Parameter(Mandatory = $true)][string] $OutputFile
)

$ErrorActionPreference = 'Stop'
function Assert-Condition([bool] $Condition, [string] $Message) { if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }
Assert-Condition ($LegacySessionDirectories.Count -eq 3) 'three independent Legacy sessions are required'
Assert-Condition ($PassAndPhaseSessionDirectories.Count -eq 3) 'three independent PassAndPhase sessions are required'
foreach ($path in @($QueryExe, $AllowRoot) + $LegacySessionDirectories + $PassAndPhaseSessionDirectories) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $QueryExe
$start.Arguments = "--mcp --allow-root `"$AllowRoot`" --allow-source-root `"C:\workflow`""
$start.WorkingDirectory = $AllowRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$script:nextId = 1
$script:opened = @()

function Send-Rpc([string] $Method, [hashtable] $Params = @{}) {
    $id = $script:nextId++
    $payload = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 60
    $script:process.StandardInput.WriteLine($payload)
    $script:process.StandardInput.Flush()
    $read = $script:process.StandardOutput.ReadLineAsync()
    Assert-Condition ($read.Wait(600000)) "MCP timeout: $Method"
    Assert-Condition ($null -ne $read.Result) "MCP stdout closed: $Method"
    $response = $read.Result | ConvertFrom-Json
    Assert-Condition ([string]$response.id -eq [string]$id) "unexpected MCP response id: $Method"
    return $response
}

function Tool([string] $Name, [hashtable] $Arguments = @{}) {
    $response = Send-Rpc 'tools/call' @{ name = $Name; arguments = $Arguments }
    $value = $response.result.structuredContent
    Assert-Condition ($null -ne $value) "$Name omitted structuredContent"
    Assert-Condition (-not [bool]$response.result.isError) "$Name returned MCP error: $($value | ConvertTo-Json -Compress -Depth 12)"
    Assert-Condition ([bool]$value.ok) "$Name query failed"
    return $value
}

function Wait-Ready([string] $TraceId) {
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Tool 'tracy_trace_status' @{ trace_id = $TraceId }
        $state = [string]$status.data.status.state
        if ($state -eq 'ready') { return $status }
        if ($state -in @('failed', 'closed')) { throw "trace $TraceId reached $state" }
        Start-Sleep -Milliseconds 100
    }
    throw "trace not ready: $TraceId"
}

function Open-Trace([string] $Path) {
    $opened = Tool 'tracy_trace_open' @{ path = $Path }
    $id = [string]$opened.data.trace_id
    $script:opened += $id
    [void](Wait-Ready $id)
    return $id
}

function Close-Trace([string] $TraceId) {
    [void](Tool 'tracy_trace_close' @{ trace_id = $TraceId })
    $script:opened = @($script:opened | Where-Object { $_ -ne $TraceId })
}

function Inspect([string] $TraceId, [string] $Method, [hashtable] $Params = @{}) {
    return Tool 'tracy_inspect' @{ trace_id = $TraceId; method = $Method; params = $Params }
}

function Median([double[]] $Values) {
    $ordered = @($Values | Sort-Object)
    return $ordered[[int][Math]::Floor($ordered.Count / 2)]
}

function Measure-Run([string] $TracePath, [string] $StreamPath, [string] $Mode, [int] $Session, [int] $Run) {
    $traceId = Open-Trace $TracePath
    try {
        $label = "$Mode-S$Session-R$Run"
        $context = Inspect $traceId 'capture.context'
        Assert-Condition ([bool]$context.data.present -and [bool]$context.data.complete) "$label capture context incomplete"
        Assert-Condition ([string]$context.data.context.runtime.graphics_api -eq 'd3d12') "$label graphics API mismatch"
        Assert-Condition ([string]$context.data.context.runtime.graphics_jobs_effective -eq 'off') "$label gfx-jobs mismatch"
        Assert-Condition ([string]$context.data.context.workload.scene -eq 'taijibase_constructedarmor_main_01') "$label scene mismatch"
        $expectedProfile = if ($Mode -eq 'Legacy') { 'Legacy' } else { 'PassAndPhase' }
        $expectedEnabled = $Mode -eq 'PassAndPhase'
        Assert-Condition ([string]$context.data.context.capture_config.profile -eq $expectedProfile) "$label profile mismatch"
        Assert-Condition ([int]$context.data.context.capture_config.callstack.domains.io.effective -eq 0) "$label I/O depth mismatch"

        $producer = Inspect $traceId 'producer.get' @{ key = 'io.structured' }
        Assert-Condition ([bool]$producer.data.producer.enabled -eq $expectedEnabled) "$label producer enabled mismatch"
        Assert-Condition ([bool]$producer.data.producer.effective -eq $expectedEnabled) "$label producer effective mismatch"
        Assert-Condition ([UInt64]$producer.data.producer.counters.emitted -eq 0) "$label unexpectedly emitted structured I/O"
        Assert-Condition ([UInt64]$producer.data.producer.counters.dropped -eq 0) "$label dropped structured I/O"
        Assert-Condition ([UInt64]$producer.data.producer.counters.overflow -eq 0) "$label overflowed structured I/O"
        if ($expectedEnabled) { Assert-Condition ([string]$producer.data.producer.state -eq 'real_zero') "$label is not real_zero" }

        $overview = Inspect $traceId 'trace.overview'
        $frames = $overview.data.primary_frame_statistics
        Assert-Condition ($null -ne $frames -and [UInt64]$frames.count -gt 0) "$label frame statistics absent"
        $validation = Inspect $traceId 'validation.run' @{ max_cpu_ms = 60000; max_scan_events = 100000000 }
        Assert-Condition ([bool]$validation.data.valid -and [UInt64]$validation.data.error_count -eq 0) "$label validation failed"
        return [ordered]@{
            label = $label
            frame_count = [UInt64]$frames.count
            frame_mean_ns = [double]$frames.mean_ns
            frame_p50_ns = [double]$frames.p50_ns
            frame_p95_ns = [double]$frames.p95_ns
            stream_bytes = [UInt64](Get-Item -LiteralPath $StreamPath).Length
            stream_bytes_per_frame = [Math]::Round((Get-Item -LiteralPath $StreamPath).Length / [double]$frames.count, 3)
            producer_state = [string]$producer.data.producer.state
            validation_errors = [UInt64]$validation.data.error_count
        }
    }
    finally { Close-Trace $traceId }
}

function Measure-Session([string] $Directory, [string] $Mode, [int] $Session) {
    $summaryPath = Join-Path $Directory "N13-$Mode-Capture-Summary.json"
    $summary = Get-Content -LiteralPath $summaryPath -Encoding UTF8 -Raw | ConvertFrom-Json
    Assert-Condition (-not [bool]$summary.forced_stop) "$Mode session $Session forced Unity termination"
    Assert-Condition ([string]$summary.build_settings_sha256_before -eq [string]$summary.build_settings_sha256_after) "$Mode session $Session changed buildSettings.json"
    $runs = @()
    foreach ($run in 1..3) {
        $trace = Join-Path $Directory "N13-$Mode-R$run.tracy"
        $stream = Join-Path $Directory "N13-$Mode-R$run.tracy-stream"
        Assert-Condition (Test-Path -LiteralPath $trace) "missing trace: $trace"
        Assert-Condition (Test-Path -LiteralPath $stream) "missing stream: $stream"
        $runs += Measure-Run $trace $stream $Mode $Session $run
    }
    return [ordered]@{
        session = $Session
        mode = $Mode
        forced_stop = [bool]$summary.forced_stop
        build_settings_sha256 = [string]$summary.build_settings_sha256_after
        runs = $runs
        median = [ordered]@{
            frame_mean_ns = Median @($runs | ForEach-Object { [double]$_.frame_mean_ns })
            frame_p50_ns = Median @($runs | ForEach-Object { [double]$_.frame_p50_ns })
            frame_p95_ns = Median @($runs | ForEach-Object { [double]$_.frame_p95_ns })
            stream_bytes_per_frame = Median @($runs | ForEach-Object { [double]$_.stream_bytes_per_frame })
        }
    }
}

try {
    Assert-Condition ($process.Start()) 'failed to start Query MCP server'
    $script:process = $process
    $init = Send-Rpc 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'jn-n13-file-io-paired-performance'; version = '1.0' } }
    Assert-Condition ([string]$init.result.protocolVersion -eq '2025-11-25') 'MCP initialize failed'
    $process.StandardInput.WriteLine((@{ jsonrpc = '2.0'; method = 'notifications/initialized'; params = @{} } | ConvertTo-Json -Compress))
    $process.StandardInput.Flush()

    $legacySessions = @(); $passSessions = @(); $pairs = @()
    foreach ($index in 0..2) {
        $session = $index + 1
        $legacy = Measure-Session $LegacySessionDirectories[$index] 'Legacy' $session
        $pass = Measure-Session $PassAndPhaseSessionDirectories[$index] 'PassAndPhase' $session
        $legacySessions += $legacy
        $passSessions += $pass
        $pairs += [ordered]@{
            pair = $session
            legacy_p95_ns = [double]$legacy.median.frame_p95_ns
            pass_and_phase_p95_ns = [double]$pass.median.frame_p95_ns
            p95_regression_percent = [Math]::Round((([double]$pass.median.frame_p95_ns / [double]$legacy.median.frame_p95_ns) - 1.0) * 100.0, 3)
            stream_bytes_per_frame_percent = [Math]::Round((([double]$pass.median.stream_bytes_per_frame / [double]$legacy.median.stream_bytes_per_frame) - 1.0) * 100.0, 3)
        }
    }
    $legacySessionMedian = Median @($legacySessions | ForEach-Object { [double]$_.median.frame_p95_ns })
    $passSessionMedian = Median @($passSessions | ForEach-Object { [double]$_.median.frame_p95_ns })
    $aggregateRegression = [Math]::Round((($passSessionMedian / $legacySessionMedian) - 1.0) * 100.0, 3)
    $pairedRegression = Median @($pairs | ForEach-Object { [double]$_.p95_regression_percent })
    $gatePassed = $pairedRegression -le 3.0
    $result = [ordered]@{
        ok = $gatePassed
        schema_version = '1.12.0'
        independent_sessions_per_mode = 3
        captures_per_session = 3
        workload = [ordered]@{ scene = 'taijibase_constructedarmor_main_01'; graphics_api = 'd3d12'; graphics_jobs = 'off'; stable_frames = 300; warmup_seconds = 45; capture_seconds = 10 }
        gate = [ordered]@{
            metric = 'median_of_three_paired_session_p95_regression_percent'
            limit_percent = 3.0
            measured_percent = $pairedRegression
            passed = $gatePassed
            aggregate_session_median_percent = $aggregateRegression
        }
        pairs = $pairs
        legacy_sessions = $legacySessions
        pass_and_phase_sessions = $passSessions
    }
    $parent = Split-Path -Parent $OutputFile
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($OutputFile, ($result | ConvertTo-Json -Depth 80) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    $result | ConvertTo-Json -Compress -Depth 18
    Assert-Condition $gatePassed "paired PassAndPhase P95 regression exceeds 3%: $pairedRegression%"
}
finally {
    foreach ($traceId in @($script:opened)) {
        if ($process -and -not $process.HasExited) { try { [void](Tool 'tracy_trace_close' @{ trace_id = $traceId }) } catch {} }
    }
    if ($process -and -not $process.HasExited) {
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $process.WaitForExit() }
    }
    if ($process) { $process.Dispose() }
}
