param(
    [Parameter(Mandatory=$true)][string]$Converter,
    [Parameter(Mandatory=$true)][string]$InputStream,
    [Parameter(Mandatory=$true)][string]$Root
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Root){throw 'finalize_cancel_root_exists'}
New-Item -ItemType Directory -Path $Root | Out-Null
$input=Join-Path $Root 'capture.tracy-stream';Copy-Item -LiteralPath $InputStream -Destination $input
$output=Join-Path $Root 'out.tracy'
& $Converter -i $input -o $output --compression fast --threads 1 --require-clean-end --test-cancel-finalize-after-rows 2 --process-memory-limit 21474836480
if($LASTEXITCODE -ne 130){throw "finalize_cancel_exit_$LASTEXITCODE"}
$progress=Get-Content -Raw -LiteralPath ($output+'.progress.json') | ConvertFrom-Json
$resources=Get-Content -Raw -LiteralPath ($output+'.resources.json') | ConvertFrom-Json
if($progress.stage -ne 'Cancelled' -or -not $progress.cancel_requested){throw 'finalize_cancel_progress'}
if($resources.success -or $resources.failure -ne 'conversion_cancelled' -or -not $resources.cleanup_complete){throw 'finalize_cancel_report'}
if((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath ($output+'.converting')) -or (Test-Path -LiteralPath ($output+'.work'))){throw 'finalize_cancel_residual'}
Write-Output 'Finalize cooperative cancellation, report and cleanup passed'
