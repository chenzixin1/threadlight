# Threadlight

<p align="center">
  <strong>让任务状态，亮在键盘上。</strong><br>
  See your Codex task status in the light of your keyboard.
</p>

<p align="center">
  <img src="assets/threadlight-breathing.gif" alt="Threadlight — Codex task status as whole-board keyboard light" width="100%">
</p>

<p align="center">
  <a href="#中文">中文</a> · <a href="#english">English</a> ·
  <a href="assets/threadlight-breathing.mp4">MP4 Demo</a> ·
  <a href="assets/threadlight-hero-4k.jpg">4K Hero</a>
</p>

> Threadlight is an experimental, independent macOS utility. It is not affiliated with or endorsed by OpenAI, FEKER, QMK, or VIA. The keyboard in the hero is an illustrative product render; compatibility is defined by the USB table below.

---

# 中文

Threadlight 是一个轻量的 macOS 菜单栏应用。它只读观察本机 Codex 任务事件，并把最需要你注意的状态显示成整块键盘的颜色和节奏。

- **不打断工作：** 不弹通知也能从键盘灯光看见任务进度。
- **不接管按键：** 没有数字键映射、全局快捷键或 Karabiner。
- **不需要 Hook：** 无须修改 Codex 配置，也不需要 API Key 或“输入监控”权限。
- **用完即恢复：** 空闲、暂停或退出时恢复键盘原有 RGB 灯效。

## 界面

<table>
  <tr>
    <td width="62%"><img src="assets/screenshots/threadlight-settings-zh.png" alt="Threadlight 中文灯光设置"></td>
    <td width="38%"><img src="assets/screenshots/threadlight-menu-zh.png" alt="Threadlight 中文菜单栏菜单"></td>
  </tr>
  <tr>
    <td align="center">配色、状态动画与亮度</td>
    <td align="center">菜单栏里的全部操作</td>
  </tr>
</table>

## 状态如何变成灯光

| Codex 状态 | Threadlight 灯效 |
| --- | --- |
| 执行中 | 蓝色平滑呼吸 |
| 刚刚完成 | 绿色呼吸两次，然后常亮至下一项任务开始 |
| 等待输入或批准 | 橙色慢速呼吸 |
| 任务失败 | 红色双闪后常亮；用户主动取消不算失败 |
| 空闲、暂停或退出 | 恢复键盘原有灯效 |

多个任务同时存在时，整块键盘显示优先级最高的状态：

```text
任务失败 > 等待操作 > 已完成提示 > 执行中 > 空闲
```

## 三套配色

| 方案 | 执行中 | 已完成 | 等待 | 失败 |
| --- | --- | --- | --- | --- |
| Codex | `#304FFE` | `#00FF4C` | `#FF6D00` | `#FF0033` |
| Ocean | `#00B8FF` | `#00E5A8` | `#FFB000` | `#FF416C` |
| Violet | `#8B5CF6` | `#2DD4BF` | `#F59E0B` | `#E11D48` |

设置窗口会实时预览各状态的动画。界面使用 Core Animation 以 60 FPS 绘制，键盘通过 Raw HID 以 30 FPS 更新。亮度可在 20%–100% 之间调节。

## 兼容性

目前只支持下面这一款经过 USB 实测的键盘：

| 项目 | 要求 |
| --- | --- |
| 型号 | 新版 FEKER Alice80，QMK/VIA 固件版本 |
| USB 产品名 | `Alice80` |
| USB VID:PID | `36B0:305F` |
| Raw HID | usage page `FF60`，usage `0061` |
| macOS 实测厂商字符串 | `RDMCTMZT` |

Alice80 存在不同 PCB 与固件版本。旧版 EVision `320F:5055` 不支持；Alice75、Alice98 以及其他 QMK/VIA 键盘也不会自动兼容。请以 USB 身份为准，而不是只看商品名称。

参考：[FEKER Alice80 手册](https://fekertech.com/blogs/manual/feker-alice-80-manual)、[FEKER QMK/VIA 下载](https://fekertech.com/blogs/qmk-via)、[QMK USB endpoint 说明](https://docs.qmk.fm/config_options#usb-endpoint-limitations)、[VIA Raw HID 配置](https://www.caniusevia.com/docs/configuring_qmk/)。

### 必须使用有线连接

| 连接方式 | 普通打字 | Threadlight 状态灯 |
| --- | --- | --- |
| USB-C 有线 | 支持 | **支持，也是唯一保证方式** |
| 2.4GHz 接收器 | 支持 | 未支持、未验证 |
| Bluetooth | 支持 | 不支持 |

即使 USB-C 已经插入，键盘处于无线模式时线缆也可能只负责充电。请把 Alice80 实体开关拨到 `OFF`，连接 USB-C，再按 `Fn + N` 进入有线模式。

## Codex 设置

Codex 现在作为专门的开发体验集成在 macOS ChatGPT 桌面应用中：

1. 安装并登录 [ChatGPT 桌面应用](https://chatgpt.com/download/)。
2. 在产品选择器中选择 **Codex**。
3. 打开本地文件夹或项目，并至少运行一次任务。
4. 启动 Threadlight；它会自动只读发现本地任务和 rollout 事件。

官方说明：[ChatGPT 桌面应用快速开始](https://learn.chatgpt.com/docs/quickstart) 与 [Codex 最佳实践](https://learn.chatgpt.com/docs/codex/best-practices)。

Threadlight 不需要以下设置：

- 不需要 `~/.codex/hooks.json`
- 不需要 `Command + 1…9`、`Option + 数字`或其他任务切换快捷键
- 不需要 Karabiner
- 不需要“输入监控”权限
- 不需要 OpenAI API Key

Threadlight 读取的是当前桌面应用的本地数据库和 rollout 格式，而不是稳定的公开 API。Codex 更新后，本项目可能需要同步适配。

## 安装

安装构建依赖：

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

也可以在 Finder 中双击 `install-service.command`。安装器会：

1. 构建 `Threadlight.app`；
2. 复制到 `/Applications` 并进行本地 ad-hoc 签名；
3. 从旧版 FEKER Codex Bridge 迁移配色和亮度；
4. 清理旧应用包，避免菜单栏出现两个版本；
5. 启动 Threadlight。

它不会安装 privileged helper 或 `sudoers` 规则。

## 使用

1. 把 Alice80 实体开关拨到 `OFF`。
2. 连接 USB-C，按 `Fn + N` 进入有线模式。
3. 启动一个 Codex 任务。
4. 点击菜单栏 Threadlight 图标。
5. 选择配色、调整亮度，或在“测试灯光”中单独预览每种状态。

菜单还提供暂停、日志、开机启动、GitHub 主页和退出。

## 工作原理与隐私

1. 只读打开 `~/.codex/state_5.sqlite`，发现未归档任务。
2. 只读监视对应 rollout JSONL，识别执行、完成、等待和错误事件。
3. 通过 QMK/VIA 32 字节 Raw HID 设置整板 HSV，不写键盘 EEPROM。
4. 菜单进程管理一个灯控子进程；没有快捷键观察器。

Threadlight 不发起网络请求，也不会上传 Codex 数据、任务标题或任务内容。

## 命令行

```zsh
app='/Applications/Threadlight.app/Contents/MacOS/Threadlight'

"$app" --task-lights on
"$app" --task-lights off
"$app" --scheme codex
"$app" --scheme ocean
"$app" --scheme violet
"$app" --brightness 68
"$app" --request-test working
"$app" --request-test complete
```

灯光测试持续 30 秒。其他测试状态为 `idle`、`waiting`、`error` 和 `off`。

## 日志与排错

```zsh
tail -f ~/Library/Logs/Threadlight.log
```

如果没有灯光变化：

- 确认 USB 身份是 `36B0:305F`。
- 确认实体开关为 `OFF`，并按过 `Fn + N`。
- 退出 VIA 或其他可能占用 Raw HID 的键盘软件。
- 先从菜单执行一次“测试灯光”。
- 确认 Codex 已经运行过至少一个本地任务。

卸载：

```zsh
./uninstall-service.command
```

---

# English

Threadlight is a lightweight macOS menu bar app. It watches local Codex task events read-only and turns the most important state into whole-board keyboard color and motion.

- **Glanceable:** see whether a task is working, complete, waiting, or failed without another notification.
- **No key takeover:** no number-key mapping, global shortcut, or Karabiner dependency.
- **No Codex hook:** no Codex configuration change, API key, or Input Monitoring permission.
- **Leaves no lighting residue:** idle, pause, and quit restore the keyboard's previous RGB effect.

## Interface

<p align="center">
  <img src="assets/screenshots/threadlight-settings-en.png" alt="Threadlight light settings in English" width="70%">
</p>

## Status behavior

| Codex state | Threadlight behavior |
| --- | --- |
| Working | Smooth blue breathing |
| Just completed | Two green breaths, then solid until the next task starts |
| Waiting for input or approval | Slow amber breathing |
| Failed | Two red flashes, then solid; user interruption is not a failure |
| Idle, paused, or quit | Restore the keyboard's previous RGB effect |

When several tasks exist, Threadlight shows the highest-priority state:

```text
Failed > Waiting > Completion indication > Working > Idle
```

## Color schemes

| Scheme | Working | Complete | Waiting | Failed |
| --- | --- | --- | --- | --- |
| Codex | `#304FFE` | `#00FF4C` | `#FF6D00` | `#FF0033` |
| Ocean | `#00B8FF` | `#00E5A8` | `#FFB000` | `#FF416C` |
| Violet | `#8B5CF6` | `#2DD4BF` | `#F59E0B` | `#E11D48` |

The settings window previews every state animation at 60 FPS through Core Animation. Raw HID updates the keyboard at 30 FPS. Brightness is adjustable from 20% to 100%.

## Compatibility

Only this USB-tested keyboard is currently supported:

| Property | Requirement |
| --- | --- |
| Model | New FEKER Alice80 revision with QMK/VIA firmware |
| USB product | `Alice80` |
| USB VID:PID | `36B0:305F` |
| Raw HID | usage page `FF60`, usage `0061` |
| Manufacturer observed on macOS | `RDMCTMZT` |

Multiple Alice80 PCB and firmware revisions exist. The old EVision `320F:5055` revision is unsupported. Alice75, Alice98, and other QMK/VIA boards are not automatically compatible; USB identity, not the product label alone, determines support.

References: [FEKER Alice80 manual](https://fekertech.com/blogs/manual/feker-alice-80-manual), [FEKER QMK/VIA downloads](https://fekertech.com/blogs/qmk-via), [QMK USB endpoint limitations](https://docs.qmk.fm/config_options#usb-endpoint-limitations), and [VIA Raw HID configuration](https://www.caniusevia.com/docs/configuring_qmk/).

### Wired USB-C is required

| Connection | Normal typing | Threadlight lights |
| --- | --- | --- |
| Wired USB-C | Supported | **Supported and required** |
| 2.4GHz receiver | Supported | Unsupported and unverified |
| Bluetooth | Supported | Unsupported |

A connected USB-C cable may only charge the keyboard while it remains in a wireless mode. Move the Alice80 hardware switch to `OFF`, connect USB-C, and press `Fn + N` for wired mode.

## Codex setup

Codex is now a dedicated coding experience inside the macOS ChatGPT desktop app:

1. Install and sign in to the [ChatGPT desktop app](https://chatgpt.com/download/).
2. Select **Codex** in the product selector.
3. Open a local folder or project and run at least one task.
4. Start Threadlight. It discovers local tasks and rollout events read-only.

Official references: [ChatGPT desktop quickstart](https://learn.chatgpt.com/docs/quickstart) and [Codex best practices](https://learn.chatgpt.com/docs/codex/best-practices).

Threadlight does not require `~/.codex/hooks.json`, task-switching shortcuts, Karabiner, Input Monitoring, or an OpenAI API key.

Threadlight relies on the desktop app's current local database and rollout format rather than a stable public API. A future Codex release may require a compatibility update.

## Install

Install build dependencies:

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

You can also double-click `install-service.command` in Finder. It builds and ad-hoc signs `/Applications/Threadlight.app`, migrates preferences from the former FEKER Codex Bridge name, removes the legacy app bundle, and starts Threadlight. It does not install a privileged helper or `sudoers` rule.

## Use

1. Move the Alice80 hardware switch to `OFF`.
2. Connect USB-C and press `Fn + N` for wired mode.
3. Start a Codex task.
4. Click the Threadlight menu bar icon.
5. Pick a scheme, adjust brightness, or preview a state under **Test Lights**.

The menu also provides pause, logs, launch at login, the GitHub project, and Quit.

## How it works and privacy

1. Opens `~/.codex/state_5.sqlite` read-only to discover unarchived tasks.
2. Watches the corresponding rollout JSONL files read-only for working, complete, waiting, and error events.
3. Sends whole-board HSV through 32-byte QMK/VIA Raw HID reports without writing keyboard EEPROM.
4. Runs one menu process and one lighting child process; there is no shortcut observer.

Threadlight makes no network requests and does not upload Codex data, task titles, or task content.

## Command line

```zsh
app='/Applications/Threadlight.app/Contents/MacOS/Threadlight'

"$app" --task-lights on
"$app" --task-lights off
"$app" --scheme codex
"$app" --scheme ocean
"$app" --scheme violet
"$app" --brightness 68
"$app" --request-test working
"$app" --request-test complete
```

Tests last 30 seconds. Other states are `idle`, `waiting`, `error`, and `off`.

## Logs and troubleshooting

```zsh
tail -f ~/Library/Logs/Threadlight.log
```

If the lights do not change:

- Confirm USB VID:PID `36B0:305F`.
- Move the hardware switch to `OFF` and press `Fn + N`.
- Quit VIA or another keyboard utility that may hold Raw HID exclusively.
- Run a light test from the menu.
- Confirm Codex has completed at least one local task.

Uninstall:

```zsh
./uninstall-service.command
```

## License and affiliation

Threadlight is experimental and independent. It is not affiliated with or endorsed by OpenAI, FEKER, QMK, or VIA.
