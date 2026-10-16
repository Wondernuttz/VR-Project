$ErrorActionPreference = 'Stop'

$chromeCandidates = @(
    "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
)
$browser = $chromeCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $browser) {
    throw 'Chrome or Edge is required for the VR scroll-target browser test.'
}

$testFile = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'VRScrollTargetBrowserTest.html')).Path
$testUri = [Uri]::new($testFile).AbsoluteUri
$profileDir = Join-Path ([IO.Path]::GetTempPath()) ("prisma-vr-scroll-test-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profileDir | Out-Null
try {
    $stdoutPath = Join-Path $profileDir 'stdout.txt'
    $stderrPath = Join-Path $profileDir 'stderr.txt'
    $arguments = @(
        '--headless=new',
        '--disable-gpu',
        '--no-first-run',
        '--no-default-browser-check',
        "--user-data-dir=$profileDir",
        '--window-size=1100,700',
        '--virtual-time-budget=2000',
        '--dump-dom',
        $testUri
    )
    $process = Start-Process -FilePath $browser -ArgumentList $arguments -Wait -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    if ($process.ExitCode -ne 0) {
        $browserError = if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath -Raw
        } else { '' }
        throw "Browser exited with code $($process.ExitCode). $browserError"
    }
    $document = Get-Content -LiteralPath $stdoutPath -Raw
    if ($document -notmatch '<title>PASS</title>') {
        $failure = [regex]::Match($document, '<title>FAIL ([\s\S]*?)</title>').Groups[1].Value
        if (-not $failure) { $failure = 'Browser test did not reach PASS.' }
        throw $failure
    }
    Write-Host 'VR scroll-target browser tests passed'
}
finally {
    if (Test-Path -LiteralPath $profileDir) {
        $resolvedProfile = [IO.Path]::GetFullPath($profileDir)
        $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedProfile.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedProfile) -notmatch '^prisma-vr-scroll-test-[0-9a-f]{32}$') {
            throw 'Unexpected browser test cleanup directory.'
        }
        Remove-Item -LiteralPath $resolvedProfile -Recurse -Force
    }
}
