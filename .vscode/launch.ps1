<#
Runs a built FnQAA outside the debugger (after FnQL's
.vscode/launch-release.ps1): the windowed client with a renderer, or the
dedicated server, on the package in -Package, $env:QAA_PACKAGE or, by
default, ref\package\584108C8\000D0000\0ED9DD87... in this repository.
Any further arguments go to the engine as they are, e.g. +devmap q3dm6.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
	[ValidateSet('glx', 'vk', 'rtx')]
	[string]$Renderer = 'glx',

	[ValidateSet('client', 'dedicated')]
	[string]$Target = 'client',

	[string]$BuildDir = $(if ($env:FNQAA_BUILD_DIR) { $env:FNQAA_BUILD_DIR } else { 'build\msvc' }),

	# default: QAA_PACKAGE, else the package's usual place in this repository
	[string]$Package = $env:QAA_PACKAGE,

	[Parameter(ValueFromRemainingArguments = $true)]
	[string[]]$Arguments = @()
)

$ErrorActionPreference = 'Stop'
if (-not $Package) {
	$Package = Join-Path (Split-Path -Parent $PSScriptRoot) 'ref\package\584108C8\000D0000\0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458'
}
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $workspaceRoot $BuildDir }
$executable = Join-Path $buildRoot $(if ($Target -eq 'dedicated') { 'fnqaa-ded.exe' } else { 'fnqaa.exe' })
if (-not (Test-Path $executable)) {
	throw "Executable not found: $executable (run the build task first)."
}

if (-not (Test-Path -LiteralPath $Package)) {
	throw "Package not found: $Package. Put your STFS package there or set QAA_PACKAGE (README.md)."
}
$engineArgs = @('+set', 'fs_qaaPackage', $Package)
if ($Target -eq 'client') {
	$engineArgs += @('+set', 'r_fullscreen', '0', '+set', 'cl_renderer', $Renderer)
}
$engineArgs += $Arguments

Set-Location $buildRoot
# The client is a GUI-subsystem program, which PowerShell's & does not wait
# for; piping its output makes it wait (and shows the console log).
& $executable @engineArgs | Out-Host
exit $LASTEXITCODE
