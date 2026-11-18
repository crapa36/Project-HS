function Initialize-HsMsvcEnvironment([string]$BuildRoot) {
    $ErrorActionPreference = 'Stop'
    $cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
    $configuredCompiler = ''
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        foreach ($cacheLine in Get-Content -LiteralPath $cachePath) {
            if ($cacheLine -match '^CMAKE_CXX_COMPILER:[^=]+=(.+)$') {
                $configuredCompiler = $matches[1].Replace('\', '/')
                break
            }
        }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'MSVC x64 build tools were not found.' }
    $vcvarsVersionArgument = ''
    if ($configuredCompiler -match '^(?<installation>(?:[A-Za-z]:/|//)[^\r\n]+?)/VC/Tools/MSVC/(?<toolset>[0-9]+\.[0-9]+\.[0-9]+)/bin/Host[^/]+/[^/]+/cl\.exe$') {
        $installation = $matches.installation.Replace('/', '\')
        $configuredToolset = $matches.toolset
        $toolsetRoot = Join-Path $installation "VC/Tools/MSVC/$configuredToolset"
        $configuredDeveloperCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        if (!(Test-Path -LiteralPath $configuredCompiler -PathType Leaf) -or
            !(Test-Path -LiteralPath $toolsetRoot -PathType Container) -or
            !(Test-Path -LiteralPath $configuredDeveloperCommand -PathType Leaf)) {
            throw "Configured MSVC toolset $configuredToolset is unavailable at $toolsetRoot."
        }
        $vcvarsVersionArgument = " -vcvars_ver=$configuredToolset"
    }
    $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    # Import the developer environment once; subsequent commands need no nested shell quoting.
    $environmentLines = & $env:ComSpec /d /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64$vcvarsVersionArgument >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
    $environmentNames = @{}
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            $environmentName = $matches[1]
            if (!$environmentNames.ContainsKey($environmentName)) {
                $environmentNames[$environmentName] = $true
                [Environment]::SetEnvironmentVariable($environmentName, $matches[2], 'Process')
            }
        }
    }
    $env:VSLANG = '1033'
    return [string]$installation
}
