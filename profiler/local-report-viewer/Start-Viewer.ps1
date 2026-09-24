[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Config,
 [Parameter(Mandatory=$true)][string]$WebRoot,
 [string]$Python = 'python',
 [int]$Port = 8140,
 [Parameter(Mandatory=$true)][string]$StateFile,
 [string]$ReportDir
)
$ErrorActionPreference='Stop'
$configPath=(Resolve-Path -LiteralPath $Config).Path
$webPath=(Resolve-Path -LiteralPath $WebRoot).Path
$statePath=[IO.Path]::GetFullPath($StateFile)
$stateDirectory=Split-Path -Parent $statePath
New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
function Complete-Startup($url) {
 if($ReportDir){& $Python -X utf8 -B (Join-Path $PSScriptRoot 'refresh_links.py') --state $statePath --report-dir $ReportDir;if($LASTEXITCODE -ne 0){throw 'Failed to refresh generated Markdown links'}}
 Write-Output $url
}
$configIdentity=(Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash.ToLowerInvariant()
$bundleFile=Join-Path $webPath 'viewer-build.json'
$bundleIdentity=if(Test-Path -LiteralPath $bundleFile){(Get-FileHash -LiteralPath $bundleFile -Algorithm SHA256).Hash.ToLowerInvariant()}else{$null}
if(Test-Path -LiteralPath $statePath){
 try{
  $existing=Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
  if($existing.url -match '^http://127\.0\.0\.1:[0-9]+$'){
   $caps=Invoke-RestMethod -Uri ($existing.url+'/api/capabilities') -TimeoutSec 3
   if($caps.registry_sha256 -eq $configIdentity -and $caps.bundle_identity -eq $bundleIdentity){Complete-Startup $existing.url;return}
  }
 }catch{}
}
$scriptPath=Join-Path $PSScriptRoot 'viewer_server.py'
$arguments=@('-X','utf8','-B',('"'+$scriptPath+'"'),'--config',('"'+$configPath+'"'),'--web-root',('"'+$webPath+'"'),'--port',"$Port",'--state',('"'+$statePath+'"'))
$logTag=[guid]::NewGuid().ToString('N')
$process=Start-Process -FilePath $Python -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $stateDirectory "viewer-$logTag.stdout.log") -RedirectStandardError (Join-Path $stateDirectory "viewer-$logTag.stderr.log")
$deadline=[DateTime]::UtcNow.AddSeconds(45)
while([DateTime]::UtcNow -lt $deadline){
 if($process.HasExited){throw "Viewer service exited; inspect viewer-$logTag.stderr.log"}
 if(Test-Path -LiteralPath $statePath){
  $state=Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
  if($state.pid -eq $process.Id){Complete-Startup $state.url;return}
 }
 Start-Sleep -Milliseconds 200
}
throw 'Viewer start timeout; inspect the saved service logs.'
