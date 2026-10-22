# Read-only probe: graphics stack modules loaded by TTVoice
$p = Get-Process -Name TTVoice -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output 'TTVoice not running'; exit 0 }
Write-Output ("PID={0} WS={1:N0} MB" -f $p.Id, ($p.WorkingSet64/1MB))
$mods = $p.Modules | Where-Object {
    $_.ModuleName -match 'd3d11|dxgi|libEGL|libGLESv2|vulkan|opengl32|flutter_windows|dcomp|dwmapi|d3dcompiler|apphelp'
}
Write-Output '=== Graphics/Flutter Modules ==='
$mods | ForEach-Object {
    Write-Output ("{0,-28} {1,-45} {2,8:N0} KB" -f $_.ModuleName, $_.FileName, ($_.ModuleMemorySize/1KB))
}
Write-Output '=== End ==='
