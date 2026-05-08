# ReSpeaker Clip 硬件测试套件

## 概述

此测试套件为基于 nRF5340 的 ReSpeaker Clip 板提供全面的硬件组件测试。

## 构建

```bash
# 设置环境
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)

# 构建
west build --build-dir build-test --pristine --board clip/nrf5340/cpuapp tests/clip

# 烧录并重置
west flash --build-dir build-test && nrfutil device reset
```

## 串口配置

- **波特率**: 921600
- **端口**: /dev/ttyACM0 (或适当的 USB-串口端口)
- **连接**: `minicom -D /dev/ttyACM0 -b 921600`

## 测试模块

### 1. BLE 测试

**目的**: 测试作为外围设备的蓝牙低功耗功能

**描述**: BLE 在启动时自动开始广播。连接 BLE 中央设备以测试 GATT 服务和吞吐量。

**预期结果**:
- 设备广播为 "Clip_Test"
- 支持 GATT 连接和通知以进行吞吐量测试

### 2. WiFi AP 测试

**目的**: 测试 nRF7002 WiFi 模块在 AP (热点) 模式下

**AP 配置**:
- SSID: `ClipTest_XXXX` (从芯片 ID 自动生成)
- 密码: `12345678`
- 频段: 5GHz, 信道 36
- IP: 192.168.4.1
- DHCP 池: 192.168.4.2+

**命令**:
```bash
wifi on              # 启动 AP
wifi off             # 停止 AP
wifi status          # 显示 AP 状态
```

**快速测试**:
1. 运行 `wifi on`，注意串口输出中的 SSID
2. 将手机/PC 连接到 `ClipTest_XXXX`，密码 `12345678`
3. 设备应通过 DHCP 获取 192.168.4.x 地址
4. 运行 `wifi status` 以确认 AP 正在运行

---

#### WiFi 吞吐量测试 (zperf / iperf2)

**概述**: 设备使用 zperf 进行 UDP 吞吐量测试，与 iperf2 兼容。

**测试类型**: 从设备到 PC 的 UDP 上载 (设备发送，PC 接收)

**默认参数**:
- 服务器 IP: 192.168.4.10
- 端口: 5001
- 时长: 10 秒
- 速率: 100 Mbps (100000 kbps)

**测试步骤**:

**步骤 1: 在 PC 上启动 iperf2 服务器 (连接到 ClipTest AP)**
```bash
iperf -s -u -p 5001 -i 1
```

**步骤 2: 在设备上运行 iperf 测试**
```bash
iperf                       # 使用默认值 (192.168.4.10, 10s, 100Mbps)
iperf 192.168.4.10          # 指定 PC IP
iperf 192.168.4.10 30       # 30 秒测试
iperf 192.168.4.10 10 50000 # 10 秒测试，速率 50 Mbps
```

**命令参数**:
| 参数 | 描述 | 范围 | 默认值 |
|------|------|------|--------|
| server_ip | PC IP 地址 | 任何有效 IP | 192.168.4.10 |
| duration_sec | 测试时长 | 1-3600 秒 | 10 |
| rate_kbps | 发送速率 | 100-1000000 kbps | 100000 |


---

### 3. SD 卡测试

**目的**: 测试 SD 卡文件系统操作

**命令**:
```bash
sd mount             # 挂载 SD 卡
sd umount            # 卸载 SD 卡
sd format            # 将 SD 卡格式化为 FAT32
sd speed [size_kb]   # 速度测试
sd status            # 显示 SD 卡状态
fs ls /SD:           # 列出文件
```

**预期结果**:
- SD 卡在存在时挂载
- 文件列表正常工作
- 卸载安全卸载

### 4. 麦克风测试

**目的**: 测试 PDM 麦克风音频捕获和 WAV 录音

**命令**:
```bash
mic capture [time_sec]  # 捕获音频并打印采样统计
mic record [time_sec]   # 录制 WAV 文件到 SD 卡 (默认 3 秒)
```

**预期结果**:
- 音频捕获启动和停止
- 每个数据块打印采样统计 (avg/min/max)
- WAV 文件保存到 SD 卡 (RECXXXX.WAV)

**典型工作流**:
1. 插入 SD 卡并挂载: `sd mount`
2. 录制音频: `mic record 5`
3. 启用 USB MSC 访问文件: `usb msc on`
4. 在电脑上从 USB 驱动器复制 WAV 文件
5. 禁用 USB MSC: `usb msc off`

### 10. USB 大容量存储测试

**目的**: 将 SD 卡作为 USB 驱动器暴露给 PC，直接访问文件

**命令**:
```bash
usb msc on       # 卸载 SD，启用 USB MSC (SD 显示为 USB 驱动器)
usb msc off      # 禁用 USB MSC，重新挂载 SD 卡
usb status       # 显示 USB 和 SD 卡状态
```

**使用方法**:
1. 录制音频到 SD: `mic record 5`
2. 启用 USB MSC: `usb msc on`
3. 将 USB 线缆连接到 PC - SD 卡显示为 USB 大容量存储
4. 从驱动器复制文件
5. 在 PC 上安全弹出驱动器，然后: `usb msc off`

**注意事项**:
- USB MSC 和文件系统不能同时访问 SD 卡
- 再次录音前必须先禁用 MSC
- UART shell (921600 波特率) 使用独立 UART，不是 USB

### 5. 按钮测试

**目的**: 测试用户按钮功能

**预期结果**:
- 检测到按钮按下并记录日志

### 6. OLED 显示测试

**目的**: 测试 CH1115 OLED 显示 (88x48)

**命令**:
```bash
oled test            # 运行自动化测试
oled clear           # 清除显示
oled fill            # 填充显示
oled pattern         # 显示测试图案
oled circle          # 绘制圆圈
oled pixels          # 绘制测试像素
oled brightness <0-255>  # 设置亮度
```

**预期结果**:
- 显示正确显示测试图案
- 亮度调整工作
- 无可见瑕疵

### 7. PMIC 测试

**目的**: 测试 NPM1300 PMIC 电池和电源管理

**命令**:
```bash
pmic status          # 显示电池/充电器状态
pmic monitor         # 持续监控状态
pmic ship            # 进入船模式 (关机)
```

**预期结果**:
- 电池电压和百分比正确显示
- 充电状态准确
- 船模式关闭设备

### 8. 马达测试

**目的**: 测试振动马达

**命令**:
```bash
motor on             # 开启马达
motor off            # 关闭马达
motor pulse <ms>     # 脉冲持续时间
motor pattern <short|double|long|sos|alert>  # 播放图案
motor test           # 运行马达测试
```

**预期结果**:
- 马达正确开启/关闭
- 脉冲持续时间准确
- 图案按预期播放

### 9. IMU 测试

**目的**: 测试 LSM6DS3TR 6 轴 IMU 传感器

**命令**:
```bash
imu on               # 开启 IMU (GPIO0.2=HIGH)
imu off              # 关闭 IMU (GPIO0.2=LOW)
imu init             # 完整初始化 (开启电源 + I2C + 配置)
imu read             # 读取传感器数据
imu monitor [n]      # 监控 n 次迭代 (默认 10)
imu scan             # 扫描 I2C 总线
imu selftest         # 运行自检
```

**预期结果**:
- IMU 初始化并在地址 0x6A 检测
- WHO_AM_I 返回 0x6C 或 0x6A
- 加速度计和陀螺仪数据更新
- 移动设备时值改变

**解读传感器数据**:
- **加速度计**: +/- 4g 范围，静止时 ~1000 LSB/g
- **陀螺仪**: +/- 500dps 范围，静止时 ~0 LSB/s

## 故障排除

### IMU 未检测到

**症状**: WHO_AM_I 返回 0x00 或未找到设备

**解决方案**:
1. 检查 IMU 是否供电: GPIO0.2 应为高电平
2. 验证 I2C 连接: GPIO1.0 (SDA), GPIO1.1 (SCL)
3. 检查 SDO/SA0 引脚接地 (I2C 地址 0x6A)
4. 确保 I2C 上拉连接到 GPIO0.2
5. 运行 `imu scan` 检查任何 I2C 设备

### PMIC 船模式

**重要**: 进入船模式 (`pmic ship`) 后，设备将关闭电源。要唤醒:
- 连接 USB 线缆
- 按按钮
- 施加电压到 VBUS

### SD 卡问题

**症状**: 卡未挂载或错误

**解决方案**:
1. 检查卡是否正确插入
2. 尝试重新格式化为 FAT32
3. 移除卡前使用 `sd eject`
4. 检查瞬时同步错误 (这些是正常的)

### WiFi 连接失败

**症状**: 无法连接到 WiFi

**解决方案**:
1. 检查 SSID 和密码是否正确
2. 确保设备支持 5GHz WiFi (nRF7002 AP 仅 5GHz)
3. 检查天线是否连接
4. 尝试 `wifi on` 然后检查状态

## 硬件规格

### 引脚分配

| 功能 | GPIO | 描述 |
|------|------|------|
| 按钮 | GPIO1.15 | 用户按钮 (低电平有效) |
| IMU SDA | GPIO1.0 | I2C 数据 (软件) |
| IMU SCL | GPIO1.1 | I2C 时钟 (软件) |
| IMU INT1 | GPIO0.3 | IMU 中断 |
| IMU VDD_EN | GPIO0.2 | IMU 电源使能 (NFC1) |
| 马达控制 | GPIO1.6 | 振动马达控制 (通过 PMIC GPIO) |
| 麦克风 VDD_EN | GPIO1.14 | 麦克风电源使能 (GPIO 控制) |
| OLED VDD_EN | GPIO1.8 | OLED 电源使能 (GPIO 控制) |
| RFSW VDD_EN | GPIO0.29 | WiFi RF 开关使能 (GPIO 控制) |

### I2C 设备

| 设备 | 地址 | 总线 | 描述 |
|------|------|------|------|
| NPM1300 PMIC | 0x6B | I2C1 | 电源管理 IC |
| CH1115 OLED | 0x3C | I2C2 | 显示控制器 |
| LSM6DS3TR IMU | 0x6A | 软件 I2C | 6 轴 IMU 传感器 |

### 电源供应

- **USB**: 5V VBUS 用于充电和主电源
- **电池**: 由 NPM1300 管理的 Li-Po 电池
- **PMIC 调节器** (NPM1300):
  - BUCK1: MOTOR_3V3 (振动马达)
  - BUCK2: VDD_3V3 (主系统)
  - LDO1: VDDMIC_1V8 (麦克风)
  - LDO2: VDD_SD (SD 卡)
- **GPIO 控制的调节器**:
  - Mic VDD_EN: GPIO1.14 (麦克风电源使能)
  - OLED VDD_EN: GPIO1.8 (OLED 显示电源使能)
  - RFSW VDD_EN: GPIO0.29 (WiFi RF 开关使能)

## 内存使用

```
FLASH:      979 KB (93.4% of 1 MB)
RAM:        374 KB (81.5% of 448 KB)
```

## 测试覆盖矩阵

| 模块 | 电源 | 通信 | 配置 | 读取 | 写入 |
|------|------|------|------|------|------|
| BLE | ✓ | ✓ | ✓ | - | - |
| WiFi | ✓ | ✓ | ✓ | - | - |
| SD 卡 | ✓ | - | - | ✓ | ✓ |
| 麦克风 | ✓ | - | ✓ | ✓ | ✓ |
| 按钮 | ✓ | - | - | ✓ | - |
| OLED | ✓ | ✓ | ✓ | - | - |
| PMIC | - | ✓ | ✓ | ✓ | ✓ |
| 马达 | ✓ | - | - | - | - |
| IMU | ✓ | ✓ | ✓ | ✓ | ✓ |
| USB MSC | - | ✓ | ✓ | - | ✓ |

## 内置 Shell 命令

以下 Zephyr 内置 shell 命令可用于底层硬件测试。

### Regulator Shell

控制 PMIC 调节器 (BUCK1/2、LDO1/2) 和 GPIO 固定调节器 (mic、oled、rfsw):

```bash
regulator status         # 列出所有调节器及其状态
regulator enable <name>  # 启用调节器
regulator disable <name> # 禁用调节器
regulator vget <name>    # 获取调节器电压
```

调节器名称 (来自设备树):
| 名称 | 类型 | 控制对象 |
|------|------|----------|
| `BUCK1` | NPM1300 | 马达 3.3V |
| `BUCK2` | NPM1300 | 主系统 3.3V (always-on) |
| `LDO1` | NPM1300 | 麦克风 1.8V |
| `LDO2` | NPM1300 | SD 卡 3.3V |
| `mic_vdd` | GPIO 固定 | 麦克风电源使能 (GPIO1.14) |
| `oled_vdd` | GPIO 固定 | OLED 电源使能 (GPIO1.8) |
| `rfsw_vdd` | GPIO 固定 | WiFi RF 开关 (GPIO0.29) |

### GPIO Shell

```bash
gpio get <port> <pin>       # 读取 GPIO 引脚状态
gpio set <port> <pin> <0|1> # 设置 GPIO 输出
gpio conf <port> <pin> <cfg> # 配置 GPIO 引脚
```

示例:
```bash
gpio set gpio1 14 1   # 开启麦克风电源
gpio set gpio1 8 0    # 关闭 OLED 电源
gpio get gpio1 15     # 读取按钮状态
```

### I2C Shell

```bash
i2c scan i2c1       # 扫描 I2C1 总线 (PMIC @ 0x6B)
i2c scan i2c2       # 扫描 I2C2 总线 (OLED @ 0x3C)
i2c read i2c1 0x6b <reg> <len>   # 读取 NPM1300 寄存器
i2c write i2c1 0x6b <reg> <data> # 写入 NPM1300 寄存器
```

## 开发说明

### 添加新测试

1. 在 `tests/clip/src/` 中创建源文件
2. 在 `tests/clip/src/` 中创建头文件
3. 添加到 CMakeLists.txt
4. 在 main.c 中初始化
5. 添加 shell 命令

### 代码风格

- 遵循 Zephyr 编码风格
- 使用 LOG_MODULE_REGISTER 进行日志记录
- 错误时返回负 errno
- 使用 device_is_ready() 检查设备

### Shell 命令

使用 SHELL_CMD_* 宏进行 shell 命令注册:
- SHELL_CMD: 简单命令
- SHELL_CMD_ARG: 带参数的命令
- SHELL_STATIC_SUBCMD_SET_CREATE: 子命令层次结构

## 版本历史

- 2026-05-08: 添加 USB MSC 模块 (SD 卡作为 USB 驱动器)，添加 WAV 录音功能
- 2026-04-22: 更新文档以准确反映实现的特性，移除不存在的 BLE 和 WiFi 扫描命令，更正 SD 卡命令
- 2025-03-09: 添加软件 I2C 的 IMU 测试模块
- 2025-03-09: 添加振动马达测试命令
- 2025-03-09: 添加 PMIC (NPM1300) 测试命令
- 2025-03-09: 添加 OLED 显示测试命令
- 2023: 初始测试套件框架

## 许可证

Copyright (c) 2023 Nordic Semiconductor ASA

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause