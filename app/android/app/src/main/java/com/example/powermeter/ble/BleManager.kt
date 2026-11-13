package com.example.powermeter.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.util.Log
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow

/** 扫描到的一台设备 */
data class ScanItem(val name: String, val address: String, val rssi: Int)

/** 连接状态机 */
enum class BleState { Idle, Scanning, Connecting, Connected }

/**
 * BLE 传输层：只负责「扫描 / 连接 / 订通知 / 发字节 / 收字节」，
 * 不关心协议内容——收到的原始文本按行抛给上层（[lines]）。
 *
 * 上层（ViewModel）拿到行文本后交给 [com.example.powermeter.protocol.MeterProtocol] 解析。
 */
@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    companion object {
        private const val TAG = "BleManager"
        private const val DEFAULT_MTU_PAYLOAD = 20
    }

    private val _state = MutableStateFlow(BleState.Idle)
    val state = _state.asStateFlow()

    private val _devices = MutableStateFlow<List<ScanItem>>(emptyList())
    val devices = _devices.asStateFlow()

    /** 收到的整行文本（已去掉 \r\n） */
    private val _lines = MutableSharedFlow<String>(extraBufferCapacity = 128)
    val lines = _lines.asSharedFlow()

    private val _errors = MutableSharedFlow<String>(extraBufferCapacity = 16)
    val errors = _errors.asSharedFlow()

    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var notifyChar: BluetoothGattCharacteristic? = null

    private val rxBuffer = StringBuilder()
    private var payloadSize = DEFAULT_MTU_PAYLOAD

    val isBluetoothEnabled: Boolean
        get() = adapter?.isEnabled == true

    // ---------------- 扫描 ----------------

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            val name = try {
                device.name
            } catch (e: SecurityException) {
                null
            } ?: return
            if (name.isBlank()) return

            val item = ScanItem(name, device.address, result.rssi)
            _devices.value = (_devices.value.filterNot { it.address == device.address } + item)
                .sortedByDescending { it.rssi }
        }

        override fun onScanFailed(errorCode: Int) {
            _errors.tryEmit("扫描失败，错误码 $errorCode")
            _state.value = BleState.Idle
        }
    }

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            _errors.tryEmit("这台手机不支持 BLE")
            return
        }
        if (!isBluetoothEnabled) {
            _errors.tryEmit("请先打开手机蓝牙")
            return
        }

        _devices.value = emptyList()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner.startScan(null, settings, scanCallback)
            _state.value = BleState.Scanning
        } catch (e: SecurityException) {
            _errors.tryEmit("缺少扫描权限")
        }
    }

    fun stopScan() {
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (e: SecurityException) {
            // 忽略
        }
        if (_state.value == BleState.Scanning) {
            _state.value = BleState.Idle
        }
    }

    // ---------------- 连接 ----------------

    fun connect(address: String) {
        stopScan()

        val device: BluetoothDevice = try {
            adapter?.getRemoteDevice(address)
        } catch (e: IllegalArgumentException) {
            null
        } ?: run {
            _errors.tryEmit("地址无效：$address")
            return
        }

        _state.value = BleState.Connecting
        gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(context, false, gattCallback)
        }
    }

    fun disconnect() {
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (e: Exception) {
            Log.w(TAG, "disconnect", e)
        }
        gatt = null
        writeChar = null
        notifyChar = null
        rxBuffer.setLength(0)
        _state.value = BleState.Idle
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                try {
                    g.discoverServices()
                } catch (e: SecurityException) {
                    _errors.tryEmit("缺少连接权限")
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                _state.value = BleState.Idle
                writeChar = null
                notifyChar = null
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                _errors.tryEmit("服务发现失败：$status")
                return
            }

            val pair = pickCharacteristics(g)
            if (pair == null) {
                _errors.tryEmit("没有找到可用的收发特征值")
                return
            }

            notifyChar = pair.first
            writeChar = pair.second

            try {
                g.setCharacteristicNotification(notifyChar, true)
                val descriptor = notifyChar?.getDescriptor(BleUuids.CCCD)
                if (descriptor == null) {
                    // 部分模块没有 CCCD，直接认为已就绪
                    _state.value = BleState.Connected
                } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(descriptor)
                }
            } catch (e: SecurityException) {
                _errors.tryEmit("打开通知失败：缺少权限")
            }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (descriptor.uuid == BleUuids.CCCD && status == BluetoothGatt.GATT_SUCCESS) {
                _state.value = BleState.Connected
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                payloadSize = (mtu - 3).coerceAtLeast(DEFAULT_MTU_PAYLOAD)
            }
        }

        // Android 13+ 走这个重载
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleRx(value)
        }

        // Android 12 及以下走这个重载
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: return
            handleRx(value)
        }
    }

    /** 优先级：FFE0/FFE1 → Nordic UART → 任意「可通知 + 可写」的特征 */
    private fun pickCharacteristics(g: BluetoothGatt): Pair<BluetoothGattCharacteristic, BluetoothGattCharacteristic>? {
        val services = g.services ?: return null

        services.firstOrNull { it.uuid == BleUuids.SERVICE_FFE0 }?.let { svc ->
            val chars = svc.characteristics ?: emptyList()
            val notify = chars.firstOrNull { it.isNotifiable() }
            val write = chars.firstOrNull { it.isWritable() } ?: notify
            if (notify != null && write != null) return notify to write
        }

        services.firstOrNull { it.uuid == BleUuids.SERVICE_NUS }?.let { svc ->
            val tx = svc.getCharacteristic(BleUuids.CHAR_NUS_TX)
            val rx = svc.getCharacteristic(BleUuids.CHAR_NUS_RX)
            if (tx != null && rx != null) return tx to rx
        }

        for (svc in services) {
            val chars = svc.characteristics ?: continue
            val notify = chars.firstOrNull { it.isNotifiable() } ?: continue
            val write = chars.firstOrNull { it.isWritable() } ?: continue
            return notify to write
        }

        return null
    }

    private fun BluetoothGattCharacteristic.isNotifiable(): Boolean =
        properties and (BluetoothGattCharacteristic.PROPERTY_NOTIFY or
                BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0

    private fun BluetoothGattCharacteristic.isWritable(): Boolean =
        properties and (BluetoothGattCharacteristic.PROPERTY_WRITE or
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0

    // ---------------- 收发 ----------------

    /** 把一段文本按 MTU 切片后写入（命令都很短，通常是单片） */
    fun send(text: String): Boolean {
        val g = gatt ?: return false
        val ch = writeChar ?: return false
        val bytes = text.toByteArray(Charsets.UTF_8)

        try {
            var offset = 0
            while (offset < bytes.size) {
                val end = (offset + payloadSize).coerceAtMost(bytes.size)
                val chunk = bytes.copyOfRange(offset, end)
                writeChunk(g, ch, chunk)
                offset = end
            }
            return true
        } catch (e: SecurityException) {
            _errors.tryEmit("发送失败：缺少权限")
            return false
        } catch (e: Exception) {
            _errors.tryEmit("发送失败：${e.message}")
            return false
        }
    }

    private fun writeChunk(
        g: BluetoothGatt,
        ch: BluetoothGattCharacteristic,
        chunk: ByteArray
    ) {
        val noResponse =
            ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val type = if (noResponse) {
                BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            } else {
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            }
            g.writeCharacteristic(ch, chunk, type)
        } else {
            @Suppress("DEPRECATION")
            run {
                ch.writeType = if (noResponse) {
                    BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                } else {
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                }
                ch.value = chunk
                g.writeCharacteristic(ch)
            }
        }
    }

    /** 收到的字节追加进行缓冲，凑满一行就往上抛 */
    private fun handleRx(value: ByteArray) {
        if (value.isEmpty()) return

        rxBuffer.append(String(value, Charsets.UTF_8))

        // 防止异常数据把缓冲撑爆
        if (rxBuffer.length > 2048) {
            rxBuffer.setLength(0)
            return
        }

        var index = rxBuffer.indexOf("\n")
        while (index >= 0) {
            val line = rxBuffer.substring(0, index).replace("\r", "").trim()
            rxBuffer.delete(0, index + 1)
            if (line.isNotEmpty()) {
                _lines.tryEmit(line)
            }
            index = rxBuffer.indexOf("\n")
        }
    }
}
