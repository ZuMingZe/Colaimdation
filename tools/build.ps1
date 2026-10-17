# Colaimdation - configure + build helper.
#
#   pwsh -File tools/build.ps1                 # configure (if needed) + build
#   pwsh -File tools/build.ps1 -Clean          # wipe the build dir first
#   pwsh -File tools/build.ps1 -Target colaimdation_core
#   pwsh -File tools/build.ps1 -Test           # build, then run ctest
#   pwsh -File tools/build.ps1 -User           # end-user build in build-user\ (no
#                                              # developer tools menu, no self-test)
#   pwsh -File tools/build.ps1 -FetchSdk       # vendor the WebView2 SDK first
#
# The MSVC environment is imported from vcvars64.bat into this process so the
# script works from a plain PowerShell prompt (no "Developer Command Prompt").

# NOTE: no [CmdletBinding()] here. It would inject the common -Verbose switch,
# which then collides with the explicit $Verbose parameter below and makes the
# whole script fail to bind ("A parameter with the name 'Verbose' was defined
# multiple times"), which broke the documented `tools/build.ps1` workflow on
# PowerShell 5.1 and 7 alike.
param(
    # Must match the directory run.ps1 and the README expect (`build-core`).
    # Defaulting to `build` produced a second, unused build tree: the quick
    # start built into build/ and then pointed at build-core\bin\.
    [string]$BuildDir = "",
    [string]$Config = "RelWithDebInfo",
    [string]$Target = "",
    [switch]$Clean,
    [switch]$Test,
    [switch]$Reconfigure,
    [switch]$FetchSdk,
    # End-user variant: separate build tree, no "Developer tools" / "Check the
    # editor" menu entries and no F12.
    [switch]$User,
    [switch]$Verbose
)

if (-not $BuildDir) { $BuildDir = if ($User) { "build-user" } else { "build-core" } }

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if ($FetchSdk) {
        & (Join-Path $PSScriptRoot "fetch_webview2.ps1")
    }

    # --- import the MSVC x64 environment -----------------------------------
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; Visual Studio 2022 is required" }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "no Visual Studio installation with the C++ toolset was found" }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    Write-Host "==> MSVC environment: $vcvars" -ForegroundColor DarkGray
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe is not on PATH after importing vcvars64" }

    # --- work around a non-ASCII project path --------------------------------
    # This toolchain's ninja stores header dependencies with ANSI paths. Under a
    # path that is not plain ASCII - and this project lives under one - "祖铭泽"
    # becomes "绁栭摥娉?" *inside .ninja_deps*, so the next build dies with
    #   ninja: error: FindFirstFileExA(../../../../../...): The filename,
    #   directory name, or volume label syntax is incorrect.
    # and header dependency tracking is lost along the way. Build through a mapped
    # drive letter so every path the tools see is ASCII: the outputs still land in
    # <project>\build-core\bin, because the drive maps that very directory.
    $sourceRoot = $root
    if ($root -match '[^\x00-\x7F]') {
        # Reuse the mapping if one already exists, and identify it by the file on
        # the far side: subst's own output is printed in the console code page, so
        # comparing its (mangled) path text is not reliable.
        $rootHash = (Get-FileHash (Join-Path $root "CMakeLists.txt")).Hash
        $drive = $null
        foreach ($candidate in @('X', 'Y', 'Z', 'W', 'V', 'U', 'T')) {
            $probe = "${candidate}:\CMakeLists.txt"
            if (Test-Path $probe) {
                if ((Get-FileHash $probe).Hash -eq $rootHash) { $drive = "${candidate}:"; break }
                # A different project is mapped there; try the next letter.
                continue
            }
            & subst "${candidate}:" $root | Out-Null
            if ($LASTEXITCODE -eq 0 -and (Test-Path $probe)) { $drive = "${candidate}:"; break }
        }
        if ($drive) {
            Write-Host "==> non-ASCII project path: building through $drive (-> $root)" -ForegroundColor DarkGray
            $sourceRoot = "$drive\"
        } else {
            Write-Warning ("could not map a drive letter for the non-ASCII path; " +
                           "header dependency tracking will be unreliable")
        }
    }
    $buildPath = Join-Path $sourceRoot $BuildDir

    # --- configure ----------------------------------------------------------
    # Configure on every run: it costs a fraction of a second, and a cache that
    # points at a drive letter which is no longer mapped produces failures that
    # are very hard to read.
    if ($Clean -and (Test-Path $buildPath)) {
        Write-Host "==> removing $buildPath" -ForegroundColor DarkGray
        Remove-Item -Recurse -Force $buildPath
    }
    # A cache created for a different path (an earlier drive letter, a project that
    # moved) makes CMake refuse to configure at all, with an error nobody can act
    # on. Start that tree over instead.
    $cacheFile = Join-Path $buildPath "CMakeCache.txt"
    if (Test-Path $cacheFile) {
        $recorded = Select-String -Path $cacheFile -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$' |
            Select-Object -First 1
        if ($recorded) {
            $recordedDir = ($recorded.Matches[0].Groups[1].Value.Trim() -replace '/', '\').TrimEnd('\')
            if ($recordedDir -and $recordedDir -ine $buildPath.TrimEnd('\')) {
                Write-Host "==> build tree belonged to $recordedDir; rebuilding it from scratch" -ForegroundColor Yellow
                Remove-Item -Recurse -Force $buildPath
            }
        }
    }
    Write-Host "==> configuring (Ninja, $Config)" -ForegroundColor Cyan
    $cmakeArgs = @("-S", $sourceRoot, "-B", $buildPath, "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
    if ($User) { $cmakeArgs += "-DCOLAI_USER_BUILD=ON" }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

    # --- build --------------------------------------------------------------
    $targets = if ($Target) { @("--target", $Target) } else { @() }
    Write-Host "==> building" -ForegroundColor Cyan
    & cmake --build $buildPath @targets
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    if ($Test) {
        Write-Host "==> running tests" -ForegroundColor Cyan
        & ctest --test-dir $buildPath --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
    }

    # --- verify header dependencies are really being tracked ----------------
    # Ninja records "#deps 0" for an object whose /showIncludes lines it could not
    # match (that is what a mangled non-ASCII path looks like from the outside),
    # and a tree built that way silently links stale objects after any header
    # edit. Fail loudly instead.
    $depsRecorded = $true
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja -and (Test-Path (Join-Path $buildPath ".ninja_deps"))) {
        $workspaceDeps = & ninja -C $buildPath -t deps 2>$null |
            Select-String -Pattern 'workspace.cpp.obj: #deps (\d+)' |
            Select-Object -First 1
        if ($workspaceDeps -and $workspaceDeps.Matches[0].Groups[1].Value -eq '0') {
            $depsRecorded = $false
        }
    }
    if (-not $depsRecorded) {
        Write-Host ("==> WARNING: this build tree records no header dependencies " +
                    "(#deps 0). Editing a header will not rebuild the files that " +
                    "include it. Rebuild clean: tools/build.ps1 -Clean") -ForegroundColor Yellow
    }

    $bin = Join-Path $buildPath "bin"
    if (Test-Path $bin) {
        Write-Host "==> artifacts" -ForegroundColor Green
        Get-ChildItem $bin | ForEach-Object { Write-Host ("    {0}  ({1:N0} bytes)" -f $_.Name, $_.Length) }
    }

    # --- verify the staged front-end matches the sources --------------------
    # The editor loads web/ from next to the executable. Staging used to be a
    # POST_BUILD step the build system did not track, so an edit to editor.js
    # could leave the *deployed* copy behind while everything looked fixed - a
    # silent failure that is impossible to see from the source tree. Compare the
    # trees and fail loudly instead.
    $staged = Join-Path $bin "web"
    if (Test-Path $staged) {
        $sourceRoot = Join-Path $root "web"
        $stale = @()
        Get-ChildItem -Path $sourceRoot -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($sourceRoot.Length).TrimStart('\')
            $mirror = Join-Path $staged $relative
            if (-not (Test-Path $mirror)) {
                $stale += "$relative (missing)"
            } elseif ((Get-FileHash $_.FullName).Hash -ne (Get-FileHash $mirror).Hash) {
                $stale += "$relative (out of date)"
            }
        }
        if ($stale.Count -gt 0) {
            throw ("the staged front-end is not up to date:`n    " + ($stale -join "`n    ") +
                   "`n  Run the build again, or remove '$buildPath' and rebuild.")
        }
        Write-Host "==> front-end assets verified against web/" -ForegroundColor Green
    }
} finally {
    Pop-Location
}
