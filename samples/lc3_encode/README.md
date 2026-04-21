# LC3 Audio Encoder Sample

LC3 (Low Complexity Communication Codec) 实时音频编码和流式传输应用。

## 功能特性

- **LC3 编码**：蓝牙 LE Audio 标准编解码器，低延迟低复杂度
- **三种音频模式**：
  - `mono` - 单声道（仅左麦克风）
  - `stereo` - 立体声（左+右麦克风，每通道独立编码）
  - `merge` - 混合模式（左右麦克风平均）
- **多路输出**：UART / BLE / SD 卡
- **SpeexDSP 预处理**：可选噪声抑制和去混响
- **流控支持**：支持启动/停止/退出命令
- **编码统计**：实时显示每帧编码时间（最小/最大/平均）

## LC3 vs Opus 对比

| 特性 | LC3 | Opus |
|------|-----|------|
| 帧时长 | 7.5ms, 10ms | 2.5ms, 5ms, 10ms, 20ms, 60ms |
| 采样率 | 8-48 kHz | 8-48 kHz |
| 码率范围 | 16-320 kbps | 6-510 kbps |
| 用途 | Bluetooth LE Audio | 通用 VoIP/流媒体 |
| 复杂度 | 较低 | 较高 |
| 延迟 | 极低 | 低到中等 |
| Flash 占用 | ~20 KB | ~85 KB |

## 硬件配置

- **MCU**: nRF5340 (Cortex-M33 @ 64MHz)
- **DMIC**: 立体声 PDM 麦克风阵列
- **采样率**: 16 kHz
- **帧大小**: 10 ms (160 samples/帧)
- **码率**: 32 kbps/通道 (mono: 32 kbps, stereo: 64 kbps)
- **波特率**: 921600

## 构建

```bash
source ~/ncs/v3.2.1/zephyr/zephyr-env.sh
export ZEPHYR_EXTRA_MODULES=$(pwd)
west build --build-dir build-lc3 --board clip/nrf5340/cpuapp samples/lc3_encode
```

## 刷写

```bash
west flash --build-dir build-lc3 && nrfutil device reset
```

## 使用方法

### 设备命令（串口输入）

| 命令 | 功能 |
|------|------|
| `1` | 切换到单声道模式（左声道） |
| `2` | 切换到立体声模式 |
| `3` | 切换到混合模式（左右平均） |
| `s` | 开始录音 |
| `e` | 停止录音 |
| `u` | 切换 UART 输出 |
| `d` | 切换 SD 卡保存 |
| `b` | 切换 BLE 流式传输 |
| `l` | 列出 SD 卡文件 |
| `p` | 切换 SpeexDSP（NS/去混响） |
| `q` | 退出程序 |

### Python 接收脚本

```bash
# 安装依赖
pip install pyserial
pip install lc3  # 可选，用于实时解码为 WAV

# 单声道录音
python3 samples/lc3_encode/receive_lc3.py /dev/ttyACM0 921600 . --mode mono

# 立体声录音
python3 samples/lc3_encode/receive_lc3.py /dev/ttyACM0 921600 . --mode stereo

# 混合模式录音
python3 samples/lc3_encode/receive_lc3.py /dev/ttyACM0 921600 . --mode merge
```

## 数据协议

### 头部信息

```
>>> LC3_STREAM_START
SAMPLE_RATE=16000
CHANNELS=1 或 2
FRAME_SIZE=160
FRAME_DURATION_US=10000
BITRATE=32000 或 64000
>>> DATA_START
```

### 编码帧格式

```
<4位十六进制长度>\n
<十六进制数据>\n
```

对于立体声模式，每帧包含左右通道编码数据拼接。

### 结束标记

```
>>> DATA_END
```

## 模式详解

### Mono（单声道）

- **声道数**: 1
- **码率**: 32 kbps
- **说明**: 仅使用左麦克风数据
- **用途**: 单麦克风录音，节省带宽

### Stereo（立体声）

- **声道数**: 2
- **码率**: 64 kbps (每通道 32 kbps)
- **说明**: 每通道独立 LC3 编码，保留完整方向信息
- **用途**: 立体声录音，需要方向信息

### Merge（混合）

- **声道数**: 1
- **码率**: 32 kbps
- **说明**: 左右声道数据平均混合 `(L + R) / 2`
- **用途**: 全向收音，减少方向性

## LC3 编码特点

与 Opus 不同，LC3 对每个通道独立编码：
- **单声道/混合模式**: 直接编码单通道 PCM 数据
- **立体声模式**: 先去交错为独立的左/右通道，分别编码后拼接输出
- **帧大小固定**: 10ms (160 samples at 16kHz)，不可变

## 项目结构

```
samples/lc3_encode/
├── src/
│   ├── main.c              # 主程序（DMIC采集、LC3编码、多路输出）
│   ├── ble.c               # BLE音频流式传输
│   └── ble.h               # BLE接口
├── boards/
│   └── clip_nrf5340_cpuapp.overlay  # UART波特率配置
├── CMakeLists.txt
├── prj.conf                 # Zephyr配置
├── receive_lc3.py           # Python接收脚本
└── README.md
```

## 依赖

- Zephyr RTOS v3.2.1 (nRF Connect SDK)
- nRF LC3 软件编解码器 (nrfxlib)
- SpeexDSP (可选，噪声抑制)
- pyserial (Python)
- lc3 (Python，可选)

## 许可证

Apache-2.0
