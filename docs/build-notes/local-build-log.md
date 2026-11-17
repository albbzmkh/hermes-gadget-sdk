# 本地构建实录（macOS + VSCode + PlatformIO）

本文件记录本项目在 **macOS (Apple Silicon) + VSCode + PlatformIO** 上从零跑通编译与烧录的全过程，
包括遇到的每一个坑、根因、以及最终可复现的操作步骤。

适用对象：M5Stack StickS3（ESP32-S3-PICO-1-N8R8，8MB flash + 8MB octal PSRAM）。

---

## 1. 最终可用状态速查

| 项目 | 值 |
|---|---|
| 平台 | PlatformIO Core **6.2.0** |
| 平台包 | espressif32 **8.0.0** |
| 框架 | ESP-IDF **6.1.0**（`framework-espidf 4.60100.0`） |
| 工具链 | xtensa-esp-elf / riscv32-esp **15.2.0+20251204** |
| 目标环境 | `m5stack-sticks3` |
| 固件大小 | **1,838,992 B (1796 KB)**，含中文字体 |
| Flash 占用 | **90.5%**（1984 KB 槽） |
| RAM 占用 | 22.2%（72,864 / 327,680 B） |
| 构建耗时 | 全量 ~124s，增量 ~100s |
| 磁盘占用 | `~/.platformio/packages` 4.8 GB，`.pio/build` 250 MB |

---

## 2. Python 环境布局（三套，都由 PlatformIO 管理）

```
~/.platformio/python3                     3.11.7   ← 插件自动下载的内置 python（父环境）
        │
        ├── ~/.platformio/penv            3.11.7   ← PlatformIO Core（pio 命令跑这）
        │            │
        │            └── .espidf-6.1.0    3.11.7   ← ESP-IDF 编译期依赖
```

### 为什么是三套

- **`python3/`** — PlatformIO VSCode 插件的 "built-in python"。插件源码里有硬编码检查
  （`platformio-node-helpers/dist/index.js` 中的 `isBuiltinPythonOutdated()`），
  发现版本不合规会自动下载一份到这里，然后用它创建 penv。
- **`penv/`** — PlatformIO Core 自己的 virtualenv。`pio` 命令、包管理、构建脚本都在这里。
- **`.espidf-6.1.0/`** — 编译 ESP-IDF 项目时按需创建的子虚拟环境，隔离 IDF 的 Python 依赖
  （`idf-component-manager`、`kconfgen` 等），避免与 Core 依赖冲突。

**这三套都不能删。** 删 `python3/` 插件会重新下载；删 `penv/` 要重装整个 PlatformIO；
删 `.espidf-6.1.0/` 会触发重新创建（但依赖要重装，很慢）。

### 为什么不使用系统 Python

系统自带 `/Library/Developer/CommandLineTools/.../python3` 是 **3.9.6 + LibreSSL 2.8.3**：

- `urllib3 >= 2` 只支持 OpenSSL 1.1.1+，加载时会打印
  `NotOpenSSLWarning: urllib3 v2 only supports OpenSSL 1.1.1+`
- `idf-component-manager 3.x` 要求 **Python >= 3.10**，3.9 装不上
- ESP-IDF 6.1 的 `project.cmake:159` 硬编码要求 component-manager `interface_version 5`

所以必须用 3.11。Homebrew 的 `/opt/homebrew/bin/python3.11` 目前**不参与**这套体系，只是留在系统里。

### 关键约束文件

`~/.platformio/penv/.espidf-6.1.0/pio-idf-venv.json`：

```json
{
  "version": "1.0.0",
  "python_version": "3.11.7-final.0"
}
```

PlatformIO 每次构建前会读这个文件，如果实际 Python 版本与 `python_version` 不符就**重建 venv**。
手动改过 Python 版本后必须同步改这里，否则下次构建被打回。

重新安装 IDF 依赖（万一 venv 被重建空了）：

```bash
~/.platformio/penv/.espidf-6.1.0/bin/python -m pip install -U \
  "idf-component-manager>=3.1" \
  -r ~/.platformio/packages/framework-espidf/tools/requirements/requirements.core.txt
```

---

## 3. 依赖包手动安装（国内网络必需）

### 背景

PlatformIO 默认从 `dl.registry.platformio.org` 下载工具链和框架，该域名 302 到
`github.com/ivankravets/pioreg-ci-mirror`，最终落在 `release-assets.githubusercontent.com`。
国内实测 **~7-22 KB/s**，811 MB 的 riscv32 工具链要下十几个小时，UI 就卡在
`Resolving m5stack-sticks3 dependencies...` 不动。

### 解决办法：绕过 PIO，手工下载 + 校验 + 落盘

PIO 的包检测机制很简单（`platformio/package/meta.py` 的 `get_installed()`）：
扫描 `~/.platformio/packages/*` 目录，看每个目录里有没有 `.piopm` 文件。

`.piopm` 格式：

```json
{
  "type": "tool",
  "name": "toolchain-xtensa-esp-elf",
  "version": "15.2.0+20251204",
  "spec": {
    "owner": "platformio",
    "id": 8032,
    "name": "toolchain-xtensa-esp-elf",
    "requirements": "15.2.0+20251204",
    "uri": null
  }
}
```

平台依赖（framework / toolchain / gdb 等）**全部以 `type: "tool"` 装进 `packages/`**，
不存在单独的 toolchain 类型。

### 加速通道

实测（32 MB range 请求，多路并发）：

| 代理 | 单连接 | 128 路并发 |
|---|---|---|
| `gh-proxy.com` | 121 KB/s | **~4 MB/s** ✅ 最终选用 |
| `ghfast.top` | 83 KB/s | ~1 MB/s |
| `ghproxy.net` | 9 KB/s | — |

用法：代理前缀 + 原始 GitHub release URL。

```bash
# 例：下载 toolchain-xtensa-esp-elf
url="https://github.com/ivankravets/pioreg-ci-mirror/releases/download/.../xxx.tar.gz"
curl -L "https://gh-proxy.com/${url}" -o toolchain.tar.gz
```

注意：`dl.registry` 的 302 签名链接**约 1 小时过期**，过期后重新 HEAD 取新链接。

### 已安装的 16 个包

```
espressif32 @ 8.0.0                        ( platform )
framework-espidf @ 4.60100.0
tool-cmake @ 3.30.2
tool-esp-rom-elfs @ 0.0.1+20241011
tool-esptool @ 2.50400.0
tool-idf @ 1.0.1
tool-mkfatfs @ 2.0.1
tool-mklittlefs @ 1.203.210628
tool-mkspiffs @ 2.230.0
tool-ninja @ 1.13.2
tool-riscv32-esp-elf-gdb @ 12.1.0+20221002
tool-scons @ 4.41101.0
tool-xtensa-esp-elf-gdb @ 12.1.0+20221002
toolchain-esp32ulp @ 1.23800.240113
toolchain-riscv32-esp @ 15.2.0+20251204     ( 811 MB )
toolchain-xtensa-esp-elf @ 15.2.0+20251204  ( 320 MB )
```

关键 sha256（供重新下载校验）：

| 包 | 大小 | sha256 |
|---|---|---|
| toolchain-riscv32-esp | 811,719,389 B | `3ad7d4b270444c753de62d1e2a50d396dfd36b6a18fad913baef44b12611cead` |
| toolchain-xtensa-esp-elf | 320,259,449 B | `72a4b3545bdcec8b2363ad363af28c9b7d21770d30d8475c6e5ac798437a128f` |
| framework-espidf | 91,834,939 B | `dabc3f2d16cc28c61d92fa378bb249da5dd4bc601c3564318a930bce3ddb63e7` |

---

## 4. VSCode 插件配置

### 陷阱：必须打开正确的目录

插件 `package.json` 的激活条件是：

```json
"activationEvents": ["workspaceContains:**/platformio.ini"]
```

**只匹配工作区文件夹的根层，不递归子目录。**

| 打开路径 | 插件状态 |
|---|---|
| `hermes-gadget-sdk/`（仓库根） | ❌ 根层没有 platformio.ini → **不激活**，所有命令报 `command 'platformio-ide.build' not found` |
| `hermes-gadget-sdk/firmware/esp32/` | ✅ 正常 |

### 两种正确开法

**A. 只做固件**（最干净）

```
File → Open Folder → firmware/esp32
```

**B. 看全仓库 + 能用插件**：已创建多根工作区文件 `hermes-gadget.code-workspace`

```json
{
  "folders": [
    { "path": "." },
    { "name": "esp32 (PlatformIO)", "path": "firmware/esp32" }
  ],
  "settings": { "platformio-ide.disableToolbar": false }
}
```

```
File → Open Workspace from File → hermes-gadget.code-workspace
```

打开后左下角状态栏出现 PlatformIO 按钮（🏠 ⏻ ✓ →），**先点环境名切到 `m5stack-sticks3`**，再点 ✓。

---

## 5. 中文支持（CJK 字体）

官方文档见 [docs/cjk.md](../cjk.md)。要点：

- 12×12 位图字体，**7176 个字形**（Big5 繁体 + GB2312 一级简体 + CJK 符号 + 全角）
- 默认 **OFF**，开启后行高从 **9px → 13px**，**改变所有布局**
- 字形数据已内置仓库 `firmware/core/src/font_cjk_data.cpp`，**不需要重新生成字体**，不需要 Pillow
- 编译后增加约 **160 KB**

### 开启方式

在 `firmware/esp32/platformio.ini` 对应 env 的 `board_build.cmake_extra_args` 下加一行：

```ini
[env:m5stack-sticks3]
board_build.cmake_extra_args =
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/m5stack-sticks3/sdkconfig.defaults"
    -DHG_CJK_FONT=ON          ; ← 加这行
```

**当前已对 `m5stack-sticks3` 和 `aipi-lite` 两个 env 开启。**

定义位置：`firmware/core/CMakeLists.txt:36`

```cmake
option(HG_CJK_FONT "Build in the 12x12 CJK font and its taller text lines" OFF)
```

### 必须清缓存

改了 CMake 参数不清 build 目录，**不会生效**：

```bash
rm -rf .pio/build/m5stack-sticks3
```

或 `Cmd+Shift+P → PlatformIO: Clean`。

### 验证中文真的编进去了

```bash
# 符号表里应有这三个
nm .pio/build/m5stack-sticks3/firmware.elf | grep -i cjk
# _ZN2hg4font11kCjkBitmapsE     129168 B  (126.1 KB 字形位图)
# _ZN2hg4font14kCjkCodepointsE   28704 B  ( 28.0 KB 码点索引)
# _ZN2hg4font14kCjkGlyphCountE       4 B  (7176 个字形)

# 字符串表里应有 "cjk"，且 "latin1-cyrillic" 应被裁掉
strings .pio/build/m5stack-sticks3/firmware.elf | grep -E "^cjk$|latin1-cyrillic"
```

插件侧协议：`firmware/core/src/app.cpp:616`

```cpp
d.set("charset", HG_CJK_FONT ? "cjk" : "latin1-cyrillic");
```

**只有上报 `"cjk"`，插件才会保留宽字符。** 中文显示不出来 → 先查这一项。

---

## 6. 空间问题：90.5% 占用

### 现状

```
frontend 固件  1,838,992 B = 1796 KB
ota_0 槽       2,031,616 B = 1984 KB
占用           90.5%，剩余 188 KB (9.5%)
```

`firmware/esp32/tools/check_size.py` 的阈值是 **10% 最小剩余**：

```python
p.add_argument("--min-free", type=float, default=10.0, ...)
...
if free < args.min_free:
    print(f"check_size: {summary}; keep at least {...}% free. ...", file=sys.stderr)
    return 1
```

**9.5% < 10% → 返回 1 → 构建判定 FAILED → upload 被阻断。**

这就是为什么 VSCode 点 ✓ 之后什么都没发生。

### 根因：8MB flash 只切了 4MB

`firmware/esp32/partitions.csv` 当前：

```
nvs,      data, nvs,     0x9000,   0x6000     24 KB
otadata,  data, ota,     0xf000,   0x2000      8 KB
phy_init, data, phy,     0x11000,  0x1000      4 KB
ota_0,    app,  ota_0,   0x20000,  0x1f0000  1984 KB
ota_1,    app,  ota_1,   0x210000, 0x1f0000  1984 KB
```

用到 `0x400000` = **4.00 MB**，而板子是 **8 MB**，**另一半完全未分配**。

文件注释：`Fits 4 MB of flash and more. ... don't shrink the slots.`
→ **放大安全，缩小禁止**（已烧录的板子不能缩小 slot 布局）。

### 方案：扩大 OTA 槽（未执行，待定）

```csv
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x3e0000
ota_1,    app,  ota_1,   0x400000, 0x3e0000
```

每槽 1984 KB → **4032 KB**，占用率 90.5% → **44.5%**。

⚠️ **必须 USB 重新烧录**（OTA 升级改不了分区表）。

### 临时绕过（已用过）

不想改分区表时，手工烧录跳过 post-action 检查：

```bash
~/.platformio/packages/tool-esptool/esptool \
  --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     .pio/build/m5stack-sticks3/bootloader.bin \
  0x8000  .pio/build/m5stack-sticks3/partitions.bin \
  0x20000 .pio/build/m5stack-sticks3/firmware.bin
```

**地址必须对齐分区表**：`ota_0` 在 `0x20000`，不是常规的 `0x10000`。
写错地址 → bootloader 报 `No bootable app partitions` → 无限重启。

---

## 7. 烧录完整流程

### 正常流程（check_size 通过后）

```
VSCode 状态栏确认环境 = m5stack-sticks3
→ 点 →  (Upload)
或 Cmd+Shift+P → PlatformIO: Upload
```

### 端口识别

```bash
ls /dev/cu.usbmodem*        # StickS3 自带 USB Serial/JTAG
```

连上后应出现 `/dev/cu.usbmodem101`（或 102/103）。

### 进不了下载模式

StickS3 的下载模式靠**电源键**（M5PM1 管理），不是 KEY2：

> 接上 USB 后**按住电源键**，直到内部绿色 LED 闪烁。

### 复位方式

用 USB Serial/JTAG 时，**DTR/RTS 时序控制经过 M5PM1，软件复位序列会被吞掉**。
想让固件正常启动（而不是停在 `boot:0x20 DOWNLOAD`）：

**直接拔线，等 3 秒再插回去。**

### 验证启动日志

```bash
# 简单读串口
~/.platformio/penv/bin/python -c "
import serial,time
s=serial.Serial('/dev/cu.usbmodem101',115200,timeout=1)
t=time.time()
while time.time()-t<30:
    d=s.read(4096)
    if d: print(d.decode('utf-8','replace'),end='')
"
```

启动成功的标志：

| 日志 | 含义 |
|---|---|
| `boot:0x29 (SPI_FAST_FLASH_BOOT)` | ✅ 正常启动（`0x20` 是下载模式） |
| `ESP-IDF 6.1.0 2nd stage bootloader` | ✅ bootloader 正常 |
| `codecs: speaker ready, microphones ready (es8311 analog)` | ✅ 音频子系统 OK |
| `boot: No bootable app partitions` | ❌ app 烧错地址 |

---

## 8. NVS 数据为什么不会被刷固件清掉

烧录只写三个地址，**`0x9000` 的 nvs 分区完全没碰**：

```
0x0         bootloader.bin       ← 写
0x8000      partitions.bin       ← 写
0x9000      nvs (24 KB)          ← 不动，配置在这
0xf000      otadata (8 KB)       ← 不动，OTA 槽选择在这
0x20000     firmware.bin         ← 写
```

项目用三个 NVS namespace：

| Namespace | 位置 | 内容 |
|---|---|---|
| `ml_config` | `components/tailtiny/src/ml_config_httpd.c:45` | WiFi SSID/密码、hermes 服务器地址 |
| `microlink` | `components/tailtiny/src/tailtiny.c:36` | WireGuard 私钥、disco 密钥、DERP 偏好 |
| `ml_peers` | `components/tailtiny/src/ml_peer_nvs.c:24` | 已配对节点公钥、VPN IP、endpoint |

密钥只在**首次**生成，见 `tailtiny.c:68-79`：

```c
esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "NVS open failed (%d), generating ephemeral keys", err);
    generate_keypair(...);      // 只有 NVS 打不开才重新生成
    return ESP_OK;
}
size_t key_len = 32;
bool need_save = false;         // 能读到就不重新生成
```

所以 **刷固件后 WiFi、服务器地址、配对关系全部保留**，这是 ESP32 的设计特性，不是 bug。

### 需要清空时

```bash
# 只清配置（保留固件）
~/.platformio/packages/tool-esptool/esptool --chip esp32s3 --port /dev/cu.usbmodem101 \
  erase-region 0x9000 0x6000

# 全芯片擦除（固件也没了，且 ml_peers 密钥丢失 → 服务端配对失效）
~/.platformio/packages/tool-esptool/esptool --chip esp32s3 --port /dev/cu.usbmodem101 erase-flash
```

⚠️ 正常升级固件**绝不要**用 `erase-flash`。

---

## 9. 常见故障速查

| 现象 | 根因 | 解决 |
|---|---|---|
| 卡在 `Resolving ... dependencies...` | dl.registry 源被限速 | 手工下载包放进 `~/.platformio/packages/` + 写 `.piopm` |
| `command 'platformio-ide.build' not found` | 工作区根层没有 platformio.ini，插件未激活 | 打开 `firmware/esp32` 或用 `.code-workspace` |
| `FileExistsError: [Errno 17] File exists: .../m5stack-sticks3` | `.pio/build` 目录残留 | `rm -rf .pio/build/m5stack-sticks3` |
| 构建 0.33s 就 FAILED | 同上 | 同上 |
| `check_size: ... 9% free` + `Error 1` | 固件超 90% 槽容量 | 扩 partitions.csv，或手工 esptool 烧录 |
| `NotOpenSSLWarning: urllib3 v2 ...` | Python 3.9 + LibreSSL 2.8.3 | 用 3.11（本文件第 2 节） |
| `No bootable app partitions` | app 烧到了 `0x10000` 而非 `0x20000` | 用分区表里的真实地址 |
| 停在 `boot:0x20 (DOWNLOAD)` | GPIO0 被拉低 | 拔插 USB 线重新上电 |
| `No module named kconfgen` | IDF venv 依赖缺失 | 见第 2 节 pip install 命令 |
| `ModuleNotFoundError: esptool` | penv 里没有 esptool 模块 | 直接用 `~/.platformio/packages/tool-esptool/esptool` |

---

## 10. 当前工作区变更清单

```
M  firmware/esp32/platformio.ini          加了 -DHG_CJK_FONT=ON（m5stack-sticks3 与 aipi-lite）
?? firmware/esp32/.gitignore              忽略 .pio / .vscode 中间文件
?? hermes-gadget.code-workspace           多根工作区，让插件在仓库根也能激活
```

其他改动均已还原：`~/.platformio/platforms/espressif32/platform.py` 未修改；
VSCode 全局 `settings.json` 只有 `{"editor.accessibilitySupport": "on"}`。

---

## 11. 待办

- [ ] 决定是否扩 `partitions.csv` 到 8MB（90.5% → 44.5%），需 USB 重烧
- [ ] 如需给其他 env 开中文，同样在对应 `board_build.cmake_extra_args` 加 `-DHG_CJK_FONT=ON`
- [ ] 开启 CJK 后 14 个布局测试假设 9px 行距，会失败（预期行为，见 docs/cjk.md）
