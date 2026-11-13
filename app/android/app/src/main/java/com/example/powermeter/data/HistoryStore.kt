package com.example.powermeter.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import com.example.powermeter.protocol.MeterProtocol
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 历史数据存储：直接追加 CSV 到应用私有目录，导出时用 FileProvider 分享出去。
 *
 * 这么做的原因：
 *  - 不需要数据库依赖，写入就是一次 append，掉电最多丢最后一行；
 *  - 导出的文件本身就是 CSV，手机端可以直接发微信/邮件/网盘。
 */
class HistoryStore(context: Context) {

    private val appContext = context.applicationContext
    private val file = File(appContext.filesDir, "power_history.csv")
    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    init {
        if (!file.exists()) {
            file.writeText(HEADER + "\n")
        }
    }

    /** 追加一条样本；K/B 写当时的校准值，便于事后复算 */
    fun append(sample: MeterProtocol.Sample, k: Double?, b: Double?) {
        val line = buildString {
            append(timeFormat.format(Date()))
            append(',')
            append(format(sample.voltage, 3))
            append(',')
            append(format(sample.current, 5))
            append(',')
            append(format(sample.power, 3))
            append(',')
            append(sample.adc)
            append(',')
            append(k?.let { format(it, 6) } ?: "")
            append(',')
            append(b?.let { format(it, 6) } ?: "")
            append('\n')
        }

        runCatching { file.appendText(line) }
    }

    fun clear() {
        runCatching { file.writeText(HEADER + "\n") }
    }

    fun sizeBytes(): Long = if (file.exists()) file.length() else 0L

    /** 生成分享 Intent；文件不存在时返回 null */
    fun buildShareIntent(): Intent? {
        if (!file.exists() || file.length() <= HEADER.length + 1) return null

        val uri: Uri = FileProvider.getUriForFile(
            appContext,
            "${appContext.packageName}.fileprovider",
            file
        )

        return Intent(Intent.ACTION_SEND).apply {
            type = "text/csv"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, "功率计历史数据")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    private fun format(value: Double, digits: Int): String =
        String.format(Locale.US, "%.${digits}f", value)

    companion object {
        private const val HEADER = "time,V,I,P,ADC,K,B"
    }
}
