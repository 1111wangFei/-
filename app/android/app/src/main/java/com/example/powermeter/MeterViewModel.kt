package com.example.powermeter

import android.app.Application
import android.content.Intent
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.powermeter.ble.BleManager
import com.example.powermeter.ble.BleState
import com.example.powermeter.ble.ScanItem
import com.example.powermeter.data.HistoryStore
import com.example.powermeter.protocol.MeterProtocol
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 实时值（对应四行数据） */
data class LiveValues(
    val voltage: Double = 0.0,
    val current: Double = 0.0,
    val power: Double = 0.0,
    val adc: Int = 0,
    val hasData: Boolean = false
)

/** 历史表里的一行 */
data class HistoryRow(
    val time: String,
    val voltage: Double,
    val current: Double,
    val power: Double,
    val adc: Int,
    val k: Double?,
    val b: Double?
)

/** 校准系数 */
data class Calibration(
    val k: Double = 0.0,
    val b: Double = 0.0,
    val lineOhm: Double = 0.0,
    val known: Boolean = false
)

/**
 * 把 BLE 传输层 + 协议解析 + 历史存储粘起来，界面只跟它打交道。
 */
class MeterViewModel(app: Application) : AndroidViewModel(app) {

    private val ble = BleManager(app)
    private val parser = MeterProtocol.Parser()
    private val history = HistoryStore(app)

    val state = ble.state
    val devices = ble.devices
    val errors = ble.errors

    private val _values = MutableStateFlow(LiveValues())
    val values = _values.asStateFlow()

    private val _calibration = MutableStateFlow(Calibration())
    val calibration = _calibration.asStateFlow()

    private val _rows = MutableStateFlow<List<HistoryRow>>(emptyList())
    val rows = _rows.asStateFlow()

    private val _message = MutableStateFlow("")
    val message = _message.asStateFlow()

    /** 实测帧率（帧/秒）：由最近若干个完整样本的时间戳算出，用来判断链路是否跟得上 */
    private val _fps = MutableStateFlow(0.0)
    val fps = _fps.asStateFlow()

    /** 最近 10 个完整帧的时间戳（毫秒） */
    private val sampleTimes = ArrayDeque<Long>()

    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    init {
        viewModelScope.launch {
            ble.lines.collect { line -> handleLine(line) }
        }
        viewModelScope.launch {
            ble.errors.collect { _message.value = it }
        }
        viewModelScope.launch {
            ble.state.collect { st ->
                // 连上之后主动问一次当前 K/B，界面就能显示设备里的值
                if (st == BleState.Connected) {
                    ble.send(MeterProtocol.cmdQueryConfig())
                }
            }
        }
    }

    private fun handleLine(line: String) {
        when (val event = parser.feed(line)) {
            is MeterProtocol.Event.Partial -> {
                _values.value = _values.value.copy(
                    voltage = event.voltage ?: _values.value.voltage,
                    current = event.current ?: _values.value.current,
                    power = event.power ?: _values.value.power,
                    adc = event.adc ?: _values.value.adc,
                    hasData = true
                )
            }

            is MeterProtocol.Event.Complete -> {
                val s = event.sample
                _values.value = LiveValues(s.voltage, s.current, s.power, s.adc, true)

                val k = if (_calibration.value.known) _calibration.value.k else null
                val b = if (_calibration.value.known) _calibration.value.b else null
                history.append(s, k, b)

                val row = HistoryRow(
                    time = timeFormat.format(Date()),
                    voltage = s.voltage,
                    current = s.current,
                    power = s.power,
                    adc = s.adc,
                    k = k,
                    b = b
                )
                _rows.value = (_rows.value + row).takeLast(MAX_ROWS)

                // 用最近 10 帧的时间戳算实测帧率
                val now = System.currentTimeMillis()
                sampleTimes.addLast(now)
                while (sampleTimes.size > 10) {
                    sampleTimes.removeFirst()
                }
                if (sampleTimes.size >= 2) {
                    val span = now - sampleTimes.first()
                    if (span > 0) {
                        _fps.value = (sampleTimes.size - 1) * 1000.0 / span
                    }
                }
            }

            is MeterProtocol.Event.Config -> {
                _calibration.value = Calibration(
                    k = event.k,
                    b = event.b,
                    lineOhm = event.lineOhm ?: _calibration.value.lineOhm,
                    known = true
                )
                val line = event.lineOhm?.let { "  L=$it" } ?: ""
                _message.value = "当前 K=${event.k}  B=${event.b}$line"
            }

            is MeterProtocol.Event.Calibration -> {
                _message.value = event.offsetLsb
                    ?.let { "零点校准完成：OFF=$it LSB" }
                    ?: "零点校准失败"
            }

            is MeterProtocol.Event.Info -> _message.value = event.text

            MeterProtocol.Event.Unknown -> Unit
        }
    }

    // ---------------- 供界面调用 ----------------

    fun startScan() = ble.startScan()

    fun stopScan() = ble.stopScan()

    fun connect(item: ScanItem) = ble.connect(item.address)

    fun disconnect() {
        parser.reset()
        ble.disconnect()
    }

    fun writeK(text: String) {
        if (text.isBlank()) return
        _message.value = if (ble.send(MeterProtocol.cmdSetK(text))) "已发送 K=$text" else "未连接"
    }

    fun writeB(text: String) {
        if (text.isBlank()) return
        _message.value = if (ble.send(MeterProtocol.cmdSetB(text))) "已发送 B=$text" else "未连接"
    }

    fun writeLine(text: String) {
        if (text.isBlank()) return
        _message.value = if (ble.send(MeterProtocol.cmdSetLine(text))) "已发送 L=$text" else "未连接"
    }

    fun queryConfig() {
        _message.value = if (ble.send(MeterProtocol.cmdQueryConfig())) "已查询 K/B" else "未连接"
    }

    fun zeroCalibration() {
        _message.value = if (ble.send(MeterProtocol.cmdZeroCalibration())) {
            "已在空载下校准零点"
        } else {
            "未连接"
        }
    }

    fun buildShareIntent(): Intent? = history.buildShareIntent()

    fun clearHistory() {
        history.clear()
        _rows.value = emptyList()
        _message.value = "历史数据已清空"
    }

    override fun onCleared() {
        ble.disconnect()
        super.onCleared()
    }

    companion object {
        /** 内存里保留的历史点上限：500ms 一点，3000 点约 25 分钟，足够画长趋势 */
        private const val MAX_ROWS = 3000
    }
}
