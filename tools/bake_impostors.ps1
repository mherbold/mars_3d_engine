# bake_impostors.ps1
# Regenerates impostor_atlas.dds for every vegetation species that has a HighPoly FBX.
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
	$fbx = Get-ChildItem $sp.FullName -Recurse -Filter "*.fbx" |
			   Where-Object { $_.DirectoryName -match "HighPoly" } |
			   Select-Object -First 1

	if (-not $fbx) {
		$fbx = Get-ChildItem $sp.FullName -Recurse -Filter "*.fbx" | Select-Object -First 1
	}

	if (-not $fbx) {
		Write-Warning "[$index/$total] $($sp.Name): no FBX found - skipping"
		continue
	}

	$output = "$($sp.FullName)\impostor_atlas.dds"
	Write-Host "[$index/$total] Baking $($sp.Name) ..." -ForegroundColor Cyan
	Write-Host "  model : $($fbx.FullName)"
	Write-Host "  output: $output"

	# Run from the baker's own directory so the relative dxil/ path resolves.
	Push-Location (Split-Path -Parent $baker)
	& $baker --model $fbx.FullName --output $output
	$exitCode = $LASTEXITCODE
	Pop-Location

	if ($exitCode -ne 0) {
		Write-Error "Baker failed for $($sp.Name) (exit code $exitCode)"
		exit $exitCode
	}

	Write-Host "  Done." -ForegroundColor Green
}

Write-Host ""
Write-Host "All impostor atlases baked successfully." -ForegroundColor Green
