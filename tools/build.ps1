param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw '请安装 Visual Studio 2022 C++ Build Tools。' }
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (!(Test-Path -LiteralPath $cmake)) { throw '请安装 Visual Studio 的 CMake 工具。' }
& $cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake 配置失败。' }
& $cmake --build (Join-Path $projectRoot 'build') --config $Configuration --parallel 6
if ($LASTEXITCODE) { throw '构建失败。' }
if (!$SkipTests) {
    & $ctest --test-dir (Join-Path $projectRoot 'build') -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { throw '测试失败。' }
}
