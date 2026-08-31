[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Convert', 'Finalize')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$ResolvedProfile,
    [Parameter(Mandatory = $true)][string]$TaskDirectory,
    [Parameter(Mandatory = $true)][string]$StreamPath,
    [Parameter(Mandatory = $true)][string]$OutputTrace,
    [string]$ValidationEvidence = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'JNTracySkill.Common.psm1') -Force

$profile = Read-JNJson -LiteralPath $ResolvedProfile
$taskRoot = New-JNTaskDirectories -TaskDirectory $TaskDirectory
$streamFull = [System.IO.Path]::GetFullPath($StreamPath)
$traceFull = [System.IO.Path]::GetFullPath($OutputTrace)
$candidate = $traceFull + '.converting'
$resultPath = Join-Path $taskRoot 'conversion-result.json'

if ($Mode -eq 'Convert') {
    if (-not (Test-Path -LiteralPath $streamFull -PathType Leaf)) {
        throw "Stream file not found: $streamFull"
    }
    if (Test-Path -LiteralPath $traceFull) {
        throw "Refusing to overwrite final trace: $traceFull"
    }
    if (Test-Path -LiteralPath $candidate) {
        throw "Candidate already exists: $candidate"
    }

    $converter = [string]$profile.toolchain.converter.path
    $stdoutPath = Join-Path $taskRoot 'logs\converter.stdout.log'
    $stderrPath = Join-Path $taskRoot 'logs\converter.stderr.log'
    foreach ($path in @($stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $path) {
            throw "Refusing to overwrite converter log: $path"
        }
    }

    $arguments = @('-i', $streamFull, '-o', $candidate)
    $process = Start-Process -FilePath $converter -ArgumentList (Join-JNProcessArguments -Values $arguments) -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
    $processIdentity = Get-JNProcessIdentity -Id $process.Id
    $processRecord = [ordered]@{
        schema_version = 1
        state = 'converting'
        started_at_utc = Get-JNUtcNow
        process = $processIdentity
        executable = Get-JNFileIdentity -LiteralPath $converter
        input_stream = Get-JNFileIdentity -LiteralPath $streamFull
        candidate_path = $candidate
        stdout_log = $stdoutPath
        stderr_log = $stderrPath
    }
    Write-JNJsonAtomic -LiteralPath (Join-Path $taskRoot 'converter-process.json') -Value $processRecord

    $process.WaitForExit()
    $exitCode = $process.ExitCode
    if ($exitCode -ne 0 -or -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $failedPath = ''
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $failedPath = Join-Path $taskRoot ('failed-artifacts\' + [System.IO.Path]::GetFileName($candidate) + '.' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))
            Move-Item -LiteralPath $candidate -Destination $failedPath
        }
        $failure = [ordered]@{
            schema_version = 1
            state = 'conversion_failed'
            exit_code = $exitCode
            failed_candidate = $failedPath
            stream = Get-JNFileIdentity -LiteralPath $streamFull
            stdout_log = $stdoutPath
            stderr_log = $stderrPath
        }
        Write-JNJsonAtomic -LiteralPath $resultPath -Value $failure
        $failure | ConvertTo-Json -Depth 20
        exit 2
    }

    $success = [ordered]@{
        schema_version = 1
        state = 'candidate_ready_for_mcp_validation'
        exit_code = $exitCode
        stream = Get-JNFileIdentity -LiteralPath $streamFull
        candidate = Get-JNFileIdentity -LiteralPath $candidate
        final_trace_path = $traceFull
        stdout_log = $stdoutPath
        stderr_log = $stderrPath
    }
    Write-JNJsonAtomic -LiteralPath $resultPath -Value $success
    $success | ConvertTo-Json -Depth 20
    exit 0
}

if (-not $ValidationEvidence) {
    throw 'Finalize requires -ValidationEvidence.'
}
if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
    throw "Candidate not found: $candidate"
}
if (Test-Path -LiteralPath $traceFull) {
    throw "Refusing to overwrite final trace: $traceFull"
}
$validation = Read-JNJson -LiteralPath $ValidationEvidence
$candidateIdentity = Get-JNFileIdentity -LiteralPath $candidate
if ($validation.publishable -ne $true) {
    throw 'MCP validation did not mark the candidate publishable.'
}
if ([string]$validation.trace_sha256 -ne [string]$candidateIdentity.sha256) {
    throw 'MCP validation trace SHA-256 does not match the candidate.'
}
if ([string]$validation.protocol -ne '90' -or [string]$validation.query_schema -ne '1.32.0') {
    throw 'MCP validation does not prove Protocol 90 and Query 1.32.0.'
}
if ([string]$validation.completeness -notin @('Complete', 'RecoverablePrefix')) {
    throw 'MCP validation completeness is not publishable.'
}

Move-Item -LiteralPath $candidate -Destination $traceFull
$published = [ordered]@{
    schema_version = 1
    state = 'published'
    published_at_utc = Get-JNUtcNow
    completeness = [string]$validation.completeness
    validation_evidence = (Resolve-Path -LiteralPath $ValidationEvidence).Path
    trace = Get-JNFileIdentity -LiteralPath $traceFull
    stream = Get-JNFileIdentity -LiteralPath $streamFull
}
Write-JNJsonAtomic -LiteralPath $resultPath -Value $published
$published | ConvertTo-Json -Depth 20
