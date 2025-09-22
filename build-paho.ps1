<#
.SYNOPSIS
    Builds the Paho MQTT C++ library for both Release and Debug configurations.
.DESCRIPTION
    This script automates building the Paho MQTT C++ library and its C dependency.
    It builds both "Release" and "Debug" versions and places them in separate
    'install/Release' and 'install/Debug' subdirectories.
.PARAMETER VerboseCMake
    If specified, enables verbose output for CMake commands.
.EXAMPLE
    ./build-paho.ps1 -VerboseCMake
#>
[CmdletBinding()]
param(
    [string]$BuildRoot = (Join-Path $PSScriptRoot "paho-build"),

    [ValidateScript({
        if (-not (Test-Path $_ -PathType Leaf)) {
            throw "vcpkg.cmake toolchain file not found at path: $_"
        }
        return $true
    })]
    [string]$VcpkgToolchainFile = "$($env:VCPKG_ROOT)\scripts\buildsystems\vcpkg.cmake",

    [switch]$VerboseCMake
)

## Helper Functions
#----------------------------------------------------------------------

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [string[]]$Arguments
    )
    Write-Host ">>> & '$Command' $($Arguments -join ' ')" -ForegroundColor Cyan
    $process = Start-Process -FilePath $Command -ArgumentList $Arguments -Wait -NoNewWindow -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Command failed with exit code $($process.ExitCode): $Command $($Arguments -join ' ')"
    }
}

function Build-PahoMultiConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectName,
        [Parameter(Mandatory = $true)]
        [string]$RepoUrl,
        [Parameter(Mandatory = $true)]
        [string[]]$BaseCMakeArgs,
        [Parameter(Mandatory = $true)]
        [string[]]$Configurations
    )

    $sourceDir = Join-Path $BuildRoot $ProjectName
    $installPaths = @{}

    # --- Clone the repository once ---
    if (Test-Path $sourceDir) {
        Write-Host "Removing existing '$sourceDir'..."
        Remove-Item -Recurse -Force $sourceDir
    }
    Invoke-Process -Command "git" -Arguments @("clone", "--recurse-submodules", "--depth", "1", $RepoUrl, $sourceDir)

    Push-Location $sourceDir
    try {
        # --- Loop through each configuration to configure and build it ---
        foreach ($config in $Configurations) {
            Write-Host "`n--- Configuring and Building for '$config' ---" -ForegroundColor Magenta
            
            $buildDir = "build-$config"
            $installPath = Join-Path -Path (Join-Path $sourceDir "install") -ChildPath $config
            $installPaths[$config] = $installPath

            # 1. Configure Step (specific to this configuration)
            $configureArgs = @(
                "-S", ".",
                "-B", $buildDir,
                "-DCMAKE_INSTALL_PREFIX=$installPath"
            ) + $BaseCMakeArgs

            if ($VerboseCMake) {
                $configureArgs += "--log-level=VERBOSE"
            }
            Invoke-Process -Command "cmake" -Arguments $configureArgs

            # 2. Build and Install Step
            $buildArgs = @(
                "--build", $buildDir,
                "--config", $config,
                "--target", "install"
            )

            if ($VerboseCMake) {
                $buildArgs += "--verbose"
            }
            Invoke-Process -Command "cmake" -Arguments $buildArgs
        }
    }
    finally {
        Pop-Location
    }

    return $installPaths
}


## Main Execution
#----------------------------------------------------------------------

try {
    # Define the configurations to build
    $ConfigurationsToBuild = @("Release", "Debug")

    # 1. Check for prerequisites
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "Git is not installed or not in your PATH." }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "CMake is not installed or not in your PATH." }
    if (-not $env:VCPKG_ROOT) { Write-Warning "VCPKG_ROOT environment variable is not set."}

    # 2. Ensure the build root directory exists
    if (-not (Test-Path $BuildRoot)) {
        New-Item -Path $BuildRoot -ItemType Directory | Out-Null
    }

    # 3. Build paho.mqtt.cpp for all configurations
    Write-Host "`n--- Building paho.mqtt.cpp (and bundled paho.mqtt.c) ---" -ForegroundColor Yellow
    
    $paho_BaseCMakeArgs = @(
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchainFile",
        "-DPAHO_WITH_MQTT_C=TRUE",
        "-DPAHO_WITH_SSL=TRUE",
        "-DPAHO_BUILD_STATIC=TRUE",
        "-DPAHO_BUILD_SHARED=FALSE",
        "-DPAHO_BUILD_DOCUMENTATION=FALSE",
        "-DPAHO_BUILD_EXAMPLES=FALSE",
        "-DPAHO_BUILD_TESTS=FALSE"
    )

    # --- FIX APPLIED HERE: Use Splatting for a robust function call ---
    # Group parameters into a hashtable
    $buildParams = @{
        ProjectName    = "paho.mqtt.cpp"
        RepoUrl        = "https://github.com/eclipse/paho.mqtt.cpp.git"
        BaseCMakeArgs  = $paho_BaseCMakeArgs
        Configurations = $ConfigurationsToBuild
    }
    # Call the function using the '@' splatting operator
    $allInstallPaths = Build-PahoMultiConfig @buildParams

    # 4. Success Message
    Write-Host "`nSUCCESS! 🎉 All configurations built and installed." -ForegroundColor Green
    foreach ($config in $allInstallPaths.Keys) {
        Write-Host "$config libraries installed at: $($allInstallPaths[$config])"
    }
    
    Write-Host "`nTo use in your Visual Studio project, set the following paths:"
    Write-Host "  For Release builds:"
    Write-Host "    Include Directories: $($allInstallPaths['Release'])\include"
    Write-Host "    Library Directories: $($allInstallPaths['Release'])\lib"
    Write-Host "`n  For Debug builds:"
    Write-Host "    Include Directories: $($allInstallPaths['Debug'])\include"
    Write-Host "    Library Directories: $($allInstallPaths['Debug'])\lib"
    Write-Host "`n`n  Linker -> Input -> Additional Dependencies: paho-mqttpp3-static.lib;paho-mqtt3as-static.lib"

}
catch {
    Write-Host "`nBUILD FAILED!" -ForegroundColor Red
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}