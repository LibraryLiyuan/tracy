param(
    [Parameter(Mandatory=$true)][string]$Converter,
    [Parameter(Mandatory=$true)][string]$InputStream,
    [Parameter(Mandatory=$true)][string]$Root
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Root){throw 'finalize_progress_root_exists'}
New-Item -ItemType Directory -Path $Root | Out-Null
$input=Join-Path $Root 'capture.tracy-stream'
Copy-Item -LiteralPath $InputStream -Destination $input
$sourceStop=[IO.Path]::ChangeExtension($InputStream,'.stop')
if(Test-Path -LiteralPath $sourceStop){Copy-Item -LiteralPath $sourceStop -Destination ([IO.Path]::ChangeExtension($input,'.stop'))}
$output=Join-Path $Root 'out.tracy'
$report=$output+'.conversion.json'
& $Converter -i $input -o $output --compression fast --threads 1 --require-clean-end --report-json $report --process-memory-limit 21474836480
if($LASTEXITCODE -ne 0){throw "finalize_progress_conversion_exit_$LASTEXITCODE"}
$progress=Get-Content -Raw -LiteralPath ($output+'.progress.json') | ConvertFrom-Json
$resources=Get-Content -Raw -LiteralPath ($output+'.resources.json') | ConvertFrom-Json
$conversion=Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
foreach($name in 'task_disk_current_bytes','task_disk_peak_bytes','task_disk_limit_bytes','workspace_current_bytes','workspace_peak_bytes','workspace_limit_bytes'){
    if($null -eq $progress.$name){throw "finalize_progress_missing_$name"}
}
if($null -eq $resources.evidence.pass_time_build){throw 'finalize_progress_missing_pass_time_build'}
if($null -eq $resources.evidence.catalog_batch_directory){throw 'finalize_progress_missing_catalog_batch_directory'}
if($null -eq $resources.evidence.catalog_interval_activity){throw 'finalize_progress_missing_catalog_interval_activity'}
if($null -eq $resources.evidence.replay_storage -or $null -eq $resources.evidence.replay_storage.paged_scratch_grows){throw 'finalize_progress_missing_replay_storage'}
if($resources.evidence.catalog_interval_activity.storage_backend -ne 'small-plus-mapped-arena-v1'){throw 'finalize_progress_interval_backend'}
if($null -eq $resources.evidence.catalog_interval_activity.small_interval_bytes -or $null -eq $resources.evidence.catalog_interval_activity.mapped_interval_bytes -or $null -eq $resources.evidence.catalog_interval_activity.mapped_interval_chunks){throw 'finalize_progress_interval_mapping_evidence'}
if($null -eq $conversion.finalize_substages){throw 'finalize_progress_missing_substages'}
if(@($conversion.finalize_substages.PSObject.Properties).Count -lt 4){throw 'finalize_progress_incomplete_substages'}
if($null -eq $conversion.write_pipeline -or [uint64]$conversion.write_pipeline.logical_blocks -eq 0 -or [uint64]$conversion.write_pipeline.compression_blocks -eq 0 -or [uint64]$conversion.write_pipeline.physical_writes -eq 0){throw 'finalize_progress_missing_write_pipeline'}
if($null -eq $conversion.write_substages -or @($conversion.write_substages.PSObject.Properties).Count -lt 5){throw 'finalize_progress_missing_write_substages'}
if($null -eq $conversion.replay_pipeline -or [uint64]$conversion.replay_pipeline.client_payload_bytes -eq 0 -or @($conversion.replay_pipeline.event_samples).Count -eq 0){throw 'finalize_progress_missing_replay_pipeline'}
if(-not $resources.success -or -not $resources.cleanup_complete){throw 'finalize_progress_cleanup'}
Write-Output 'Finalize progress, resource accounting and substage evidence passed'
