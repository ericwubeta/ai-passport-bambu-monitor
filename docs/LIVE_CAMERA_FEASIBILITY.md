# 打印机摄像头监控：接口与可行性调研

调研与实测日期：2026-11-07。分支：`research/live-camera-monitor`，基于与 `origin/main` 一致的 `a894c5d`。下文记录最初电脑端与独立实验固件的可行性验证；后续已接入主应用第二页，正式页面、动态尺寸与按机器配置见 [CAMERA_UI.md](CAMERA_UI.md)。独立实验的内存数据不代表主应用内存余量。

## 结论

可以开发摄像头画面监控，但摄像头取流与本项目已有的 MQTT 状态订阅是两条链路。未找到可直接用当前账号 token、适配所有机型且有公开稳定契约的通用云端视频 REST API。

用户已确认：打印机是 **X2D**，画面显示在 **AI Passport**，并要求 **Passport 独立运行，不依赖电脑 / NAS 桥接**。实测结论：**定期更新的 JPEG 画面可以在现有 Passport 独立运行**。高清视频 H.264 直解仍不适合本板；实际可用的是读取打印机 JPEG 预览并流式缩放显示。主应用接入的测试结果另记于 CAMERA_UI.md。

## 本项目实测结果

目标 X2D 通过 SSDP 识别为 `N6`，固件 `01.02.00.00`。LAN 访问码读取自本机 Bambu Studio，TLS 证书先核对设备身份并本地固定，未将凭据写入受版本控制的文件。没有修改打印机设置或发送打印控制命令。

| 项目 | 结果 |
| --- | --- |
| 电脑端采样 | 180 秒，每 2 秒一次，共 91 次，91/91 成功 |
| 图片是否变化 | 91 个不同 JPEG MD5；解码后的 160×90 灰度缩略图也有 91 个不同像素哈希，不只是图片元数据变化 |
| JPEG | 1920×1080，baseline，4:2:0；电脑采样大小 100,943–111,113 字节 |
| 电脑端下载耗时 | 中位数 243.8 ms，范围 226.1–313.2 ms；不含 TLS 连接和图像解码 |
| Passport | ESP32-C3，无 PSRAM；20/20 次独立接收、MD5 校验、ROM JPEG 解码与 LCD 传输成功，无崩溃 |
| Passport 图片大小 | 实机测试期间为 122,953–125,415 字节，不能假设 JPEG 永远小于 100 KB |
| Passport 输出 | 按 1/8 缩小为 240×135 RGB565，135 行均提交 LCD 并收到 SPI 完成回调 |
| Passport 处理耗时 | 每张约 2.1–2.3 秒，首次约 2.6 秒，含建连、取图、解码与 LCD 传输 |
| 实验刷新周期 | 处理后再等待 2 秒，实际约 4.1–4.3 秒一张；未测试取消等待后的持续最高帧率 |
| 内存 | 解码工作区 4,096 字节，显示条带 960 字节；不缓存完整 JPEG / RGB 图。测试最低可用堆 158,908 字节，解码后最大连续可用块 114,688 字节 |
| 长期运行与共存 | 本次约 80 秒、20 张的板上验证；未做长时间运行测试，未同时运行原菜单、音频、MQTT |
| 现场延迟 | 用户暂时无法做可见动作测试，因此没有测量动作到画面出现的端到端延迟；接口更新已确认，不能把采样间隔当作现场延迟上限 |

测试场景为当前空闲打印机。打印中、不同固件、不同光照、弱 WiFi 或其他客户端同时观看时，仍需补测。用户已在后续持续预览中肉眼确认画面可见、方向与颜色正常。表中 20 张统计取自完整的一轮测试，后续预览不混入该统计。

用户确认后已恢复原固件：被实验固件改写的应用区域与备份逐字节一致（预览轮通过设备 MD5 校验），NVS 分区与测试前逐字节一致；恢复启动后 WiFi 获得 IP、拓竹云 MQTT 已连接，无崩溃。没有把实验程序作为当前常用固件留在设备上。

实现上先收帧头和有界 JSON 前缀，再把 JPEG 分片直接送入 C3 ROM `jd_prepare` / `jd_decomp(scale=3)`。ROM 解码器每次只输出小块 RGB888，转换成两行 RGB565 后发送 LCD。JPEG 尾部及剩余协议帧继续读完，MD5 和总长度都验证通过才计为成功。这避免了 1080p 完整解码帧和整张压缩文件的内存开销。

实机还确认了一点协议差异：X2D 返回帧头的高 magic 字节及 reserved 字段可能非零，接收端校验稳定的低 24 位通道标记，不把这些未定义字节当成错误。

### 复现工具与验证边界

- `tools/probe_camera_snapshot.py`：只支持登录、建立会话和读取 `mem:/26`；每张重新建连，校验偏移、总长度、MD5 与完整 JPEG。需要 Python、Pillow、cryptography。
- `tools/test_probe_camera_snapshot.py`：6 项协议回归测试通过，覆盖 TCP 分片、元数据跳过、完整性检查、错误偏移、截断和过大帧。
- `tools/camera_probe_idf/`：独立 ESP-IDF 5.5.3 实验工程，复用现有 BSP 显示驱动，编译通过且无编译警告；WiFi 从原 NVS 只读加载，兼容旧版字段。LAN 配置从被忽略的本地头文件注入。
- 实验 TLS 在握手后、发送访问码前，验证电脑采样时固定的服务器证书 SHA256。该实验工程没有正式配网、证书更新、断线重连或摄像头页面，不应直接当作正式发布固件。
- 本地证据、图片、配置、实验二进制、刷写备份及收据放在 `.local-backups/camera-validation/` 与 `build-camera-probe/`，均由 Git 排除规则保护；不上传设备照片或凭据。

电脑采样命令（从仓库根目录执行）：

```powershell
python -m unittest discover -s tools -p test_probe_camera_snapshot.py -v
python tools/probe_camera_snapshot.py --config .local-backups/camera-validation/connection.json --output .local-backups/camera-validation/series --count 91 --interval 2
```

`connection.json` 的本地字段为 `host`、`serial`、`access_code`；不应把值复制到命令行或文档。构建板上实验时，以 `-D PROBE_PRIVATE_DIR=<本地目录>` 指向含 `camera_probe_private.h` 的私有目录，并把 `SDKCONFIG` 和 build 目录放到被忽略的位置。该头文件提供 `CAMERA_HOST`、`CAMERA_SERIAL`、`CAMERA_ACCESS_CODE` 和 32 字节 `CAMERA_CERT_SHA256`。板上实验依赖已有有效 WiFi NVS，不能对空配置使用擦除恢复。

## 当前目标：X2D → AI Passport 独立运行

- 社区 X2D 实测记录确认：在打印机上启用单独的 **LAN Only Liveview / Local RTSP Stream** 开关后，端口 322 可出 RTSPS 视频；记录中的固件为 `01.02.00.00`，画面为 1920×1080、30 fps。这是上游实测，不是对用户机器的测试。设置菜单名称可能因固件不同。
- 候选地址为 `rtsps://PRINTER_IP:322/streaming/live/1`，用户名 `bblp`，密码为 LAN access code。无需把已有云 token 当作视频密码。不能因为 6000 端口开放，就按 A1/P1 的 JPEG 视频鉴权方式处理 X2D。
- 普通 H.264 解码仍需要原始分辨率的帧 / 参考帧内存。仅单张 1080p I420 图像即需 3,110,400 字节（约 2.97 MiB），尚未计入解码器、参考帧或网络。降低拉流帧率或只显示小图，不能自动消除这一开销。当前未查到 X2D 提供适用于 C3 的低分辨率 JPEG 子码流的可靠依据。
- **已实测的独立路径**：通过 TLS/6000 的文件隧道读取 `mem:/26` JPEG 预览，采用流式接收和分块缩放解码。Studio 官方源码使用此预览槽；本项目已按社区协议资料完成自有探测程序和 C3 实验固件，不需要在 MCU 上运行桌面插件。
- Studio 把此预览标成非实时图片；本次实测确认它会持续更新，但并未证明所有打印状态、固件下都具有固定的新鲜度。仍需通过可见动作测量现场延迟，并补测打印中的更新行为。
- 下一阶段可在正式应用中增加摄像头页面与本地配置，在保留原有功能的条件下测量堆、TLS 建连峰值和最大连续内存，再决定进入摄像头页时是否暂停 MQTT。当前实验不依赖电脑桥接；电脑仅用于开发、刷写和日志采集。

## 已找到的取流方式

| 对象 | 接口 / 协议 | 认证及前提 | 资料性质 |
| --- | --- | --- | --- |
| A1 / P1 系列 | TLS TCP `PRINTER_IP:6000`；自定义鉴权和帧头，后接 JPEG | 用户名 `bblp`，密码为打印机 LAN access code；需局域网可达，按固件检查 LAN Liveview 开关 | 社区原始协议研究；官方 Studio 源码确认存在 local/6000 路径 |
| X2D（当前目标） | RTSPS `PRINTER_IP:322/streaming/live/1`；JPEG 预览 `mem:/26` 经 TLS/6000 文件隧道读取 | `bblp` + LAN access code；RTSPS 需单独开启 LAN Only Liveview | 本项目已实测 JPEG 路径；未主动验证用户机器 RTSPS |
| X1 系列、P2S | `rtsps://PRINTER_IP:322/streaming/live/1` | 同上；不是普通 HTTP 图片地址 | 社区原始协议研究；官方 Studio 确认 RTSPS 路径 |
| H2S / H2D | 社区观察到同类 RTSPS；部分固件默认关闭 | 需启用 LAN Mode Liveview，具体名称与固件有关 | 社区对特定固件的观察，不能外推到所有 H2 型号 |
| 官方云端观看 | Studio 调用 network agent 的 `get_camera_url`，消费 `bambu:///...` 隧道；包含 TUTK / Agora 路径 | 账号与会话相关，需网络插件或实现其传输协议 | 官方客户端源码 + 社区协议研究；不是浏览器或 MCU 可直接播放的 HTTPS 视频 URL |
| 官方 Fleet Hub 抓图 | `GET /v1/hub/file/info?file_path=liveview/{dev_sn}/liveview.jpeg` | 专用 Hub、mTLS 客户端证书、激活绑定、Hub 会话 token | 官方文档；只适用于 Fleet Hub 环境 |

机型表是调研时已知情况，最终应以目标打印机报告的能力与固件为准。`bambu:///` 是插件内部协议，不可简单改成 `http://` 或当作标准 RTSP 地址。

### A1 / P1 的直连候选协议

OpenBambuAPI 记录：连接 TLS/6000 后发送 80 字节小端鉴权包，前 16 字节为 4 个 u32（`0x40, 0x3000, 0, 0`），之后为各 32 字节、补零的用户名和 LAN access code。接收端逐帧先读 16 字节头，其中首个 u32 是 JPEG 长度，再完整读取对应图片。

若实现，需要处理 TLS / TCP 任意分片、最大帧长度、超时重连和丢弃旧帧。它是私有协议的社区记录，不能认为是厂家承诺兼容的公共 SDK。是否需要更改打印机设置，应在型号和固件确认后逐项验证。

### 抓图与实时视频的区别

Fleet Hub 文档明确说 JPEG 是打印机周期上传的，客户端检查 `liveview_time`；这不代表连续视频或固定帧率。

Studio 当前还存在 `start_device_image_flow()`：经 `FileTransferObject` 下载 `mem:/26`，受 `is_support_liveview_preview` 能力限制，并把画面标成 “Non-live image”。这也是预览抓图，不应作为已证实的实时视频 API。

## 当前工程的缺口与内存限制

- `main/app_mqtt.c` 负责状态报文，没有图像接收或解码链路。
- `main/app_config.h` 仅存 WiFi、账号 UID / token、打印机 SN 和名称，没有每台打印机的 IP、LAN access code 或摄像头协议。
- 主分支目标是无 PSRAM 的 ESP32-C3 AI Passport；源码已记录联网后堆内存紧张。CYD 适配保存在 `feat/cyd`，本研究分支尚未合入。
- 240×320 RGB565 完整画面需 153,600 字节（150 KiB）；240×135 视频区域也需 64,800 字节。分块 JPEG 解码可以减少输出缓冲，但网络、TLS、输入 JPEG、LVGL 和 MQTT 仍需共同预算。
- Espressif 的 H.264 组件公开测试中，320×192 软件解码约需 1 MB，720p 约需 6.2 MB，且与流内容有关。这不是本板测量值，但足以说明现有无 PSRAM 板不适合把打印机高清视频直接交给该组件解码。缩小显示区域不能消除原分辨率解码需要的参考帧内存。

## 通用桥接备选（用户未采用，不作为当前实现方向）

1. 在电脑或 NAS 验证目标打印机能稳定出图，记录协议、尺寸和真实更新间隔。先验证一台，不改打印、暂停等控制行为。
2. 桥接服务仅保留最新一帧，缩成 240×135 JPEG，提供项目自建的抓图接口；先设每秒 1 次作为验证目标，实际受源帧率与设备性能限制。这是计划新增的接口，并非拓竹现成 API。
3. 小屏新增摄像头页，按需拉图，显示最后成功更新时间与离线状态；离开页面停止拉取，不积压历史帧。JPEG 使用分块解码或受限缓冲，并测量最小可用堆与最大连续内存。
4. 如确认是 A1/P1，可测直接 TLS/6000 + JPEG 缩放解码，比较有无桥接时的稳定性，再决定是否支持纯设备方案。
5. 若显示端是电脑或手机网页，则独立评估桥接后的 MJPEG / WebRTC；如要求离开家中网络观看，还需设计远程访问链路，局域网接口本身不解决外网访问。

当前在 AI Passport 方向继续以本分支的主分支基线为基础，不合入 CYD 适配。

## 后续产品化还需验证

- 打印中与长时间运行的更新行为，以及可见动作的端到端延迟。
- 完整 LVGL / MQTT 应用中使用快照时的内存峰值、重连与切页生命周期。
- 固件升级、网络异常、证书更新与 JPEG 大小变化时的处理。
- X2D 与 AI Passport 当前通过同一局域网直连；外网独立观看不在本次验证范围内。
- LAN 连接需要的 IP 与 access code 应在本地配置，避免写进源码、调研文档或日志。现有云 token 不等同于 LAN access code。

## 来源

1. [Bambu Studio 官方 MediaPlayCtrl.cpp](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/MediaPlayCtrl.cpp)：`SetMachineObject` 读取 IP / access code，`Play` 选择 local/6000、RTSPS 或云隧道；`start_device_image_flow` 实现非实时预览。
2. [OpenBambuAPI：video.md](https://github.com/Doridian/OpenBambuAPI/blob/main/video.md)：原作者整理的局域网协议和机型观察，非官方规范。
3. [OpenBambuAPI：cloud-video.md](https://github.com/Doridian/OpenBambuAPI/blob/main/cloud-video.md)：原作者对插件云端传输的研究，非官方 SDK。
4. [Bambu Fleet Hub HTTP API v1.0.0（官方 PDF）](https://portal.bblmw.com/fleet-hub/wb76oog4i8k/files/Bambu%20Fleet%20Hub%20HTTP%20API%20EN%20v1.0.0.pdf)：2026-05-20 更新，章节 1–2 说明 Hub 与认证，4.3 说明抓图。
5. [Espressif ESP_H264 官方 README](https://github.com/espressif/esp-h264-component/blob/master/esp_h264/README.md)：解码约束、内存和不同芯片的基准数据。
6. [open-bamboo-networking #105：X2D 实测与最终解决说明](https://github.com/ClusterM/open-bamboo-networking/issues/105)：应读最后的解决说明，最初“X2D 不支持 RTSP”的推断已被作者撤回；开启 LAN Liveview 后成功取流。
7. [社区原始 TLS/6000 协议研究](https://github.com/ClusterM/open-bamboo-networking/blob/master/research/06.04-port-6000.md)：文件隧道的登录与分块下载，不同于 A1/P1 视频协议；本项目据此完成 X2D JPEG 预览实测。

以上外部链接指向调研时的文档 / 主分支，未来可能变化。自有实测结论以本文件前部的范围与限制为准，不把上游能力列表当作本机验证结果。
