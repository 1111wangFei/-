package com.example.powermeter.ble

import java.util.UUID

/**
 * 蓝牙服务和特征值的 UUID。
 *
 * 本项目适配两类透明传输模块：
 *  1. HC-04 / HC-08 / CC2541 这类 BLE 串口模块，服务 0xFFE0，特征 0xFFE1（收发同一个）；
 *  2. Nordic UART (NUS)，服务 6E400001，RX 用于写、TX 用于通知。
 */
object BleUuids {

    val SERVICE_FFE0: UUID = UUID.fromString("0000ffe0-0000-1000-8000-00805f9b34fb")
    val CHAR_FFE1: UUID = UUID.fromString("0000ffe1-0000-1000-8000-00805f9b34fb")

    val SERVICE_NUS: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
    val CHAR_NUS_RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
    val CHAR_NUS_TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")

    /** 客户端特征配置描述符，打开通知要写它 */
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}
