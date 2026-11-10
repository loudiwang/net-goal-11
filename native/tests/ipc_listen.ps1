$ErrorActionPreference = 'Stop'
$udp = New-Object System.Net.Sockets.UdpClient(45989)
$udp.Client.ReceiveTimeout = 1000
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline) {
    try {
        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $data = $udp.Receive([ref]$ep)
        if ($data.Length -lt 20) { continue }
        $magic = [BitConverter]::ToUInt32($data, 0)
        $type = $data[4]
        $len = [BitConverter]::ToUInt16($data, 16)
        Write-Output ("ipc type={0} len={1} magic=0x{2:X8}" -f $type, $len, $magic)
        if ($type -eq 10) {
            $self = [BitConverter]::ToUInt32($data, 20)
            Write-Output ("RELAY READY self={0}" -f (($self -as [uint32])))
            break
        }
    } catch {
        continue
    }
}
$udp.Close()
