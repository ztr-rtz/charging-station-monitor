# 设备树节点清单（Device Tree Bring-up）

> v1.0 ｜ 2026-08-17 ｜ 对应总体框架 §6.1 ｜ 本文只定义设备树梳理与修改方案，不含驱动源码实现。
> 硬件基准：正点原子 i.MX6ULL MINI（Cortex-A7 @528MHz）+ 800×480 RGB LCD。
> **说明**：下文 pinmux/引脚以 i.MX6ULL 参考设计为准，最终以板级原理图与正点原子出厂 DTS 为准核对后再烧录。

---

## 1. 目的与意义

设备树（DT）是 Linux 下"描述硬件、不做逻辑"的声明式配置。本项目的驱动能力展示从设备树开始：

- 能读懂 i.MX6ULL 的 dtsi 分层结构（SoC 级 → 板级 → 用户改动）
- 能为每个外设配置 pinmux（引脚复用）与设备节点
- 能确认内核是否成功注册设备（验证：`/proc/device-tree`、`dmesg`、`/sys`）
- 为 nRF24L01 自研驱动、LED/按键驱动提供"设备树匹配"的入口

> 面试话术：设备树让内核与硬件描述解耦——换板不改代码、只换 dts；这也是驱动"即插即用"的基础。

---

## 2. i.MX6ULL 设备树体系

```
imx6ull.dtsi                      # SoC 级：所有外设控制器、中断、时钟（只读不改）
└─ imx6ull-14x14-evk.dts          # NXP 官方参考板（正点原子基于此裁剪）
   └─ imx6ull-14x14-evk-emmc.dts  # 正点原子 MINI 板（eMMC 版启动，在此基础上改）
```

修改原则：

| 原则 | 说明 |
|---|---|
| 只改板级 dts，不动 dtsi | SoC 级描述是所有板子共用的事实 |
| pinmux 集中改在 `iomuxc` 节点 | 每个外设定义自己的 `pinctrl_xxx` 子节点 |
| 用 `&外设` 引用追加 | `status = "okay"` 启用，不直接改原节点 |
| 改动最小化 | 只加本项目需要的节点，其余保持出厂配置 |

编译产物：`imx6ull-14x14-evk-emmc.dtb`，放到启动分区（uEnv.txt / bootargs 指定 dtb 名）。

---

## 3. 需要梳理的外设清单与节点映射

| 外设 | 内核设备 | 设备树节点 | 用途 |
|---|---|---|---|
| 调试串口 | `/dev/ttymxc0` | `&uart1` | 串口终端、日志输出（出厂已配好，确认即可） |
| RS485 串口 | `/dev/ttymxc3` | `&uart3` + 方向控制 GPIO | Modbus 主站有线链路 |
| SPI 控制器 | `spi0`（ecspi1） | `&ecspi1` + `nrf24l01@0` 子节点 | 无线链路（自研驱动挂载点） |
| LCD | `/dev/fb0` | `&lcdif` + `&pwm1`（背光） | LVGL 显示 800×480 |
| 触摸屏 | `input` 子系统 | 取决于触摸 IC（I2C 或 ADC） | UI 交互（阶段 2 确认型号） |
| LED | 自研 miscdevice | `leds` / 自定义 GPIO | 链路状态灯（阶段 2 驱动） |
| 按键 | `input` 子系统 | `gpio-keys` 节点 | 页面切换/告警确认（阶段 2 驱动） |
| RS485 方向脚 | GPIO 输出 | iomuxc pin 配置 | 半双工收发切换 |

---

## 4. pinmux 配置（iomuxc 节点）

本项目需要新配置的引脚组（其余沿用出厂）：

### 4.1 SPI1（ecspi1）—— nRF24L01 挂载

i.MX6ULL 的 ECSPI1 默认复用脚在 GPIO3 组：

```dts
&iomuxc {
    pinctrl-names = "default";

    pinctrl_ecspi1: ecspi1grp {
        fsl,pins = <
            MX6UL_PAD_GPIO3_IO28__ECSPI1_SCLK  0x10b0   /* SCLK  时钟 */
            MX6UL_PAD_GPIO3_IO29__ECSPI1_MOSI  0x10b0   /* MOSI  主出从入 */
            MX6UL_PAD_GPIO3_IO30__ECSPI1_MISO  0x10b0   /* MISO  主入从出 */
            /* 片选 CS：nRF24L01 的 CSN 用 GPIO 控制（更灵活） */
            MX6UL_PAD_GPIO3_IO31__GPIO3_IO31   0x10b0   /* CSN  片选（软控） */
        >;
    };

    /* nRF24L01 控制脚：CE 使能 + IRQ 中断 */
    pinctrl_nrf24_ctrl: nrf24ctrlgrp {
        fsl,pins = <
            MX6UL_PAD_GPIO3_IO12__GPIO3_IO12   0x400010b0   /* CE  GPIO 输出 */
            MX6UL_PAD_GPIO3_IO13__GPIO3_IO13   0x400010b8   /* IRQ GPIO 输入（下降沿） */
        >;
    };
};
```

> 说明：pad 值（如 0x10b0）是电气配置（上拉/速率/驱动强度），出厂 dts 里有完整宏定义表，复制参考格式即可。IRQ 脚必须配输入模式。

### 4.2 RS485 串口（uart3）+ 方向控制

```dts
&iomuxc {
    pinctrl_uart3: uart3grp {
        fsl,pins = <
            MX6UL_PAD_UART3_TX_DATA__UART3_DCE_TX  0x1b0b1  /* TXD */
            MX6UL_PAD_UART3_RX_DATA__UART3_DCE_RX  0x1b0b1  /* RXD */
            MX6UL_PAD_UART3_CTS_B__GPIO1_IO26      0x400010b0 /* DE/RE 方向控制（GPIO） */
        >;
    };
};

&uart3 {
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_uart3>;
    status = "okay";
};
```

> RS485 是半双工：发送时方向脚拉高（DE），接收时拉低（RE）。方向脚由驱动代码在发送前切换，设备树只需把它配置成 GPIO 输出。

### 4.3 LCD 800×480（确认项）

正点原子 MINI 出厂 dts 已带 LCD 节点（`&lcdif` + 显示参数），本阶段只需**核对**：

- 分辨率/时序：`display-timings` 中 800×480、pixel clock 33.3MHz、60Hz 刷新
- 像素格式：RGB666 / RGB888，与 LVGL `lv_color_format` 匹配
- 背光：`&pwm1` 或 GPIO 背光，确认能点亮
- `/dev/fb0` 存在且 `fbset` 显示 800×480

### 4.4 LED / 按键（阶段 2 预埋）

```dts
/* 按键走内核 gpio-keys（input 子系统），驱动零编写 */
gpio-keys {
    compatible = "gpio-keys";
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_gpio_keys>;
    status = "okay";

    key0 {
        label = "KEY0";
        linux,code = <KEY_F1>;          /* 页面切换 */
        gpios = <&gpio5 1 GPIO_ACTIVE_LOW>;
    };
};

/* LED 自研 miscdevice 驱动，匹配自定义 compatible */
myleed {
    compatible = "alientek,led";
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_led>;
    led-gpios = <&gpio5 3 GPIO_ACTIVE_LOW>;   /* 状态灯：链路在线/离线/告警三色语义 */
    status = "okay";
};
```

---

## 5. nRF24L01 设备节点（自研驱动挂载点）

```dts
&ecspi1 {
    fsl,spi-num-chipselects = <1>;
    cs-gpios = <&gpio3 31 GPIO_ACTIVE_LOW>;    /* CSN 由 GPIO 软控 */
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_ecspi1>;
    status = "okay";

    nrf24l01@0 {
        compatible = "alientek,nrf24l01";       /* 与驱动 spi_driver 匹配 */
        reg = <0>;
        spi-max-frequency = <10000000>;         /* 10MHz，nRF24L01 上限 */
        interrupt-parent = <&gpio3>;
        interrupts = <13 IRQ_TYPE_EDGE_FALLING>;/* IRQ 下降沿触发 */
        ce-gpios = <&gpio3 12 GPIO_ACTIVE_HIGH>;/* CE 使能脚 */
        status = "okay";
    };
};
```

关键点（面试）：

- `compatible` 是驱动与设备匹配的**唯一键**——内核通过它找到 `spi_driver` 的 `of_match_table`
- `interrupts` + `interrupt-parent` 把 IRQ 脚映射为内核中断号，驱动 `request_irq` 即可用
- 自定义属性（`ce-gpios`）用 `of_property_read` 系列 API 在 `probe` 里解析
- `spi-max-frequency` 直接决定传输速率，由驱动 spi_sync 时使用

---

## 6. 修改与验证流程

### 6.1 修改流程

1. 备份出厂 dts：`cp imx6ull-14x14-evk-emmc.dts imx6ull-14x14-evk-emmc.dts.bak`
2. 按 §4/§5 追加节点（用 `&外设{}` 引用，不删出厂配置）
3. 编译 dtb：内核源码目录 `make dtbs`（或独立编译 `dtc`）
4. 拷贝到启动分区（如 `/boot` 或 eMMC 启动分区），uEnv.txt 指定 dtb 名
5. 重启验证

### 6.2 验证清单（逐个外设确认）

| 检查项 | 命令 / 位置 | 预期结果 |
|---|---|---|
| dtb 是否加载 | `dmesg \| grep -i "machine\|dtb"` | 显示板级名 |
| 设备树内容 | `ls /proc/device-tree/` | 看到 `ecspi1`、`uart3`、`nrf24l01@0` |
| 串口注册 | `ls /dev/ttymxc*` | `ttymxc0`(调试) `ttymxc3`(RS485) |
| SPI 设备绑定 | `cat /sys/bus/spi/devices/spi0.0/uevent` | 出现 `OF_NAME=nrf24l01` |
| LCD 帧缓冲 | `fbset` | 800×480，色深正确 |
| 中断生效 | `cat /proc/interrupts \| grep nrf24` | 有 nrf24 中断计数 |
| GPIO 状态 | `cat /sys/kernel/debug/gpio` | 方向/电平正确 |

### 6.3 常见坑（面试可讲）

1. **引脚被占用不报错**——两个外设配了同一 pin，后加载者悄悄失效；用 `/sys/kernel/debug/gpio` 查占用
2. **IRQ 不触发**——多半是 pinmux 没配输入模式（0x400010b8 里含输入使能位）
3. **SPI 通信失败**——速率超 nRF24L01 上限（>10MHz）、或片选极性不对
4. **dts 语法错误编译不过**——`&外设` 引用必须存在于 dtsi，否则 dtc 报 unresolved

---

## 7. 简历与面试落点

**简历 bullet：**
> 完成 i.MX6ULL 板级设备树 bring-up：梳理 uart/spi/lcdif/gpio 外设节点，配置 ecspi1+nRF24L01 挂载与 GPIO 中断映射，产出板级移植文档；基于设备树机制为自研 SPI 字符驱动提供匹配入口。

**面试可讲的故事线：**
"拿到一块板子 → 看懂 dtsi 分层 → 为外设配 pinmux → 挂自研 nRF24L01 节点 → 通过 /sys、/proc/interrupts 验证注册与中断 → 踩过 GPIO 复用冲突的坑"

---

## 8. 遗留细化项

- [ ] 按正点原子 MINI 原理图核对每个 GPIO 编号与 pad 配置值（本文档为参考设计）
- [ ] 触摸屏型号确认后补 `touch` 节点（I2C 或 ADC 电阻触摸）
- [ ] LCD timing 参数核对（800×480 @60Hz，pixel clock 33.3MHz 附近）
