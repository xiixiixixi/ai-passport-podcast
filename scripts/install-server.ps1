param([int]$Port = 8899, [string]$PublicUrl = '', [switch]$NoOpen)
$ErrorActionPreference = 'Stop'
$passportRoot = Split-Path -Parent $PSScriptRoot
$passportServer = Join-Path $passportRoot 'server'
function Test-PassportAddress([string]$Address) {
  if (-not $Address) { return $true }
  if ($Address -notmatch '^https?://[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?(?::([1-9][0-9]{0,4}))?/?$') { return $false }
  if ($Matches[1] -and [int]$Matches[1] -gt 65535) { return $false }
  return $true
}
function Test-PassportIdentity([string]$Identity) {
  if ($Identity -notmatch '^(0|[1-9][0-9]{0,9})$') { return $false }
  return [UInt64]$Identity -le 4294967294
}
function Protect-PassportPath([string]$Path, [bool]$Directory = $false) {
  if ($env:OS -ne 'Windows_NT') { return }
  $passportIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().User
  $passportAcl = Get-Acl -LiteralPath $Path
  $passportAcl.SetAccessRuleProtection($true, $false)
  $passportAcl.SetOwner($passportIdentity)
  if ($Directory) {
    $passportRule = [Security.AccessControl.FileSystemAccessRule]::new($passportIdentity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
  } else {
    $passportRule = [Security.AccessControl.FileSystemAccessRule]::new($passportIdentity, 'FullControl', 'Allow')
  }
  $passportAcl.AddAccessRule($passportRule)
  Set-Acl -LiteralPath $Path -AclObject $passportAcl
}
if ($Port -lt 1 -or $Port -gt 65535) { throw '端口需在1–65535之间。' }
if (-not (Test-PassportAddress $PublicUrl)) { throw '请填写硬件可访问的后台根地址，不要带口令或页面路径。' }
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw '请先安装并启动 Docker（容器运行工具）。' }
& docker compose version | Out-Null
if ($LASTEXITCODE -ne 0) { throw '需要 Docker Compose（组合启动工具）第二版。' }
& docker info *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker尚未启动，或当前用户无权使用。' }
$passportEnv = Join-Path $passportServer '.env'
$passportCompose = Join-Path $passportServer 'compose.yaml'
if (-not (Test-Path $passportCompose)) { throw '缺少server/compose.yaml，请下载完整项目。' }
if (-not $PublicUrl -and -not (Test-Path $passportEnv)) {
  $passportCandidates = Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.IPv4DefaultGateway -and $_.IPv4Address -and $_.NetAdapter.Status -eq 'Up' }
  foreach ($passportCandidate in $passportCandidates) {
    $passportIp = $passportCandidate.IPv4Address.IPAddress
    if ($passportIp -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)') { $PublicUrl="http://${passportIp}:$Port"; break }
  }
}
if (-not (Test-Path $passportEnv)) {
  $passportRandom = New-Object byte[] 32
  $passportRng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
  try { $passportRng.GetBytes($passportRandom) } finally { $passportRng.Dispose() }
  $passportKey = ([BitConverter]::ToString($passportRandom)).Replace('-','').ToLowerInvariant()
  $passportContent = "PODCAST_SETUP_KEY=$passportKey`nPODCAST_PORT=$Port`nPODCAST_PUBLIC_URL=$PublicUrl`nPODCAST_UID=1000`nPODCAST_GID=1000`n"
  $passportFile = [IO.File]::Open($passportEnv,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try { $passportBytes = [Text.Encoding]::UTF8.GetBytes($passportContent); $passportFile.Write($passportBytes,0,$passportBytes.Length) } finally { $passportFile.Dispose() }
}
Protect-PassportPath $passportEnv
$passportText = [IO.File]::ReadAllText($passportEnv)
foreach ($passportField in @('PODCAST_SETUP_KEY','PODCAST_PORT','PODCAST_PUBLIC_URL','PODCAST_UID','PODCAST_GID')) {
  if ([regex]::Matches($passportText, '(?m)^'+$passportField+'=').Count -ne 1) { throw '现有.env的必要设置缺失或重复；为保护原配置，未覆盖它。' }
}
$passportSavedUid = [regex]::Match($passportText,'(?m)^PODCAST_UID=([^\r\n]*)\r?$').Groups[1].Value
$passportSavedGid = [regex]::Match($passportText,'(?m)^PODCAST_GID=([^\r\n]*)\r?$').Groups[1].Value
if (-not (Test-PassportIdentity $passportSavedUid) -or -not (Test-PassportIdentity $passportSavedGid)) { throw '现有.env的运行身份无效；为保护原配置和资料，未覆盖它。' }
$passportMatch = [regex]::Match($passportText,'(?m)^PODCAST_SETUP_KEY=([0-9a-f]{64})\r?$')
if (-not $passportMatch.Success) { throw '现有安装码无效；为保护原配置，脚本没有覆盖它。' }
$passportKey = $passportMatch.Groups[1].Value
$passportOldPort = [regex]::Match($passportText,'(?m)^PODCAST_PORT=([1-9][0-9]{0,4})\r?$')
if (-not $passportOldPort.Success -or [int]$passportOldPort.Groups[1].Value -gt 65535) { throw '现有.env的端口无效；为保护原配置，未覆盖它。' }
if ($passportOldPort.Success) {
  if ($PSBoundParameters.ContainsKey('Port') -and [int]$passportOldPort.Groups[1].Value -ne $Port) { throw '已有安装使用不同端口，请沿用原端口。' }
  $Port = [int]$passportOldPort.Groups[1].Value
}
$passportOldUrl = [regex]::Match($passportText,'(?m)^PODCAST_PUBLIC_URL=([^\r\n]*)\r?$')
if (-not $passportOldUrl.Success -or -not (Test-PassportAddress $passportOldUrl.Groups[1].Value)) { throw '现有.env的后台地址无效；为保护原配置，未覆盖它。' }
if ($PublicUrl -and (-not $passportOldUrl.Success -or $passportOldUrl.Groups[1].Value -ne $PublicUrl)) {
  if ($passportOldUrl.Success) { $passportText = [regex]::Replace($passportText,'(?m)^PODCAST_PUBLIC_URL=[^\r\n]*', [System.Text.RegularExpressions.MatchEvaluator]{ param($m) 'PODCAST_PUBLIC_URL='+$PublicUrl }) }
  else { $passportText += "`nPODCAST_PUBLIC_URL=$PublicUrl`n" }
  $passportPending = Join-Path $passportServer ('.env.pending.'+[Guid]::NewGuid().ToString('N'))
  try {
    [IO.File]::WriteAllText($passportPending,$passportText,[Text.UTF8Encoding]::new($false))
    Protect-PassportPath $passportPending
    [IO.File]::Replace($passportPending,$passportEnv,$null)
  } finally {
    if (Test-Path $passportPending) { Remove-Item -LiteralPath $passportPending -Force }
  }
} elseif (-not $PublicUrl -and $passportOldUrl.Success) { $PublicUrl=$passportOldUrl.Groups[1].Value }
$env:PODCAST_PORT = [string]$Port
$env:PODCAST_SETUP_KEY = $passportKey
$env:PODCAST_PUBLIC_URL = $PublicUrl
$env:PODCAST_UID = $passportSavedUid
$env:PODCAST_GID = $passportSavedGid
foreach ($passportFolder in @('data','media')) { New-Item -ItemType Directory -Force -Path (Join-Path $passportServer $passportFolder) | Out-Null }
Protect-PassportPath (Join-Path $passportServer 'data') $true
& docker compose --env-file $passportEnv -f $passportCompose config --quiet
if ($LASTEXITCODE -ne 0) { throw '后台配置检查失败，原数据未改。' }
& docker compose --env-file $passportEnv -f $passportCompose up -d --build
if ($LASTEXITCODE -ne 0) { throw '后台启动失败；请检查容器日志，原数据未删除。' }
$passportLocal = "http://127.0.0.1:$Port"
$passportReady = $false
for ($passportAttempt=0; $passportAttempt -lt 60; $passportAttempt++) {
  try { $passportResponse = Invoke-RestMethod -Uri "$passportLocal/healthz" -TimeoutSec 2; if ($passportResponse.ok) { $passportReady=$true; break } } catch {}
  Start-Sleep -Seconds 1
}
if (-not $passportReady) { throw '后台尚未就绪，原数据保留。请检查容器日志。' }
$passportPrivate = Join-Path $passportRoot '.local'
New-Item -ItemType Directory -Force -Path $passportPrivate | Out-Null
Protect-PassportPath $passportPrivate $true
$passportPage = Join-Path $passportPrivate 'first-setup.html'
$passportBrowser = $passportLocal
if ($PublicUrl) { $passportBrowser=$PublicUrl }
$passportLink = "$passportBrowser/setup#install=$passportKey"
$passportHtml = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>打开播客后台</title><body><h1>后台已启动</h1><p><a href="'+$passportLink+'">打开首次设置，创建自己的管理口令</a></p><p>普通后台地址：'+$passportBrowser+'。硬件请填写服务器的局域网地址，不能填127.0.0.1。</p><p>这份本地说明包含一次性安装信息，不应发送给别人或上传。</p></body></html>'
[IO.File]::WriteAllText($passportPage,$passportHtml,[Text.UTF8Encoding]::new($false))
Protect-PassportPath $passportPage
Write-Host "后台已启动：$passportLocal"
Write-Host "首次设置入口保存在 $passportPage"
Write-Host '创建口令后，在「我的设备」生成配对码，用手机连接硬件设置热点。'
if (-not $NoOpen) { Start-Process $passportPage }
