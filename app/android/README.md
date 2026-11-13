# 功率计 BLE 手机 App（Android / Kotlin + Compose）

配套固件：本仓库 `Core/Src/main.c`（USART2 + BLE 透传模块）。

> ⚠️ 本目录只包含源码。生成 APK 需要 Android Studio（JDK 17 + Android SDK 34）。

## 1. 目录结构

```
app/android/
├─ settings.gradle.kts
├─ build.gradle.kts
├─ gradle.properties
└─ app/
   ├─ build.gradle.kts
   └─ src/main/
      ├─ AndroidManifest.xml
      ├─ res/values/{strings,themes}.xml
      ├─ res/xml/file_paths.xml
      └─ java/com/example/powermeter/
         ├─ MainActivity.kt          界面入口 + 权限申请
         ├─ MeterViewModel.kt        把 BLE / 协议 / 存储串起来
         ├─ ble/
         │  ├─ BleManager.kt         传输层：扫描、连接、订通知、收发字节
         │  └─ BleUuids.kt           服务/特征 UUID
         ├─ protocol/
         │  └─ MeterProtocol.kt      协议层：行解析 + 命令构造（含状态机）
         ├─ data/
         │  └─ HistoryStore.kt       历史数据：CSV 落盘 + 分享导出
         └─ ui/
            ├─ MeterScreen.kt        界面：数值、趋势图、校准、历史
            └─ Theme.kt              深色主题
```

分层关系：

```
Compose UI  →  MeterViewModel  →  ┬ BleManager（字节流）
                                  ├ MeterProtocol.Parser（行 → 事件）
                                  └ HistoryStore（CSV）
```

UI 不碰 BLE，BLE 不解析协议，协议不落盘——每层只做一件事。

## 2. 构建

1. Android Studio（Hedgehog 或更新）→ Open → 选择 `app/android` 目录；
2. Gradle 同步（首次会下载 AGP 8.5 / Kotlin 1.9 / Compose BOM 2024.06）；
3. 手机开启开发者模式 → USB 调试 → Run。

命令行方式（需本机有 JDK 17 + Android SDK）：

```bash
cd app/android
./gradlew :app:assembleDebug
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

## 3. 通信协议

设备 → 手机（每 500ms 一轮，四行）：

```
CH2 V 23.759V
CH2 I 2.20224A
CH2 P 52.322W
CH2 R 1850
CFG K=0.012793 B=0.000000     ← 发 ? 或写入 K/B 后返回
CAL OFF=-10LSB                ← 发 z 后返回
RAW S=340 O=-10 N=350         ← 发 r 后返回
VRAW R=1850 V=23.670V         ← 发 v 后返回
```

手机 → 设备（ASCII，必须 `\n` 结尾）：

| 命令 | 作用 |
|---|---|
| `K0.013207` | 设置电压增益 K |
| `B-0.46646` | 设置电压截距 B |
| `?` | 查询当前 K/B |
| `z` | 空载零点校准 |
| `r` / `v` | 读分流 / 读电压 ADC 原始值 |
| `1` / `0` | 输出开 / 关 |

## 4. 功能

| 功能 | 说明 |
|---|---|
| 实时显示 | U / I / P / ADC，四行到齐才算一帧 |
| 趋势图 | 60 / 300 / 全部 三种窗口，V 与 I 双曲线，自动量程 |
| 历史缓冲 | 内存 3000 点（500ms 一点 ≈ 25 分钟），表格显示最近 40 条 |
| K/B | 连接后自动查询；输入框改完点写入即下发，设备回 `CFG` 刷新 |
| 空载校零 | 一键发 `z` |
| 导出 | CSV 落盘到私有目录，导出走系统分享（微信/邮件/网盘） |

CSV 表头：`time,V,I,P,ADC,K,B`。

## 5. 限制

- **K/B 不持久化**：固件里只存 RAM，断电后回到 `0.012793 / 0`，App 每次连接都会重新查询。
- **CSV 只追加不清空**（除手动清空），长期使用建议定期导出后清空。
- BLE 兼容：优先 `FFE0/FFE1`，其次 Nordic UART，最后自动找「可通知+可写」的特征。
