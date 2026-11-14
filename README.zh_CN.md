# PrintPet Core 1.5.2

[English](README.md) | 简体中文

面向拓竹打印机的只读监控固件，运行于第三方FoloToy AI Passport（ESP32-C3、8MiB Flash、无PSRAM）。本项目不制造硬件，与FoloToy、Bambu Lab无官方关联。

## 两条固件，按喜好选择

- **[Core 1.5.2](https://github.com/ericwubeta/printpet-firmware/releases/tag/v1.5.2)**：1.5.x最终功能版，保留纯监控界面，不显示精灵角色或角色语音。
- **[Spirit 2.0.1](https://github.com/ericwubeta/printpet-firmware/releases/tag/v2.0.1)**：保留核心监控，加入六位精灵、状态动作、短语音和音量/亮度设置。它沿用此前本地2.2.0功能重新编号，不是旧实验代码回退。

两者共用新的身份区保护布局，可以使用对应包的升级模式切换。桌面版仍是独立产品，固件没有桌面AI聊天或两端联动。历史v1.5.0/v1.5.1不删改；新用户请选择上面的完整包，不要使用历史裸应用作为通用安装包。

## 相机监控与自动关机

详情第二页显示支持的打印机局域网画面、帧率和打印信息，设备重启后恢复上次页面。未检测到 USB 供电且没有已知的活动任务时，连续息屏 10 分钟后进入深度睡眠。详见[相机页面](docs/CAMERA_UI.md)和[自动关机](docs/AUTO_POWEROFF.md)。

## 为什么此次改为完整安装包

早先2.2.0 ZIP只包含应用与精灵资源，适合已在PrintPet专用旧布局上的升级，不能恢复被覆盖的bootloader/分区表。应用误写0x0会破坏引导区；旧4MiB应用布局又与厂商Arduino身份保护布局冲突。

本版提供配套bootloader、分区表、应用和Spirit所需资源，使用清单锁定地址并校验SHA。只有bootloader写0x0，**应用写0x370000**，资源写0x10000；0x356000起16KiB的cardid为只读，整个0x310000–0x36FFFF保留。不要合并填充该间隙、不要全片擦除，也不要把应用写到0x0或沿用旧0x10000应用地址。

完整步骤见[INSTALL.md](docs/INSTALL.md)。包内脚本默认只做离线检查；仅明确指定安装/升级模式、端口和confirm-write后才连接设备。它先保留本机备份，失败不自动重刷。误刷前已丢失的原厂身份或账号只能从原机备份/厂商渠道恢复，不能用别人的Flash替代。

## 从源码构建

使用ESP-IDF5.5，目标esp32c3。将`main/secrets.h.example`复制为`main/secrets.h`，保持示例占位值，不填开发者账号进发布包。

```text
idf.py build
python tools/build_bundle.py --build build --output dist
```

使用dist中的完整包和校验工具；不要直接运行构建日志中的默认烧录命令来处理未知设备。SDK的默认flash目标没有自动验证本项目保护区或完整资源输入。

## 验证范围

Build: PASS。主机只读监控/状态逻辑及安装包、模拟Flash保护测试通过。Device tests: NOT RUN（本轮新布局没有实机刷写）。旧版本的人工验收不能替代新布局验收；多机、全部真实打印状态、长期稳定性仍需分别确认。

## 角色与声音

精灵版素材保留各自来源与权利。芙宁娜、胡桃为《原神》/米哈游的非官方同人形象，相关短语音是AI克隆合成、非商用；原作中文演绎为钱琛和陶典，不是演员为本项目新录音，也不暗示官方或演员背书。软件许可证不转授第三方角色、商标或声线权利。
