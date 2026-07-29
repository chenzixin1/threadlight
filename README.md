# FEKER Alice80 × Codex task lights

把 FEKER Alice80 数字行的 `1`–`9` 变成 Codex 任务状态灯：

| 灯光 | 状态 |
| --- | --- |
| 蓝色 | 任务正在执行 |
| 绿色 | 任务已经完成 |
| 橙色 | 任务正在等待输入或批准 |
| 红色 | 任务出错 |
| 熄灭 | 槽位空闲，或任务已经打开 |

按 Alice80 上的 `Option + 1` … `Option + 9`，可以打开与亮灯槽位对应的 Codex 任务，并清除该槽位。Codex 自带的侧栏位置切换是 `Command + 1` … `Command + 9`；本项目的 `Option` 组合键通过 Karabiner-Elements 实现，按任务 ID 打开，二者可以同时使用。

> **实验性项目。** Codex 的本地数据库、rollout 日志格式和 `codex://threads/…` 深层链接都不是稳定的公共 API。RGB 协议已能获得键盘回包，但不同 Alice80 固件的实际显示行为仍需要逐台验证。请先完成下面的 RGB 测试，再启用任务桥接。

## 兼容范围

- macOS
- OpenAI Codex 桌面 App
- 老款 FEKER Alice80，USB `VID:PID = 320F:5055`
- HID usage page `0xFF1C`、usage `0x0092`
- 有线模式

2024 年以后带 QMK/VIA 的新 Alice80 使用不同协议，本项目不适用。项目不会刷新键盘固件。

## 工作原理

1. `feker-rgb` 通过 HID 向 Alice80 发送 EVision V2 `0x12` 实时 RGB 数据。
2. `FekerCodexBridge` 只读打开 `~/.codex/state_5.sqlite`，发现未归档的 Codex 任务。
3. 它继续只读监视对应 rollout JSONL 中的任务事件，把状态分配到数字键 `1`–`9`。
4. `--open-slot N` 从本地状态文件取得任务 ID，打开 `codex://threads/<id>`，然后通知守护进程清除灯光。

程序不会上传 Codex 数据，也不包含网络请求。

## 依赖

安装 Xcode Command Line Tools、Homebrew，以及三个构建依赖：

```zsh
xcode-select --install
brew install hidapi sqlite3 pkg-config
```

Karabiner-Elements 只在需要 `Option + 1` … `Option + 9` 时安装：

```zsh
brew install --cask karabiner-elements
```

## 构建

```zsh
git clone https://github.com/chenzixin1/feker-codex-bridge.git
cd feker-codex-bridge
./build.sh
```

构建产物：

- `./feker-rgb`
- `./Feker Codex Bridge.app`

## 第一步：确认键盘和 RGB

把 Alice80 的实体开关拨到 `OFF`，接上 USB-C，并按一次 `Fn + N` 进入有线模式。

列出 HID 接口：

```zsh
./feker-rgb list
```

输出中应出现：

```text
usage_page=0xFF1C usage=0x0092
```

macOS 可能把这个复合 HID 接口当作受保护的键盘接口，因此灯控命令通常需要管理员权限：

```zsh
sudo ./feker-rgb all FF0000
sudo ./feker-rgb key 1 00FF00
sudo ./feker-rgb slots 1=00FF00 2=FFBF00 3=FF0000
sudo ./feker-rgb off
```

也可以双击 `test-root-rgb.command`，按提示完成三阶段测试。

如果命令有回包但完全不亮：

1. 按一次 `Fn + L` 打开背光。
2. 按几次 `Fn + Del` 切换灯光模式。
3. 调高键盘背光亮度。
4. 仍无效时，长按 `Fn + Backspace` 三秒重置灯光设置。

## 第二步：安装 App

构建成功后复制到 `/Applications`：

```zsh
sudo ditto './Feker Codex Bridge.app' '/Applications/Feker Codex Bridge.app'
```

手动测试数字键 `1` 的绿色状态灯：

```zsh
sudo '/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' \
  --test-key 1 green
```

关闭全部任务灯：

```zsh
sudo '/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' --off
```

## 第三步：运行任务监视器

目前最透明的运行方式是在一个终端窗口中以前台方式启动：

```zsh
sudo env HOME="$HOME" \
  '/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' \
  --daemon
```

`HOME="$HOME"` 很重要：守护进程需要读取当前用户的 `~/.codex`，并把槽位状态写到：

```text
~/Library/Application Support/Feker Codex Bridge/slots.tsv
```

停止时按 `Control + C`。程序会尝试熄灭任务灯。

> 输入监控权限本身可能不足以打开这个 Alice80 的受保护 HID 接口；因此当前版本使用 `sudo`。项目暂未提供需要长期 root 权限的自动安装器。

## 第四步：配置 Option 快捷键

仓库中的 [`karabiner-rule.json`](./karabiner-rule.json) 是 Alice80 专用规则，只匹配 `VID 12815 / PID 20565`，不会改变 MacBook 内置键盘或其他键盘。

1. 打开 Karabiner-Elements。
2. 选择 **Complex Modifications**。
3. 把 `karabiner-rule.json` 中的 `rules[0]` 合并到当前 profile 的 `complex_modifications.rules`。
4. 重新启动 Karabiner-Elements。

也可以不安装 Karabiner，直接测试某个槽位：

```zsh
'/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge' \
  --open-slot 1
```

槽位存在时，该命令会打开对应 Codex 任务；槽位为空时返回非零状态。

## 命令参考

### `feker-rgb`

```text
feker-rgb list
feker-rgb off
feker-rgb all RRGGBB
feker-rgb key 1-9 RRGGBB
feker-rgb led 0-127 RRGGBB
feker-rgb slots [1=RRGGBB ... 9=RRGGBB]
```

每条颜色命令都会发送完整的 128 槽颜色帧；未指定的槽位会熄灭。

### `FekerCodexBridge`

```text
FekerCodexBridge --daemon
FekerCodexBridge --open-slot 1-9
FekerCodexBridge --test-key 1-9 [blue|green|amber|red|off]
FekerCodexBridge --off
```

## 故障排查

### `Unable to open FEKER RGB interface`

- 确认使用有线模式：实体开关 `OFF`、USB-C 已连接、按 `Fn + N`。
- 尝试在命令前加 `sudo`。
- 拔插键盘后重新运行 `./feker-rgb list`。

### `Unable to open the Codex task database`

- 先启动 Codex 桌面 App 并至少创建一个任务。
- 确认 `~/.codex/state_5.sqlite` 存在。
- 使用 `sudo` 时不要遗漏 `env HOME="$HOME"`。

### 任务能打开，但灯没有清除

确认以下目录由当前用户拥有并可写：

```zsh
ls -ld "$HOME/Library/Application Support/Feker Codex Bridge"
```

### Codex 更新后不再识别任务状态

Codex 的数据库和日志属于内部实现，升级后可能改变。请在 issue 中附上：

- Codex App 版本
- macOS 版本
- 脱敏后的日志事件类型
- `feker-rgb list` 的 usage page、usage 和 interface

不要上传 `.codex` 数据库、完整 rollout 文件、任务标题或任务内容。

## 协议与参考

- [OpenAI × Work Louder Codex Micro](https://openai.com/supply/co-lab/work-louder/)
- [Work Louder Codex Micro setup](https://worklouder.cc/openai-micro-setup)
- [OpenRGB EVision V2 controller](https://gitlab.com/CalcProgrammer1/OpenRGB/-/tree/master/Controllers/EVisionKeyboardController/EVisionV2KeyboardController)
- [FEKER Alice80 manual](https://manuals.plus/feker/alice-80-keyboard-manual.pdf)

本项目与 OpenAI、FEKER、Work Louder、Karabiner-Elements 和 OpenRGB 均无隶属或背书关系。

## 当前限制

- 仅实现数字键 `1`–`9` 的固定 LED 映射。
- 同时最多显示九个活跃槽位。
- Codex 深层链接和本地存储结构随版本变化时可能失效。
- 尚未实现安全的特权 helper；后台自动运行仍需进一步设计。
- 不同 Alice80 固件的 LED 数量和实时命令兼容性可能不同。

欢迎提交 issue，但请先确认基础 RGB 测试能够改变键盘灯光。
