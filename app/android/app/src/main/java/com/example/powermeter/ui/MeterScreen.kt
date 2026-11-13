package com.example.powermeter.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Divider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.powermeter.Calibration
import com.example.powermeter.HistoryRow
import com.example.powermeter.LiveValues
import com.example.powermeter.MeterViewModel
import com.example.powermeter.ble.BleState
import java.util.Locale

private val Bg = Color(0xFF0F1115)
private val CardBg = Color(0xFF171A21)
private val Line = Color(0xFF242A35)
private val Dim = Color(0xFF8B96A8)
private val VColor = Color(0xFF4FC3F7)
private val IColor = Color(0xFFFFB74D)
private val PColor = Color(0xFF81C784)
private val AColor = Color(0xFFBA68C8)

@Composable
fun MeterScreen(
    vm: MeterViewModel,
    onExport: () -> Unit,
    onRequestPermissions: () -> Unit
) {
    val state by vm.state.collectAsState()
    val devices by vm.devices.collectAsState()
    val values by vm.values.collectAsState()
    val calibration by vm.calibration.collectAsState()
    val rows by vm.rows.collectAsState()
    val message by vm.message.collectAsState()
    val fps by vm.fps.collectAsState()

    var showScanDialog by remember { mutableStateOf(false) }
    var kText by remember { mutableStateOf("") }
    var bText by remember { mutableStateOf("") }
    var lText by remember { mutableStateOf("") }

    // 设备回报 K/B/L 后回填到输入框
    if (calibration.known) {
        if (kText.isEmpty()) kText = calibration.k.toString()
        if (bText.isEmpty()) bText = calibration.b.toString()
        if (lText.isEmpty()) lText = calibration.lineOhm.toString()
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Bg)
            .verticalScroll(rememberScrollState())
            .padding(14.dp)
    ) {
        Header(
            state = state,
            fps = fps,
            onConnectClick = {
                when (state) {
                    BleState.Idle -> {
                        onRequestPermissions()
                        vm.startScan()
                        showScanDialog = true
                    }

                    BleState.Scanning -> vm.stopScan()
                    BleState.Connecting -> Unit
                    BleState.Connected -> vm.disconnect()
                }
            }
        )

        Spacer(Modifier.height(12.dp))

        ValuesCard(values)

        Spacer(Modifier.height(12.dp))

        TrendCard(rows)

        Spacer(Modifier.height(12.dp))

        CalibrationCard(
            calibration = calibration,
            kText = kText,
            bText = bText,
            lText = lText,
            onKChange = { kText = it },
            onBChange = { bText = it },
            onLChange = { lText = it },
            onWriteK = { vm.writeK(kText) },
            onWriteB = { vm.writeB(bText) },
            onWriteL = { vm.writeLine(lText) },
            onQuery = vm::queryConfig,
            onZero = vm::zeroCalibration
        )

        Spacer(Modifier.height(12.dp))

        ItemsAround(message)

        Spacer(Modifier.height(12.dp))

        HistoryCard(rows = rows, onExport = onExport, onClear = vm::clearHistory)
    }

    if (showScanDialog) {
        ScanDialog(
            devices = devices,
            scanning = state == BleState.Scanning,
            onSelect = {
                vm.connect(it)
                showScanDialog = false
            },
            onDismiss = {
                vm.stopScan()
                showScanDialog = false
            }
        )
    }
}

@Composable
private fun Header(state: BleState, fps: Double, onConnectClick: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .width(10.dp)
                .height(10.dp)
                .background(
                    color = if (state == BleState.Connected) Color(0xFF66BB6A) else Color(0xFF555B66),
                    shape = RoundedCornerShape(50)
                )
        )
        Spacer(Modifier.width(8.dp))
        Text(
            text = if (state == BleState.Connected && fps > 0.0) {
                "${state.text()}  ·  ${format(fps, 1)} 帧/秒"
            } else {
                state.text()
            },
            color = Dim,
            fontSize = 13.sp,
            modifier = Modifier.weight(1f)
        )
        Button(onClick = onConnectClick) {
            Text(
                when (state) {
                    BleState.Idle -> "扫描连接"
                    BleState.Scanning -> "停止扫描"
                    BleState.Connecting -> "连接中…"
                    BleState.Connected -> "断开"
                }
            )
        }
    }
}

@Composable
private fun ValuesCard(values: LiveValues) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(Modifier.fillMaxWidth()) {
                ValueBox("电压 U", "V", format(values.voltage, 3), VColor, Modifier.weight(1f))
                Spacer(Modifier.width(10.dp))
                ValueBox("电流 I", "A", format(values.current, 5), IColor, Modifier.weight(1f))
            }
            Spacer(Modifier.height(10.dp))
            Row(Modifier.fillMaxWidth()) {
                ValueBox("功率 P", "W", format(values.power, 3), PColor, Modifier.weight(1f))
                Spacer(Modifier.width(10.dp))
                ValueBox("ADC", "", values.adc.toString(), AColor, Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun ValueBox(
    label: String,
    unit: String,
    value: String,
    color: Color,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .background(Color(0xFF12151B), RoundedCornerShape(12.dp))
            .padding(12.dp)
    ) {
        Text(label, color = Dim, fontSize = 12.sp)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                text = value,
                color = color,
                fontSize = 26.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                maxLines = 1
            )
            if (unit.isNotEmpty()) {
                Spacer(Modifier.width(4.dp))
                Text(unit, color = Dim, fontSize = 13.sp, modifier = Modifier.padding(bottom = 4.dp))
            }
        }
    }
}

@Composable
private fun CalibrationCard(
    calibration: Calibration,
    kText: String,
    bText: String,
    lText: String,
    onKChange: (String) -> Unit,
    onBChange: (String) -> Unit,
    onLChange: (String) -> Unit,
    onWriteK: () -> Unit,
    onWriteB: () -> Unit,
    onWriteL: () -> Unit,
    onQuery: () -> Unit,
    onZero: () -> Unit
) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(Modifier.padding(14.dp)) {
            Text("校准参数 K / B / L", color = Dim, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(10.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = kText,
                    onValueChange = onKChange,
                    label = { Text("K（V/LSB）") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.weight(1f)
                )
                Spacer(Modifier.width(8.dp))
                Button(onClick = onWriteK) { Text("写入 K") }
            }

            Spacer(Modifier.height(8.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = bText,
                    onValueChange = onBChange,
                    label = { Text("B（V）") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.weight(1f)
                )
                Spacer(Modifier.width(8.dp))
                Button(onClick = onWriteB) { Text("写入 B") }
            }

            Spacer(Modifier.height(8.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = lText,
                    onValueChange = onLChange,
                    label = { Text("L 线路电阻（Ω）") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.weight(1f)
                )
                Spacer(Modifier.width(8.dp))
                Button(onClick = onWriteL) { Text("写入 L") }
            }

            Spacer(Modifier.height(10.dp))

            Row {
                OutlinedButton(onClick = onQuery) { Text("读取参数") }
                Spacer(Modifier.width(8.dp))
                OutlinedButton(onClick = onZero) { Text("空载校零") }
            }

            if (calibration.known) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "设备当前：K=${calibration.k}  B=${calibration.b}  L=${calibration.lineOhm}",
                    color = Dim,
                    fontSize = 12.sp
                )
            }
        }
    }
}

@Composable
private fun ItemsAround(message: String) {
    if (message.isBlank()) return

    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Text(
            text = message,
            color = Dim,
            fontSize = 12.sp,
            modifier = Modifier.padding(14.dp)
        )
    }
}

@Composable
private fun HistoryCard(rows: List<HistoryRow>, onExport: () -> Unit, onClear: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "历史数据（${rows.size} 条）",
                    color = Dim,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                OutlinedButton(onClick = onClear) { Text("清空") }
                Spacer(Modifier.width(8.dp))
                Button(onClick = onExport) { Text("导出 CSV") }
            }

            Spacer(Modifier.height(10.dp))

            Divider(color = Line)

            Row(Modifier.padding(vertical = 6.dp)) {
                Cell("时间", 1.1f)
                Cell("U/V", 1.0f)
                Cell("I/A", 1.4f)
                Cell("P/W", 1.1f)
                Cell("ADC", 0.9f)
            }

            Divider(color = Line)

            if (rows.isEmpty()) {
                Text(
                    "还没有数据，连接后每 500ms 记录一条",
                    color = Dim,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(vertical = 12.dp)
                )
            } else {
                Column {
                    rows.takeLast(40).reversed().forEach { row ->
                        Row(Modifier.padding(vertical = 5.dp)) {
                            Cell(row.time, 1.1f)
                            Cell(format(row.voltage, 3), 1.0f)
                            Cell(format(row.current, 5), 1.4f)
                            Cell(format(row.power, 3), 1.1f)
                            Cell(row.adc.toString(), 0.9f)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun androidx.compose.foundation.layout.RowScope.Cell(text: String, weight: Float) {
    Text(
        text = text,
        color = Dim,
        fontSize = 11.sp,
        fontFamily = FontFamily.Monospace,
        textAlign = TextAlign.End,
        maxLines = 1,
        modifier = Modifier
            .weight(weight)
            .padding(end = 4.dp)
    )
}

@Composable
private fun ScanDialog(
    devices: List<com.example.powermeter.ble.ScanItem>,
    scanning: Boolean,
    onSelect: (com.example.powermeter.ble.ScanItem) -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (scanning) "搜索中…" else "选择设备") },
        text = {
            if (devices.isEmpty()) {
                Text("没有搜到设备\n确认模块已上电、手机蓝牙已打开", color = Dim, fontSize = 13.sp)
            } else {
                LazyColumn(contentPadding = PaddingValues(vertical = 4.dp)) {
                    items(devices) { item ->
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 10.dp)
                                .background(Color(0xFF12151B), RoundedCornerShape(10.dp))
                                .padding(12.dp)
                        ) {
                            Text(item.name, color = Color.White, fontSize = 14.sp)
                            Text(
                                "${item.address}   RSSI ${item.rssi}",
                                color = Dim,
                                fontSize = 11.sp
                            )
                            Spacer(Modifier.height(6.dp))
                            TextButton(onClick = { onSelect(item) }) { Text("连接") }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("关闭") }
        }
    )
}

@Composable
private fun TrendCard(rows: List<HistoryRow>) {
    var window by remember { mutableStateOf(300) }

    Card(
        colors = CardDefaults.cardColors(containerColor = CardBg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "趋势",
                    color = Dim,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                WindowChip("60", window == 60) { window = 60 }
                WindowChip("300", window == 300) { window = 300 }
                WindowChip("全部", window == 0) { window = 0 }
            }

            Spacer(Modifier.height(8.dp))

            val data = remember(rows, window) {
                if (window == 0) rows else rows.takeLast(window)
            }

            if (data.size < 2) {
                Text(
                    "至少 2 个点才能画线（当前 ${data.size} 个，每 500ms 一个点）",
                    color = Dim,
                    fontSize = 12.sp
                )
            } else {
                ChartCanvas(data)
                Spacer(Modifier.height(8.dp))
                Row {
                    Text(
                        "U ${format(data.minOf { it.voltage }, 2)} ~ ${format(data.maxOf { it.voltage }, 2)} V",
                        color = VColor,
                        fontSize = 11.sp,
                        modifier = Modifier.weight(1f)
                    )
                    Text(
                        "I ${format(data.minOf { it.current }, 3)} ~ ${format(data.maxOf { it.current }, 3)} A",
                        color = IColor,
                        fontSize = 11.sp,
                        textAlign = TextAlign.End,
                        modifier = Modifier.weight(1f)
                    )
                }
            }
        }
    }
}

@Composable
private fun WindowChip(text: String, selected: Boolean, onClick: () -> Unit) {
    TextButton(onClick = onClick) {
        Text(
            text = text,
            color = if (selected) Color.White else Dim,
            fontSize = 12.sp,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal
        )
    }
}

@Composable
private fun ChartCanvas(data: List<HistoryRow>) {
    val vScale = maxOf(0.001, data.maxOf { kotlin.math.abs(it.voltage) }) * 1.1
    val iScale = maxOf(0.001, data.maxOf { kotlin.math.abs(it.current) }) * 1.1

    Canvas(
        modifier = Modifier
            .fillMaxWidth()
            .height(150.dp)
    ) {
        val w = size.width
        val h = size.height
        val pad = 2f

        for (g in 0..4) {
            val y = pad + g * (h - 2 * pad) / 4f
            drawLine(Line, Offset(0f, y), Offset(w, y), strokeWidth = 1f)
        }

        fun build(selector: (HistoryRow) -> Double, scale: Double): Path {
            val path = Path()
            val last = (data.size - 1).coerceAtLeast(1)
            val usable = h - 2 * pad
            data.forEachIndexed { index, row ->
                val x = index.toFloat() * w / last
                // 归一化到 0~1，再乘可用高度：全部用 Float，Canvas 只吃 Float
                val ratio = (selector(row) / scale).toFloat()
                val y = (h - pad - ratio * usable).coerceIn(pad, h - pad)
                if (index == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            return path
        }

        drawPath(build({ it.voltage }, vScale), VColor, style = Stroke(width = 3f))
        drawPath(build({ it.current }, iScale), IColor, style = Stroke(width = 3f))
    }
}

private fun BleState.text(): String = when (this) {
    BleState.Idle -> "未连接"
    BleState.Scanning -> "正在扫描…"
    BleState.Connecting -> "正在连接…"
    BleState.Connected -> "已连接"
}

private fun format(value: Double, digits: Int): String =
    String.format(Locale.US, "%.${digits}f", value)
