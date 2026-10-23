<#
.SYNOPSIS
    Deploy GuitarBattleServer to the production VM (fretfight.online).

.DESCRIPTION
    Packs the server directory, uploads it to the VM, backs up the current
    release, syncs the files (keeping the production .env intact), rebuilds
    the api image and waits for /health/ready.

    Requires an SSH key that can log in as the VM user. The production .env
    is never uploaded and never printed.

.PARAMETER KeyPath
    SSH private key. Defaults to ~/.ssh/guitarbattle_prod.

.PARAMETER Target
    SSH destination. Defaults to ubuntu@84.201.151.183.

.PARAMETER NoBuild
    Sync files only, do not restart containers (useful for a dry run).

.EXAMPLE
    pwsh -File GuitarBattleServer/scripts/deploy-prod.ps1
#>
[CmdletBinding()]
param(
    [string]$KeyPath = (Join-Path $env:USERPROFILE ".ssh\guitarbattle_prod"),
    [string]$Target = "ubuntu@84.201.151.183",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$serverDir = Split-Path -Parent $PSScriptRoot
$localTar = Join-Path $env:TEMP "gb-deploy.tar.gz"
$remoteDir = "/opt/guitarbattle/GuitarBattleServer"

if (-not (Test-Path $KeyPath)) { throw "SSH key not found: $KeyPath" }

Write-Host "== 1/5 packing $serverDir"
if (Test-Path $localTar) { Remove-Item $localTar -Force }
tar -czf $localTar -C $serverDir `
    --exclude ".env" `
    --exclude "tests" `
    --exclude "__pycache__" `
    --exclude "*.pyc" `
    --exclude ".pytest_cache" `
    --exclude ".venv*" `
    --exclude ".ruff_cache" .
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Write-Host ("    {0:N0} bytes" -f (Get-Item $localTar).Length)

Write-Host "== 2/5 uploading to $Target"
scp -i $KeyPath -o BatchMode=yes $localTar "${Target}:/tmp/gb-deploy.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "scp failed" }

$build = if ($NoBuild) { "echo 'skipping rebuild'" } else { "sudo docker compose up -d --build" }

$remote = @"
set -eu
TGT=$remoteDir
TS=`$(date +%Y%m%d-%H%M%S)
echo '== 3/5 backup'
sudo cp -a "`$TGT" "/opt/guitarbattle/GuitarBattleServer.backup-`$TS"
echo "    backup: /opt/guitarbattle/GuitarBattleServer.backup-`$TS"
echo '== 4/5 sync'
sudo rm -rf /tmp/gb-deploy && sudo mkdir -p /tmp/gb-deploy
sudo tar -xzf /tmp/gb-deploy.tar.gz -C /tmp/gb-deploy
sudo rsync -a --delete --exclude '.env' --exclude '.deploy-backup-*' /tmp/gb-deploy/ "`$TGT/"
sudo chown -R deploy:deploy "`$TGT"
sudo chmod 600 "`$TGT/.env"
sudo rm -rf /tmp/gb-deploy /tmp/gb-deploy.tar.gz
cd "`$TGT"
echo '== 5/5 build'
$build
sudo docker compose ps
"@

Write-Host "== 3-5/5 remote"
$remote = ($remote -replace "`r", "") + "`n"
# Ship the script as base64 so PowerShell never rewrites line endings on the wire.
$payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remote))
ssh -i $KeyPath -o BatchMode=yes $Target "echo $payload | base64 -d | bash -s"
if ($LASTEXITCODE -ne 0) { throw "remote deploy failed" }

Write-Host "== checking https://fretfight.online/health/ready"
$ready = $null
for ($i = 1; $i -le 10; $i++) {
    $ready = (curl.exe -s -m 10 https://fretfight.online/health/ready) -join ""
    if ($ready -match '"status":"ready"') { break }
    Start-Sleep -Seconds 2
}
Write-Host "    $ready"
if ($ready -notmatch '"status":"ready"') { throw "production health check failed" }
Write-Host "done"
