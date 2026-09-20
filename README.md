# ping看看（pingkk）

适合专业人士和非专业人士的网络检测工具。运维人员可以将它发给办公人员，用于自行排查网络问题并直接导出测试结果。

支持多平台，兼有图形界面和命令行；图形界面采用现代化设计，操作简单，并提供原生级中文支持。

支持：

- TCP、UDP 端口测试
- Ping 和路由追踪
- 持续测试与自定义超时时间
- 一键导出测试报告

## 下载

前往 [Releases](https://github.com/trah01/pingkk/releases) 下载适合你系统的版本。

**Windows 推荐下载 `pingkk-windows-x64-portable.exe`，双击即可使用，无需安装或手动解压。** ARM 设备选择 `pingkk-windows-arm64-portable.exe`。运行时会自动释放依赖到临时目录，正常关闭后清理。

按系统和处理器架构选择下载包，具体文件以对应 Release 的附件为准：

| 系统 | 处理器架构 | 图形界面便携包 / 应用包 | 安装包 | 命令行（CLI）包 | 文件名前缀 |
| --- | --- | --- | --- | --- | --- |
| Windows | x64（Intel / AMD） | `portable.exe`（单文件） | `setup.exe` | `cli.zip` | `pingkk-windows-x64-` |
| Windows | ARM64 | `portable.exe`（单文件） | `setup.exe` | `cli.zip` | `pingkk-windows-arm64-` |
| macOS | Intel（x86_64） | `gui.tar.xz`（含 `.app`） | — | `cli.tar.gz` | `pingkk-macos-intel-` |
| macOS | Apple Silicon（ARM64） | `gui.tar.xz`（含 `.app`） | — | `cli.tar.gz` | `pingkk-macos-arm64-` |
| Linux | x86（32 位） | `portable.tar.xz` | `installer.deb` | `cli.tar.gz` | `pingkk-linux-x86-` |
| Linux | x64（含麒麟 / 统信 x64） | `portable.tar.xz` | `installer.deb` | `cli.tar.gz` | `pingkk-linux-x64-` |
| Linux | ARM64（含麒麟 / 统信 ARM64） | `portable.tar.xz` | `installer.deb` | `cli.tar.gz` | `pingkk-linux-arm64-` |

文件名前缀加上包名即为完整下载文件名，例如 Windows x64 单文件版为 `pingkk-windows-x64-portable.exe`。表中的“—”表示不提供该类型的包。

- **安装包**：Windows 使用 `setup.exe`；Linux 使用 `sudo apt install ./文件名.deb`，由系统安装所需 Qt 运行库，包内不重复携带。首次安装依赖可能需要联网。
- **便携包 / 应用包**：Windows 的 `portable.exe` 直接双击；macOS 解压后打开 `pingkk.app`；Linux 解压后运行 `run-pingkk.sh`。
- **CLI 包**：仅使用命令行时选择。图形界面包也已内置命令行程序，无需重复下载。

Windows 不再提供 x86 / XP 版本。

Windows 单文件版也支持 `程序.exe --extract 新目录`，用于保留解压文件或替换 Qt 动态库；随后可直接运行目录内的 `pingkk-gui.exe`。目标目录必须尚不存在。

## 命令行用法

```bash
# 测试 TCP 端口
pingkk example.com 443 tcp

# 测试 UDP 端口
pingkk example.com 53 udp

# 同时测试 TCP 和 UDP
pingkk example.com 53 all

# 测试多个端口
pingkk example.com 80,443 tcp

# 持续测试
pingkk -t example.com 443 tcp

# Ping 和路由追踪
pingkk example.com
pingkk example.com -r
```

端口类型可使用 `tcp`、`udp` 或 `all`。选项 `-t`、`-r`/`--route` 和 `--timeout`/`-w` 可放在命令中的任意位置。

```bash
# 自定义超时时间（毫秒）
pingkk --timeout 1000 example.com 443 tcp
```

UDP 端口只有在收到目标响应时才会显示为连通；没有响应时显示“状态未知”。

## 许可证

本项目采用 [GNU LGPL v3.0](licenses/LGPL-3.0.txt) 许可证。
