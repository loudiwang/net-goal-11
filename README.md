# Net Goal 11

Net Goal 11 是一个局域网对战联机工具（原生 C++ 版，Release 1.02），
把游戏的 UDP 流量通过 Hook 直接中继到一台服务器，让异地玩家像在同一局域网里一样
进入游戏的 LAN 对战模式。

本目录是**开源免注册版**（MIT 许可证），与同一套源码的 `Link Goal 11`（保留注册/注册机）相互独立。

- 仓库地址：<https://github.com/loudiwang/net-goal-11>
- 许可证：[MIT](LICENSE)
- 克隆源码：

  ```bash
  git clone https://github.com/loudiwang/net-goal-11.git
  ```

## 与 Link Goal 11 的区别

| 项目 | Net Goal 11 | Link Goal 11 |
| --- | --- | --- |
| 授权方式 | 无授权、无机器码、无注册机 | 注册机 + 注册码 + `license.dat` |
| 登录窗口 | 服务器 IP、端口、昵称、固定口令 | 账号、密码（服务器地址来自注册码） |
| 默认账号 | 昵称 `net11`（可改） | `OL11 / VLAN11` |
| 固定口令 | `goal11` | 由注册码决定 |
| 键盘花式辅助 | 已移除 | 已移除 |

两个目录的源码、二进制名、日志名、窗口类名、服务名都各自独立，
可以同时安装在同一台机器上。

## 目录结构

```text
Net Goal 11/
  README.md              本文件
  native/                全部源码与构建脚本
    common/              协议、版本、游戏名称表
    hook/                注入游戏的 x86 Hook（直连中继）
    client/              Win32 客户端与登录窗口
    server/              Linux / Windows UDP 中继服务端
    build/               用 Zig 一键构建
    tests/               联机与界面测试脚本
```

详细的构建、部署、使用与协议说明见 [`native/README.md`](native/README.md)。

## 快速开始

### 1. 构建

需要 Zig 0.17 或更新版本：

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build\build_all.ps1
```

产物在 `native/dist/`：

- `client-win-x86/Net Goal 11.exe` 与 `Net Goal 11.Hook.dll`
- `server-linux-x64/Net Goal 11.Server`
- `server-win-x64/Net Goal 11.Server.exe`（本机测试）
- 对应的 zip 分发包

### 2. 启动服务器

Linux（默认 UDP 45888）：

```bash
sudo bash deploy-linux.sh
```

Windows 本机测试：

```powershell
.\native\dist\server-win-x64\"Net Goal 11.Server.exe" --port 45888
```

记得在云安全组和系统防火墙放行对应的 **UDP 端口**。

### 3. 客户端登录

运行 `Net Goal 11.exe`，在登录窗口填写：

- 服务器 IP：中继服务器地址（本机测试填 `127.0.0.1`）
- 端口：默认 `45888`
- 昵称：默认 `net11`，可随意修改
- 密码：固定口令 `goal11`

然后按 `native/README.md` 中的步骤设置游戏路径、创建/加入房间并开始比赛。

## 版本与兼容性

- 客户端与 Hook 必须是 x86（游戏主程序是 32 位）；
- 客户端、Hook、服务端请使用同一版本构建，不要混用旧版；
- 协议没有加密，只有房间密码哈希，请勿在不可信网络中传递敏感信息。

## 测试

`native/tests/` 内含手动/半自动测试脚本，例如：

- `direct_peer.ps1` / `direct_target.cpp`：直连与中继验证
- `lobby_test.cpp` / `lobby_list_test.cpp` / `lobby_peer.cpp`：大厅协议测试
- `ui_test.ps1` / `ui_step.ps1`：Windows 界面自动化脚本

## 许可证

本项目采用 [MIT License](LICENSE) 开源发布，可自由使用、修改、分发和商用，
但需保留版权声明与许可声明。

Copyright (c) 2026 Net Goal 11 Contributors
