# Net Goal 11 原生 C++ 版（Hook 直连） Release 1.02

Net Goal 11 是一个局域网对战联机工具（原生 C++ 重写版），用来让同一款足球游戏的不同机器
像接在同一台交换机上一样进入 LAN 对战。数据面由注入游戏的 Hook 直接连到中继服务器，
不再经过两次本地 IPC 和每包 `Sleep(5)` 轮询：

```text
旧版：游戏主程序 -> Hook -> 127.0.0.1 UDP -> 客户端 GUI -> 公网 -> VPS
新版：游戏主程序 -> Hook -------------------------> 公网 -> VPS
```

本地 127.0.0.1 现在只传状态、房间成员、统计和“重连/断开”命令，不再传游戏包。

本版本是**开源免注册版**：

- 没有机器码、`license.dat`、注册窗口和注册机；
- 登录窗口直接填写服务器 IP / UDP 端口，密码使用固定口令 `goal11`；
- “账号”输入框就是游戏内昵称，默认 `net11`，可随意修改；
- 已移除原版的「键盘花式辅助」（按键宏）功能。

## 目录

```text
native/
  common/protocol.h          新旧协议兼容定义 + Probe/ProbeAck + Hook IPC
  common/game_names.h        游戏主程序/目录的混淆名称表（不要修改）
  hook/hook_direct.cpp       x86 Hook，直接连接中继服务器，虚拟 IP 伪装/流量注入
  client/client.cpp          Win32 x86 主界面，配置、探测、注入、状态
  client/login.cpp           登录窗口（服务器 IP/端口 + 昵称 + 固定口令）
  server/server.cpp          Linux epoll / Windows select 的 UDP 中继
  build/build_all.ps1        用 Zig 构建全部二进制
  tests/                     手动/半自动联机与界面测试脚本
  dist/
    client-win-x86/          客户端目录（exe + Hook DLL + login_bg.jpg + main_banner.jpg）
    server-linux-x64/        Linux 静态服务端
    server-win-x64/          Windows 测试服务端
    *.zip                    可直接分发的包
```

## 构建（不需要 Visual Studio）

需要 Zig 0.17 或更新版本（当前环境路径：
`<Zig 安装目录>\zig.exe`）。

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1
# 或者：
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1 -Zig "C:\path\to\zig.exe"
```

### Windows 7 版客户端

Zig 自带的 mingw 运行库是 UCRT 版本，还引用了 Windows 8 才有的
`GetSystemTimePreciseAsFileTime`，所以默认构建在纯净的 Windows 7 SP1 上无法启动。
加 `-Win7Gcc` 指向一个使用 msvcrt 运行库的 32 位 mingw-w64 GCC
（例如 winlibs 的 `i686-...-mingw-w64msvcrt-...`）即可额外生成 Win7 版：

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1 `
  -Zig "C:\path\to\zig.exe" `
  -Win7Gcc "C:\path\to\mingw32\bin\i686-w64-mingw32-g++.exe"
```

Win7 版输出到 `native/dist/client-win-x86-win7/`，压缩包为
`native/dist/Net Goal 11-client-win-x86-win7.zip`，只依赖系统自带的
`msvcrt.dll`，不需要安装 UCRT 或 VC++ 运行库。

输出：

- `native/dist/client-win-x86/Net Goal 11.exe`（x86）
- `native/dist/client-win-x86/Net Goal 11.Hook.dll`（x86）
- `native/dist/client-win-x86-win7/`（Windows 7 版，可选）
- `native/dist/server-linux-x64/Net Goal 11.Server`（x86_64 Linux，静态 musl）
- `native/dist/server-win-x64/Net Goal 11.Server.exe`（本机测试用）
- `native/dist/Net Goal 11-client-win-x86.zip`
- `native/dist/Net Goal 11-client-win-x86-win7.zip`（Windows 7 版，可选）
- `native/dist/Net Goal 11-server-linux-x64.zip`
- `native/dist/Net Goal 11-server-win-x64.zip`（Windows 本机测试用）

游戏主程序是 32 位程序，客户端和 Hook 必须用 x86，不要用 x64 客户端注入。

## 部署 Linux VPS

服务端监听一个指定的 UDP 端口（默认 `45888`，部署时可用 `--port` 修改）。
服务端协议对原来的消息 1..9 兼容，并新增 GUI 探测消息 `Probe=10 / ProbeAck=11`。

把 `Net Goal 11-server-linux-x64.zip` 上传到 VPS，解压后：

```bash
sudo bash deploy-linux.sh
```

（Windows 打的 zip 不保留 Linux 可执行位；用 `bash` 运行，或先 `chmod +x deploy-linux.sh`。）

或者手动：

```bash
sudo install -d /opt/net-goal-11
sudo install -m 755 "Net Goal 11.Server" "/opt/net-goal-11/Net Goal 11.Server"
sudo install -m 644 net-goal-11.service /etc/systemd/system/net-goal-11.service
sudo systemctl daemon-reload
sudo systemctl enable --now net-goal-11
sudo systemctl status net-goal-11 --no-pager
```

检查：

```bash
ss -lunp | grep 45888
journalctl -u net-goal-11 -f
```

云安全组和系统防火墙都要放行该 **UDP 端口**。如果使用 ufw（把端口换成实际端口）：

```bash
sudo ufw allow 45888/udp
```

回滚：`/opt/net-goal-11/Net Goal 11.Server.bak.*` 是部署脚本自动保留的上一版；
停服务后复制回去即可。

## 登录（服务器 IP / 端口 + 固定口令）

本版**没有注册码，也不访问任何授权服务**，登录完全在本地完成：

- **服务器 IP**：中继服务器的公网/局域网地址，默认 `127.0.0.1`；
- **端口**：中继服务器的 UDP 端口，默认 `45888`；
- **昵称**：默认 `net11`，也就是游戏内显示的名字，可以随意修改；
- **密码**：固定口令 `goal11`（开源版共享口令，不是注册码）。

填好后点“登录”。服务器地址、端口和昵称会记住在
`HKCU\Software\Net Goal 11\Login`（密码用 Windows DPAPI 加密保存），下次打开自动填入。

登录页使用 1120x700 无边框窗口，背景图 `login_bg.jpg` 必须和 `Net Goal 11.exe`
放在同一目录；缺失时会回退到纯色渐变背景。

## 主界面

主窗口标题为 `Net Goal 11 对战系统 Release 1.02`，固定为 `1046x735`，采用深色 + 金色边框 +
球场顶部横幅的对战平台风格：

- **顶部**：全宽球场广告横幅，不显示工具栏按钮
- **左侧**：`对战游戏平台` 树，显示 `Net Goal 11` 和当前所在房间
- **中间**：`游戏房间列表`
  - 右上角是 `刷新` 和 `新建房间`
  - 每行显示房间号、房主、模式、人数（如 `2/4`）、密码标记和 `加入房间`
  - 已开始的房间会标记为不可加入
  - 停留在主页时每 2 秒自动刷新一次
- **右侧**：`我的设置`：昵称、游戏路径、浏览、保存设置、启动单机游戏、
  报文日志和运行日志
  - 金色 `启动单机游戏` 按钮在 `浏览... / 保存设置` 正下方，宽度与两个按钮之和一致；
    点击后直接启动游戏并注入 Hook，不需要先建房间，成功日志为
    `[YY-MM-DD HH:MM:SS] 游戏已启动。`
  - 运行日志的时间戳统一为 `[YY-MM-DD HH:MM:SS]`，右侧是细窄深色滚动条
  - 房间卡片底部留白加大，卡片之间不再额外画分隔线
- **底部**：当前状态和上下行统计

登录后主页就是公开房间列表，不再需要先输入房间号：

- 点击 `新建房间` 打开创建窗口，房间号默认填好 `Net11_昵称`（可修改）、
  可选密码，并选择 **1v1 / 2v2 / 3v3 / 4v4 / 5v5**；创建成功后自动进入房间。
- 点击列表行或该行的 `加入房间` 直接加入；有密码的房间会先弹出密码框。
- 房间人数达到上限、或房间已经开始比赛时，服务端会拒绝加入。

### 独立房间窗口

创建或加入成功后，会打开独立的 `游戏房间` 窗口：

- 顶部显示房间号、模式、人数（如 `2/4`）、是否有密码；
- 右上角关闭 X 左侧有最小化按钮；最小化后房间不会解散，可点任务栏恢复；
- 成员列表显示昵称、`[房主]` 标记和游戏路径是否就绪；
- 房主在每个非房主成员行右侧看到 `踢出`，点击后由服务端校验并移出该成员；
- 底部是 `开始比赛`（仅房主可点）和 `离开房间`；
- 左侧是 `房间聊天`，右侧是成员列表：输入文字后点 `发送` 或按回车，
  消息会广播给房间内所有成员；
- 有玩家加入时，所有成员会看到系统消息，例如：
  `【2026-11-04 17:36:20】 欢迎Guest1用户加入Net11_net11房间。`；
- 点击 `开始比赛` 时才检查人数和游戏路径：少于 2 人、或仍有成员没有设置有效
  游戏路径时，会弹窗提示原因，不会启动游戏；
- 房主点击并校验通过后，服务端向房间内所有客户端广播 `LobbyStart`，
  各客户端收到后自动启动游戏并注入 Hook。

## 客户端使用

1. 双方都解压 `Net Goal 11-client-win-x86.zip`，运行 `Net Goal 11.exe`
   （同目录保留 `Net Goal 11.Hook.dll`、`login_bg.jpg`、`main_banner.jpg` 和 `app.ico`）。
2. 在登录窗口填写服务器 IP、端口、昵称和密码 `goal11`，点击登录。
3. 登录成功后进入主页。先在右侧 `我的设置` 里确认昵称和游戏主程序路径，
   点击 `保存设置`。
4. **房主**点击 `新建房间`：
   - 房间号默认是 `Net11_昵称`，可修改，密码可选；
   - 选择 **1v1 / 2v2 / 3v3 / 4v4 / 5v5**，模式决定房间最大人数
     （1v1=2 人、2v2=4 人、3v3=6 人、4v4=8 人、5v5=10 人）；
   - 创建成功后自动进入独立房间窗口，房间会立即出现在主页列表中。
5. **其他玩家**在主页房间列表中找到该房间，点击 `加入房间`；有密码时输入密码。
   加入成功后打开同一个房间窗口，双方都能看到成员列表。
6. 房主可以在房间窗口里把不想要的成员 `踢出`；被踢出的客户端会收到提示并回到主页。
7. 双方都进入房间后，**只有房主**可以点 `开始比赛`。服务端会校验：
   - 请求者确实是房主；
   - 房间里至少有房主和一个客户端；
   - 所有成员都已设置游戏路径；
   - 房主的游戏路径文件确实存在。
   校验通过后，服务端向房主和所有客户端同时发送启动消息，各客户端自动启动游戏并注入 Hook。
8. 每台机器启动时：
   - 客户端以挂起方式启动游戏；
   - 注入 Hook；
   - Hook 直接向服务器发 `Hello`，拿到虚拟 IP（由服务端自动分配，每个玩家不同）；
   - 客户端收到 `RelayReady` 后才恢复游戏主线程，保证游戏第一次枚举网卡时已经看到虚拟 IP；
   - 如果 10 秒内没有连上中继，客户端会终止这次启动，避免游戏带着真实 IP 进入 LAN。
9. 进游戏的 LAN 模式，双方应能看到彼此/房间。双方必须使用这一版客户端、服务端和 Hook。
10. **房主的游戏路径没有设置或文件不存在时，点击 `开始比赛` 会弹窗提示**；
    即使通过其他方式触发，服务端和客户端也会拒绝启动。其他成员没有设置游戏路径时，
    房主点击后同样会提示具体成员，避免对方无法自动进游戏。
11. **离开房间**只退出当前大厅房间；**断开**只让 Hook 退出中继，不会关闭游戏。
    关闭客户端不会中断已经建立的直连，除非你点了断开或退出游戏。
12. 如果房主离开房间或关闭客户端，服务端会关闭该房间并通知其他成员；
    其他成员会被移出房间，需要重新创建/加入。

## 客户端配置

`client_config.ini`（与 `Net Goal 11.exe` 同目录）保存上次使用的设置：

```ini
# Net Goal 11 client configuration
room=
password=
name=
game=
mode=1
```

- `room` / `password`：上次使用的房间号和密码；
- `name`：昵称，留空时使用登录窗口里填写的昵称；
- `game`：游戏主程序路径；
- `mode`：房间模式（1～5 对应 1v1～5v5；实际以创建窗口选择为准）。

## 日志与排查

- Hook 日志：`%TEMP%\Net Goal 11 Hook.log`（客户端默认 `log_level=1`，只记录加入/断开/错误；
  勾选“报文日志”会写成 2，**会明显增加开销，联机时不要开**）。
- 客户端“运行日志”会显示服务器探测、注入、Hook 状态、虚拟 IP、房间成员和上下行统计。
- 服务端：`journalctl -u net-goal-11 -f`，正常会看到 `[room] ... created`、
  `[join] <虚拟地址> ...`、`[leave] ...`。
- 如果登录窗口提示“密码错误”：开源版固定口令是 `goal11`。
- 如果“连接”失败：
  - 确认 VPS 上 `ss -lunp | grep <端口>` 有输出；
  - 确认云安全组放行对应的 UDP 端口；
  - 确认登录窗口里的服务器 IP 和端口填写正确；
  - 确认服务端是这一版 C++ 服务端。
- 如果双方都显示虚拟 IP，但游戏 LAN 里看不到房间：
  - 确认双方房间号、密码完全一致；
  - 确认 Hook 日志里有 `relay joined room=...`；
  - 检查 Windows 防火墙是否允许游戏主程序的专用/公用网络 UDP；
  - 双方必须是 x86 Hook，且注入发生在游戏枚举网卡之前。

## 协议说明

- 原有 `Hello/HelloAck/Data/Broadcast/PeerList/Ping/Pong/Bye/Error` 的 32 字节头和负载语义保持不变。
- 服务端额外支持 `Probe=10`（无会话探测）和 `ProbeAck=11`（返回 subnet/mask）。
- 房间大厅消息（GUI 与服务端之间，不经过 Hook）：
  - `LobbyCreate=13`：房主创建房间，携带模式、房间号、密码哈希、昵称、是否已设置游戏路径；
  - `LobbyJoin=14`：客户端加入房间；
  - `LobbyLeave=15`：离开房间；
  - `LobbyState=16`：服务端下发的模式、成员列表、房主标记和游戏路径状态；
  - `LobbyStart=17`：房主请求启动，服务端校验后广播给房间内所有客户端；
  - `LobbyPing=18`：大厅心跳，同时刷新昵称和“是否已设置游戏路径”；
  - `LobbyBrowse=19`：主页请求公开房间列表；
  - `LobbyList=20`：服务端返回房间列表（最多 16 个房间，每项包含房间号、模式、
    当前人数、容量、是否已开始/是否有密码、房主昵称）；
  - `LobbyKick=21`：房主按成员槽位踢出成员，服务端校验后广播新的 `LobbyState`；
  - `LobbyChat=22`：客户端发送房间聊天文本；
  - `LobbyChatMessage=23`：服务端广播格式化后的聊天或系统消息（含时间、昵称、
    是否为系统消息），加入房间时用于发送欢迎消息。
- Hook IPC 头仍为 20 字节，新增：
  - `RelayConfig=6`：GUI 要求 Hook 用新服务器/房间重连；
  - `Status=7`：Hook -> GUI，状态 + 虚拟 IP/subnet/mask + 文本；
  - `Peers=8`：Hook -> GUI，PeerList 负载；
  - `Stats=9`：Hook -> GUI，上下行包/字节计数；
  - `RelayReady=10`：HelloAck 成功，客户端可以恢复游戏；
  - `RelayStop=11`：GUI 要求 Hook 断开。

## 安全与限制

- 客户端/Hook 与服务器之间的所有数据（房间列表、房间聊天、转发的游戏 UDP 包）都已加密：
  先做临时 X25519 密钥交换，再用 XSalsa20-Poly1305（NaCl secretbox）逐包认证加密，
  并带重放窗口；服务器不再接受明文数据报。
- 握手没有身份认证，能防止被动窃听，但无法防止恶意网络中的主动中间人攻击。
- 中继服务器能看到解密后的协议头（用于按房间/虚拟 IP 转发），但不会解析游戏负载。
- 开源版没有授权校验，任何人都可以运行客户端并连接你部署的服务器；
  如需限制使用范围，请自行在网络层（防火墙/白名单/VPN）做访问控制。
- 手柄/按键映射问题与中继无关：如果键盘能控制、手柄不能控制，仍然需要在游戏的
  控制器设置里把对应球队的控制器槽位改成 Xbox 手柄。
