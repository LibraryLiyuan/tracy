#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $ProducerExe,
    [Parameter(Mandatory = $true)][string] $CaptureExe,
    [Parameter(Mandatory = $true)][string] $ConverterExe,
    [Parameter(Mandatory = $true)][string] $QueryExe,
    [Parameter(Mandatory = $true)][string] $SemanticCompareScript,
    [Parameter(Mandatory = $true)][string] $OutputRoot,
    [ValidateRange(1, 1000000)][int] $PostEndBatches,
    [ValidateSet('Linear', 'Quadratic')][string] $Expectation = 'Linear',
    [ValidateSet('DirectAndStream', 'StreamOnly')][string] $CaptureMode = 'DirectAndStream',
    [ValidateRange(2, 120)][int] $CaptureSeconds = 5,
    [switch] $UnresolvedEnrichment,
    [switch] $UnresolvedCore,
    [UInt64] $ExpectedTotalUnresolved = 0,
    [UInt64] $ExpectedCoreUnresolved = 0
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

foreach ($path in @($ProducerExe, $CaptureExe, $ConverterExe, $QueryExe, $SemanticCompareScript, $OutputRoot)) {
    Assert-Condition (Test-Path -LiteralPath $path) "required path does not exist: $path"
}

$caseRoot = Join-Path (Resolve-Path -LiteralPath $OutputRoot).Path (
    "gpu-catalog-$PostEndBatches-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $caseRoot)

$streamPath = Join-Path $caseRoot 'capture.tracy-stream'
$directPath = Join-Path $caseRoot 'direct.tracy'
$convertedPath = Join-Path $caseRoot 'converted.tracy'
$converterReportPath = Join-Path $caseRoot 'converter-report.json'
$semanticReportPath = Join-Path $caseRoot 'semantic-report.json'
$producerStdout = Join-Path $caseRoot 'producer.stdout.log'
$producerStderr = Join-Path $caseRoot 'producer.stderr.log'
$captureStdout = Join-Path $caseRoot 'capture.stdout.log'
$captureStderr = Join-Path $caseRoot 'capture.stderr.log'
$converterStdout = Join-Path $caseRoot 'converter.stdout.log'
$converterStderr = Join-Path $caseRoot 'converter.stderr.log'

$producer = $null
$capture = $null
try {
    $producerArguments = @(
        '--duration-seconds', [string]($CaptureSeconds + 2),
        '--gpu-catalog-post-end-batches', [string]$PostEndBatches
    )
    if ($UnresolvedEnrichment) { $producerArguments += '--gpu-catalog-unresolved-enrichment' }
    if ($UnresolvedCore) { $producerArguments += '--gpu-catalog-unresolved-core' }
    $producerOptions = @{
        FilePath = (Resolve-Path -LiteralPath $ProducerExe).Path
        ArgumentList = $producerArguments
        WorkingDirectory = (Split-Path (Resolve-Path -LiteralPath $ProducerExe).Path)
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = $producerStdout
        RedirectStandardError = $producerStderr
    }
    $producer = Start-Process @producerOptions

    Start-Sleep -Milliseconds 300

    $captureArguments = @('-j', $streamPath, '-s', [string]$CaptureSeconds, '-f')
    if ($CaptureMode -eq 'DirectAndStream') {
        $captureArguments = @('-o', $directPath) + $captureArguments
    }
    $captureOptions = @{
        FilePath = (Resolve-Path -LiteralPath $CaptureExe).Path
        ArgumentList = $captureArguments
        WorkingDirectory = (Split-Path (Resolve-Path -LiteralPath $CaptureExe).Path)
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = $captureStdout
        RedirectStandardError = $captureStderr
    }
    $capture = Start-Process @captureOptions

    Assert-Condition ($capture.WaitForExit(($CaptureSeconds + 60) * 1000)) 'capture timed out'
    Assert-Condition ($capture.ExitCode -eq 0) "capture failed with exit code $($capture.ExitCode)"
    Assert-Condition (Test-Path -LiteralPath $streamPath -PathType Leaf) 'stream was not created'
    if ($CaptureMode -eq 'DirectAndStream') {
        Assert-Condition (Test-Path -LiteralPath $directPath -PathType Leaf) 'direct trace was not created'
    }

    if (-not $producer.HasExited) {
        if (-not $producer.WaitForExit(5000)) {
            Stop-Process -Id $producer.Id -Force
            $producer.WaitForExit()
        }
    }
    Assert-Condition ($producer.ExitCode -eq 0) "producer failed with exit code $($producer.ExitCode)"

    $converterStart = [DateTime]::UtcNow
    $converterOptions = @{
        FilePath = (Resolve-Path -LiteralPath $ConverterExe).Path
        ArgumentList = @(
            '-i', $streamPath,
            '-o', $convertedPath,
            '-f',
            '--diagnostics',
            '--report-json', $converterReportPath
        )
        WorkingDirectory = (Split-Path (Resolve-Path -LiteralPath $ConverterExe).Path)
        WindowStyle = 'Hidden'
        PassThru = $true
        Wait = $true
        RedirectStandardOutput = $converterStdout
        RedirectStandardError = $converterStderr
    }
    $converterProcess = Start-Process @converterOptions
    $converterSeconds = ([DateTime]::UtcNow - $converterStart).TotalSeconds

    Assert-Condition ($converterProcess.ExitCode -eq 0) "converter failed with exit code $($converterProcess.ExitCode)"
    Assert-Condition (Test-Path -LiteralPath $convertedPath -PathType Leaf) 'converted trace was not created'
    Assert-Condition (Test-Path -LiteralPath $converterReportPath -PathType Leaf) 'converter report was not created'

    $report = Get-Content -LiteralPath $converterReportPath -Raw | ConvertFrom-Json
    $resolver = $report.gpu_catalog_resolver
    Assert-Condition ($null -ne $resolver) 'converter report omitted gpu_catalog_resolver'

    [UInt64]$calls = [UInt64]$resolver.full_resolve_calls
    [UInt64]$generationEndCalls = [UInt64]$resolver.generation_end_calls
    [UInt64]$postEndCalls = [UInt64]$resolver.post_end_batch_calls
    [UInt64]$finalizeCalls = [UInt64]$resolver.finalize_save_calls
    [UInt64]$inputUnits = [UInt64]$resolver.input_units
    [UInt64]$totalUnresolved = [UInt64]$resolver.total_unresolved
    [UInt64]$coreUnresolved = [UInt64]$resolver.core_unresolved
    Assert-Condition ($totalUnresolved -eq $ExpectedTotalUnresolved) (
        "expected $ExpectedTotalUnresolved total unresolved records, found $totalUnresolved")
    Assert-Condition ($coreUnresolved -eq $ExpectedCoreUnresolved) (
        "expected $ExpectedCoreUnresolved unresolved Catalog Core records, found $coreUnresolved")

    if ($Expectation -eq 'Linear') {
        Assert-Condition ($calls -eq 1) "expected one full resolver call, found $calls"
        Assert-Condition ($generationEndCalls -eq 0) "GenerationEnd triggered $generationEndCalls full resolves"
        Assert-Condition ($postEndCalls -eq 0) "post-End batches triggered $postEndCalls full resolves"
        Assert-Condition ($finalizeCalls -eq 1) "expected one final-save resolve, found $finalizeCalls"
    }
    else {
        Assert-Condition ($calls -gt 1) "quadratic baseline unexpectedly made only $calls resolve calls"
        Assert-Condition ($generationEndCalls -eq 1) "quadratic baseline expected one GenerationEnd resolve"
        Assert-Condition ($postEndCalls -eq [UInt64]$PostEndBatches) (
            "quadratic baseline expected $PostEndBatches post-End resolves, found $postEndCalls")
        Assert-Condition ($finalizeCalls -eq 1) "quadratic baseline expected one final-save resolve"
    }

    & $QueryExe --doctor --trace $convertedPath --allow-root $caseRoot | Out-Null
    Assert-Condition ($LASTEXITCODE -eq 0) 'tracy-query doctor rejected converted trace'

    if ($CaptureMode -eq 'DirectAndStream') {
        $compareOptions = @{
            QueryExe = $QueryExe
            BaselineTrace = $directPath
            CandidateTrace = $convertedPath
            AllowRoot = $caseRoot
            ReportJson = $semanticReportPath
            AllowSyntheticIncompleteContext = $true
        }
        & $SemanticCompareScript @compareOptions
        Assert-Condition ($LASTEXITCODE -eq 0) 'direct/converted semantic comparison failed'
    }

    $result = [ordered]@{
        schema = 1
        status = 'PASS'
        expectation = $Expectation
        capture_mode = $CaptureMode
        post_end_batches = $PostEndBatches
        converter_seconds = $converterSeconds
        full_resolve_calls = $calls
        generation_end_calls = $generationEndCalls
        post_end_batch_calls = $postEndCalls
        finalize_save_calls = $finalizeCalls
        input_units = $inputUnits
        total_unresolved = $totalUnresolved
        core_unresolved = $coreUnresolved
        peak_working_set_bytes = [UInt64]$report.peak_working_set_bytes
        stream = $streamPath
        direct_trace = if ($CaptureMode -eq 'DirectAndStream') { $directPath } else { $null }
        converted_trace = $convertedPath
        converter_report = $converterReportPath
        semantic_report = if ($CaptureMode -eq 'DirectAndStream') { $semanticReportPath } else { $null }
    }
    $resultPath = Join-Path $caseRoot 'result.json'
    $result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resultPath -Encoding utf8NoBOM
    Write-Host "RESULT=PASS ARTIFACT_ROOT=$caseRoot RESOLVE_CALLS=$calls INPUT_UNITS=$inputUnits CONVERTER_SECONDS=$converterSeconds"
}
finally {
    if ($null -ne $capture -and -not $capture.HasExited) {
        Stop-Process -Id $capture.Id -Force
        $capture.WaitForExit()
    }
    if ($null -ne $producer -and -not $producer.HasExited) {
        Stop-Process -Id $producer.Id -Force
        $producer.WaitForExit()
    }
}
