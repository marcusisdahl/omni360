param([switch]$Clean, [switch]$Tests)
# Builds Omni360.xex (Release | Xbox 360) with the newest Visual Studio's
# MSBuild and the Xbox 360 SDK - Visual Studio 2022 with the RXDK
# integration, or 2010 with the official XDK - then copies it, with a
# settings.txt template, into dist\.
#
#   .\build.ps1            build
#   .\build.ps1 -Clean     rebuild everything, the ATG framework included
#   .\build.ps1 -Tests     also build and run tools\test_usenet on the PC
#
# Nothing is sent to a console: DeploymentType=None skips the copy to a dev
# kit, which a retail console with homebrew doesn't have.
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!$env:XEDK -or !(Test-Path -LiteralPath $env:XEDK)) { throw 'The Xbox 360 SDK is required (XEDK is not set).' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    if (!$vsPath) { throw 'Install Visual Studio with MSBuild.' }
    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'

    $target = if ($Clean) { 'Rebuild' } else { 'Build' }
    & $msbuild Omni360.sln "/t:$target" /p:Configuration=Release '/p:Platform=Xbox 360' /p:DeploymentType=None /m /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Xbox compilation failed. Check the SDK and RXDK integration.' }

    # MSBuild 17 writes its tracking logs beside the projects; they aren't source.
    Get-ChildItem -Path . -Filter *.tlog -File | Remove-Item -Force
    Get-ChildItem -Path Common -Filter *.tlog -File | Remove-Item -Force

    New-Item -ItemType Directory -Force dist | Out-Null
    Copy-Item -Force Release\Omni360.xex dist\Omni360.xex
    Copy-Item -Force tools\settings.example.txt dist\settings.example.txt

    if ($Tests) {
        Push-Location tools\test_usenet
        try {
            & python make_test_data.py
            if ($LASTEXITCODE) { throw 'Making the test data failed.' }
            # By its full path: cmd starts in the process's folder, not
            # Push-Location's. vcvars32.bat calls vswhere by name, so its folder
            # goes on PATH; and cl's chatter on stderr isn't an error here -
            # the exit code says how it went.
            $bat = Join-Path (Get-Location) 'build_and_run.bat'
            $env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
            $ErrorActionPreference = 'Continue'
            & $env:ComSpec /d /c "`"$bat`"" 2>&1 | ForEach-Object { "$_" }
            $failed = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($failed) { throw 'tools\test_usenet failed.' }
        } finally { Pop-Location }
    }

    $xex = Get-Item dist\Omni360.xex
    Write-Host ("Built dist\Omni360.xex ({0:N0} bytes, {1}). Nothing was deployed." -f $xex.Length, $xex.LastWriteTime)
} finally { Pop-Location }
