# PrintPet Core v1.5.1 升级说明

下载页：[PrintPet Core v1.5.1](https://github.com/ericwubeta/printpet-firmware/releases/tag/v1.5.1)。

PrintPet Core 是运行在第三方 FoloToy AI Passport（ESP32-C3、8MB Flash）上的社区 3D 打印机伴侣固件，与 FoloToy、Bambu Lab 无官方关联。此版本仅监控打印状态，移除了暂停、继续、停止、灯控入口；状态刷新仅发送固定 pushall 查询。Spirit 不包含在本次发布中。

## 适用范围

本包是**已有 Core 1.x 设备的应用区升级包**，适用于已使用本项目分区表的 AI Passport：应用地址 `0x10000`，应用分区 4MiB，总 Flash 8MiB。分区依据：[本项目 partitions.csv](https://github.com/ericwubeta/printpet-firmware/blob/1ead42d79fa5d291d3cae5431ee4ea724ee83c68/partitions.csv)。

`passport_printer.bin` 只有应用镜像，不是整片镜像，不能写到 `0x0`。原厂固件、其他衍生固件或未知分区布局不能直接套用本升级步骤；首次安装还需要匹配的 bootloader 和分区表，本 Release 不提供一键首次安装包。

## 下载与校验

下载 `passport_printer.bin`、`SHA256SUMS.txt` 和 `UPGRADE.md`。固件大小 **2,176,240 字节**，SHA256：

```text
08317a456c10beef12b7414c24d7436234fe004073ab09d0a7eff8a6b74e4e9d
```

Windows 可将三个文件放在 `E:\PrintPet-Core-v1.5.1`，在该文件夹打开 PowerShell：

```powershell
$expected = '08317a456c10beef12b7414c24d7436234fe004073ab09d0a7eff8a6b74e4e9d'
$actual = (Get-FileHash -LiteralPath '.\passport_printer.bin' -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw '固件校验不匹配，请重新下载' }
```

## 备份与升级

使用已有 Python/esptool 或 ESP-IDF 环境，关闭占用该串口的监视器，连接 AI Passport 的数据 USB 线。下面 `COM5` 只是示例，替换为设备管理器显示的实际串口。

示例使用 **esptool 4.x**（发布环境为 4.12.0）：

```powershell
python -m esptool version
if ($LASTEXITCODE -ne 0) { throw '请先配置可用的 Python/esptool 环境' }
$backup = 'backup-before-core-v1.5.1-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bin'
if (Test-Path -LiteralPath $backup) { throw '备份文件已存在，请换一个文件名' }
python -m esptool --chip esp32c3 --port COM5 --baud 460800 read_flash 0x0 0x800000 $backup
if ($LASTEXITCODE -ne 0) { throw '备份失败，停止升级' }
python -m esptool --chip esp32c3 --port COM5 --baud 460800 write_flash 0x10000 passport_printer.bin
if ($LASTEXITCODE -ne 0) { throw '写入失败，请保留备份并检查串口和连接' }
```

先确认备份成功再执行写入。备份包含 Wi-Fi、账号等设备数据，只在本机妥善保存。若连接不稳定，把波特率 `460800` 降为 `115200`。esptool 5.x 使用 `read-flash` / `write-flash` 命令名，其他参数和地址相同；工具准备见 [Espressif 官方文档](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/)。

**升级只写 `0x10000` 应用区。不执行全片擦除，不写 bootloader、分区表、NVS 或 SPIFFS，也不使用从 `0x0` 开始的合并镜像。** 本项目现有布局中 NVS 位于 `0x9000–0xefff`，应用区升级不覆盖它；原 Wi-Fi/账号配置应保留。本次发布没有对你的设备执行刷写。

需要单独核验写入时，可在 esptool 4.x 中运行：

```powershell
python -m esptool --chip esp32c3 --port COM5 verify_flash 0x10000 passport_printer.bin
```

esptool 5.x 对应命令为 `verify-flash`。不要把整片备份文件当成应用镜像写入 `0x10000`。

## 重启后检查

1. 启动/菜单名称为 PrintPet Core，设备页显示 `v1.5.1`。
2. 原 Wi-Fi 和拓竹账号仍可使用，能收到打印进度、温度、AMS 和设备信息。
3. 上下键循环浏览监控页，没有打印控制页；进度页 OK 切换剩余时间/完成时刻，其他详情页 OK 只请求状态刷新。
4. 若未配置或需要重新登录，长按 OK 进入配网；现有热点标识仍为 `Bambu-Setup`，密码 `setup123`，配网页为 `http://192.168.4.1`。

发布二进制只含示例占位凭据；设备继续读取自己的 NVS 配置。拓竹登录凭据过期时仍需重新配网登录，与固件校验和是否匹配是两件事。

## 验证范围与回退

已完成 ESP-IDF 编译、MQTT 只读发送检查和实际 LVGL 主机导航测试。测试覆盖 1–4 台模拟打印机、0–4 台 AMS；不等于多台真实打印机同时工作或本次二进制已经完成实机验收。当前发布镜像尚未在硬件上刷写验证，声音、休眠/唤醒和长期稳定性仍需实机确认。

保留刷前备份；如需回退，使用与原分区匹配的旧版应用和相应恢复流程。原 `v1.5.0` tag 保持不变，但它仍含旧控制入口；回退旧版会恢复当时的行为。本次升级说明不建议直接用整片备份覆盖当前设备数据。

## English quick reference

This is a Core 1.5.1 application-only update for third-party FoloToy AI Passport hardware that already uses this project's 8MiB Flash / 4MiB factory-app layout. The app address is **0x10000**, not 0x0. Verify the SHA256 above, back up existing Flash, and flash only `passport_printer.bin` to 0x10000. Do not erase all Flash or replace the bootloader, partition table or NVS during this upgrade. Stock firmware or unknown layouts require a separate first-install procedure.

Core 1.5.1 removes printer-control UI/APIs and retains monitoring and fixed pushall telemetry requests. The original v1.5.0 tag remains unchanged. Spirit remains unpublished. Compilation and host tests passed; this binary has not been physically flashed/accepted as part of this release process.
