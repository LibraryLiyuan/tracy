param(
    [string]$CMakeExe='cmake',
    [string]$BuildRoot='',
    [string]$OutputDirectory='',
    [string]$DependencyCache='',
    [string]$InitialCache='',
    [string]$RuntimeDirectory=''
)
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(!$BuildRoot){$BuildRoot=Join-Path $repository 'build-capture-gui'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $BuildRoot ('package-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Output directory already exists; choose a new directory.'}
if(!$RuntimeDirectory){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(Test-Path -LiteralPath $vswhere){$vsRoot=& $vswhere -latest -products '*' -property installationPath; $redist=Join-Path $vsRoot 'VC/Redist/MSVC'; if(Test-Path -LiteralPath $redist){$version=Get-ChildItem -LiteralPath $redist -Directory | Where-Object Name -match '^\d' | Sort-Object Name -Descending | Select-Object -First 1;$RuntimeDirectory=Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory -Filter '*.CRT' | Select-Object -First 1 -ExpandProperty FullName}}
}
if(!$RuntimeDirectory -or !(Test-Path -LiteralPath $RuntimeDirectory)){throw 'Specify -RuntimeDirectory pointing to the redistributable x64 Microsoft Visual C++ CRT directory.'}
foreach($entry in @(@{Source='capture-gui';Targets=@('tracy-capture-gui','capture-gui-core-tests')},@{Source='capture';Targets=@('tracy-capture','tracy-stream-convert')},@{Source='query';Targets=@('tracy-query')})){
    $build=Join-Path $BuildRoot $entry.Source
    $options=@('-S',(Join-Path $repository $entry.Source),'-B',$build,'-A','x64','-DNO_ISA_EXTENSIONS=ON','-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF')
    if($DependencyCache){$options+=('-DCPM_SOURCE_CACHE='+$DependencyCache)}
    if($InitialCache){$options+=@('-C',$InitialCache)}
    & $CMakeExe @options
    if($LASTEXITCODE -ne 0){throw "Configure failed: $($entry.Source)"}
    & $CMakeExe --build $build --config Release --target @($entry.Targets) --parallel 8
    if($LASTEXITCODE -ne 0){throw "Build failed: $($entry.Source)"}
}
& (Join-Path $BuildRoot 'capture-gui/Release/capture-gui-core-tests.exe')
if($LASTEXITCODE -ne 0){throw 'Core tests failed'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
foreach($file in @('capture-gui/Release/tracy-capture-gui.exe','capture/Release/tracy-capture.exe','capture/Release/tracy-stream-convert.exe','query/Release/tracy-query.exe')){
    Copy-Item -LiteralPath (Join-Path $BuildRoot $file) -Destination $OutputDirectory
}
Copy-Item -LiteralPath (Join-Path $repository 'capture-gui/README.md') -Destination $OutputDirectory
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File | Copy-Item -Destination $OutputDirectory
$revision=git -C $repository rev-parse HEAD
$manifest=[ordered]@{version='1.0.0';revision=$revision;query_schema='1.35.0';platform='Windows x64';files=@()}
Get-ChildItem -LiteralPath $OutputDirectory -File | ForEach-Object {$manifest.files+=@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower();bytes=$_.Length}}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'BUILD-MANIFEST.json') -Encoding utf8
Compress-Archive -LiteralPath $OutputDirectory -DestinationPath ($OutputDirectory+'.zip')
Write-Output $OutputDirectory
