# FEKER Codex Bridge

Use a supported FEKER keyboard as a whole-board Codex status light.

[中文](#中文) · [English](#english)

---

# 中文

FEKER Codex Bridge 是一个 macOS 菜单栏应用。它只读监听本机 Codex 任务状态，并让整块键盘显示当前最高优先级状态。它不映射数字键、不监听快捷键、不需要 Karabiner、Codex Hook 或“输入监控”权限。

## 状态灯逻辑

| 状态 | 默认 Codex 配色 |
| --- | --- |
| 执行中 | 蓝色呼吸，峰值 `#304FFE` |
| 任务刚完成 | 绿色 `#00FF4C`，显示 10 秒 |
| 空闲 | 白色 `#FFFFFF` |
| 等待输入或批准 | 橙色 `#FF6D00` |
| 出错 | 红色 `#FF0033` |
| 没有任务 | 熄灭 |

多个任务同时存在时，整板显示优先级最高的状态：

```text
出错 > 等待操作 > 执行中 > 刚刚完成 > 空闲 > 无任务
```

## 支持的键盘

当前只支持下面这一款经过 USB 实测的设备：

| 项目 | 要求 |
| --- | --- |
| 型号 | 新版 FEKER Alice80，QMK/VIA 固件版本 |
| USB 产品名 | `Alice80` |
| USB VID:PID | `36B0:305F` |
| Raw HID | usage page `FF60`，usage `0061` |
| macOS 实测厂商字符串 | `RDMCTMZT` |

FEKER 官网提供 Alice80 手册和 Alice80 VIA JSON，但 Alice80 存在不同 PCB/固件版本。购买或安装前请确认是新版 QMK/VIA 版本；旧版 EVision `320F:5055` 不支持。Alice75、Alice98 或其他标有 QMK/VIA 的 FEKER 键盘也不会自动兼容，因为本项目目前只匹配上表中的 USB 身份。

参考资料：[FEKER 官方 Alice80 手册](https://fekertech.com/blogs/manual/feker-alice-80-manual)、[FEKER 官方 QMK/VIA 下载页](https://fekertech.com/blogs/qmk-via)、[Alice80 连接模式说明](https://www.manualslib.com/guide/3387017/feker-alice-80-ergonomic-gasket-keyboard-manual.html)。

### 是否必须有线连接？

是。任务灯功能目前要求有线 USB-C 连接。

Alice80 键盘本身可以通过 Type-C、2.4GHz 和 Bluetooth 打字，但本项目需要 QMK/VIA Raw HID 双向通信。QMK 官方文档把 `RAW_ENABLE` 列为 USB endpoint 功能，VIA 也通过 Raw HID 交换命令。因此：

| 连接方式 | 普通打字 | FEKER Codex Bridge 状态灯 |
| --- | --- | --- |
| USB-C 有线 | 支持 | 支持，唯一保证方式 |
| 2.4GHz 接收器 | 支持 | 未支持/未验证；尚未确认接收器会暴露相同 Raw HID 接口 |
| Bluetooth | 支持 | 不支持 |

即使 USB-C 同时插着，如果键盘仍处于 Bluetooth 或 2.4GHz 模式，线缆可能只负责充电。请把实体开关拨到 `OFF`，连接 USB-C，然后按 `Fn + N` 进入有线 USB 模式。该步骤来自 Alice80 手册。

协议依据：[QMK USB endpoint limitations](https://docs.qmk.fm/config_options#usb-endpoint-limitations)、[VIA configuring QMK](https://www.caniusevia.com/docs/configuring_qmk/)。

## 配色方案

在菜单栏图标中打开“配色方案”，可选择四套整板配色。选择会保存在 `~/Library/Application Support/Feker Codex Bridge/color-scheme.txt`。

| 方案 | 执行中 | 完成 | 空闲 | 等待 | 出错 |
| --- | --- | --- | --- | --- | --- |
| Codex 默认 | `#304FFE` | `#00FF4C` | `#FFFFFF` | `#FF6D00` | `#FF0033` |
| 海洋 Ocean | `#00B8FF` | `#00E5A8` | `#BDEBFF` | `#FFB000` | `#FF416C` |
| 紫罗兰 Violet | `#8B5CF6` | `#2DD4BF` | `#F3E8FF` | `#F59E0B` | `#E11D48` |
| 日落 Sunset | `#FF8A00` | `#84CC16` | `#FFF3D6` | `#FFD000` | `#FF1744` |

所有方案的“执行中”颜色都会使用约两秒一轮的呼吸效果；其他状态常亮。

## Codex 设置

本项目面向 macOS ChatGPT 桌面应用中的 Codex：

1. 从 [ChatGPT 下载页](https://chatgpt.com/download/)安装 macOS 桌面应用并登录。
2. 新建任务时，在 ChatGPT 下拉菜单中选择 **Codex**。
3. 选择一个本地文件夹或项目，让 Codex 至少运行一次任务。
4. 确认 `~/.codex/state_5.sqlite` 已生成。
5. 启动 FEKER Codex Bridge。应用会自动只读发现任务和 rollout 事件。

OpenAI 官方快速开始说明了桌面应用登录、选择文件夹/项目和切换到 Codex 的流程：[OpenAI ChatGPT/Codex quickstart](https://learn.chatgpt.com/docs/quickstart)。

不需要做以下设置：

- 不需要修改 `~/.codex/hooks.json`
- 不需要配置 `Command + 1…9` 或 `Option + 数字`
- 不需要给 FEKER Codex Bridge 开启“输入监控”
- 不需要安装 Karabiner
- 不需要 API key

本项目依赖的是 Codex 桌面应用当前的本地数据库与 rollout 文件；这些不是稳定的公开 API，Codex 更新后可能需要同步适配。

## 安装

依赖：

```zsh
xcode-select --install
brew install hidapi sqlite3 pkg-config
```

构建并安装：

```zsh
git clone https://github.com/chenzixin1/feker-codex-bridge.git
cd feker-codex-bridge
./install-service.command
```

也可以在 Finder 中双击 `install-service.command`。安装器会构建 App、复制到 `/Applications`、进行本地 ad-hoc 签名并启动菜单栏应用。新版不安装 privileged helper 或 `sudoers` 规则。

## 使用

1. 把 Alice80 实体开关拨到 `OFF`。
2. 连接 USB-C，按 `Fn + N` 进入有线模式。
3. 启动 Codex 并运行一个任务。
4. 点击菜单栏 Logo，确认显示“FEKER QMK/VIA · 整板状态灯”。
5. 选择配色，或在“测试灯光”中预览状态。

菜单提供：任务灯开关、四套配色、状态测试、日志、开机启动、GitHub 和退出。暂停或退出时，应用会恢复键盘原有灯效。

## 命令行

```zsh
app='/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge'

"$app" --task-lights on
"$app" --task-lights off
"$app" --scheme codex
"$app" --scheme ocean
"$app" --scheme violet
"$app" --scheme sunset
"$app" --request-test working
"$app" --request-test complete
```

测试持续 30 秒。其他测试状态为 `idle`、`waiting`、`error` 和 `off`。

## 工作原理与隐私

1. 只读打开 `~/.codex/state_5.sqlite`，发现未归档任务。
2. 只读监视对应 rollout JSONL 事件，识别执行、完成、等待和错误。
3. 通过 QMK/VIA 32 字节 Raw HID 设置整板 HSV，不写键盘 EEPROM。
4. 菜单进程管理一个灯控子进程；没有快捷键 observer。

程序不发起网络请求，也不会上传 Codex 数据、任务标题或内容。

## 日志与排错

```zsh
tail -f ~/Library/Logs/FekerCodexBridge.log
```

正常启动应出现：

```text
[READY] Watching Codex task status for whole-board lighting.
[DEVICE] FEKER QMK/VIA keyboard detected
[READY] Menu bar UI is accepting mouse input.
```

如果没有灯光变化：

- 确认是 `36B0:305F` 的新版 Alice80。
- 确认实体开关为 `OFF`，并按过 `Fn + N`。
- 关闭 VIA、其他 RGB 软件或可能独占 Raw HID 的键盘工具。
- 在菜单中先运行一个颜色测试。
- 确认 Codex 已运行过任务，且 `~/.codex/state_5.sqlite` 存在。

卸载：

```zsh
./uninstall-service.command
```

---

# English

FEKER Codex Bridge is a macOS menu bar app. It reads local Codex task events and displays the highest-priority state across the entire keyboard. It does not map number keys, listen for shortcuts, require Karabiner, install a Codex hook, or request Input Monitoring permission.

## Status behavior

| State | Default Codex colors |
| --- | --- |
| Working | Breathing blue, peak `#304FFE` |
| Just completed | Green `#00FF4C` for 10 seconds |
| Idle | White `#FFFFFF` |
| Waiting for input or approval | Amber `#FF6D00` |
| Error | Red `#FF0033` |
| No task | Off |

When several tasks exist, the board shows the highest-priority state:

```text
Error > Waiting > Working > Just completed > Idle > No task
```

## Supported keyboard

Only this USB-tested device is currently supported:

| Property | Requirement |
| --- | --- |
| Model | New FEKER Alice80 revision with QMK/VIA firmware |
| USB product name | `Alice80` |
| USB VID:PID | `36B0:305F` |
| Raw HID | usage page `FF60`, usage `0061` |
| Manufacturer string observed on macOS | `RDMCTMZT` |

FEKER publishes an Alice80 manual and an Alice80 VIA JSON, but multiple Alice80 PCB/firmware revisions exist. Confirm that you have the new QMK/VIA revision. The old EVision `320F:5055` revision is not supported. Alice75, Alice98, and other FEKER QMK/VIA boards are not automatically compatible because this app currently matches only the USB identity above.

Sources: [official FEKER Alice80 manual](https://fekertech.com/blogs/manual/feker-alice-80-manual), [official FEKER QMK/VIA downloads](https://fekertech.com/blogs/qmk-via), and the [readable Alice80 connection guide](https://www.manualslib.com/guide/3387017/feker-alice-80-ergonomic-gasket-keyboard-manual.html).

### Is a wired connection required?

Yes. The task-light feature currently requires a wired USB-C connection.

The Alice80 itself supports typing over Type-C, 2.4GHz, and Bluetooth. This bridge, however, needs bidirectional QMK/VIA Raw HID communication. QMK documents `RAW_ENABLE` as a feature that consumes a USB endpoint, and VIA exchanges commands over Raw HID.

| Connection | Normal typing | FEKER Codex Bridge lights |
| --- | --- | --- |
| Wired USB-C | Supported | Supported and required |
| 2.4GHz receiver | Supported | Unsupported/unverified; the receiver has not been confirmed to expose the same Raw HID interface |
| Bluetooth | Supported | Unsupported |

A connected USB-C cable may only charge the keyboard while it remains in a wireless mode. Move the hardware switch to `OFF`, connect USB-C, and press `Fn + N` to select wired USB mode, as described by the Alice80 manual.

Protocol references: [QMK USB endpoint limitations](https://docs.qmk.fm/config_options#usb-endpoint-limitations) and [VIA configuring QMK](https://www.caniusevia.com/docs/configuring_qmk/).

## Color schemes

Open **Color scheme / 配色方案** from the menu bar icon. The selection is saved to `~/Library/Application Support/Feker Codex Bridge/color-scheme.txt`.

| Scheme | Working | Complete | Idle | Waiting | Error |
| --- | --- | --- | --- | --- | --- |
| Codex Default | `#304FFE` | `#00FF4C` | `#FFFFFF` | `#FF6D00` | `#FF0033` |
| Ocean | `#00B8FF` | `#00E5A8` | `#BDEBFF` | `#FFB000` | `#FF416C` |
| Violet | `#8B5CF6` | `#2DD4BF` | `#F3E8FF` | `#F59E0B` | `#E11D48` |
| Sunset | `#FF8A00` | `#84CC16` | `#FFF3D6` | `#FFD000` | `#FF1744` |

The working color breathes on a roughly two-second cycle in every scheme. Other states are solid.

## Codex setup

This project targets Codex inside the macOS ChatGPT desktop app:

1. Install the macOS app from the [ChatGPT download page](https://chatgpt.com/download/) and sign in.
2. When creating a task, choose **Codex** from the ChatGPT dropdown.
3. Select a local folder or project and run at least one Codex task.
4. Confirm that `~/.codex/state_5.sqlite` exists.
5. Start FEKER Codex Bridge. It discovers tasks and rollout events automatically in read-only mode.

OpenAI's quickstart covers desktop sign-in, choosing a folder or project, and selecting Codex: [OpenAI ChatGPT/Codex quickstart](https://learn.chatgpt.com/docs/quickstart).

You do not need to configure `~/.codex/hooks.json`, number-key shortcuts, Input Monitoring, Karabiner, or an API key.

The bridge relies on the desktop app's current local database and rollout format. These are not stable public APIs and may require updates after a Codex release.

## Install

Dependencies:

```zsh
xcode-select --install
brew install hidapi sqlite3 pkg-config
```

Build and install:

```zsh
git clone https://github.com/chenzixin1/feker-codex-bridge.git
cd feker-codex-bridge
./install-service.command
```

You can also double-click `install-service.command` in Finder. It builds the app, copies it to `/Applications`, applies a local ad-hoc signature, and starts the menu bar app. The current version does not install a privileged helper or a `sudoers` rule.

## Use

1. Move the Alice80 hardware switch to `OFF`.
2. Connect USB-C and press `Fn + N` for wired mode.
3. Start a Codex task.
4. Click the menu bar logo and confirm **FEKER QMK/VIA · whole-board task light**.
5. Pick a scheme or preview states under **Test lights / 测试灯光**.

The menu includes the light toggle, four schemes, state tests, logs, launch at login, GitHub, and Quit. Pausing or quitting restores the keyboard's previous RGB effect.

## Command line

```zsh
app='/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge'

"$app" --task-lights on
"$app" --task-lights off
"$app" --scheme codex
"$app" --scheme ocean
"$app" --scheme violet
"$app" --scheme sunset
"$app" --request-test working
"$app" --request-test complete
```

Tests last 30 seconds. Other states are `idle`, `waiting`, `error`, and `off`.

## How it works and privacy

1. Opens `~/.codex/state_5.sqlite` read-only to discover unarchived tasks.
2. Watches the corresponding rollout JSONL files read-only for working, complete, waiting, and error events.
3. Sends whole-board HSV through 32-byte QMK/VIA Raw HID reports without writing keyboard EEPROM.
4. Runs one menu process and one lighting child process; there is no shortcut observer.

The app makes no network requests and does not upload Codex data, task titles, or task content.

## Logs and troubleshooting

```zsh
tail -f ~/Library/Logs/FekerCodexBridge.log
```

Expected startup lines:

```text
[READY] Watching Codex task status for whole-board lighting.
[DEVICE] FEKER QMK/VIA keyboard detected
[READY] Menu bar UI is accepting mouse input.
```

If the lights do not change:

- Confirm the new `36B0:305F` Alice80 revision.
- Move the hardware switch to `OFF` and press `Fn + N`.
- Quit VIA or other RGB/keyboard tools that may hold Raw HID exclusively.
- Run a color test from the menu.
- Confirm Codex has run at least one task and `~/.codex/state_5.sqlite` exists.

Uninstall:

```zsh
./uninstall-service.command
```

## License and affiliation

This experimental project is not affiliated with or endorsed by OpenAI, FEKER, Work Louder, QMK, or VIA.
