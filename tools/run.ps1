# Colaimdation - run the editor and drive it over MCP.
#
#   pwsh -File tools/run.ps1                    # build, start, print the endpoint
#   pwsh -File tools/run.ps1 -Demo              # open the welcome document
#   pwsh -File tools/run.ps1 -File notes.md     # open a file
#   pwsh -File tools/run.ps1 -Smoke             # start, call one tool, stop
#
# Prints the exact MCP endpoint, the discovery file path and a ready-to-paste
# stdio bridge command.

[CmdletBinding()]
param(
    [string]$File = "",
    [switch]$Demo,
    [switch]$Smoke,
    [switch]$NoBuild,
    [int]$Port = 0,
    [switch]$Confirm
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build-core/bin/colaimdation.exe"

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Target colaimdation
}
if (-not (Test-Path $exe)) { throw "colaimdation.exe was not built at $exe" }

$arguments = @()
if ($File) { $arguments += @("--file", (Resolve-Path $File).Path) }
if ($Demo) { $arguments += "--demo" }
if ($Port -gt 0) { $arguments += @("--port", "$Port") }
if ($Confirm) { $arguments += "--confirm" }

$discovery = Join-Path $env:LOCALAPPDATA "colaimdation\mcp.json"
Remove-Item $discovery -ErrorAction SilentlyContinue

Write-Host "==> starting $exe $($arguments -join ' ')" -ForegroundColor Cyan
$app = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru

try {
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path $discovery) { break }
    }
    if (-not (Test-Path $discovery)) {
        throw "the editor did not publish its MCP endpoint; check the window for errors"
    }

    $config = Get-Content $discovery -Raw | ConvertFrom-Json
    $bridge = Join-Path $root "build-core/bin/colaimdation_mcp_bridge.exe"
    $baseUrl = $config.url -replace '/mcp$', ''

    Write-Host ""
    Write-Host "Colaimdation is running (pid $($app.Id))" -ForegroundColor Green
    Write-Host ""
    Write-Host "  MCP endpoint : $($config.url_with_token)" -ForegroundColor White
    Write-Host "  plain URL    : $($config.url)"
    Write-Host "  discovery    : $discovery"
    Write-Host "  tools        : $($config.tools.Count)"
    Write-Host ""
    Write-Host "  stdio bridge : $bridge" -ForegroundColor White
    Write-Host "                 (reads $discovery automatically)"
    Write-Host ""

    if ($Smoke) {
        $health = Invoke-RestMethod -Uri "$baseUrl/health" -TimeoutSec 5
        Write-Host "  health       : $($health.status), $($health.tools) tools, document '$($health.document)'" -ForegroundColor Green

        $headers = @{ Authorization = "Bearer $($config.token)" }
        $request = @{
            jsonrpc = "2.0"; id = 1; method = "tools/call"
            params = @{ name = "colaimdation_get_document_info"; arguments = @{} }
        } | ConvertTo-Json -Depth 6 -Compress
        $reply = Invoke-RestMethod -Uri $config.url -Method Post -Body $request `
            -ContentType "application/json" -Headers $headers -TimeoutSec 15
        $info = $reply.result.structuredContent
        Write-Host ("  tool call    : {0} - {1} chars, {2} blocks, revision {3}" -f `
            $info.name, $info.characters, $info.blocks, $info.revision.Substring(0, 8)) -ForegroundColor Green

        # The editor must actually hold the document. When Workspace is not wired
        # to the UI bridge the page never receives it, and every write is applied
        # without the writer's confirmation - a silent failure that is invisible
        # from the outside, so it is checked here.
        if ($info.blocks -lt 1) {
            throw "the editor reported an empty document ($($info.characters) chars, $($info.blocks) blocks); the document was not loaded"
        }

        $listRequest = @{ jsonrpc = "2.0"; id = 2; method = "tools/list" } | ConvertTo-Json -Compress
        $tools = Invoke-RestMethod -Uri $config.url -Method Post -Body $listRequest `
            -ContentType "application/json" -Headers $headers -TimeoutSec 15
        Write-Host "  tools/list   : $($tools.result.tools.Count) tools advertised" -ForegroundColor Green
        Write-Host ""
        Write-Host "==> smoke test passed; stopping the editor" -ForegroundColor Green
        Stop-Process -Id $app.Id -Force
        Start-Sleep -Milliseconds 600
        exit 0
    }

    Write-Host "  quick manual call (PowerShell):" -ForegroundColor DarkGray
    Write-Host "    `$h = @{ Authorization = 'Bearer $($config.token)' }"
    Write-Host "    `$b = '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}'"
    Write-Host "    Invoke-RestMethod -Uri '$($config.url)' -Method Post -Body `$b -ContentType 'application/json' -Headers `$h"
    Write-Host ""
    Write-Host "==> close the editor window or press Ctrl+C to stop" -ForegroundColor DarkGray
    Wait-Process -Id $app.Id
} finally {
    Get-Process -Name "colaimdation" -ErrorAction SilentlyContinue | Stop-Process -Force
}
