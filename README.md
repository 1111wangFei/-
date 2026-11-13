# 自制功率计（STM32G030 + INA226 + 手机 App）

一块用 STM32G030F6P6 做的直流功率计：电流由 INA226（I²C1，2mΩ 分流）测量，
电压由片内 ADC1（PA0，15k/1k 分压）测量，串口/蓝牙上报，手机 App 实时显示与记录。

## 目录

```
Core/                     固件（STM32G030，CubeMX 生成 + 用户代码）
Drivers/                  STM32 HAL / CMSIS
app/android/              手机 App（Kotlin + Jetpack Compose）
app/dist/                 打好的 APK（不提交，见 Releases）
.github/workflows/        GitHub Actions：自动构建 APK
Power_Test.ioc            CubeMX 工程
```

## 固件

- 电流：INA226 寄存器 0x01（分流电压），`I = (raw + offset) × 2.5µV / R_shunt`
- 电压：ADC1 PA0，`V = K × ADC + B`，48 点滑动平均
- 串口协议（9600 8N1，每 200ms 一帧）：

```
CH2 V 23.759V
CH2 I 2.20224A
CH2 P 52.322W
CH2 R 1850
```

- 支持的命令：`K<值>` `B<值>` `L<值>` `?` `z` `r` `v` `1` `0` `3` `5`

## 手机 App

- 实时显示 U / I / P / ADC，带帧率和趋势图
- 在线修改 K / B / L（线路压降补偿）
- 历史数据存 CSV，一键导出分享

构建：

```bash
cd app/android
./gradlew assembleDebug        # 调试包
./gradlew assembleRelease      # 发布包（需 keystore.properties）
```

## 量程与精度

| 量 | 量程 | 分辨率 | 实测误差 |
|---|---|---|---|
| 电压 | 0 ~ 36V（INA226 共模上限） | 13mV | ≤0.1%（27V 处） |
| 电流 | ±40A（分流功率限制约 22~38A） | 1.47mA | ≤0.2% |
| 功率 | 由 V×I 计算 | — | — |
