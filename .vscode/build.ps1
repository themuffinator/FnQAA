<#
Builds FnQAA for VS Code's tasks (after FnQL's .vscode/build-release.ps1).

Imports the Visual Studio x64 developer environment, configures (or
reconfigures) the Meson build directory and builds the runtime targets: the
client, its renderer modules and the dedicated server. -RunTests builds and
runs every test instead (headless, as meson test always is).

-Recomp first recompiles QAA's game modules and its Scaleform front end from
the user's package with tools/recomp/build.py (README.md); -RecompOnly does
only that. The package comes from -Package, $env:QAA_PACKAGE or, by default,
ref\package\584108C8\000D0000\0ED9DD87... in this repository. The engine
finds the recompiled libraries in build\recomp\native\bin, beside every
build directory.
#>
[CmdletBinding()]
param(
	[ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
	[string]$Configuration = 'Release',

	[ValidateSet('client', 'dedicated', 'both')]
	[string]$Target = 'both',

	[string]$Renderers = 'glx,vk,rtx',

	[ValidateSet('glx', 'vk', 'rtx')]
	[string]$RendererDefault = 'glx',

	[string]$BuildDir = $(if ($env:FNQAA_BUILD_DIR) { $env:FNQAA_BUILD_DIR } else { 'build\msvc' }),

	[switch]$SetupOnly,
	[switch]$RunTests,
	[switch]$Recomp,
	[switch]$RecompOnly,
	# default: QAA_PACKAGE, else the package's usual place in this repository
	[string]$Package = $env:QAA_PACKAGE
)

$ErrorActionPreference = 'Stop'
if (-not $Package) {
	$Package = Join-Path (Split-Path -Parent $PSScriptRoot) 'ref\package\584108C8\000D0000\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458'
}
$workspaceRoot = Split-Path -Parent $PSScriptRoot

function Convert-BuildType {
	param([string]$SelectedConfiguration)
	switch ($SelectedConfiguration) {
		'Debug' { return 'debug' }
		'RelWithDebInfo' { return 'debugoptimized' }
		default { return 'release' }
	}
}

function Resolve-CommandPath {
	param([string]$Name)
	$cmd = Get-Command $Name -ErrorAction SilentlyContinue
	if (-not $cmd) {
		throw "Unable to locate $Name. Install it or add it to PATH (BUILD.md)."
	}
	return $cmd.Source
}

# The x64 developer environment, as scripts/msvc-env.ps1 imports it.
function Import-MsvcEnvironment {
	if ($env:OS -ne 'Windows_NT' -or $env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
		return
	}
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path $vswhere)) {
		if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
			return
		}
		throw 'vswhere.exe not found: install Visual Studio 2022 with the C++ workload.'
	}
	$installation = & $vswhere -latest -products * `
		-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $installation) {
		throw 'No Visual Studio installation with the x64 C++ tools found.'
	}
	$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
	$env:VSCMD_SKIP_SENDTELEMETRY = '1'
	$lines = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
	if ($LASTEXITCODE -ne 0) {
		throw 'Visual Studio x64 environment setup failed.'
	}
	foreach ($line in $lines) {
		if ($line -match '^([^=]+)=(.*)$') {
			Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
		}
	}
}

function Resolve-RendererList {
	param([string]$RendererCsv)
	$result = New-Object System.Collections.Generic.List[string]
	foreach ($item in ($RendererCsv -split ',')) {
		$name = $item.Trim()
		if (-not $name) {
			continue
		}
		if ($name -notin @('glx', 'vk', 'rtx')) {
			throw "Unsupported renderer module: $name"
		}
		if (-not $result.Contains($name)) {
			$result.Add($name)
		}
	}
	if ($result.Count -eq 0) {
		throw 'At least one renderer module must be selected.'
	}
	return $result
}

function Invoke-Recompiler {
	if (-not $Package) {
		throw 'No package: pass -Package or set QAA_PACKAGE to your STFS package (README.md).'
	}
	if (-not (Test-Path -LiteralPath $Package)) {
		throw "Package not found: $Package. Put your STFS package there or set QAA_PACKAGE (README.md)."
	}
	$python = Resolve-CommandPath -Name 'python'
	$buildPy = Join-Path $workspaceRoot 'tools\recomp\build.py'
	Write-Host "==> $python $buildPy --package <QAA_PACKAGE>"
	& $python $buildPy --package $Package
	if ($LASTEXITCODE -ne 0) {
		throw 'Recompiling the game modules failed.'
	}
}

$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $workspaceRoot $BuildDir }
$buildType = Convert-BuildType -SelectedConfiguration $Configuration
$rendererList = Resolve-RendererList -RendererCsv $Renderers
if ($rendererList -notcontains $RendererDefault) {
	throw "The default renderer ($RendererDefault) is not among the built ones ($($rendererList -join ','))."
}

Import-MsvcEnvironment

if ($Recomp -or $RecompOnly) {
	Invoke-Recompiler
	if ($RecompOnly) {
		Write-Host 'Recompiled modules are in build\recomp\native\bin.'
		exit 0
	}
}

$mesonPath = Resolve-CommandPath -Name 'meson'
Write-Host "Meson: $mesonPath"
Write-Host "Configuration: $Configuration ($buildType)"
Write-Host "Target: $Target"
Write-Host "Build directory: $buildPath"
Write-Host "Renderer modules: $($rendererList -join ',') (default $RendererDefault)"

$setupArgs = @(
	'setup',
	$buildPath,
	"--buildtype=$buildType",
	'-Drenderer-dlopen=true',
	"-Drenderers=$($rendererList -join ',')",
	"-Drenderer-default=$RendererDefault"
)
switch ($Target) {
	'client' { $setupArgs += @('-Dbuild-client=true', '-Dbuild-server=false') }
	'dedicated' { $setupArgs += @('-Dbuild-client=false', '-Dbuild-server=true') }
	default { $setupArgs += @('-Dbuild-client=true', '-Dbuild-server=true') }
}

$isReconfigure = Test-Path (Join-Path $buildPath 'meson-private\coredata.dat')
if ($isReconfigure) {
	$setupArgs = @('setup', '--reconfigure') + $setupArgs[1..($setupArgs.Count - 1)]
}
Write-Host "==> meson $($setupArgs -join ' ')"
& $mesonPath @setupArgs
if ($LASTEXITCODE -ne 0) {
	if (-not $isReconfigure) {
		throw 'Meson setup failed.'
	}
	# A stale option definition in coredata (an option that changed between
	# pulls) fails a reconfigure; a wipe rebuilds the configuration.
	$wipeArgs = @('setup', '--wipe') + $setupArgs[2..($setupArgs.Count - 1)]
	Write-Host 'Meson reconfigure failed; retrying with a clean configuration (--wipe).'
	Write-Host "==> meson $($wipeArgs -join ' ')"
	& $mesonPath @wipeArgs
	if ($LASTEXITCODE -ne 0) {
		throw 'Meson setup failed.'
	}
}
if ($SetupOnly) {
	Write-Host 'Meson setup completed.'
	exit 0
}

# Meson's default target includes every test executable; a build task makes
# only what runs. -RunTests builds everything and runs the tests.
$compileArgs = @('compile', '-C', $buildPath)
if (-not $RunTests) {
	if ($Target -in @('client', 'both')) {
		$compileArgs += 'fnqaa'
		foreach ($name in $rendererList) {
			$compileArgs += "fnqaa_${name}_x86_64"
		}
	}
	if ($Target -in @('dedicated', 'both')) {
		$compileArgs += 'fnqaa-ded'
	}
}
Write-Host "==> meson $($compileArgs -join ' ')"
& $mesonPath @compileArgs
if ($LASTEXITCODE -ne 0) {
	throw 'Meson compile failed.'
}

$expected = @()
if ($Target -in @('client', 'both')) {
	$expected += 'fnqaa.exe'
	$expected += $rendererList | ForEach-Object { "fnqaa_${_}_x86_64.dll" }
}
if ($Target -in @('dedicated', 'both')) {
	$expected += 'fnqaa-ded.exe'
}
foreach ($name in $expected) {
	if (-not (Test-Path (Join-Path $buildPath $name))) {
		throw "Build output missing: $(Join-Path $buildPath $name)"
	}
}

if ($RunTests) {
	Write-Host "==> meson test -C $buildPath --print-errorlogs"
	& $mesonPath test -C $buildPath --print-errorlogs
	if ($LASTEXITCODE -ne 0) {
		throw 'Meson tests failed.'
	}
}

$modules = Join-Path $workspaceRoot 'build\recomp\native\bin'
foreach ($name in 'qaa_qagame.dll', 'qaa_cgame.dll', 'qaa_ui.dll') {
	if (-not (Test-Path (Join-Path $modules $name))) {
		Write-Warning "$name is not built yet: run the 'recomp' task before playing."
	}
}
Write-Host 'FnQAA build completed.'
