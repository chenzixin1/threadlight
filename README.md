# FEKER Alice80 × Codex task lights

把 FEKER Alice80 变成 Codex 任务状态灯。项目同时识别老款 EVision 和新款 QMK/VIA 两代键盘：

| 模式 | 效果 |
| --- | --- |
| 数字键任务模式 | 老款：数字行 `1`–`9` 分别显示侧栏中第 1～9 个 Codex 任务；新款自动使用整板聚合色 |
| 全键盘状态灯模式 | 两代键盘都把整板显示为一个聚合状态，像交通灯 |

状态颜色采用 Codex Micro 的逻辑：

| 灯光 | 状态 |
| --- | --- |
| 蓝色 `#304FFE` | 任务正在执行 |
| 绿色 `#00FF4C` | 任务完成、尚未查看 |
| 白色 `#FFFFFF` | 任务空闲或已经查看 |
| 橙色 `#FF6D00` | 任务正在等待输入或批准 |
| 红色 `#FF0033` | 任务出错 |
| 熄灭 | 没有对应任务 |

全键盘模式按以下优先级选择颜色：

```text
错误 > 等待操作 > 正在执行 > 完成未读 > 空闲 > 无任务
```

Codex 在 macOS 上原生使用 `Command + 1` … `Command + 9` 打开侧栏中对应位置的任务。项目只旁听这组快捷键来标记“已查看”，不拦截或重新发送按键，不使用 Karabiner，也不修改 Codex 快捷键。

> 实验性项目：Codex 的本地数据库和 rollout 日志格式不是稳定的公共 API。两代 RGB 协议都已通过 USB 实测。

## 兼容范围

- macOS
- OpenAI Codex 桌面 App
- FEKER Alice80 老款 EVision 或新款 QMK/VIA
- 有线模式
- 老款：USB `320F:5055`，HID `FF1C:0092`
- 新款：USB `36B0:305F`，QMK Raw HID `FF60:0061`，VIA 协议 `0x000C`

新款原厂固件支持 VIA RGB Matrix 的整板效果、颜色和亮度控制，但会以 `0xFF` 拒绝 VIA 标准逐键绘色通道。因此新款只提供整板状态色；项目不会刷新或修改键盘固件。

## 菜单栏 UI

安装后，macOS 菜单栏会出现“键帽 + 状态灯”图标和“任务灯”文字标签。图标是原生 Template Image，会自动适配浅色、深色和菜单选中状态；文字标签可避免它淹没在其他菜单栏图标中。菜单采用类似 Caffeine 的简单交互：

- 第一项直接开启或暂停任务灯；暂停时恢复键盘原有灯效
- 当前键盘一行说明正在使用新款整板模式还是旧款逐键模式
- “显示方式”子菜单切换“数字键任务灯（1–9 对应任务）”和“整板状态灯（显示最高优先级）”
- “测试灯光”子菜单提供蓝、绿、橙、红、白和熄灭测试
- “使用说明”子菜单解释鼠标操作、Codex 快捷键和每种颜色的含义
- “设置”里可以直接开关登录时自动启动，也可以打开输入监控与系统登录项设置
- 可跳转到 GitHub 项目主页、查看日志或退出应用

左键或右键菜单栏图标都会打开完整菜单；第一项直接开启或暂停任务灯。这条交互使用 AppKit 原生菜单绑定，由 macOS 负责弹出位置和菜单栏选中状态。

模式保存在：

```text
~/Library/Application Support/Feker Codex Bridge/lighting-mode.txt
```

任务灯开关保存在同一目录的 `task-lights-enabled.txt`。登录和重启后会自动恢复这两个设置。

## 工作原理

1. 后台桥接程序只读打开 `~/.codex/state_5.sqlite`，按最近可见任务顺序取得最多九个任务。
2. 它只读监视对应 rollout JSONL 的任务事件，识别执行、完成、等待和错误。
3. 菜单栏进程只在 Codex 位于前台时，以 listen-only event tap 旁听原生 `Command + 1…9`。
4. 老款由受限 helper 发送 EVision V2 `0x12` 动态颜色帧并每 100ms 保活；新款由普通用户进程通过 QMK/VIA 32 字节 Raw HID 设置整板 HSV，不写 EEPROM。
5. App 作为标准 macOS 登录项启动，同时管理菜单栏、快捷键旁听和灯控进程；安装后不需要重复输入管理员密码。

程序没有网络请求，不会上传 Codex 数据。

### 为什么不需要 Codex Hook

PromLight 通过 Codex lifecycle hooks 把事件转给自己的服务。本项目直接只读监听本机 Codex rollout 事件，因此不需要修改 `~/.codex/hooks.json`，也不会占用或覆盖其他 Hook。

## 安装依赖

```zsh
xcode-select --install
brew install hidapi sqlite3 pkg-config
```

## 安装

```zsh
git clone https://github.com/chenzixin1/feker-codex-bridge.git
cd feker-codex-bridge
./install-service.command
```

也可以在 Finder 中双击 `install-service.command`。

安装器会：

- 编译 `feker-rgb` 和 `Feker Codex Bridge.app`
- 安装 App 到 `/Applications`
- 安装受限 helper 到 `/Library/PrivilegedHelperTools`
- 写入一条仅允许启动该 helper 的精确 `sudoers` 规则
- 清理本项目旧版本遗留的 LaunchDaemon/LaunchAgent
- 启动菜单栏 App

安装时输入一次管理员密码。随后不论开机、切换模式还是测试灯光，都不需要再次输入密码。

为了在登录后自动启动，请在下面的位置点 `+`，选择 `/Applications/Feker Codex Bridge.app`：

```text
系统设置 → 通用 → 登录项与扩展 → 登录时打开
```

本项目使用标准登录项，不依赖 LaunchAgent；这是 macOS 26 上更稳定、也更容易看见和关闭的方式。

### 建议开启“输入监控”

本地构建使用 ad-hoc 签名。每次重新编译后，macOS 会把它视为新的二进制，因此安装器会清理旧授权。安装结束后请到：

```text
系统设置 → 隐私与安全性 → 输入监控
```

添加并允许：

```text
/Applications/Feker Codex Bridge.app
```

这个权限只用于让 App 旁听 Codex 的 `Command + 1…9`，从而在切换任务后把绿色或红色的“未查看”状态改成白色。RGB 控制本身不依赖输入监控：老款由受限 helper 打开 HID，新款由普通用户进程打开 QMK Raw HID。

如果暂时不开这个权限，任务状态灯仍会工作，只是快捷键切换后不会自动标记为已查看。

如果列表里已有同名 App 但灯仍不工作，请先移除旧行，再从 `/Applications` 重新添加。不要添加仓库中的构建副本。

## 使用

1. 把 Alice80 实体开关拨到 `OFF`。
2. 接上 USB-C，按一次 `Fn + N` 进入有线模式。
3. 在菜单栏 `⌨︎` 中选择显示模式。
4. 在 Codex 前台使用 `Command + 1…9` 切换任务。

`Option + 数字` 不是当前 macOS 版 Codex 的任务切换快捷键。

不需要先按 `Fn + Del` 切换板载灯效。USB 动态直控与板载效果是两套状态。如果背光完全关闭，可先用 `Fn + L` 打开并调高亮度；必要时长按 `Fn + Backspace` 三秒重置板载灯光设置。

## 灯光测试

安装后可直接使用菜单栏中的测试项。也可以运行：

```zsh
./test-installed.command
```

或者：

```zsh
'/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' \
  --request-test-key 1 unread
```

测试持续 30 秒：

- 老款数字键任务模式：数字 `1` 显示绿色
- 老款全键盘模式和所有新款模式：整把键盘显示绿色

### 安装前的底层测试

```zsh
./build.sh
./feker-rgb list
sudo ./feker-rgb all FF0000
sudo ./feker-rgb key 1 00FF4C
sudo ./feker-rgb off
```

底层颜色命令保持直控模式 60 秒，然后恢复板载灯效。也可以双击 `test-root-rgb.command`。

## 命令参考

### 模式切换

```zsh
FekerCodexBridge --mode per-key
FekerCodexBridge --mode whole-board
```

任务灯总开关也可以从命令行控制：

```zsh
FekerCodexBridge --task-lights on
FekerCodexBridge --task-lights off
```

对已安装版本使用完整路径：

```zsh
'/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' \
  --mode whole-board
```

### 测试请求

```zsh
FekerCodexBridge --request-test-key 1-9 \
  [working|unread|idle|waiting|error|off]
```

### `feker-rgb`

```text
feker-rgb list
feker-rgb probe
feker-rgb qmk-probe
feker-rgb qmk-all RRGGBB
feker-rgb scan
feker-rgb off
feker-rgb all RRGGBB
feker-rgb key 1-9 RRGGBB
feker-rgb led 0-89 RRGGBB
feker-rgb slots [1=RRGGBB ... 9=RRGGBB]
```

## 老款数字键映射

FEKER 的 `320F:5055` 是多个 EVision 设备共用的控制器编号，网上没有可靠的 FEKER Alice80 专属逐键表。本项目采用 OpenRGB EVision V2 驱动中的标准列优先矩阵：

```text
数字键:  1   2   3   4   5   6   7   8   9
RGB槽:   7  13  19  25  31  37  43  49  55
```

每列占六个槽；槽 `6` 是空槽，OpenRGB 同样使用其字节偏移 `18` 发送动态模式保活。

Alice80 的键帽不透光、透明定位板又会扩散光线，所以单颗 LED 在照片中可能看起来像多个键缝发光。需要明确辨识状态时，推荐使用“全键盘状态灯模式”。新款 QMK/VIA 原厂固件未开放标准逐键绘色接口，所以始终使用聚合色。

## 日志

```zsh
tail -f ~/Library/Logs/FekerCodexBridge.log
```

正常启动会出现：

```text
[READY] Watching Codex tasks
[READY] Passively observing native Codex Command+1...9 shortcuts
```

## 卸载

```zsh
./uninstall-service.command
```

卸载器会删除 App、helper、`sudoers` 规则以及旧版本遗留的 LaunchAgent/LaunchDaemon。灯光模式选择和日志会保留；如果系统设置仍显示一个失效的登录项，可手动将它移除。

## 故障排查

### 灯完全没有变化

- 确认实体开关为 `OFF`、USB-C 已连接，并按过 `Fn + N`。
- 在菜单栏执行一种颜色测试。
- 确认“输入监控”中的条目来自 `/Applications`。
- 查看日志中是否有 `[PERMISSION]`。

### `Command + 数字` 能切任务，但完成灯没有变白

- 确保 Codex 是前台应用。
- 确保“输入监控”已允许 `Feker Codex Bridge`。
- 查看日志是否包含 `Passively observing`。

### `Unable to open the Codex task database`

- 先启动 Codex 桌面 App 并至少创建一个任务。
- 确认 `~/.codex/state_5.sqlite` 存在。

### Codex 更新后不再识别状态

Codex 的本地存储结构可能变化。提交 issue 时可附上：

- Codex App 版本
- macOS 版本
- 脱敏后的事件类型
- `feker-rgb list` 输出的 usage page、usage 和 interface

不要上传 `.codex` 数据库、完整 rollout、任务标题或任务内容。

## 协议参考

- [OpenAI × Work Louder Codex Micro](https://openai.com/supply/co-lab/work-louder/)
- [Work Louder Codex Micro setup](https://worklouder.cc/openai-micro-setup)
- [QMK Raw HID](https://docs.qmk.fm/features/rawhid)
- [VIA app keyboard protocol implementation](https://github.com/the-via/app/blob/main/src/utils/keyboard-api.ts)
- [OpenRGB EVision V2 controller](https://gitlab.com/CalcProgrammer1/OpenRGB/-/tree/master/Controllers/EVisionKeyboardController/EVisionV2KeyboardController)
- [OpenRGB EVision V2 raw LED map](https://gitlab.com/CalcProgrammer1/OpenRGB/-/blob/master/Controllers/EVisionKeyboardController/EVisionV2KeyboardController/EVisionV2KeyboardController.cpp)

本项目与 OpenAI、FEKER、Work Louder、Karabiner-Elements 和 OpenRGB 均无隶属或背书关系。

## 当前限制

- 同时最多显示九个最近可见任务。
- 最近任务的数据库顺序通常与侧栏顺序一致；特殊筛选或分组可能暂时错位。
- 鼠标点击侧栏暂时不会清除绿色/红色未读；原生 `Command + 1…9` 会。
- 新款 `36B0:305F` 只能使用整板状态色；逐键模式仅适用于老款 `320F:5055`。
- Codex 本地存储结构随版本变化时可能需要更新解析器。
- helper 是透明的本地开发者工具结构，不是经过 Apple 公证和发行签名的商业安装包；安装前应查看源码。
