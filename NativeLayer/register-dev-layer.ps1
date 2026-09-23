<#
.SYNOPSIS
  Registers (or unregisters) the Donutz VR HUD OpenXR API layer
  system-wide (HKEY_LOCAL_MACHINE), so the OpenXR loader picks it up for
  any OpenXR application (e.g. iRacing) started afterwards - mirroring how
  other working implicit layers on this machine (OpenKneeboard,
  XRFrameTools, OpenXR-Toolkit) are registered under
  HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit. A per-user (HKCU)
  registration was tried first but iRacing's OpenXR loader never picked it
  up. Requires an elevated (Run as Administrator) PowerShell session.

.PARAMETER Unregister
  Removes the registry value instead of adding it.

.PARAMETER Configuration
  Build configuration whose output directory contains the built DLL + JSON
  manifest. Defaults to Release.
#>
param(
	[switch]$Unregister,
	[string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$manifestPath = Join-Path $PSScriptRoot "bin\x64\$Configuration\XR_APILAYER_DONUTZ_vrhud.json"
$registryKey = "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"

if ($Unregister) {
	if (Test-Path $registryKey) {
		Remove-ItemProperty -Path $registryKey -Name $manifestPath -ErrorAction SilentlyContinue
		Write-Host "Unregistered: $manifestPath"
	}
	return
}

if (-not (Test-Path $manifestPath)) {
	throw "Manifest not found at '$manifestPath'. Build NativeLayer.vcxproj ($Configuration|x64) first."
}

if (-not (Test-Path $registryKey)) {
	New-Item -Path $registryKey -Force | Out-Null
}

New-ItemProperty -Path $registryKey -Name $manifestPath -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "Registered: $manifestPath"
Write-Host "Verify via the 'OpenXR API Layers' tool (Win64-HKLM tab) or by re-running this script."
