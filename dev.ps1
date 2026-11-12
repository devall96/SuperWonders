# Development environment for SwitchGamesRecomp (Windows).
# Prefer the self-contained toolchain shipped under tools/portable. The legacy
# Visual Studio path remains available while the portable release is validated.
$portable = Join-Path $PSScriptRoot 'tools\portable'
$portableBin = Join-Path $portable 'bin'
if ((Test-Path (Join-Path $portable 'llvm-mingw\bin\clang.exe')) -and
    (Test-Path (Join-Path $portable 'cmake\bin\cmake.exe')) -and
    (Test-Path (Join-Path $portableBin 'ninja.exe'))) {
    $env:PATH = "$(Join-Path $portable 'llvm-mingw\bin');$portableBin;$(Join-Path $portable 'cmake\bin');$env:PATH"
    $env:CC = 'clang'
    $env:CXX = 'clang++'
    $env:RC = 'llvm-rc'
    $env:SUPERWONDERS_PORTABLE_TOOLCHAIN = '1'
    $env:VULKAN_SDK = Join-Path $portable 'vulkan'
    $env:PATH = (Join-Path $portable 'spirv-tools\bin') + ";$env:PATH"
    Write-Host "SuperWonders portable toolchain: $((clang --version)[0])"
} else {
    $vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    $cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
    $env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$cm\CMake\bin;$cm\Ninja;$env:PATH"
    if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = (Get-ChildItem C:\VulkanSDK -Directory | Sort-Object Name | Select-Object -Last 1).FullName }
    $env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
    Write-Host "SwitchGamesRecomp legacy dev env: clang $((clang-cl --version)[0] -replace '.*version ','')"
}
$env:PATH = "$PSScriptRoot\private\tools\nstool;$env:PATH"
Set-Location $PSScriptRoot
Write-Host "Build directory: $(Get-Location)"
