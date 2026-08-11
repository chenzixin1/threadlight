# Threadlight RGB9 v0.2.0

这是为 **FEKER Alice80 三模版 `36B0:3042`** 制作并完成实机验证的实验固件。它基于原厂 `V0103`，为 Threadlight 增加数字键 `1`–`9` 独立 RGB 和聚焦清屏能力。

> 不要根据键盘商品名称直接刷写。只有正常有线模式 USB VID:PID 为 `36B0:3042` 的 Alice80 才在已验证范围内。旧版 `36B0:305F`、EVision `320F:5055` 及其他 Alice 型号均不适用。

## 下载与校验

- 固件：[FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin](FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin)
- 文件大小：`74044` 字节
- SHA-256：`cc5b1697491ec1b3b99500a8df554e284f473ac80b81d2f3ceee7b7786f4e21a`
- 补丁清单：[manifest.json](manifest.json)

### 刷写前准备

1. 从 [FEKER 下载页](https://www.fekergaming.com/download/)保存对应的原厂 `V0103.bin`，作为恢复镜像。
2. 退出 Threadlight、VIA 和其他键盘灯控软件。
3. 在 macOS“系统信息 → USB”中确认正常设备的产品名是 `Alice80`、厂商 ID 是 `0x36B0`、产品 ID 是 `0x3042`。
4. 在终端校验下载文件：

```zsh
shasum -a 256 ~/Downloads/FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin
```

结果必须与上面的 SHA-256 完全一致。任何一项型号或校验不一致，都不要继续。

## 刷写

1. 把键盘切到中间的有线档，然后拔掉 USB 线。
2. 按住最左上角 `Esc`，插回 USB 后继续按约 3 秒，再松开 `Esc`。
3. 确认系统出现 `03EB:2045` / `RDMCTMZT DFU`，并挂载名为 `NO NAME` 的小容量磁盘。没有同时满足这两个特征就不要写入。
4. 在 Finder 中把下载的 `.bin` 复制到 `NO NAME` 根目录，并把复制进去的文件命名为 `FLASH.BIN`。也可以在终端执行：

```zsh
cp ~/Downloads/FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin "/Volumes/NO NAME/FLASH.BIN"
sync
```

5. 等待 `NO NAME` 自动消失，键盘会自行重启。复制完成到自动重启期间不要拔线，也不要再次复制。
6. 在“系统信息 → USB”中确认键盘重新显示为 `Alice80 / 36B0:3042`，先测试正常输入，再启动 Threadlight 并选择“灯光设置 → 数字键 1–9”。

若复制报错、恢复盘没有自动消失，或设备身份与文档不符，请先停止，不要向其他磁盘重复复制。

## 恢复原厂固件

1. 退出 Threadlight 和其他键盘工具。
2. 重复上面的 `Esc` 插线步骤进入 `03EB:2045` 恢复模式。
3. 把预先保存的原厂 `V0103.bin` 复制为 `/Volumes/NO NAME/FLASH.BIN`。
4. 等待自动重启，再确认 `36B0:3042` 和正常输入。

恢复模式和原厂镜像是刷写前必须准备好的退路。如果无法进入恢复模式，不要尝试其他型号的固件。

## 已验证结果

- 正常输入功能保持可用。
- Raw HID `0xB0` 能力响应为协议版本 `1`、9 个指示键、32 字节报告，并声明聚焦清屏能力。
- 数字键模式在同一灯光帧中熄灭其他 59 颗 LED，只保留 `1`–`9`。
- 切换回整板模式、暂停或退出 Threadlight 时，临时覆盖会被清除。

---

## English flashing guide

This experimental firmware is hardware-verified only for the tri-mode **FEKER Alice80 with normal wired USB VID:PID `36B0:3042`**. It is based on stock `V0103` and adds Threadlight's independent RGB overlay for number keys `1`–`9`, including focus blackout for the other 59 LEDs.

Before flashing, download and keep the matching stock `V0103.bin` from FEKER, quit Threadlight/VIA, verify the normal USB identity, and check the downloaded firmware:

```zsh
shasum -a 256 ~/Downloads/FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin
```

The result must exactly match the SHA-256 listed above.

1. Select the keyboard's middle wired position and unplug USB.
2. Hold the top-left `Esc`, reconnect USB, keep holding for about three seconds, and release it.
3. Confirm bootloader identity `03EB:2045` / `RDMCTMZT DFU` and a small mounted volume named `NO NAME`.
4. Copy the downloaded firmware to that volume as `FLASH.BIN`, using Finder or:

```zsh
cp ~/Downloads/FEKER-Alice80-36B0-3042-Threadlight-RGB9-v0.2.0.bin "/Volumes/NO NAME/FLASH.BIN"
sync
```

5. Do not unplug the keyboard while copying. Wait for `NO NAME` to disappear automatically and for the keyboard to re-enumerate as `Alice80 / 36B0:3042`.
6. Test normal typing, start Threadlight, and select **Light Settings → Keys 1–9**.

To restore stock firmware, enter the same bootloader and copy the saved stock `V0103.bin` to `NO NAME` as `FLASH.BIN`. Do not flash this image to `36B0:305F`, EVision `320F:5055`, or another Alice model.
