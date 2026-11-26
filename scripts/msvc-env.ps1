# Run a command inside the Visual Studio x64 developer environment.
#
#   pwsh scripts/msvc-env.ps1 meson setup build/msvc --buildtype=release
#   pwsh scripts/msvc-env.ps1 meson compile -C build/msvc
#
# Uses vswhere to find the newest Visual Studio with the C++ x64 tools and
# imports vcvars64.bat into this process before running the command.
# No param() block: every argument, including ones like -C, is passed through.
$Command = @($args)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 with the C++ workload' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the x64 C++ tools found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not $env:VSCMD_ARG_TGT_ARCH) {
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}
if ($Command.Count -eq 0) { Write-Output "MSVC environment: $env:VSCMD_ARG_TGT_ARCH"; exit 0 }
& $Command[0] $Command[1..($Command.Count - 1)]
exit $LASTEXITCODE
