# 灯语：基于视觉与语音交互的智能陪伴台灯

灯语是一套基于瑞萨 RA8P1 的嵌入式智能台灯系统。项目将五自由度总线舵机、摄像头、麦克风、触摸屏、环境光传感器和网络协处理器集成到同一套裸机应用中，实现台灯照明、桌宠动作、人脸检测与跟踪、音乐律动、闹钟、天气查询和实时语音交互等功能。

工程使用 Renesas e² studio 与 FSP 配置外设，主程序采用协作式主循环和非阻塞状态机组织各模块，不依赖 RTOS。

## 主要功能

### 台灯模式

- 控制五个 STS3215 总线舵机完成展开和收起动作。
- 展开完成后进入增强阻尼控制，允许用户手动调整灯体姿态，同时保持承重关节的支撑力矩。
- 通过 12 位 ADC 采集环境光，并使用 GPT PWM 驱动外接 MOS 管调节灯泡亮度。
- 自动调光采用 200 ms 采样周期、一阶指数滤波、3% 死区和单次最大 5% 的亮度变化限制。
- 支持关闭自动调光后，通过触摸屏手动设置 0%～100% 的灯泡亮度。
- 提供专注计时和休息提醒界面。

### 桌宠模式

- 使用定时状态机编排五个舵机的拟人动作，避免长时间阻塞主循环。
- 摄像头检测到人脸后，根据目标中心位置执行水平和俯仰跟踪。
- 支持待机、巡查、目标回应、跟踪、音乐监听和舞蹈等行为状态。
- 音乐模式下使用音频频谱、起音检测和节拍跟踪驱动舵机动作。
- 摄像头和麦克风根据行为状态互斥启停，降低外设冲突和不必要的处理开销。

### 视觉推理

- 摄像头通过软件 I²C 配置，通过 8 位 DVP 接口输出像素数据，由 RA8P1 CEU 采集。
- 当前采集分辨率为 160×120，像素格式为 RGB565，每帧约 38.4 KB。
- 将摄像头图像缩放并填充为 256×256×3 的 INT8 输入张量。
- 使用 RA8P1 端侧 NPU 和 Ethos-U 推理链路运行目标检测模型。
- 对推理结果执行分类置信度筛选、边界框解码和 NMS，NMS IoU 阈值为 0.45。
- 模型包含闭合书本、展开书本和人脸类别；当前主程序主要使用人脸结果完成桌宠跟踪。

### 音频处理

- 通过 SSI/I²S 接收 INMP441 数字麦克风数据。
- 每帧包含 128 个音频采样点，左右声道各占一个 32 位时隙。
- 将 32 位时隙中左对齐的 24 位有效音频转换为单声道 PCM16。
- 使用两个 I²S 采集缓冲区交替接收数据，再将处理后的帧写入深度为 64 的环形队列。
- 统计每帧平均绝对幅值、峰值和丢帧数量，供音乐识别、语音模型和云端语音链路使用。
- 提供串口导出测试，可在 PC 端将采样数据保存并检查音频质量。

### 图形界面

- 移植 LVGL 8.3.8，驱动 320×480 TFT LCD。
- 使用 SPI 发送显示数据，并由 DMAC 辅助传输。
- LVGL 使用两个 20 行绘制缓冲区，使界面绘制与屏幕刷新交替进行。
- 通过 I²C 驱动 GT911 电容触摸芯片，并将坐标读取接口注册为 LVGL 指针输入设备。
- 支持主页、台灯、桌宠、闹钟、天气和实时聊天页面。
- 使用独立 PWM 调节 LCD 背光亮度。

### 时间、闹钟与网络功能

- 使用 RA8P1 RTC 保存和读取日期时间，VBATT 供电正常时主电源断开后仍可继续计时。
- 支持通过网络协处理器校准 RTC 时间。
- 最多保存 4 个闹钟，支持添加、修改、启停和删除。
- 闹钟设置保存到 OSPI Flash，并使用 Magic、版本号和 CRC 检查数据有效性。
- 支持三天天气查询与触摸刷新。
- 通过 UART 与 ESP 网络协处理器通信，使用带序号、长度和 CRC 的帧协议承载时间、天气和 WebSocket 数据。
- 支持豆包实时语音对话、PCM 音频上传、语音回复播放，以及开灯、关灯、站立、坐下和摇头等设备控制命令。

## 系统结构

```text
SCC8600 摄像头 --DVP--> CEU ----> 图像预处理 ----> Ethos-U NPU ----> 检测/跟踪
        |              
        +--软件 I²C----> 寄存器配置

INMP441 --I²S--> 双采集缓冲区 --> PCM16 帧队列 --> 音乐分析/语音模型/云端语音

GT911 --I²C--┐
             ├--> LVGL UI --> SPI + DMAC --> 320×480 TFT
RTC/OSPI ----┘

光敏电阻 --ADC--> 滤波与亮度映射 --> GPT PWM --> MOS 管 --> 灯泡

主循环状态机 --> SCS 协议 --> SCI UART 半双工总线 --> 5×STS3215

ESP 网络协处理器 <--UART 帧协议--> 时间同步/天气/WebSocket 实时语音
```

## 硬件组成

| 模块 | 接口 | 用途 |
| --- | --- | --- |
| Renesas RA8P1 Evaluation Kit | 主控 | 外设调度、图形界面和端侧推理 |
| SCC8600 摄像头（工程内驱动命名为 SCC8660） | 软件 I²C + DVP/CEU | 寄存器配置与 RGB565 图像采集 |
| 320×480 TFT LCD | SPI + DMAC | LVGL 图形显示 |
| GT911 电容触摸屏 | I²C | 触摸坐标输入 |
| 5×STS3215 总线舵机 | SCI UART 半双工 | 灯体姿态和桌宠动作 |
| INMP441 麦克风 | SSI/I²S | 数字音频采集 |
| I²S 音频输出模块 | SSI/I²S | 闹钟和云端语音播放 |
| 光敏电阻模块 | ADC | 环境光采样 |
| 灯泡与 MOS 管驱动 | GPT PWM | 灯泡开关和亮度控制 |
| OSPI Flash | OSPI | 闹钟配置持久化 |
| RTC 后备电源 | VBATT | 断电计时 |
| ESP 网络协处理器 | UART | 联网、时间、天气和 WebSocket 传输 |

具体引脚、通道、中断优先级和外设参数以 [`configuration.xml`](configuration.xml) 及硬件原理图为准。

## 软件环境

- Renesas e² studio 2025-12
- Renesas Flexible Software Package 6.5.0
- GNU Arm Embedded Toolchain 13.2.1
- LVGL 8.3.8
- C / C++
- CMSIS-DSP、CMSIS-NN、TensorFlow Lite Micro、Ethos-U 驱动
- Edge Impulse Inferencing SDK
- 裸机协作式调度，无 RTOS

## 目录结构

```text
.
├─ configuration.xml        # FSP 外设、时钟、引脚和中断配置
├─ ra/                      # FSP、CMSIS、TFLM 和 Ethos-U 组件
├─ ra_cfg/                  # FSP 配置头文件
├─ ra_gen/                  # FSP 自动生成的外设实例
├─ script/                  # 链接脚本
├─ model/                   # 视觉模型源文件
├─ model-parameters/        # Edge Impulse 音频模型参数
├─ tflite-model/            # 音频模型部署文件
├─ edge-impulse-sdk/        # Edge Impulse 推理运行库
└─ src/
   ├─ applications/         # 主应用和各外设独立测试入口
   ├─ Ai/                   # 摄像头帧预处理、NPU 推理和检测后处理
   ├─ Alarm/                # 闹钟页面与提示音
   ├─ Audio/                # 音频输出仲裁和云端语音播放
   ├─ Chat/                 # 实时语音交互业务层
   ├─ Doubao/               # 豆包实时对话协议
   ├─ Esp/                  # ESP UART 链路、帧协议和时间同步
   ├─ Flash/                # OSPI Flash 与闹钟持久化
   ├─ Keyword/              # Edge Impulse 连续关键词推理
   ├─ Light_Module/         # 光敏 ADC 与灯泡 PWM
   ├─ Middlewares/          # LVGL 等中间件
   ├─ Music_Rhythm/         # 频谱起音与节拍跟踪
   ├─ Real_time/            # RTC 封装
   ├─ Screen/               # LCD、触摸和 LVGL 端口
   ├─ Servo/                # 台灯动作与桌宠行为状态机
   ├─ ServoLib/             # STS/SCS 总线舵机协议驱动
   ├─ UI/                   # LVGL 页面与交互逻辑
   ├─ Voice/                # I²S 音频采集和帧队列
   ├─ zf_device/            # 摄像头设备驱动
   └─ zf_driver/            # 软件 I²C 等底层驱动
```

## 构建与烧录

1. 安装 e² studio、RA FSP 6.5.0 和工程指定的 GNU Arm Embedded Toolchain。
2. 在 e² studio 中选择 `File > Import > Existing Projects into Workspace`。
3. 选择本仓库根目录并导入工程。
4. 打开 `configuration.xml`，确认目标板为 `board.ra8p1ek`，然后执行 `Generate Project Content`。
5. 检查摄像头、LCD、触摸、舵机、音频和网络协处理器的接线与供电。
6. 配置需要的云端凭据，然后构建 `Debug` 或 `Release`。
7. 使用 J-Link 下载程序，并通过 UART9 查看调试输出。


## 云端凭据配置

仓库不保存真实的豆包访问凭据。首次使用时，将：

```text
src/Doubao/doubao_realtime_config.example.h
```

复制为：

```text
src/Doubao/doubao_realtime_config.h
```

然后填写以下配置：

```c
#define DOUBAO_API_APP_ID       "YOUR_DOUBAO_APP_ID"
#define DOUBAO_API_ACCESS_KEY   "YOUR_DOUBAO_ACCESS_KEY"
#define DOUBAO_API_APP_KEY      "YOUR_DOUBAO_APP_KEY"
```

`doubao_realtime_config.h` 已加入 `.gitignore`。不要将真实密钥写入示例文件、提交记录、日志或截图中。ESP 网络协处理器的 Wi-Fi 参数需要在对应固件中单独配置。

## 应用入口与独立测试

[`src/hal_entry.c`](src/hal_entry.c) 使用编译期宏选择主应用或独立测试，任意时刻必须且只能启用一个入口：

```c
#define MIC_UART_EXPORT_TEST_ENABLE       (0U)
#define CAMERA_LCD_PREVIEW_TEST_ENABLE    (0U)
#define MAIN_APP_ENABLE                   (1U)
#define RTC_TEST_ENABLE                   (0U)
#define Flash_TEST_ENABLE                 (0U)
#define LED_BLINK_TEST_ENABLE             (0U)
#define MUSIC_TEST_ENABLE                 (0U)
#define BUZZER_TEST_ENABLE                (0U)
```

默认启用 `MAIN_APP_ENABLE`。调试单个模块时，将主应用设为 `0U`，再将对应测试设为 `1U`。

## 核心数据链路

### 摄像头到识别结果

```text
软件 I²C 初始化摄像头
    -> DVP 输出 RGB565
    -> CEU 写入帧缓冲区
    -> 复制稳定帧
    -> 缩放与 Letterbox
    -> RGB565 转 INT8 RGB
    -> Ethos-U 执行推理
    -> 置信度筛选与 NMS
    -> 输出类别、置信度和目标框
    -> 桌宠状态机执行跟踪动作
```

### 麦克风到音频消费者

```text
INMP441 I²S 数据
    -> 32 位双声道时隙
    -> 双缓冲交替采集
    -> 提取有效高位并转为 PCM16
    -> 选择有效声道
    -> 64 帧环形队列
    -> 音乐分析 / 关键词推理 / 云端语音上传
```

### 舵机通信

STS/SCS 数据包由双 `0xFF` 帧头、舵机 ID、长度、指令、参数和反码校验和组成。UART 回调逐字节写入 256 字节环形缓冲区，读取命令再从队列中解析状态包。同步写指令用于一次下发多个舵机的位置、速度和加速度。

## 注意事项

- 舵机和灯泡应使用满足负载要求的独立电源，并与 RA8P1 共地。
- RA8P1 GPIO/PWM 只输出控制信号，不能直接驱动 12 V 灯泡，必须通过合适的 MOS 管和电源回路。
- 摄像头并行信号线应尽量短，并结合 PCLK 分频检查信号完整性。
- RTC 掉电保持依赖 VBATT 供电和正确的板级电源隔离。
- 视觉推理、LCD 刷新、音频传输和舵机控制共享主循环时间，应避免在中断或状态机更新中加入长时间阻塞操作。
- 修改 FSP 配置后，需要重新检查自动生成实例名称是否仍与用户代码一致。

## 第三方组件与许可证

仓库包含 Renesas FSP/CMSIS、LVGL、Edge Impulse SDK、TensorFlow Lite Micro、Ethos-U 驱动和逐飞科技摄像头相关代码。第三方组件分别受其目录中的许可证和版权声明约束。

当前仓库根目录尚未提供统一的项目许可证。在补充明确许可证之前，项目自有代码不视为已授权任意复制、修改或商用。
