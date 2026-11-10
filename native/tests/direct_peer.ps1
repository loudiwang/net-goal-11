$ErrorActionPreference = 'Stop'

function New-Header([byte]$type, [uint64]$token, [uint32]$srcVirt, [uint32]$dstVirt,
                    [uint16]$srcPort, [uint16]$dstPort, [uint16]$len) {
    $b = New-Object byte[] 32
    [BitConverter]::GetBytes([uint32]0x52313146).CopyTo($b, 0)
    $b[4] = 1
    $b[5] = $type
    [BitConverter]::GetBytes([uint16]0).CopyTo($b, 6)
    [BitConverter]::GetBytes([uint64]$token).CopyTo($b, 8)
    [BitConverter]::GetBytes([uint32]$srcVirt).CopyTo($b, 16)
    [BitConverter]::GetBytes([uint32]$dstVirt).CopyTo($b, 20)
    [BitConverter]::GetBytes([uint16]$srcPort).CopyTo($b, 24)
    [BitConverter]::GetBytes([uint16]$dstPort).CopyTo($b, 26)
    [BitConverter]::GetBytes([uint16]$len).CopyTo($b, 28)
    [BitConverter]::GetBytes([uint16]0).CopyTo($b, 30)
    return $b
}

function New-Hello([string]$room, [string]$name) {
    $p = New-Object byte[] 48
    $r = [Text.Encoding]::ASCII.GetBytes($room)
    $r.CopyTo($p, 0)
    [BitConverter]::GetBytes([uint64]0).CopyTo($p, 8)
    $n = [Text.Encoding]::UTF8.GetBytes($name)
    $n.CopyTo($p, 16)
    return $p
}

function Read-Msg($udp) {
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $data = $udp.Receive([ref]$ep)
    $type = $data[5]
    $token = [BitConverter]::ToUInt64($data, 8)
    $src = [BitConverter]::ToUInt32($data, 16)
    $dst = [BitConverter]::ToUInt32($data, 20)
    $sp = [BitConverter]::ToUInt16($data, 24)
    $dp = [BitConverter]::ToUInt16($data, 26)
    $len = [BitConverter]::ToUInt16($data, 28)
    [pscustomobject]@{
        Type = $type; Token = $token; Src = $src; Dst = $dst
        SrcPort = $sp; DstPort = $dp; Len = $len
        Payload = if ($len -gt 0) { $data[32..(31 + $len)] } else { @() }
    }
}

function IpToStr([uint32]$v) {
    $b = [BitConverter]::GetBytes($v)
    return (($b | ForEach-Object { $_ }) -join '.')
}

$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse('127.0.0.1'), 45999)
$udp = New-Object System.Net.Sockets.UdpClient(0)
$udp.Client.ReceiveTimeout = 10000

$hello = New-Hello 'DIRECT' 'Bob'
$packet = New-Object byte[] (32 + 48)
(New-Header 1 0 0 0 0 0 48).CopyTo($packet, 0)
$hello.CopyTo($packet, 32)
[void]$udp.Send($packet, $packet.Length, $ep)

$ack = Read-Msg $udp
$bobIp = $ack.Src
$bobToken = $ack.Token
Write-Output ("Bob joined: ip={0} token={1}" -f (IpToStr $bobIp), $bobToken)

# Drain the initial PeerList.
while ($udp.Available -gt 0) { [void](Read-Msg $udp) }

# Wait for Alice's Hook to send HOOK->BOB, then answer its broadcast beacon.
$deadline = (Get-Date).AddSeconds(15)
$gotData = $false
$gotBeacon = $false
while ((Get-Date) -lt $deadline) {
    try {
        $message = Read-Msg $udp
    } catch {
        continue
    }
    if ($message.Type -eq 3 -and $message.Len -gt 0) {
        $text = [Text.Encoding]::ASCII.GetString($message.Payload)
        Write-Output ("Bob got Data from {0}:{1} payload={2}" -f (IpToStr $message.Src), $message.SrcPort, $text)
        if ($text -eq 'HOOK->BOB') {
            $reply = [Text.Encoding]::ASCII.GetBytes('BOB->HOOK')
            $replyPacket = New-Object byte[] (32 + $reply.Length)
            (New-Header 3 $bobToken 0 $message.Src 3306 3306 $reply.Length).CopyTo($replyPacket, 0)
            $reply.CopyTo($replyPacket, 32)
            [void]$udp.Send($replyPacket, $replyPacket.Length, $ep)
            Write-Output 'Bob sent BOB->HOOK'
            $gotData = $true
        }
    }
    elseif ($message.Type -eq 4 -and $message.Len -gt 0) {
        $text = [Text.Encoding]::ASCII.GetString($message.Payload)
        Write-Output ("Bob got Broadcast from {0}:{1} payload={2}" -f (IpToStr $message.Src), $message.SrcPort, $text)
        if ($text -eq 'BEACON') {
            $reply = [Text.Encoding]::ASCII.GetBytes('BEACON-REPLY')
            $replyPacket = New-Object byte[] (32 + $reply.Length)
            (New-Header 4 $bobToken 0 0 9999 9999 $reply.Length).CopyTo($replyPacket, 0)
            $reply.CopyTo($replyPacket, 32)
            [void]$udp.Send($replyPacket, $replyPacket.Length, $ep)
            Write-Output 'Bob sent BEACON-REPLY'
            $gotBeacon = $true
        }
    }
    if ($gotData -and $gotBeacon) { break }
}

$bye = New-Header 8 $bobToken 0 0 0 0 0
[void]$udp.Send($bye, 32, $ep)
$udp.Close()
if (-not $gotData) { Write-Output 'Bob did not receive HOOK->BOB'; exit 2 }
if (-not $gotBeacon) { Write-Output 'Bob did not receive BEACON'; exit 3 }
