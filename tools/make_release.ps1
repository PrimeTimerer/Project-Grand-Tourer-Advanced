# Makes the release zip of a package folder for GitHub Releases: GT6_PC_<yyyyMMdd-HHmm>.zip, the stamp being the time
# of the newest of the game exes and the launcher. Only the app files go in (launcher, game exes, bats, DLLs, trainer,
# notes) plus version.txt; game data, saves, mods, logs, settings, backups and the car / parts lists never do. The
# launcher's updater (launcher\Update.cs) installs exactly this zip and compares the stamp in its name.
# Nothing is uploaded here: attach the zip to a release of the repository by hand, then put the repository's address
# into the launcher (Updates · Tools). latest.json beside the zip has the form of GitHub's "latest release" answer, for
# trying the updater on this PC first (launcher.ini [Launcher] UpdateFeed=<path of latest.json>).
#   pwsh -NoProfile -File tools\make_release.ps1 -Package dist\GT6_Test_20261117 [-Out dist\release] [-Tag v1]
param(
    [Parameter(Mandatory)][string]$Package,
    [string]$Out,
    [string]$Tag,
    [string]$Repository      # owner/name: only to print the command that would publish the zip (nothing is uploaded here)
)
$ErrorActionPreference = 'Stop'
$Pkg = (Resolve-Path -LiteralPath $Package).Path.TrimEnd('\')
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Pkg) 'release' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$OutDir = (Resolve-Path -LiteralPath $Out).Path

if (-not (Test-Path -LiteralPath (Join-Path $Pkg 'GT6 Launcher.exe'))) { throw "No 'GT6 Launcher.exe' in $Pkg (build_launcher.bat, then copy it there)." }

# The app files: by name at the top of the package, and the trainer folder without the user's own files. The car list
# and the parts list (cars.json, parts_index.txt) are not app files: they come out of the game's own database, and the
# game makes them on each PC by itself ([GT6 0089] gt6_menu_engine.cpp, AutoImports).
$top = @('GT6 Launcher.exe', 'GT6Recomp.exe', 'GT6Recomp_CheatTest.exe', 'PLAY_*.bat', '*.dll', 'SDL2-LICENSE.txt', 'README.txt', 'CHANGES_*.txt')
$files = @()
foreach ($pattern in $top) { $files += Get-ChildItem -LiteralPath $Pkg -File -Filter $pattern }
$trainer = Join-Path $Pkg 'trainer'
if (Test-Path -LiteralPath $trainer) {
    $files += Get-ChildItem -LiteralPath $trainer -File -Recurse |
        Where-Object { $_.Name -notlike '*.profiles.json*' -and $_.Name -ne 'bspec_log.txt' -and $_.Extension -ne '.pdb' -and $_.Name -ne 'cars.json' }
}
$files = $files | Sort-Object FullName -Unique

$stamped = $files | Where-Object { $_.Name -like 'GT6Recomp*.exe' -or $_.Name -eq 'GT6 Launcher.exe' }
$stamp = ($stamped | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime.ToString('yyyyMMdd-HHmm')
if (-not $Tag) { $Tag = "v$stamp" }
# The package folder itself is this version from now on: its launcher must not take the zip made from it for a newer one.
[IO.File]::WriteAllText((Join-Path $Pkg 'version.txt'), $stamp, (New-Object Text.ASCIIEncoding))
$zip = Join-Path $OutDir "GT6_PC_$stamp.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files) {
        $name = $file.FullName.Substring($Pkg.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
    }
    $entry = $archive.CreateEntry('version.txt')
    $writer = New-Object IO.StreamWriter($entry.Open())
    $writer.Write($stamp)
    $writer.Dispose()
} finally {
    $archive.Dispose()
}

$size = (Get-Item -LiteralPath $zip).Length
$latest = [ordered]@{
    tag_name = $Tag
    name     = "GT6 PC $stamp"
    body     = ''
    html_url = ''
    assets   = @([ordered]@{ name = "GT6_PC_$stamp.zip"; size = $size; browser_download_url = $zip })
}
$latest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir 'latest.json') -Encoding utf8NoBOM
"{0}  ({1:N1} MB, {2} files, version {3})" -f $zip, ($size / 1MB), ($files.Count + 1), $stamp
if ($Repository) {
    # Not run here: publishing is the maintainer's decision. The launcher finds the newest release of the repository and
    # compares the date in the zip's name; for a private repository it needs a token (launcher: Updates · Tools).
    "to publish:  gh release create $Tag `"$zip`" --repo $Repository --title `"GT6 PC $stamp`" --notes `"version $stamp`""
}
