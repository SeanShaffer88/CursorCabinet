param([string]$Destination = (Join-Path $PSScriptRoot '..\dist\x64'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repoRoot = Split-Path $PSScriptRoot -Parent
$lock = Get-Content -LiteralPath (Join-Path $repoRoot 'packaging\packs.lock.json') -Raw | ConvertFrom-Json
$cache = Join-Path $repoRoot 'build\downloads'
New-Item -ItemType Directory -Force $Destination,$cache | Out-Null
$destinationRoot = [IO.Path]::GetFullPath($Destination)
function Get-VerifiedFile($entry) {
    $path = Join-Path $cache $entry.name
    if (!(Test-Path -LiteralPath $path)) {
        $temporary = "$path.download"
        try {
            Invoke-WebRequest -Uri $entry.url -OutFile $temporary -UseBasicParsing
            if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $entry.sha256) { throw "SHA256 mismatch: $($entry.name)" }
            Move-Item -LiteralPath $temporary -Destination $path -Force
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
        }
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Cached file SHA256 mismatch: $($entry.name)" }
    return $path
}
function Expand-SafeArchive([string]$archivePath,[string]$target) {
    $root = [IO.Path]::GetFullPath($target).TrimEnd('\') + '\'
    New-Item -ItemType Directory -Force $target | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        [long]$total = 0
        foreach ($entry in $zip.Entries) {
            $full = [IO.Path]::GetFullPath((Join-Path $target $entry.FullName))
            if (!$full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archive path escapes extraction folder.' }
            if (($entry.ExternalAttributes -band 0x400) -ne 0 -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Archive links are not supported.' }
            $total += $entry.Length
            if ($total -gt 2GB -or $entry.Length -gt 256MB) { throw 'Archive exceeds extraction limits.' }
            $ancestor = $full
            while ($ancestor -and $ancestor.Length -ge $root.Length) {
                if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Extraction target contains a reparse point.' }
                $ancestor = Split-Path $ancestor -Parent
            }
        }
    } finally { $zip.Dispose() }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $target -Force
}
foreach ($collection in $lock.collections) {
    $slug = $collection.repository.Replace('/','_')
    $collectionPath = Join-Path $destinationRoot "Packs\$slug"
    $sourceArchive = Get-VerifiedFile $collection.source
    New-Item -ItemType Directory -Force $collectionPath,(Join-Path $destinationRoot 'Sources') | Out-Null
    $sourceZip = [IO.Compression.ZipFile]::OpenRead($sourceArchive)
    try {
        $licenseEntry = $sourceZip.Entries | Where-Object FullName -Match '^[^/]+/LICENSE$' | Select-Object -First 1
        if (!$licenseEntry -or $licenseEntry.Length -gt 1MB) { throw "Pinned source has no supported LICENSE: $slug" }
        $licenseStream = $licenseEntry.Open()
        $licenseFile = [IO.File]::Create((Join-Path $collectionPath 'LICENSE.txt'))
        try { $licenseStream.CopyTo($licenseFile) } finally { $licenseFile.Dispose(); $licenseStream.Dispose() }
    } finally { $sourceZip.Dispose() }
    Set-Content -LiteralPath (Join-Path $collectionPath 'SOURCE.txt') -Encoding utf8 -Value "https://github.com/$($collection.repository)`nPinned release: $($collection.tag)`nCorresponding source: Sources/$($collection.source.name)"
    Copy-Item -LiteralPath $sourceArchive -Destination (Join-Path $destinationRoot "Sources\$($collection.source.name)") -Force
    foreach ($asset in $collection.assets) {
        $archive = Get-VerifiedFile $asset
        $folder = Join-Path $collectionPath ([IO.Path]::GetFileNameWithoutExtension($asset.name))
        Expand-SafeArchive $archive $folder
        Write-Host "$slug : $($asset.name)"
    }
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\packs.lock.json') -Destination (Join-Path $destinationRoot 'packs.lock.json') -Force
