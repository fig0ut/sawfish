# Run from a Visual Studio developer PowerShell with Qt, CMake, Ninja, Python,
# Go, Protobuf and gRPC 1.84+ (MSVC, matching architecture) available.
param(
    [string]$RostraSource = "$PSScriptRoot/../../rostra",
    [string]$BuildDirectory = "$PSScriptRoot/../build",
    [string]$PrefixPath = "",
    [string]$BuildType = "Release",
    [int]$Jobs = 4
)
$ErrorActionPreference = "Stop"
$sawfishRoot = (Resolve-Path "$PSScriptRoot/..").Path
$rostraRoot = (Resolve-Path $RostraSource).Path
New-Item -ItemType Directory -Force $BuildDirectory | Out-Null
$buildRoot = (Resolve-Path $BuildDirectory).Path
function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}
$expected = ((Get-Content "$sawfishRoot/proto/REVISION" | Where-Object { $_ -like 'commit=*' }) -replace '^commit=', '').Trim()
$revision = (& git -C $rostraRoot rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or $revision.Trim() -ne $expected) {
    throw "Rostra revision differs from proto/REVISION. Review and update the pinned API before building."
}
foreach ($proto in @('daemon/started_service.proto', 'daemon/managed_service.proto', 'experimental/boxdd/desktop_service.proto')) {
    # Git must preserve the vendored definitions byte for byte (core.autocrlf=false).
    if ((Get-FileHash "$sawfishRoot/proto/$proto").Hash -ne (Get-FileHash "$rostraRoot/$proto").Hash) {
        throw "Protocol definition differs: $proto. Update the pinned definitions deliberately."
    }
}
$cmakeArguments = @('-S', $sawfishRoot, '-B', $buildRoot, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$BuildType", "-DSAWFISH_ROSTRA_EXECUTABLE=$buildRoot/rostra-daemon.exe")
if ($PrefixPath) { $cmakeArguments += "-DCMAKE_PREFIX_PATH=$PrefixPath" }
Invoke-Checked 'cmake' $cmakeArguments
Invoke-Checked 'cmake' @('--build', $buildRoot, '--parallel', "$Jobs")
# Match Rostra's single-file desktop release. Custom tags may need extra libraries.
$tags = if ($env:ROSTRA_BUILD_TAGS) { $env:ROSTRA_BUILD_TAGS } else {
    (Get-Content "$rostraRoot/release/DEFAULT_BUILD_TAGS_OTHERS" -Raw).Trim()
}
$sharedLdflags = (Get-Content "$rostraRoot/release/LDFLAGS" -Raw).Trim()
$version = "sawfish-$($revision.Substring(0, 9))"
$previousCgo = $env:CGO_ENABLED
if (-not $env:CGO_ENABLED) { $env:CGO_ENABLED = '0' }
Push-Location $rostraRoot
try {
    Invoke-Checked 'go' @('build', '-trimpath', '-tags', $tags, '-ldflags',
        "-X github.com/fig0ut/rostra/constant.Version=$version $sharedLdflags -buildid=",
        '-o', "$buildRoot/rostra-daemon.exe", './experimental/boxdd')
} finally {
    Pop-Location
    $env:CGO_ENABLED = $previousCgo
}
Invoke-Checked "$buildRoot/rostra-daemon.exe" @('version')
Write-Host "Built $buildRoot/sawfish.exe and $buildRoot/rostra-daemon.exe"
