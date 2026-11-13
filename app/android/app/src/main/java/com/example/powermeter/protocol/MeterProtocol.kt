package com.example.powermeter.protocol

/**
 * 固件通信协议（与 Core/Src/main.c 中的实现一一对应）。
 *
 * 设备 → 手机（每 500ms 一轮）：
 *   CH2 V 23.759V
 *   CH2 I 2.20224A
 *   CH2 P 52.322W
 *   CH2 R 1850
 *   CFG K=0.012793 B=0.000000      （发 ? 或写入 K/B 后返回）
 *   CAL OFF=-10LSB                 （发 z 后返回）
 *   RAW S=340 O=-10 N=350          （发 r 后返回）
 *   VRAW R=1850 V=23.670V           （发 v 后返回）
 *
 * 手机 → 设备（ASCII，必须以 \n 结尾）：
 *   K0.013207  设置电压增益 K
 *   B-0.46646  设置电压截距 B
 *   ?          查询当前 K/B
 *   z          空载零点校准
 *   r          读分流寄存器原始值
 *   v          读电压 ADC 原始值
 *   1 / 0      输出开 / 关
 *   3 / 5      I²C 自检 / 地址扫描
 */
object MeterProtocol {

    /** 一帧完整测量（V/I/P/R 四行到齐后产出一条） */
    data class Sample(
        val voltage: Double,
        val current: Double,
        val power: Double,
        val adc: Int
    )

    /** 解析结果 */
    sealed interface Event {
        /** V/I/P/R 单行更新（用于实时显示，不等整帧） */
        data class Partial(
            val voltage: Double? = null,
            val current: Double? = null,
            val power: Double? = null,
            val adc: Int? = null
        ) : Event

        /** 四行到齐，得到一条完整样本 */
        data class Complete(val sample: Sample) : Event

        /** 当前 K/B（可带线路电阻 L） */
        data class Config(val k: Double, val b: Double, val lineOhm: Double? = null) : Event

        /** 零点校准结果 */
        data class Calibration(val offsetLsb: Int?) : Event

        /** 其它提示信息（RAW / VRAW / 错误帧） */
        data class Info(val text: String) : Event

        data object Unknown : Event
    }

    private val reV = Regex("""^CH2\s+V\s+(-?\d+(?:\.\d+)?)V""", RegexOption.IGNORE_CASE)
    private val reI = Regex("""^CH2\s+I\s+(-?\d+(?:\.\d+)?)A""", RegexOption.IGNORE_CASE)
    private val reP = Regex("""^CH2\s+P\s+(-?\d+(?:\.\d+)?)W""", RegexOption.IGNORE_CASE)
    private val reR = Regex("""^CH2\s+R\s+(\d+)""", RegexOption.IGNORE_CASE)
    private val reCfg = Regex(
        """^CFG\s+K=(-?\d+(?:\.\d+)?)\s+B=(-?\d+(?:\.\d+)?)(?:\s+L=(-?\d+(?:\.\d+)?))?""",
        RegexOption.IGNORE_CASE
    )
    private val reCal = Regex("""^CAL\s+OFF=(-?\d+)""", RegexOption.IGNORE_CASE)

    /**
     * 逐行解析。
     *
     * 状态机放在这里：只有 V/I/P/R 四行都到齐才产出 [Event.Complete]，
     * 上层拿到完整样本再落库/导表，避免半帧数据。
     */
    class Parser {

        private var v: Double? = null
        private var i: Double? = null
        private var p: Double? = null
        private var r: Int? = null

        fun reset() {
            v = null
            i = null
            p = null
            r = null
        }

        fun feed(line: String): Event {
            reV.find(line)?.let {
                v = it.groupValues[1].toDoubleOrNull()
                return Event.Partial(voltage = v)
            }

            reI.find(line)?.let {
                i = it.groupValues[1].toDoubleOrNull()
                return Event.Partial(current = i)
            }

            reP.find(line)?.let {
                p = it.groupValues[1].toDoubleOrNull()
                return Event.Partial(power = p)
            }

            reR.find(line)?.let {
                r = it.groupValues[1].toIntOrNull()
                val event = Event.Partial(adc = r)
                val pv = v
                val pi = i
                val pp = p
                val pr = r
                if (pv != null && pi != null && pp != null && pr != null) {
                    reset()
                    return Event.Complete(Sample(pv, pi, pp, pr))
                }
                return event
            }

            reCfg.find(line)?.let {
                val k = it.groupValues[1].toDoubleOrNull()
                val b = it.groupValues[2].toDoubleOrNull()
                val l = it.groupValues.getOrNull(3)?.toDoubleOrNull()
                if (k != null && b != null) return Event.Config(k, b, l)
            }

            reCal.find(line)?.let {
                return Event.Calibration(it.groupValues[1].toIntOrNull())
            }

            if (line.startsWith("RAW") || line.startsWith("VRAW")) {
                return Event.Info(line)
            }

            if (line.contains("ERR", ignoreCase = true)) {
                return Event.Info(line)
            }

            return Event.Unknown
        }
    }

    // ---------------- 命令构造 ----------------

    fun cmdSetK(value: String) = "K${value.trim()}\n"

    fun cmdSetB(value: String) = "B${value.trim()}\n"

    /** 设置线路等效电阻（Ω），用于把 OUT 端电压补偿到负载端 */
    fun cmdSetLine(value: String) = "L${value.trim()}\n"

    /** 查询当前 K/B，设备回 CFG */
    fun cmdQueryConfig() = "?\n"

    /** 空载零点校准 */
    fun cmdZeroCalibration() = "z\n"

    /** 读分流寄存器原始值 */
    fun cmdRawShunt() = "r\n"

    /** 读电压 ADC 原始值 */
    fun cmdRawVoltage() = "v\n"

    /** 输出开 */
    fun cmdOutputOn() = "1\n"

    /** 输出关 */
    fun cmdOutputOff() = "0\n"
}
