# bake_impostors_debug.ps1
# Regenerates impostor_atlas_debug.dds (solid-color-per-cell debug atlas) for every
# vegetation species folder.  No FBX is required for debug atlases.
# Run from any directory - the script resolves paths relative to its own location.

$ErrorActionPreference = "Stop"

$repoRoot   = Split-Path -Parent $PSScriptRoot
$baker      = "$repoRoot\build\debug\bin\Debug\mars_impostor_baker.exe"
$vegRoot    = "$repoRoot\models\vegetation"

if (-not (Test-Path $baker)) {
	Write-Error "Impostor baker not found at: $baker`nBuild the mars_impostor_baker target first."
	exit 1
}

$species = Get-ChildItem $vegRoot -Directory
$total   = $species.Count
$index   = 0

foreach ($sp in $species) {
	$index++
	$output = "$($sp.FullName)\impostor_atlas_debug.dds"
	Write-Host "[$index/$total] Baking debug atlas for $($sp.Name) ..." -ForegroundColor Cyan
	Write-Host "  output: $output"

	# Run from the baker's own directory so the relative dxil/ path resolves.
	Push-Location (Split-Path -Parent $baker)
	& $baker --debug-colors --output $output
	$exitCode = $LASTEXITCODE
	Pop-Location

	if ($exitCode -ne 0) {
		Write-Error "Baker failed for $($sp.Name) (exit code $exitCode)"
		exit $exitCode
	}

	Write-Host "  Done." -ForegroundColor Green
}

Write-Host ""
Write-Host "All debug atlases baked successfully." -ForegroundColor Green
