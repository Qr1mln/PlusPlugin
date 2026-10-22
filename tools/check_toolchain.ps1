# Read-only: detect MSVC / CMake toolchain
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -property installationPath
    Write-Output ("vswhere installationPath: {0}" -f $vs)
    $cl = Join-Path $vs 'VC\Tools\MSVC'
    if (Test-Path $cl) {
        Get-ChildItem $cl -Directory | ForEach-Object {
            Write-Output ("MSVC toolset: {0}" -f $_.Name)
            $clExe = Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe'
            Write-Output ("  cl.exe exists: {0}" -f (Test-Path $clExe))
        }
    } else {
        Write-Output 'MSVC toolset not found under VS (C++ workload may be missing)'
    }
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    if (Test-Path $sdk) { Get-ChildItem $sdk -Directory | Select-Object -First 3 | ForEach-Object { Write-Output ("Windows SDK: {0}" -f $_.Name) } }
} else {
    Write-Output 'vswhere not found (Visual Studio likely not installed)'
}
$cm = Get-Command cmake -ErrorAction SilentlyContinue
Write-Output ("cmake: {0}" -f ($(if ($cm) { $cm.Source } else { 'not found' })))
