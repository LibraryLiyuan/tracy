[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QueryExe,
    [Parameter(Mandatory = $true)][string]$SnapshotTrace,
    [Parameter(Mandatory = $true)][string]$StreamTrace,
    [Parameter(Mandatory = $true)][string]$AllowRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($QueryExe, $SnapshotTrace, $StreamTrace, $AllowRoot))
{
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

function Invoke-Query(
    [string]$Trace,
    [string]$Id,
    [string]$Method,
    [hashtable]$Params = @{})
{
    $request = [ordered]@{
        protocol = 'tracy-query/1'
        id = $Id
        method = $Method
        params = $Params
    } | ConvertTo-Json -Compress -Depth 12

    $watch = [Diagnostics.Stopwatch]::StartNew()
    $raw = $request | & $QueryExe --trace $Trace --request - --indexed --allow-root $AllowRoot
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    Assert-Condition ($exitCode -eq 0) "$Method failed for $Trace with exit code $exitCode"
    $response = $raw | ConvertFrom-Json
    Assert-Condition ([bool]$response.ok) "$Method returned ok=false for $Trace"
    return [ordered]@{ elapsed_ms = $watch.ElapsedMilliseconds; data = $response.data }
}

function Get-TraceSummary([string]$Trace)
{
    $timings = [ordered]@{}

    $countsResult = Invoke-Query $Trace 'counts' 'trace.counts'
    $timings.trace_counts = $countsResult.elapsed_ms

    $imagesResult = Invoke-Query $Trace 'images' 'frame_image.list' @{ limit = 1000 }
    $timings.frame_image_list = $imagesResult.elapsed_ms
    $images = @($imagesResult.data.images | Sort-Object raw_frame_index | ForEach-Object {
        [ordered]@{
            raw_frame_index = [string]$_.raw_frame_index
            width = [int]$_.width
            height = [int]$_.height
            raw_bc1_bytes = [string]$_.raw_bc1_bytes
            flipped = [bool]$_.flipped
        }
    })

    $scriptResult = Invoke-Query $Trace 'script' 'runtime.script.summary'
    $timings.runtime_script_summary = $scriptResult.elapsed_ms

    $jobResult = Invoke-Query $Trace 'jobs' 'job.statistics' @{
        max_scan_events = 100000000
        max_cpu_ms = 60000
    }
    $timings.job_statistics = $jobResult.elapsed_ms

    $gpuResult = Invoke-Query $Trace 'gpu' 'memory.gpu.summary'
    $timings.gpu_memory_summary = $gpuResult.elapsed_ms

    $attributionResult = Invoke-Query $Trace 'attribution' 'memory.gpu.attribution' @{ limit = 1 }
    $timings.gpu_memory_attribution = $attributionResult.elapsed_ms

    $costResult = Invoke-Query $Trace 'cost' 'trace.telemetry_cost'
    $timings.telemetry_cost = $costResult.elapsed_ms

    $coverageResult = Invoke-Query $Trace 'coverage' 'capture.coverage'
    $timings.capture_coverage = $coverageResult.elapsed_ms
    $producers = @($coverageResult.data.producers | Sort-Object key | ForEach-Object {
        [ordered]@{
            key = [string]$_.key
            state = [string]$_.state
            complete = [bool]$_.complete
            counters = $_.counters
        }
    })

    $gpu = $gpuResult.data
    $attribution = $attributionResult.data
    $summary = [ordered]@{
        trace_counts = $countsResult.data
        frame_images = $images
        script = [ordered]@{
            complete = [bool]$scriptResult.data.complete
            counts = $scriptResult.data.counts
            quality = $scriptResult.data.quality
        }
        job = [ordered]@{
            schema_version = [string]$jobResult.data.job_schema_version
            counts = $jobResult.data.counts
            quality = $jobResult.data.quality
        }
        gpu_memory = [ordered]@{
            protocol = [string]$gpu.protocol
            logical_resource_count = [string]$gpu.logical_resource_count
            working_set_count = [string]$gpu.working_set_count
            owner_rollup_count = [string]$gpu.owner_rollup_count
            quality = $gpu.quality
            attribution = [ordered]@{
                complete = [bool]$attribution.complete
                pass_count = [string]$attribution.pass_count
                logical_resource_count = [string]$attribution.logical_resource_count
                working_set_count = [string]$attribution.working_set_count
                owner_rollup_count = [string]@($attribution.owner_rollups).Count
            }
        }
        telemetry_totals = $costResult.data.totals
        producers = $producers
    }

    return [ordered]@{ timings_ms = $timings; summary = $summary }
}

$snapshotWatch = [Diagnostics.Stopwatch]::StartNew()
$snapshot = Get-TraceSummary $SnapshotTrace
$snapshotWatch.Stop()

$streamWatch = [Diagnostics.Stopwatch]::StartNew()
$stream = Get-TraceSummary $StreamTrace
$streamWatch.Stop()

$snapshotCanonical = $snapshot.summary | ConvertTo-Json -Compress -Depth 40
$streamCanonical = $stream.summary | ConvertTo-Json -Compress -Depth 40
$equal = $snapshotCanonical -ceq $streamCanonical

$result = [ordered]@{
    passed = $equal
    comparison = 'byte-exact canonical JSON over selected persisted evidence domains'
    snapshot = [ordered]@{
        path = (Get-Item -LiteralPath $SnapshotTrace).FullName
        sha256 = (Get-FileHash -LiteralPath $SnapshotTrace -Algorithm SHA256).Hash.ToLowerInvariant()
        total_ms = $snapshotWatch.ElapsedMilliseconds
        timings_ms = $snapshot.timings_ms
    }
    stream = [ordered]@{
        path = (Get-Item -LiteralPath $StreamTrace).FullName
        sha256 = (Get-FileHash -LiteralPath $StreamTrace -Algorithm SHA256).Hash.ToLowerInvariant()
        total_ms = $streamWatch.ElapsedMilliseconds
        timings_ms = $stream.timings_ms
    }
    evidence = $snapshot.summary
}

$parent = Split-Path -Parent $OutputFile
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
$result | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $OutputFile -Encoding UTF8
[ordered]@{
    passed = $equal
    output = (Get-Item -LiteralPath $OutputFile).FullName
    snapshot_total_ms = $snapshotWatch.ElapsedMilliseconds
    stream_total_ms = $streamWatch.ElapsedMilliseconds
    frame_images = @($snapshot.summary.frame_images).Count
    script_zones = [string]$snapshot.summary.script.counts.zones
    jobs = [string]$snapshot.summary.job.counts.jobs
    gpu_reference_passes = [string]$snapshot.summary.gpu_memory.attribution.pass_count
    gpu_logical_resources = [string]$snapshot.summary.gpu_memory.logical_resource_count
    producer_count = @($snapshot.summary.producers).Count
} | ConvertTo-Json -Compress

Assert-Condition $equal 'snapshot and indexed stream evidence summaries differ'
