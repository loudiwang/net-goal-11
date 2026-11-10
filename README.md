<p align="center">
  <img src="docs/images/banner.jpg" alt="Net Goal 11" width="100%">
</p>

# Net Goal 11

**FIFA 11 局域网对战联机工具 · LAN-play tunnel for FIFA 11**

[简体中文](#简体中文) · [English](#english)

- 仓库 / Repository：<https://github.com/loudiwang/net-goal-11>
- 许可证 / License：[GNU GPL v3.0](LICENSE)
- 克隆 / Clone：

  ```bash
  git clone https://github.com/loudiwang/net-goal-11.git
  ```

---

## 简体中文

Net Goal 11 是一个 **FIFA 11 局域网对战** 联机工具（原生 C++ 版，Release 1.02）。

FIFA 11 的对战模式只在局域网（LAN）里可见，异地玩家原本无法互相加入。本工具把游戏
发出的 UDP 数据通过注入游戏的 Hook 直接中继到一台公网服务器，再由服务器转发给对手，
让不同城市的玩家像接在同一台交换机上一样进入游戏的 LAN 对战模式——不需要公网 IP，
不需要路由器端口映射，也不需要安装虚拟网卡驱动。

本目录是**开源免注册版**（GPL-3.0 许可证），没有机器码、`license.dat`、注册窗口和注册机。

### 功能特性

- **FIFA 11 LAN 对战联机**：游戏里照常进入 LAN 模式，联机过程对游戏完全透明。
- **Hook 直连中继**：数据面路径为 `游戏主程序 → Hook → 公网中继 → 对手`，不再多绕一次
  本地 IPC 和非停轮询，延迟更低。
- **虚拟局域网伪装**：Hook 为游戏伪造 `10.213.77.0/24` 虚拟网段，游戏看到的仍是标准
  局域网广播与单播。
- **房间系统**：房间列表 / 房间号直连、可选房间密码、`1v1` `2v2` `3v3` `4v4` `5v5`
  五种对战模式。
- **无授权门槛**：登录窗口只要填服务器 IP、UDP 端口、昵称和固定口令 `goal11`。
- **双平台服务端**：Linux（`epoll`，静态 musl 单文件）与 Windows（`select`，本机测试用）。
- **同时可装多套**：源码、二进制名、日志名、窗口类名、服务名各自独立，可与保留注册机
  的同源版本 `Link Goal 11` 装在同一台机器上。

### 界面截图

| 登录窗口 | 主界面 |
| --- | --- |
| ![登录窗口](docs/images/screenshot-login.png) | ![主界面](docs/images/screenshot-main.png) |

- **登录窗口**：填写中继服务器 IP、端口（默认 `45888`）、游戏内昵称和固定口令 `goal11`。
- **主界面**：左侧对战游戏平台，中间房间列表（房间号 / 房主 / 模式 / 人数），右侧设置
  昵称、游戏主程序路径、启动单机游戏，底部实时显示上下行流量与运行日志。

### 目录结构

```text
Net Goal 11/
  README.md              本文件
  docs/images/           README 用截图与海报
  native/                全部源码与构建脚本
    common/              协议、版本、游戏名称表
    hook/                注入游戏的 x86 Hook（直连中继）
    client/              Win32 客户端与登录窗口
    server/              Linux / Windows UDP 中继服务端
    build/               用 Zig 一键构建
    tests/               联机与界面测试脚本
```

详细的构建、部署、使用与协议说明见 [`native/README.md`](native/README.md)。

### 快速开始

#### 1. 构建

需要 Zig 0.17 或更新版本：

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1
```

产物在 `native/dist/`：

- `client-win-x86/Net Goal 11.exe` 与 `Net Goal 11.Hook.dll`
- `server-linux-x64/Net Goal 11.Server`
- `server-win-x64/Net Goal 11.Server.exe`（本机测试）
- 对应的 zip 分发包

Windows 7 版本需要额外指定 32 位 msvcrt 工具链，见 `native/README.md`。

#### 2. 启动服务器

Linux（默认 UDP 45888）：

```bash
sudo bash deploy-linux.sh
```

Windows 本机测试：

```powershell
.\native\dist\server-win-x64\"Net Goal 11.Server.exe" --port 45888
```

记得在云安全组和系统防火墙放行对应的 **UDP 端口**。

#### 3. 客户端登录

运行 `Net Goal 11.exe`，在登录窗口填写：

- 服务器 IP：中继服务器地址（本机测试填 `127.0.0.1`）
- 端口：默认 `45888`
- 昵称：默认 `net11`，可随意修改
- 密码：固定口令 `goal11`

然后按 `native/README.md` 中的步骤设置游戏路径、创建/加入房间并开始比赛。

### 版本与兼容性

- 客户端与 Hook 必须是 x86（游戏主程序是 32 位）；
- 客户端、Hook、服务端请使用同一版本构建，不要混用旧版；
- 协议没有加密，只有房间密码哈希，请勿在不可信网络中传递敏感信息。

### 测试

`native/tests/` 内含手动/半自动测试脚本，例如：

- `direct_peer.ps1` / `direct_target.cpp`：直连与中继验证
- `lobby_test.cpp` / `lobby_list_test.cpp` / `lobby_peer.cpp`：大厅协议测试
- `ui_test.ps1` / `ui_step.ps1`：Windows 界面自动化脚本

---

## English

**Net Goal 11** is a LAN-play tunneling tool for **FIFA 11** (native C++, release 1.02).

FIFA 11 only exposes its multiplayer mode on a local network, so players in different places
cannot normally join each other. Net Goal 11 injects a small x86 Hook into the game and relays
the game's UDP traffic straight to a public relay server, which forwards it to the other players.
Everyone ends up in the game's normal LAN mode as if they were plugged into the same switch —
no public IP, no router port forwarding, and no virtual network adapter driver required.

This directory is the **open-source, registration-free edition** (GPL-3.0): no machine ID,
no `license.dat`, no registration window and no key generator.

### Features

- **FIFA 11 LAN play over the internet** — the game keeps using its standard LAN mode; the
  tunneling is completely transparent to it.
- **Hook-to-relay data path** — `game → Hook → public relay → peers`, skipping the extra local
  IPC hop and the per-packet polling of the older design, which reduces latency.
- **Virtual LAN spoofing** — the Hook fakes a `10.213.77.0/24` subnet, so the game still sees
  ordinary LAN broadcast and unicast traffic.
- **Room system** — room list and join-by-code, optional room passwords, and `1v1` `2v2` `3v3`
  `4v4` `5v5` modes.
- **No licensing friction** — the login window only asks for the relay IP, UDP port, nickname
  and the fixed passphrase `goal11`.
- **Relay server for two platforms** — Linux (`epoll`, single static musl binary) and Windows
  (`select`, for local testing).
- **Coexists with other builds** — source tree, binary names, log names, window classes and
  service names are all distinct, so it can live next to the registration-based variant.

### Screenshots

| Login window | Main window |
| --- | --- |
| ![Login window](docs/images/screenshot-login.png) | ![Main window](docs/images/screenshot-main.png) |

### Repository layout

```text
Net Goal 11/
  README.md              this file
  docs/images/           screenshots and artwork used by this README
  native/
    common/              protocol, version, game-name tables
    hook/                x86 Hook injected into the game (direct relay)
    client/              Win32 client and login window
    server/              Linux / Windows UDP relay server
    build/               one-shot build with Zig
    tests/               manual / semi-automatic network and UI tests
```

### Quick start

```powershell
# 1. build (needs Zig 0.17+); output goes to native/dist/
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1

# 2. run the relay server locally for testing
.\native\dist\server-win-x64\"Net Goal 11.Server.exe" --port 45888
```

On a Linux VPS, unpack `Net Goal 11-server-linux-x64.zip` and run `sudo bash deploy-linux.sh`.
Remember to open the chosen **UDP port** (default `45888`) in the firewall and cloud security group.

Then start `Net Goal 11.exe`, enter the relay IP, port `45888`, your nickname and the passphrase
`goal11`, pick your game executable, create or join a room and start the match.
See [`native/README.md`](native/README.md) for the full build, deployment and protocol notes.

### Compatibility

- Client and Hook must be x86 — the game executable is 32-bit;
- build client, Hook and server from the same revision;
- the protocol is not encrypted (room passwords are only hashed), so do not send sensitive
  data over untrusted networks.

---

## 许可证 / License

本项目采用 [GNU General Public License v3.0](LICENSE) 开源发布。
This project is released under the [GNU General Public License v3.0](LICENSE).

Copyright (C) 2026 Net Goal 11 Contributors

你可以自由使用、修改和分发本项目；但若分发修改后的版本（包括仅分发二进制），必须同样以
GPL-3.0 授权，并向接收者提供对应的完整源代码。本程序不提供任何担保。
You may use, modify and redistribute this project freely; if you distribute a modified
version (including binaries only), it must also be licensed under the GPL-3.0 and you must
make the complete corresponding source code available to the recipients. There is no warranty.
