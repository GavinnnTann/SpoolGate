#Requires -Version 5.1
<#
.SYNOPSIS
    Measures the end-to-end network path from this laptop out to the internet, and
    records the router's own state while it does.

.DESCRIPTION
    Run this twice from the same spot, a few minutes apart:

      1. joined to the SpoolGate SoftAP  -> the NAT-routed path
      2. joined to the upstream Wi-Fi    -> the baseline, without the ESP32 in it

    Then diff the two with -Compare. That difference is the only number that says
    what the ESP32 actually costs you. A single reading cannot separate "the ESP32
    is slow" from "the upstream network is slow", and on a busy campus network the
    second is common.

    Each run measures four things, because a video stream can break for any of them
    and they have nothing in common as fixes:

      throughput           bulk download and upload over HTTPS to Cloudflare's
                           speed-test endpoints, for a fixed number of seconds, so
                           no server of your own is needed
      idle latency         a ping ladder: this router, the upstream gateway, the
                           open internet - so a bad hop can be named, not guessed
      latency under load   the same ping, sampled *during* each transfer. A path
                           whose RTT goes from 4 ms idle to 800 ms loaded will
                           stutter a stream at any throughput figure, and this is
                           the single most common cause of a stream that plays for
                           ten seconds and then freezes
      router-side state    /stat.json before and after, for the per-client signal
                           strength and the heap low-water mark

    Nothing here writes to the router or changes any setting.

.PARAMETER Label
    Name for this run, used in the output filename. Defaults to the current SSID.

.PARAMETER Router
    SoftAP address of the ESP32. Skipped automatically when it does not answer,
    which is what happens on the baseline run.

.PARAMETER Seconds
    Length of each transfer. Both directions ask for more data than can possibly
    arrive in the window and stop on the clock, so the load period is the same
    whether the path is fast or slow.

.PARAMETER DownMB
    Size of the download request, for the one endpoint that takes a size. Defaults to
    its 99 MB maximum, which outlasts the window on any path this project produces.

.PARAMETER DownUrl
    A large file to download instead of the built-in endpoint list - your own server,
    for instance. Used on its own, with no fallbacks.

.PARAMETER Iperf
    Host running "iperf3 -s". Adds a TCP retransmit count and a UDP loss/jitter
    ramp, which are sharper tools than the HTTPS test. It needs a server you can
    actually reach: a box on the campus LAN will NOT work - client isolation is the
    whole reason this project exists - but one reachable over Tailscale does.

.PARAMETER UdpRates
    Megabits per second to try in the UDP ramp. The lowest rate that starts losing
    datagrams is this path's real ceiling for a live stream.

.PARAMETER Compare
    Two result files (paths or globs) to diff instead of measuring.

.EXAMPLE
    .\tools\speedtest.ps1 -Label through-spoolgate
    .\tools\speedtest.ps1 -Label campus-direct
    .\tools\speedtest.ps1 -Compare .\results\speedtest-through-spoolgate-*.json,.\results\speedtest-campus-direct-*.json

.EXAMPLE
    .\tools\speedtest.ps1 -Label through-spoolgate -Iperf vx15
#>
[CmdletBinding(DefaultParameterSetName = 'Measure')]
param(
  [Parameter(ParameterSetName = 'Measure')][string]$Label,
  [Parameter(ParameterSetName = 'Measure')][string]$Router = '192.168.5.1',
  [Parameter(ParameterSetName = 'Measure')][int]$Seconds = 12,
  [Parameter(ParameterSetName = 'Measure')][int]$DownMB = 0,
  [Parameter(ParameterSetName = 'Measure')][string]$DownUrl,
  [Parameter(ParameterSetName = 'Measure')][int]$UpMB = 0,
  [Parameter(ParameterSetName = 'Measure')][int]$PingCount = 20,
  [Parameter(ParameterSetName = 'Measure')][string]$PingTarget = '1.1.1.1',
  [Parameter(ParameterSetName = 'Measure')][string]$Iperf,
  [Parameter(ParameterSetName = 'Measure')][int[]]$UdpRates = @(1, 3, 5, 8),
  [Parameter(ParameterSetName = 'Measure')][int]$IperfSeconds = 10,
  [Parameter(ParameterSetName = 'Measure')][string]$OutDir,
  [Parameter(ParameterSetName = 'Compare', Mandatory = $true)][string[]]$Compare
)

$ErrorActionPreference = 'Stop'

# Download endpoints, tried in order until one answers with data. All of them are
# plain HTTPS GETs of a large file, which is the one thing the campus firewall
# demonstrably allows - the printer's MQTT traffic gets out, so outbound TLS has
# somewhere to go.
#
# More than one, because a single endpoint is a single point of failure in the
# measurement itself, and being told "download FAILED" when the endpoint is simply
# refusing you is worse than useless while debugging a router. Ordered nearest-first:
# a Singapore endpoint keeps the RTT floor low, so the latency-under-load figure
# reflects this Wi-Fi path rather than a trip across an ocean.
$DownEndpoints = @(
  [pscustomobject]@{ name = 'hetzner-sin'; url = 'https://sin-speed.hetzner.com/100MB.bin'; sized = $false },
  [pscustomobject]@{ name = 'linode-sin'; url = 'https://speedtest.singapore.linode.com/100MB-singapore.bin'; sized = $false },
  # Takes the transfer size as a parameter, which is tidier, but rate-limits a machine
  # that asks for ~100 MB several times in a row - which is exactly what an afternoon
  # of debugging looks like. Hence last, not first.
  [pscustomobject]@{ name = 'cloudflare'; url = 'https://speed.cloudflare.com/__down?bytes='; sized = $true }
)

# Upload has no such choice: it needs somewhere willing to accept a large POST and
# throw it away, and Cloudflare's is the only free one of those worth relying on.
$UpUrl = 'https://speed.cloudflare.com/__up'

# Just under Cloudflare's hard cap - ask __down for 100,000,000 or more and it answers
# 403 instead of sending anything. Also the size of the fixed files above, so the
# window and not the file is what ends a transfer on any path this project produces.
$MaxRequestBytes = 99000000

# ---------------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------------

function Write-Head($text) {
  Write-Host ''
  Write-Host $text -ForegroundColor Cyan
  Write-Host ('-' * $text.Length) -ForegroundColor DarkGray
}

function Write-Row($key, $value) {
  Write-Host ('  {0,-30} {1}' -f $key, $value)
}

# ---------------------------------------------------------------------------------
# Environment
# ---------------------------------------------------------------------------------

# Which network this laptop is actually on. Recorded in the result file because the
# easiest way to ruin this measurement is to label a run "through-spoolgate" while
# Windows has quietly roamed back to the campus SSID.
function Get-WlanInfo {
  $info = [ordered]@{ ssid = $null; signal = $null; rx_mbps = $null; tx_mbps = $null; radio = $null }
  try {
    $text = netsh wlan show interfaces 2>$null
  } catch {
    return $info
  }
  foreach ($line in $text) {
    # Anchored so that "BSSID" does not match the SSID line.
    if ($line -match '^\s*SSID\s*:\s*(.+?)\s*$') { $info.ssid = $Matches[1] }
    if ($line -match '^\s*Signal\s*:\s*(.+?)\s*$') { $info.signal = $Matches[1] }
    if ($line -match '^\s*Receive rate \(Mbps\)\s*:\s*(.+?)\s*$') { $info.rx_mbps = $Matches[1] }
    if ($line -match '^\s*Transmit rate \(Mbps\)\s*:\s*(.+?)\s*$') { $info.tx_mbps = $Matches[1] }
    if ($line -match '^\s*Radio type\s*:\s*(.+?)\s*$') { $info.radio = $Matches[1] }
  }
  return $info
}

function Get-DefaultGateway {
  try {
    $r = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop |
      Sort-Object -Property RouteMetric |
      Select-Object -First 1
    if ($r) { return $r.NextHop }
  } catch {}
  return $null
}

function Get-RouterStat($router) {
  if (-not $router) { return $null }
  try {
    return Invoke-RestMethod -Uri ('http://{0}/stat.json' -f $router) -TimeoutSec 4 -ErrorAction Stop
  } catch {
    return $null
  }
}

# ---------------------------------------------------------------------------------
# Latency
# ---------------------------------------------------------------------------------

# Turns a set of round-trip samples into a distribution. The average alone hides
# exactly the behaviour that breaks a video stream: a handful of multi-hundred-
# millisecond stalls in an otherwise healthy sample. Windows reports whole
# milliseconds, which is coarse for a LAN hop but ample for spotting those stalls.
function New-LatencySummary($target, $samples, $sent) {
  $s = @($samples)
  $result = [ordered]@{
    target = $target; sent = $sent; received = $s.Count; loss_pct = $null
    min_ms = $null; avg_ms = $null; p95_ms = $null; max_ms = $null; over_100ms = $null
  }
  if ($sent -gt 0) {
    $result.loss_pct = [math]::Round(100.0 * ($sent - $s.Count) / $sent, 1)
  }
  if ($s.Count -gt 0) {
    $sorted = @($s | Sort-Object)
    $idx = [math]::Min($sorted.Count - 1, [int][math]::Floor(0.95 * ($sorted.Count - 1)))
    $result.min_ms = $sorted[0]
    $result.max_ms = $sorted[$sorted.Count - 1]
    $result.avg_ms = [math]::Round(($s | Measure-Object -Average).Average, 1)
    $result.p95_ms = $sorted[$idx]
    $result.over_100ms = @($s | Where-Object { $_ -ge 100 }).Count
  }
  return $result
}

# System.Net.NetworkInformation.Ping rather than ping.exe. ping.exe would have to be
# run detached to sample during a transfer, and its stdout is block-buffered when
# redirected to a file, so killing it at the end of the window discards up to 4 KiB -
# which for a short run is every sample it took. Calling ICMP directly has no such
# failure mode and gives control of the interval.
$script:Pinger = New-Object System.Net.NetworkInformation.Ping

function Send-OnePing($target, $timeoutMs) {
  try {
    $reply = $script:Pinger.Send($target, $timeoutMs)
    if ($reply.Status -eq [System.Net.NetworkInformation.IPStatus]::Success) {
      return [int]$reply.RoundtripTime
    }
  } catch {}
  return $null
}

function Measure-Ping($target, $count) {
  if (-not $target) { return $null }
  $samples = New-Object System.Collections.ArrayList
  for ($i = 0; $i -lt $count; $i++) {
    $rtt = Send-OnePing $target 1000
    if ($null -ne $rtt) { [void]$samples.Add($rtt) }
    Start-Sleep -Milliseconds 150
  }
  return New-LatencySummary $target $samples $count
}

# ---------------------------------------------------------------------------------
# Transfers
# ---------------------------------------------------------------------------------

# Start-Process joins its ArgumentList with spaces and does no quoting of its own, so
# anything with a space in it has to arrive already quoted.
function Format-ProcArg($a) {
  $s = [string]$a
  if ($s -match '[\s"]') {
    return '"' + ($s -replace '"', '\"') + '"'
  }
  return $s
}

# Runs one curl transfer detached and samples RTT in the foreground for exactly as
# long as it lasts, so the latency figures are taken with the link saturated and
# nothing is measured after the load stops.
#
# curl rather than Invoke-WebRequest: Invoke-WebRequest on PowerShell 5.1 buffers the
# whole body and pushes it through the response-parsing machinery, so it measures
# .NET more than it measures the network. curl.exe ships with Windows 10 and later
# and reports the achieved rate itself.
#
# --max-time ends the transfer on the clock. curl exits 28 when it does, and still
# writes the -w line with however many bytes actually moved, which is what makes a
# fixed-length window possible against an endpoint that only takes a byte count.
# The -w separator is ';' and not a space so the format survives Start-Process.
function Invoke-LoadedTransfer($curlArgs, $writeOut, $pingTarget, $maxSeconds) {
  $wfile = [System.IO.Path]::Combine($env:TEMP, ('spoolgate-curl-{0}.txt' -f [guid]::NewGuid().ToString('N')))
  $all = @('-s', '-o', 'NUL', '--max-time', "$maxSeconds", '-w', $writeOut) + $curlArgs
  $quoted = @($all | ForEach-Object { Format-ProcArg $_ })

  $samples = New-Object System.Collections.ArrayList
  $sent = 0
  $transfer = $null

  try {
    $proc = Start-Process -FilePath 'curl.exe' -ArgumentList $quoted `
      -RedirectStandardOutput $wfile -NoNewWindow -PassThru

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $proc.HasExited) {
      # curl enforces the window itself; this only stops the sampler running away if
      # curl somehow outlives it.
      if ($sw.Elapsed.TotalSeconds -gt ($maxSeconds + 10)) { break }
      if ($pingTarget) {
        $sent++
        $rtt = Send-OnePing $pingTarget 1000
        if ($null -ne $rtt) { [void]$samples.Add($rtt) }
      }
      Start-Sleep -Milliseconds 150
    }
    $proc.WaitForExit(15000) | Out-Null

    $raw = ''
    try { $raw = (Get-Content -LiteralPath $wfile -Raw -ErrorAction Stop) } catch {}
    $parts = ([string]$raw).Trim() -split ';'
    if ($parts.Count -ge 4) {
      # The record is built even for a refused request, so the caller can report the
      # status code. A bare "FAILED" here is nearly useless: 429 from the endpoint and
      # no route out of the network look identical from the outside and mean opposite
      # things about the router.
      $code = 0
      try { $code = [int]$parts[0] } catch {}
      $bytes = [int64]$parts[1]
      $transfer = [ordered]@{
        http_code = $code
        bytes     = $bytes
        mbps      = [math]::Round(([double]$parts[2]) * 8.0 / 1e6, 2)
        seconds   = [math]::Round([double]$parts[3], 2)
        # 100 KB, not 1 byte: an error page has a body too, and it would otherwise be
        # scored as a successful transfer at a hilarious rate.
        ok        = ($code -eq 200 -and $bytes -gt 100000)
        # True when the data ran out before the clock did. The rate is still valid;
        # the latency sample just covers less than the full window.
        completed = ([double]$parts[3] -lt $maxSeconds)
      }
    }
  } finally {
    Remove-Item -LiteralPath $wfile -Force -ErrorAction SilentlyContinue
  }

  return [pscustomobject]@{
    transfer = $transfer
    latency  = New-LatencySummary $pingTarget $samples $sent
  }
}

# The endpoints rate-limit a machine that asks for ~100 MB several times in a row,
# which is exactly what running this script twice in quick succession does. That
# comes back as 429 and is worth waiting out, because the alternative is a result
# file with a hole in it that looks like a network fault.
function Invoke-TransferWithRetry($curlArgs, $writeOut, $pingTarget, $seconds) {
  $r = Invoke-LoadedTransfer $curlArgs $writeOut $pingTarget $seconds
  if ($r.transfer -and -not $r.transfer.ok -and $r.transfer.http_code -eq 429) {
    Write-Host '  rate-limited by the endpoint (HTTP 429) - waiting 30 s and retrying once' -ForegroundColor Yellow
    Start-Sleep -Seconds 30
    $r = Invoke-LoadedTransfer $curlArgs $writeOut $pingTarget $seconds
  }
  return $r
}

function Show-TransferFailure($t) {
  if (-not $t -or $t.http_code -eq 0) {
    Write-Host '  FAILED - nothing came back. No route out of this network, or HTTPS is blocked here.' -ForegroundColor Red
    return
  }
  if ($t.http_code -eq 429) {
    Write-Host '  FAILED - HTTP 429, the endpoint is still rate-limiting this machine.' -ForegroundColor Red
    Write-Host '  Leave it a few minutes before the next run. This is not the router.' -ForegroundColor DarkGray
    return
  }
  Write-Host ('  FAILED - HTTP {0} after {1} bytes.' -f $t.http_code, $t.bytes) -ForegroundColor Red
}

function Invoke-Iperf3($iperfArgs) {
  $raw = & iperf3.exe @iperfArgs 2>$null
  if (-not $raw) { return $null }
  try {
    return ($raw -join "`n") | ConvertFrom-Json
  } catch {
    return $null
  }
}

function Show-Latency($label, $lat) {
  if (-not $lat) { return }
  Write-Row $label ('avg {0} ms, p95 {1} ms, max {2} ms  ({3} samples, {4}% loss, {5} over 100 ms)' -f `
      $lat.avg_ms, $lat.p95_ms, $lat.max_ms, $lat.received, $lat.loss_pct, $lat.over_100ms)
}

# ---------------------------------------------------------------------------------
# Compare mode
# ---------------------------------------------------------------------------------

if ($PSCmdlet.ParameterSetName -eq 'Compare') {
  # A single argument holding both patterns is accepted as well as two. Passing the
  # two globs through an intermediate shell (or an -File invocation) collapses them
  # into one comma-joined string, and failing on that would be a pointless trap.
  $patterns = @()
  foreach ($p in $Compare) { $patterns += ($p -split ',' | Where-Object { $_.Trim() }) }

  $files = @()
  foreach ($pattern in $patterns) {
    $match = Get-ChildItem -Path $pattern.Trim() -ErrorAction Stop | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($match) { $files += $match }
  }
  if ($files.Count -ne 2) {
    throw "Expected two result files, got $($files.Count). Pass two paths, or two globs that each match at least one file."
  }
  $a = Get-Content -LiteralPath $files[0].FullName -Raw | ConvertFrom-Json
  $b = Get-Content -LiteralPath $files[1].FullName -Raw | ConvertFrom-Json

  function Show-Pair($key, $va, $vb) {
    if ($null -eq $va) { $va = '-' }
    if ($null -eq $vb) { $vb = '-' }
    Write-Host ('  {0,-30} {1,14} {2,14}' -f $key, $va, $vb)
  }

  # A refused transfer still carries a rate, and printing it would invent a
  # measurement out of an error page.
  function Get-Rate($transfer) {
    if ($transfer -and $transfer.ok) { return $transfer.mbps }
    return $null
  }

  Write-Head 'Comparison'
  Show-Pair '' $a.label $b.label
  Write-Host ''
  Show-Pair 'SSID' $a.wlan.ssid $b.wlan.ssid
  Show-Pair 'Download Mbit/s' (Get-Rate $a.download.transfer) (Get-Rate $b.download.transfer)
  Show-Pair 'Upload Mbit/s' (Get-Rate $a.upload.transfer) (Get-Rate $b.upload.transfer)
  Write-Host ''
  Show-Pair 'Internet RTT idle avg ms' $a.ping_idle_internet.avg_ms $b.ping_idle_internet.avg_ms
  Show-Pair 'RTT under download avg ms' $a.download.latency.avg_ms $b.download.latency.avg_ms
  Show-Pair 'RTT under download p95 ms' $a.download.latency.p95_ms $b.download.latency.p95_ms
  Show-Pair 'RTT under download max ms' $a.download.latency.max_ms $b.download.latency.max_ms
  Show-Pair 'RTT under upload avg ms' $a.upload.latency.avg_ms $b.upload.latency.avg_ms
  Show-Pair 'RTT under upload max ms' $a.upload.latency.max_ms $b.upload.latency.max_ms
  Show-Pair 'Loss under download %' $a.download.latency.loss_pct $b.download.latency.loss_pct

  Write-Host ''
  Write-Host '  Upload is the direction a camera stream depends on - the printer is the' -ForegroundColor DarkGray
  Write-Host '  sender. Latency under load matters as much as the rate: a stream on a path' -ForegroundColor DarkGray
  Write-Host '  that buffers to hundreds of milliseconds stalls and resyncs regardless of' -ForegroundColor DarkGray
  Write-Host '  how much bandwidth is nominally available. See docs/THROUGHPUT.md.' -ForegroundColor DarkGray
  Write-Host ''
  return
}

# ---------------------------------------------------------------------------------
# Measure mode
# ---------------------------------------------------------------------------------

$wlan = Get-WlanInfo
if (-not $Label) {
  $Label = $wlan.ssid
  if (-not $Label) { $Label = 'run' }
}
$safeLabel = ($Label -replace '[^\w.-]', '_')

if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot '..\results' }
if (-not (Test-Path -LiteralPath $OutDir)) {
  New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$gateway = Get-DefaultGateway

Write-Head 'Where this laptop is'
Write-Row 'SSID' $wlan.ssid
Write-Row 'Signal' $wlan.signal
Write-Row 'Radio / negotiated rate' ('{0} / rx {1} tx {2} Mbps' -f $wlan.radio, $wlan.rx_mbps, $wlan.tx_mbps)
Write-Row 'Default gateway' $gateway
Write-Row 'Label for this run' $Label

$statBefore = Get-RouterStat $Router
if ($statBefore) {
  Write-Head 'Router state before'
  Write-Row 'NAPT' $statBefore.napt
  Write-Row 'Channel' $statBefore.channel
  Write-Row 'Uplink signal' ('{0} dBm' -f $statBefore.sta_rssi)
  Write-Row 'Clients joined' $statBefore.clients
  Write-Row 'Client signal' ('{0} dBm' -f $statBefore.client_rssi)
  Write-Row 'Free heap' ('{0} KiB' -f [math]::Round($statBefore.heap_free / 1024))
  Write-Row 'Min free heap since boot' ('{0} KiB' -f [math]::Round($statBefore.heap_min / 1024))
} else {
  Write-Host ''
  Write-Host ('  {0} did not answer - recording this as a run with the router out of the path.' -f $Router) -ForegroundColor DarkGray
}

# --- Idle latency ladder ---------------------------------------------------------
# Three hops so a bad one can be named rather than guessed at. Hop 1 is the ESP32's
# own radio, hop 2 is whatever it is NAT'd behind, hop 3 is the open internet.

Write-Head ('Idle latency ({0} pings per hop)' -f $PingCount)
$pingRouter = $null
if ($statBefore) {
  $pingRouter = Measure-Ping $Router $PingCount
  Show-Latency 'ESP32 SoftAP' $pingRouter
}
$pingGateway = $null
if ($gateway -and $gateway -ne $Router) {
  $pingGateway = Measure-Ping $gateway $PingCount
  Show-Latency 'Default gateway' $pingGateway
}
$pingIdleInternet = Measure-Ping $PingTarget $PingCount
Show-Latency ('Internet ({0})' -f $PingTarget) $pingIdleInternet

# --- Download --------------------------------------------------------------------

if ($DownMB -le 0) {
  $downBytes = $MaxRequestBytes
} else {
  $downBytes = [math]::Min([int64]($DownMB * 1MB), [int64]$MaxRequestBytes)
}

if ($DownUrl) {
  $candidates = @([pscustomobject]@{ name = 'custom'; url = $DownUrl; sized = $false })
} else {
  $candidates = $DownEndpoints
}

Write-Head ('Download, {0} s window' -f $Seconds)
$down = $null
$downSource = $null
foreach ($ep in $candidates) {
  $url = $ep.url
  if ($ep.sized) { $url = '{0}{1}' -f $ep.url, $downBytes }

  $attempt = Invoke-LoadedTransfer @('-L', $url) `
    '%{http_code};%{size_download};%{speed_download};%{time_total}' $PingTarget $Seconds
  $down = $attempt
  $downSource = $ep.name
  if ($attempt.transfer -and $attempt.transfer.ok) { break }

  # Straight on to the next endpoint rather than waiting out a rate limit: a working
  # mirror answers now, and the point of the list is not having to.
  Write-Host ('  {0} unusable (HTTP {1}) - trying the next endpoint' -f `
      $ep.name, $(if ($attempt.transfer) { $attempt.transfer.http_code } else { 0 })) -ForegroundColor DarkGray
}

if ($down.transfer -and $down.transfer.ok) {
  Write-Row 'Endpoint' $downSource
  Write-Row 'Throughput' ('{0} Mbit/s ({1} MiB in {2} s)' -f `
      $down.transfer.mbps, [math]::Round($down.transfer.bytes / 1MB, 1), $down.transfer.seconds)
  if ($down.transfer.completed) {
    # Only reachable on a link fast enough to pull the whole file inside the window,
    # which the NAT-routed run never will. The rate is still right; the load period
    # was just shorter than asked for, so the latency sample below is thinner.
    Write-Host '  (the file ran out before the window closed - the rate is still valid)' -ForegroundColor DarkGray
  }
} else {
  Write-Host '  every download endpoint failed.' -ForegroundColor Red
  Show-TransferFailure $down.transfer
}
Show-Latency 'RTT under load' $down.latency

# --- Upload ----------------------------------------------------------------------
# The direction that matters for a camera: the printer is the sender, so the stream
# runs client -> ESP32 -> upstream. It is also the weaker direction through a SoftAP,
# which has to receive each packet over the air before it can re-send it.

if ($UpMB -le 0) {
  # Sized to outlast the window at any rate this path could plausibly reach, so the
  # clock and not the file is what ends the transfer.
  $UpMB = [math]::Min([int]($Seconds * 2), [int]($MaxRequestBytes / 1MB))
}
Write-Head ('Upload, {0} s window ({1} MiB of filler)' -f $Seconds, $UpMB)
$upFile = [System.IO.Path]::Combine($env:TEMP, ('spoolgate-up-{0}.bin' -f [guid]::NewGuid().ToString('N')))
$up = $null
try {
  $bytes = New-Object byte[] ($UpMB * 1MB)
  (New-Object System.Random).NextBytes($bytes)
  [System.IO.File]::WriteAllBytes($upFile, $bytes)
  $bytes = $null
  [System.GC]::Collect()

  $up = Invoke-TransferWithRetry @('-X', 'POST', '--data-binary', ('@{0}' -f $upFile), $UpUrl) `
    '%{http_code};%{size_upload};%{speed_upload};%{time_total}' $PingTarget $Seconds
  if ($up.transfer -and $up.transfer.ok) {
    Write-Row 'Throughput' ('{0} Mbit/s ({1} MiB in {2} s)' -f `
        $up.transfer.mbps, [math]::Round($up.transfer.bytes / 1MB, 1), $up.transfer.seconds)
    if ($up.transfer.completed) {
      Write-Host '  (the filler ran out before the window did - raise -UpMB for a longer load period)' -ForegroundColor DarkGray
    }
  } else {
    Show-TransferFailure $up.transfer
  }
  Show-Latency 'RTT under load' $up.latency
} finally {
  Remove-Item -LiteralPath $upFile -Force -ErrorAction SilentlyContinue
}

# --- iperf3, when a reachable server was named -----------------------------------

$iperfResults = $null
if ($Iperf) {
  if ($null -eq (Get-Command iperf3.exe -ErrorAction SilentlyContinue)) {
    Write-Head 'iperf3'
    Write-Host '  iperf3.exe is not on PATH - skipping. Install with: winget install iperf3' -ForegroundColor Yellow
  } else {
    Write-Head ('iperf3 against {0}' -f $Iperf)
    $iperfResults = [ordered]@{ server = $Iperf; tcp_up = $null; tcp_down = $null; udp = @() }

    # Retransmits are the point of running TCP here rather than trusting the HTTPS
    # figure: a rate that looks adequate while thousands of segments are being resent
    # is a path that will stall a stream every few seconds.
    $j = Invoke-Iperf3 @('-c', $Iperf, '-J', '-t', $IperfSeconds)
    if ($j -and $j.end) {
      $iperfResults.tcp_up = [ordered]@{
        mbps        = [math]::Round($j.end.sum_sent.bits_per_second / 1e6, 2)
        retransmits = $j.end.sum_sent.retransmits
      }
      Write-Row 'TCP upload' ('{0} Mbit/s, {1} retransmits' -f $iperfResults.tcp_up.mbps, $iperfResults.tcp_up.retransmits)
    } else {
      Write-Host '  TCP upload failed - is "iperf3 -s" running on that host, and reachable from here?' -ForegroundColor Yellow
    }

    $j = Invoke-Iperf3 @('-c', $Iperf, '-J', '-R', '-t', $IperfSeconds)
    if ($j -and $j.end) {
      $iperfResults.tcp_down = [ordered]@{
        mbps        = [math]::Round($j.end.sum_received.bits_per_second / 1e6, 2)
        retransmits = $j.end.sum_sent.retransmits
      }
      Write-Row 'TCP download' ('{0} Mbit/s, {1} retransmits' -f $iperfResults.tcp_down.mbps, $iperfResults.tcp_down.retransmits)
    }

    # The ramp maps most directly onto a camera: a stream is a fixed offered rate, and
    # the question is the rate at which this path starts dropping. The first row with
    # non-trivial loss is the ceiling, and if it sits below the stream's bitrate the
    # stream cannot work no matter what else is fixed.
    foreach ($rate in $UdpRates) {
      $j = Invoke-Iperf3 @('-c', $Iperf, '-J', '-u', '-b', ('{0}M' -f $rate), '-l', '1200', '-t', $IperfSeconds)
      if ($j -and $j.end -and $j.end.sum) {
        $row = [ordered]@{
          offered_mbps = $rate
          actual_mbps  = [math]::Round($j.end.sum.bits_per_second / 1e6, 2)
          loss_pct     = [math]::Round($j.end.sum.lost_percent, 2)
          jitter_ms    = [math]::Round($j.end.sum.jitter_ms, 2)
        }
        $iperfResults.udp += $row
        $colour = 'Gray'
        if ($row.loss_pct -ge 1) { $colour = 'Yellow' }
        if ($row.loss_pct -ge 5) { $colour = 'Red' }
        Write-Host ('  {0,-30} {1}' -f ('UDP at {0} Mbit/s' -f $rate),
          ('{0}% loss, {1} ms jitter, {2} Mbit/s delivered' -f $row.loss_pct, $row.jitter_ms, $row.actual_mbps)) -ForegroundColor $colour
      }
    }
  }
}

# --- Router state after ----------------------------------------------------------

$statAfter = Get-RouterStat $Router
if ($statAfter) {
  Write-Head 'Router state after'
  Write-Row 'Free heap' ('{0} KiB' -f [math]::Round($statAfter.heap_free / 1024))
  Write-Row 'Min free heap since boot' ('{0} KiB' -f [math]::Round($statAfter.heap_min / 1024))
  if ($statBefore) {
    # The low-water mark only ever falls, so any movement here was caused by this run.
    # The Wi-Fi driver's transmit buffers come out of the same heap, and a forwarding
    # path that runs out of them is a ceiling no amount of signal will lift.
    $drop = [math]::Round(($statBefore.heap_min - $statAfter.heap_min) / 1024)
    Write-Row 'Low-water mark fell by' ('{0} KiB during this run' -f $drop)
  }
  Write-Row 'Largest free block' ('{0} KiB' -f [math]::Round($statAfter.heap_largest_block / 1024))
  Write-Row 'Uplink signal' ('{0} dBm' -f $statAfter.sta_rssi)
  Write-Row 'Client signal' ('{0} dBm' -f $statAfter.client_rssi)
  Write-Row 'Gateway / client reachable' ('{0} / {1}' -f $statAfter.health_up, $statAfter.health_down)
}

# --- Save ------------------------------------------------------------------------

$result = [ordered]@{
  label              = $Label
  timestamp          = (Get-Date).ToString('o')
  window_seconds     = $Seconds
  wlan               = $wlan
  gateway            = $gateway
  router             = $Router
  router_stat_before = $statBefore
  router_stat_after  = $statAfter
  ping_idle_router   = $pingRouter
  ping_idle_gateway  = $pingGateway
  ping_idle_internet = $pingIdleInternet
  download           = $down
  upload             = $up
  iperf3             = $iperfResults
}

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$outFile = Join-Path $OutDir ('speedtest-{0}-{1}.json' -f $safeLabel, $stamp)
$result | ConvertTo-Json -Depth 8 | Out-File -LiteralPath $outFile -Encoding utf8

Write-Head 'Saved'
Write-Row 'Result file' $outFile
Write-Host ''
Write-Host '  Run this again on the other network, then compare the two:' -ForegroundColor DarkGray
Write-Host ('    .\tools\speedtest.ps1 -Compare "{0}\speedtest-<a>-*.json","{0}\speedtest-<b>-*.json"' -f $OutDir) -ForegroundColor DarkGray
Write-Host '  docs/THROUGHPUT.md explains how to read the numbers.' -ForegroundColor DarkGray
Write-Host ''
