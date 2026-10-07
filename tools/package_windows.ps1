# Packages a built tabletennis.exe into TableTennisRecomp-Windows.zip. The zip
# carries no game files: the app asks for the user's own ISO on first launch
# and extracts it next to the exe.
#
# usage: tools/package_windows.ps1 [-Build <build_dir>] [-Out <output_dir>]
param(
    [string]$Build = "$PSScriptRoot/../out/build/win-amd64-release",
    [string]$Out = "$PSScriptRoot/../out/package"
)
$ErrorActionPreference = "Stop"

$Build = (Resolve-Path $Build).Path
$Exe = Join-Path $Build "tabletennis.exe"
if (-not (Test-Path $Exe)) { throw "no tabletennis.exe in $Build" }

$Name = "TableTennisRecomp"
$Stage = Join-Path $Out $Name
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null

# The SDK's post-build step stages every runtime DLL next to the exe.
Copy-Item $Exe $Stage
Get-ChildItem $Build -Filter *.dll | Copy-Item -Destination $Stage

# Fail rather than ship a binary that names the machine it was built on.
$UserDir = [Environment]::GetFolderPath("UserProfile")
foreach ($File in Get-ChildItem $Stage -File) {
    if (Select-String -Path $File.FullName -SimpleMatch $UserDir -Quiet) {
        throw "refusing to package: $UserDir appears in $($File.Name)"
    }
}

$Zip = Join-Path $Out "$Name-Windows.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $Stage -DestinationPath $Zip
Write-Output $Zip
