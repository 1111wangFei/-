/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <stddef.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
//STM32 HAL 的接口参数需要左移一位。
/* 测量分工（依据 PCB 原理图 ProPrj_自制功率计_2026-11-12）：
 *   电流：U9 = INA226AIDGSR，挂 I2C1（PB8=SCL / PB9=SDA，4.7k 上拉），只读分流电压寄存器 0x01
 *         分流电阻 R2 = 2mΩ，跨接在 OUT 与 POWER- 之间；VIN+ / VBUS 接 OUT
 *   电压：STM32 片内 ADC1 的 PA0(IN0)，测 OUT 母线，R14=15kΩ 串入 / R17=1kΩ 下拉到地
 * 板上 U9 的 A1、A0 都接 GND → 7 位地址应为 0x40；不同贴装可能是 0x40~0x4F，
 * 因此 INA226_Init 会在这个范围里按厂商 ID 自动认出实际地址。
 * INA226 地址 = 0x40 + 4*(A1档位) + (A0档位)，档位可选 GND / VS+ / SDA / SCL。 */
#define INA226_ADDR7       0x40U
#define INA226_ADDR_RT     ((uint16_t)(meter_ina_addr7 << 1)) // 传给 HAL 的 8 位地址

#define INA226_REG_CONFIG       0x00U  // 配置寄存器：设置转换模式、转换时间和平均次数
#define INA226_REG_SHUNT        0x01U  // 分流电压寄存器：有符号数，每位代表 2.5μV
#define INA226_REG_BUS          0x02U  // 总线电压寄存器：本工程不再使用，电压改由 ADC1 采样
#define INA226_REG_MANUFACTURER 0xFEU  // 厂商 ID 寄存器：TI 的标识值为 0x5449

/* 只测电流：AVG=64 次平均、分流电压每次 1.1ms、只开分流通道连续转换（MODE=101）
   = 0x0725，一组约 70.4ms（远快于 500ms 上报周期）。
   原来用 AVG=4 / 140μs（0x0205）是最快的设置，零载噪声达 ±3 LSB（±4.4mA），
   现在平均次数×16、转换时间×8，噪声约降到 ±1 LSB（±1.5mA）。 */
#define INA226_Measure          0x0725

/* 采样电阻等效阻值，单位 Ω。
   标定（2026-11-12）：负载实测 0.500A 时固件原读 0.4130A，且空载零点约 -10 LSB，
   反推 INA226 实际看到的等效阻值 = (330.4+10) × 2.5µV / 0.5A ≈ 1.70mΩ（丝印 R2 是 2mΩ）。
   二次微调：0.5A 点 -0.23%、2.2A 点 -0.19%，按平均比例 0.9979 再收一点到 1.696mΩ。 */
#define SHUNT_RESISTANCE_OHM    0.001696f

/* 电压采样：ADC1 的 PA0(IN0)，R14=15kΩ 串入、R17=1kΩ 下拉到地，理论分压比 16
   引脚电压 ×16 即被测电压；3.3V 参考下满量程约 52.8V
   历史版本（改下面 K/B 或切回分压比公式即可）：
     版本 1          分压比 16.0        —— 纯理论值
     版本 2          分压比 15.945     —— 按显示 24.026 / 表测 23.944 单点微调
     版本 3（当前）  K = 0.012793，B = 0            ← K/B 第 1 版（等效分压比 15.875）
     版本 4          K = 0.013207，B = -0.46646    （用户多点拟合）
     版本 5          K = 0.013072，B = -0.0562     （28V 日志三点拟合） */
#define ADC_VREF_VOLT           3.3f
#define ADC_FULL_SCALE          4095.0f
#define VOLTAGE_DIVIDER_RATIO   16.0f   /* 仅作理论参考，当前换算走下面的 K/B */

/* 电压线性校准：V = K × ADC原始值 + B（等同参考工程里的 y = kx + b）
   运行期可通过串口/蓝牙改写：发 "K0.013050\n" 设 K，"B0\n" 设 B，"?" 查询当前值
   只存 RAM，掉电回到下面的默认值

   标定记录（2026-11-13，空载，负载仪电压 27.027V ↔ App 显示 26.494V → ADC≈2071）：
     K = 27.027 / 2071 = 0.013050
   说明：之前用"24V/23.659V ↔ ADC 1849"算出的 K=0.012793 与本次相差约 2%，
   两组参考电压不可能同时成立（ADC 本身线性度远好于 2%），本次以空载负载仪读数为准。 */
#define ADC_VOLT_K_DEFAULT      0.013050f
#define ADC_VOLT_B_DEFAULT      0.0f

/* 线路压降的分段线性补偿：ΔV = K × I + B，按电流绝对值分成 4 段。
   由 2026-11-13 五点实测（0/1/2/3/4A，App 与负载仪差值）逐段拟合得到，
   段与段在 1A / 2A / 3A 处首尾相接，不会跳变：
     0~1A : Δ = 0.044×I + 0.018
     1~2A : Δ = 0.061×I + 0.001
     2~3A : Δ = 0.038×I + 0.047
     3~4A : Δ = 0.041×I + 0.038
   显示电压 = 测量电压 − ΔV。 */
static const float LINE_SEG_K[4] = { 0.044f, 0.061f, 0.038f, 0.041f };
static const float LINE_SEG_B[4] = { 0.018f, 0.001f, 0.047f, 0.038f };

/* 额外的全局线性补偿（默认 0），串口/蓝牙发 "L<值>" 可再叠加一个 I×R 修正 */
#define LINE_RESISTANCE_DEFAULT 0.0f

/* 大电流段的固定偏差（按幅值扣掉）：
     3.0 ~ 3.4A：读数偏高约 4mA
     3.4A 以上 ：读数偏高约 7mA */
#define CURRENT_MID_FROM_A      3.0f
#define CURRENT_MID_OFFSET_A    0.004f
#define CURRENT_HIGH_FROM_A     3.4f
#define CURRENT_HIGH_OFFSET_A   0.007f

/* 电压平均：主循环每 ADC_SAMPLE_PERIOD_MS 采一次 PA0，环形缓存 ADC_AVG_WINDOW 个点，
   上报时取窗口平均。48 点 × 10ms ≈ 480ms，正好覆盖一个上报周期，
   开关电源纹波和随机噪声按 √48 ≈ 7 倍衰减。 */
#define ADC_SAMPLE_PERIOD_MS    10U
#define ADC_AVG_WINDOW          48U

typedef struct
{
  float voltage_V;  //电压
  float current_A;  //电流
  float power_W;   //功率
} MeterData;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 发送槽位：0=测量帧，1=0x03 通信自检回帧，2=0x05 地址扫描回帧 */
#define METER_TX_SLOTS      3U
#define METER_TX_MEASURE    0U
#define METER_TX_DIAG       1U
#define METER_TX_SCAN       2U
#define METER_TX_TEXT_SIZE  96U

/* 帧格式：1 = 可读三行（CH2 V 24.123V / CH2 I 1.23456A / CH2 P 29.808W）
           0 = 紧凑单行（C2,24.123,1.23456,29.808）
   ADC 原始值不再每帧发送，需要时串口发 'v' 单独取一帧；
   每行都控制在 20 字节以内，正好一条 BLE 通知发完，不会再被拆包截断。 */
#define METER_FRAME_STYLE   1U

/* 上报周期（毫秒）。四行一帧（V/I/P/R），200ms 就是 20 条 BLE 通知/秒。
   带宽算一下：一行约 18 字节，9600bps 发 72 字节 ≈ 75ms，200ms 周期还有 2 倍余量；
   INA226 一组 64 次平均是 70.4ms，也来得及出一组新值。
   如果手机端出现丢帧，把它调到 250~300；想更快就把模块波特率提到 115200 再改小。 */
#define REPORT_PERIOD_MS    200U

/* 发送方式：1 = USART2 TX DMA（原方式）；0 = 阻塞发送
   排查“有 INIT 却没有数据帧”时用 0，用来判断问题是否出在 DMA 发送链上 */
#define METER_TX_USE_DMA    0U

/* 分流电压零点偏移（单位 LSB，1 LSB = 2.5µV）：上电默认 0。
   空载时读到的 -10 LSB 左右就是 INA226 的失调，收到 0x06 / 文本 'z' 时
   在空载下连采 64 次求平均写进来修正；只存 RAM，掉电需要重标。 */
/* 空载零点校准的采样组数：每组间隔 80ms（对应 INA226 的 64 次平均周期） */
#define SHUNT_ZERO_SAMPLES      8U

/* 默认零点偏移（LSB）：2026-11-12 空载实测 -0.0120A ≈ -10 LSB，直接作为上电初值；
   串口发 'z' 可用当场实测值覆盖（只存 RAM） */
#define SHUNT_OFFSET_LSB_DEFAULT  (-10)

/* INA226 厂商 ID（寄存器 0xFE），用于识别芯片是否真的挂在总线上 */
#define INA226_MANUFACTURER_ID 0x5449U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static uint8_t bluetooth_rx_byte;

/* 调试时观察这两个变量 */
volatile uint8_t bluetooth_last_byte;
volatile uint32_t bluetooth_rx_count;

static volatile bool bluetooth_rx_rearm;      // 中断里重挂接收失败时置位，由主循环重试
static volatile uint8_t bluetooth_echo_byte;  // 收到的指令，由主循环回显

MeterData meter_data;  // 电流来自 INA226（U9，I2C1），电压来自 ADC1（PA0），功率由两者相乘

/* INA226 当前使用的 7 位地址：上电按原理图取 0x40，初始化时若对不上会自动扫描并记住 */
static uint8_t meter_ina_addr7 = INA226_ADDR7;

/* 电压校准系数（运行期可改）：V = K × ADC原始值 + B */
static float meter_k = ADC_VOLT_K_DEFAULT;
static float meter_b = ADC_VOLT_B_DEFAULT;

/* 线路等效电阻（运行期可改）：显示电压 = 测量电压 − I × R_line */
static float meter_line_ohm = LINE_RESISTANCE_DEFAULT;

/* 串口行命令缓冲：收 "K0.013207" / "B-0.46646" / "?" 这类多字节命令 */
static char meter_cmd_line[24];
static uint8_t meter_cmd_len;

/* 需要回一帧 "CFG K=… B=…"（中断置标志，主循环发送） */
static volatile bool meter_cfg_request;

/* 分流电压零点偏移，校准后由 Meter_ReadCurrent 从原始值里减掉；上电用出厂实测值 */
static int16_t shunt_offset_lsb = SHUNT_OFFSET_LSB_DEFAULT;

/* 串口收到 0x06 / 'z' 后置位，由主循环做一次空载零点校准 */
static volatile bool meter_zero_request;

/* 串口收到 0x07 / 'r' 后置位，由主循环回一帧分流寄存器原始值，用于对表排查 */
static volatile bool meter_raw_request;



/* 串口收到 0x08 / 'v' 后置位，由主循环回一帧电压 ADC 原始值，用于逐点标定 */
static volatile bool meter_vraw_request;



/* 电压滑动平均窗口：环形缓存 PA0 的原始 ADC 值 */
static uint16_t adc_ring[ADC_AVG_WINDOW];
static uint8_t  adc_ring_index;
static uint8_t  adc_ring_count;
static uint32_t adc_ring_sum;
static uint32_t adc_sample_tick;

/* 电压 ADC 原始值的窗口平均值：每次采样入窗后更新，发送和换算都直接读这个变量，
   调试时在 watch 里看它即可，等于串口打出来的 CH2 R */
uint16_t adc_raw_avg;

bool meter_ready;   //INA226 是否初始化成功
bool adc_ready;     //ADC1 自校准是否成功
bool meter_valid;   //本次电压、电流是否都读取成功、可以使用
static char meter_tx_text[METER_TX_SLOTS][METER_TX_TEXT_SIZE];  // 独立缓冲区，发送完成前不能覆盖
static uint16_t meter_tx_length[METER_TX_SLOTS];  // 每槽实际发送字节数，不包含字符串结束符
static volatile bool meter_tx_pending[METER_TX_SLOTS];  // true：待发或正在发送，禁止覆盖
static volatile uint8_t meter_tx_active;  // 0：空闲，1..METER_TX_SLOTS：正在发送的槽位+1
static volatile uint32_t meter_tx_start_tick;  // 本次 DMA 发送开始的时刻，用于超时判断
static volatile bool meter_diag_request;  // 串口收到 0x03 后置位，由主循环生成自检回帧
static volatile bool meter_scan_request;  // 串口收到 0x05 后置位，由主循环扫描 I²C 地址
volatile uint32_t meter_uart_error_count;  // 调试时观察串口和 DMA 错误次数

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
bool INA226_ReadReg(I2C_HandleTypeDef *bus,uint8_t reg,uint16_t *value);
//向芯片写
bool INA226_WriteReg(I2C_HandleTypeDef *bus,uint8_t reg,uint16_t value);
/*
检查地址有没有应答。
检查制造商 ID 是否符合 INA226。
配置芯片开始连续测量。
*/
bool INA226_Init(I2C_HandleTypeDef *bus);
/**
 * @brief  读取 INA226 的分流电压，算出电流写入 result->current_A
 * @param  bus     该 INA226 所连接的 I²C 句柄
 * @param  result  保存测量结果的地址，必须指向有效的 MeterData 变量
 * @return true：读取成功；false：读取失败，result 原有内容不变
 */
bool Meter_ReadCurrent(I2C_HandleTypeDef *bus, MeterData *result);
/* 把一次 PA0 采样推进滑动窗口，并取窗口平均值（原始 ADC 值） */
static void Meter_AdcPush(uint16_t raw);
//串口蓝牙发送
void Meter_Send(const MeterData *data,bool valid);
static void Meter_TxStart(void);
static void Meter_TxPublish(uint8_t index,char *end);
static bool Meter_DiagSend(void);
static bool Meter_ScanSend(void);
static char *Meter_WriteText(char *text,const char *value);
static char *Meter_WriteInt(char *text,int32_t value);
static char *Meter_WriteHex(char *text,uint16_t value,uint8_t digits);
static char *Meter_WriteFixed(char *text,float value,uint8_t decimals);
static bool INA226_ProbeAddr(I2C_HandleTypeDef *bus,uint8_t addr7);
static bool Meter_ZeroCalibrate(void);
static bool Meter_ReadAdc(uint16_t *raw0,uint16_t *raw1);
static float Meter_AdcVoltage(uint16_t raw);
static void Meter_Trace(char step);

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_USART2_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  /* 开机横幅：一上电就能确认“固件在跑 + 串口通 + 烧的是哪一版”，
     内容带编译日期时间，换版本立刻能看出来。阻塞约 28ms(9600，27 字节)，只在启动时执行一次。 */
  static const char boot_msg[] = "BOOT " __DATE__ " " __TIME__ "\r\n";
  HAL_UART_Transmit(&huart2,(uint8_t *)boot_msg,(uint16_t)(sizeof(boot_msg)-1U),100);

  //开启串口中断接收
  if (HAL_UART_Receive_IT(&huart2, &bluetooth_rx_byte, 1) != HAL_OK)
  {
    Error_Handler();
  }
  /* ADC1 自校准：G0 上电必须做一次，否则零点有偏移；必须在 ADC 未启动时执行 */
  adc_ready = (HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK);

  //初始化 INA226：U9，连接 I2C1（只用来测电流）
  meter_ready = INA226_Init(&hi2c1);

  /* 初始化结果：1=成功，0=失败（INA226 无应答 / ID 不符；ADC 校准失败） */
  {
    char init_msg[] = "INIT CH2=0 ADC=0\r\n";
    init_msg[9]  = meter_ready ? '1' : '0';
    init_msg[15] = adc_ready ? '1' : '0';
    HAL_UART_Transmit(&huart2,(uint8_t *)init_msg,(uint16_t)(sizeof(init_msg)-1U),100);
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /* 诊断用：进主循环立刻打一次、1 秒后再打一次发送链状态，
     用来区分“主循环根本没跑起来”和“跑起来了但发送链卡住” */
  uint32_t state_tick = HAL_GetTick();
  uint32_t report_tick = HAL_GetTick();
  uint8_t state_stage = 0U;
  uint32_t loop_count = 0U;
  bool trace_done = false;

  while (1)
  {
    /* 指令回显：确认上位机的 0x01/0x02/0x03/0x05 到底有没有到达 MCU */
    if (bluetooth_echo_byte != 0U)
    {
      uint8_t echo = bluetooth_echo_byte;
      char echo_msg[12];
      char *p = echo_msg;

      bluetooth_echo_byte = 0U;

      p = Meter_WriteText(p,"RX ");
      p = Meter_WriteHex(p,(uint16_t)echo,2U);
      *p++ = '\r';
      *p++ = '\n';

      HAL_UART_Transmit(&huart2,(uint8_t *)echo_msg,(uint16_t)(p-echo_msg),50U);
    }

    /* 补挂接收：中断里因 HAL 返回 BUSY 没挂上时，由主循环补挂 */
    if (bluetooth_rx_rearm && huart2.RxState == HAL_UART_STATE_READY)
    {
      if (HAL_UART_Receive_IT(&huart2, &bluetooth_rx_byte, 1) == HAL_OK)
      {
        bluetooth_rx_rearm = false;
      }
    }

    /* 发送超时保护：一行 20 字节在 9600 下约 21ms，超过 50ms 还没完成
       说明 DMA 传输完成中断没来（或启动失败留下 pending），强制复位发送链，
       否则 pending 会一直挂着，采集和发送都停住。 */
    if (meter_tx_active!=0U && (uint32_t)(HAL_GetTick()-meter_tx_start_tick) > 50U)
    {
      HAL_UART_AbortTransmit(&huart2);
      meter_tx_pending[meter_tx_active-1U] = false;
      meter_tx_active = 0;
      meter_uart_error_count++;
    }

    /* 状态回显：STATE1 立即打（证明主循环和阻塞发送活着），STATE2 在第 2000 次循环打
       （用循环次数触发，不依赖 SysTick；同时打出 tick 用来判断 SysTick 是否在走）
       act=正在发送的槽+1，uart=huart2.gState（32=READY，33=BUSY_TX），
       dma=TX DMA 状态（0=READY，1=BUSY），pend=各槽待发位，err=错误计数 */
    loop_count++;

    bool want_state = (state_stage == 0U)
                   || (state_stage == 1U && loop_count >= 2000U);

    if (want_state)
    {
      state_stage++;

      char state_msg[96];
      char *p = state_msg;

      p = Meter_WriteText(p,"STATE");
      *p++ = (char)('0'+state_stage);
      p = Meter_WriteText(p," act=");
      p = Meter_WriteInt(p,(int32_t)meter_tx_active);
      p = Meter_WriteText(p," uart=");
      p = Meter_WriteInt(p,(int32_t)huart2.gState);
      p = Meter_WriteText(p," dma=");
      p = Meter_WriteInt(p,(int32_t)huart2.hdmatx->State);
      p = Meter_WriteText(p," pend=");
      p = Meter_WriteInt(p,(int32_t)((meter_tx_pending[METER_TX_MEASURE]?1:0)
                                    |(meter_tx_pending[METER_TX_DIAG]?2:0)
                                    |(meter_tx_pending[METER_TX_SCAN]?4:0)));
      p = Meter_WriteText(p," err=");
      p = Meter_WriteInt(p,(int32_t)meter_uart_error_count);
      p = Meter_WriteText(p," tick=");
      p = Meter_WriteInt(p,(int32_t)((uint32_t)(HAL_GetTick()-state_tick)));
      p = Meter_WriteText(p," loop=");
      p = Meter_WriteInt(p,(int32_t)loop_count);
      *p++ = '\r';
      *p++ = '\n';

      HAL_UART_Transmit(&huart2,(uint8_t *)state_msg,(uint16_t)(p-state_msg),100);
    }

    /* 收到 0x03：先回一帧 I²C / 串口自检结果，再继续采集 */
    if (meter_diag_request && Meter_DiagSend())
    {
      meter_diag_request = false;
    }

    /* 收到 0x05：扫描 I2C1 上的 0x40~0x4F，回一帧地址表 */
    if (meter_scan_request && Meter_ScanSend())
    {
      meter_scan_request = false;
    }

    /* 收到 0x06 / 'z'：空载零点校准，回一帧 CAL 结果 */
    if (meter_zero_request)
    {
      meter_zero_request = false;

      char cal_msg[32];
      char *p = cal_msg;

      if (Meter_ZeroCalibrate())
      {
        p = Meter_WriteText(p,"CAL OFF=");
        p = Meter_WriteInt(p,(int32_t)shunt_offset_lsb);
        p = Meter_WriteText(p,"LSB\r\n");
      }
      else
      {
        p = Meter_WriteText(p,"CAL FAIL\r\n");
      }

      HAL_UART_Transmit(&huart2,(uint8_t *)cal_msg,(uint16_t)(p-cal_msg),50U);
    }

    /* 收到 0x07 / 'r'：回一帧分流寄存器原始值，方便和万用表对表
       格式 RAW S=<原始LSB> O=<偏移LSB> N=<修正后LSB>，U = N × 2.5µV */
    if (meter_raw_request)
    {
      meter_raw_request = false;

      uint16_t raw_shunt;

      if (INA226_ReadReg(&hi2c1, INA226_REG_SHUNT, &raw_shunt))
      {
        char raw_msg[48];
        char *p = raw_msg;

        int32_t raw_lsb = (int32_t)(int16_t)raw_shunt;
        int32_t net_lsb = raw_lsb - (int32_t)shunt_offset_lsb;

        p = Meter_WriteText(p,"RAW S=");
        p = Meter_WriteInt(p,raw_lsb);
        p = Meter_WriteText(p," O=");
        p = Meter_WriteInt(p,(int32_t)shunt_offset_lsb);
        p = Meter_WriteText(p," N=");
        p = Meter_WriteInt(p,net_lsb);
        p = Meter_WriteText(p,"\r\n");

        HAL_UART_Transmit(&huart2,(uint8_t *)raw_msg,(uint16_t)(p-raw_msg),50U);
      }
      else
      {
        static const char raw_err[] = "RAW FAIL\r\n";
        HAL_UART_Transmit(&huart2,(uint8_t *)raw_err,(uint16_t)(sizeof(raw_err)-1U),50U);
      }
    }

    Meter_TxStart();

    /* 收到 0x08 / 'v'：回一帧电压 ADC 原始值（480ms 窗口平均），用于逐点标定
       格式 VRAW R=<原始值> V=<当前换算电压> */
    if (meter_vraw_request)
    {
      meter_vraw_request = false;

      uint16_t raw = adc_raw_avg;

      if (adc_ready && (adc_ring_count>0U))
      {
        char vmsg[48];
        char *p = vmsg;

        p = Meter_WriteText(p,"VRAW R=");
        p = Meter_WriteInt(p,(int32_t)raw);
        p = Meter_WriteText(p," V=");
        p = Meter_WriteFixed(p,Meter_AdcVoltage(raw),3U);
        p = Meter_WriteText(p,"V\r\n");

        HAL_UART_Transmit(&huart2,(uint8_t *)vmsg,(uint16_t)(p-vmsg),50U);
      }
      else
      {
        static const char v_err[] = "VRAW FAIL\r\n";
        HAL_UART_Transmit(&huart2,(uint8_t *)v_err,(uint16_t)(sizeof(v_err)-1U),50U);
      }
    }

    /* 回一帧当前参数：CFG K=0.013050 B=0.000000 L=0.050000
       手机端连上后发 "?" 就能读到（App 目前只解析 K/B，L 在后面不影响解析） */
    if (meter_cfg_request)
    {
      meter_cfg_request = false;

      char cfg_msg[64];
      char *p = cfg_msg;

      p = Meter_WriteText(p,"CFG K=");
      p = Meter_WriteFixed(p,meter_k,6U);
      p = Meter_WriteText(p," B=");
      p = Meter_WriteFixed(p,meter_b,6U);
      p = Meter_WriteText(p," L=");
      p = Meter_WriteFixed(p,meter_line_ohm,6U);
      p = Meter_WriteText(p,"\r\n");

      HAL_UART_Transmit(&huart2,(uint8_t *)cfg_msg,(uint16_t)(p-cfg_msg),50U);
    }

    /* 电压连续采样：每 10ms 采一次 PA0（rank1），攒满一个窗口再平均，
       这样 500ms 内的纹波和随机噪声都被压掉，不再依赖单次采样 */
    if ((uint32_t)(HAL_GetTick()-adc_sample_tick) >= ADC_SAMPLE_PERIOD_MS)
    {
      adc_sample_tick = HAL_GetTick();

      uint16_t raw_pa0;
      uint16_t raw_pa1;

      if (Meter_ReadAdc(&raw_pa0,&raw_pa1))
      {
        Meter_AdcPush(raw_pa0);
      }
    }

    /* 测量帧发完、且过了上报周期才开始下一轮采集 */
    if (!meter_tx_pending[METER_TX_MEASURE] && meter_tx_active==0U && huart2.gState==HAL_UART_STATE_READY
        && (uint32_t)(HAL_GetTick()-report_tick) >= REPORT_PERIOD_MS)
    {
      report_tick = HAL_GetTick();
      /* 第一轮逐段打点：定位到底卡在哪一步（只在第 1 轮输出） */
      if (!trace_done)
      {
        Meter_Trace('1');   /* 进了采集分支，gState 是 READY */
      }

      /* 初始化失败时，定期重试 */
      if (!meter_ready)
      {
        meter_ready = INA226_Init(&hi2c1);
      }

      /* 电流：读 INA226 的分流电压寄存器（U9，I2C1） */
      bool current_ok = meter_ready && Meter_ReadCurrent(&hi2c1, &meter_data);

      /* 大电流段偏差修正（按幅值扣掉，反向电流同样处理）：
         3.0~3.4A 扣 4mA，3.4A 以上扣 7mA */
      if (current_ok)
      {
        float abs_i = (meter_data.current_A < 0.0f) ? -meter_data.current_A : meter_data.current_A;
        float offset = 0.0f;

        if (abs_i >= CURRENT_HIGH_FROM_A)
        {
          offset = CURRENT_HIGH_OFFSET_A;
        }
        else if (abs_i >= CURRENT_MID_FROM_A)
        {
          offset = CURRENT_MID_OFFSET_A;
        }

        if (offset > 0.0f)
        {
          if (meter_data.current_A > 0.0f)
          {
            meter_data.current_A -= offset;
          }
          else
          {
            meter_data.current_A += offset;
          }
        }
      }

      if (!trace_done)
      {
        Meter_Trace('2');   /* 电流读完了，没卡在 I²C 总线上 */
      }

      /* 读取失败，下一轮重新初始化 */
      if (!current_ok)
      {
        meter_ready = false;
      }

      /* 电压：ADC1 的 PA0，经 R14=15k / R17=1k 分压（×16），取最近 480ms 的窗口平均 */
      float voltage_V = 0.0f;
      bool voltage_ok = false;

      if (adc_ready && (adc_ring_count>0U))
      {
        voltage_V = Meter_AdcVoltage(adc_raw_avg);
        voltage_ok = true;
      }

      /* 电压、电流都成功才算本次数据有效，功率 = U × I */
      if (current_ok && voltage_ok)
      {
        /* 线路压降补偿：ADC 采的是 OUT 节点，负载端还低一段电压，
           按电流所在区间取 ΔV = K × I + B，再加一个可选的全局 I × L。 */
        float current_A = meter_data.current_A;
        float abs_current = (current_A < 0.0f) ? -current_A : current_A;
        uint8_t seg = (abs_current < 1.0f) ? 0U :
                      ((abs_current < 2.0f) ? 1U :
                       ((abs_current < 3.0f) ? 2U : 3U));

        float drop = (LINE_SEG_K[seg] * abs_current) + LINE_SEG_B[seg]
                     + (meter_line_ohm * abs_current);

        /* 反向电流时压降方向相反 */
        if (current_A < 0.0f)
        {
          drop = -drop;
        }

        voltage_V -= drop;

        if (voltage_V < 0.0f)
        {
          voltage_V = 0.0f;
        }

        meter_data.voltage_V = voltage_V;
        meter_data.power_W = voltage_V * current_A;
      }

      meter_valid = current_ok && voltage_ok;

      /* 输出测量值或错误状态 */
      Meter_Send(&meter_data, meter_valid);

      if (!trace_done)
      {
        Meter_Trace('3');   /* 帧已提交发送，本轮结束 */
        trace_done = true;
      }
    }


    
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* 解析十进制浮点数（支持符号和一个小数点，不处理指数）；成功返回 true */
static bool Meter_ParseFloat(const char *text,float *value)
{
  bool negative = false;
  bool any_digit = false;
  bool has_dot = false;
  float result = 0.0f;
  float fraction = 0.1f;

  while (*text==' ')
  {
    text++;
  }

  if (*text=='-')
  {
    negative = true;
    text++;
  }
  else if (*text=='+')
  {
    text++;
  }

  while (*text!='\0')
  {
    char c = *text;

    if ((c>='0') && (c<='9'))
    {
      any_digit = true;

      if (!has_dot)
      {
        result = (result*10.0f) + (float)(c-'0');
      }
      else
      {
        result += (float)(c-'0')*fraction;
        fraction *= 0.1f;
      }
    }
    else if ((c=='.') && (!has_dot))
    {
      has_dot = true;
    }
    else
    {
      break;
    }

    text++;
  }

  if (!any_digit)
  {
    return false;
  }

  *value = negative ? -result : result;

  return true;
}

/* 处理一行命令："K<值>" 设增益，"B<值>" 设截距，"L<值>" 设线路电阻，"?" 查询 */
static void Meter_HandleCommandLine(char *line)
{
  float value;

  while (*line==' ')
  {
    line++;
  }

  if ((line[0]=='K') || (line[0]=='k'))
  {
    if (Meter_ParseFloat(line+1,&value))
    {
      meter_k = value;
      meter_cfg_request = true;
    }
  }
  else if ((line[0]=='L') || (line[0]=='l'))
  {
    if (Meter_ParseFloat(line+1,&value) && (value >= 0.0f) && (value < 10.0f))
    {
      meter_line_ohm = value;
      meter_cfg_request = true;
    }
  }
  else if ((line[0]=='B') || (line[0]=='b'))
  {
    if (Meter_ParseFloat(line+1,&value))
    {
      meter_b = value;
      meter_cfg_request = true;
    }
  }
  else if (line[0]=='?')
  {
    meter_cfg_request = true;
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    uint8_t data = bluetooth_rx_byte;
    bool echo = true;

    /* 行命令优先：K…/B…/? 先攒进缓冲区，收到 CR/LF 再处理；
       其它单字节命令（1/0/3/5/z/r/v）仍立即执行 */
    if ((data=='\r') || (data=='\n'))
    {
      if (meter_cmd_len>0U)
      {
        meter_cmd_line[meter_cmd_len] = '\0';
        Meter_HandleCommandLine(meter_cmd_line);
        meter_cmd_len = 0U;
      }

      echo = false;
    }
    else if ((meter_cmd_len>0U) || (data=='K') || (data=='k')
             || (data=='B') || (data=='b') || (data=='?'))
    {
      if (meter_cmd_len < (uint8_t)(sizeof(meter_cmd_line)-1U))
      {
        meter_cmd_line[meter_cmd_len++] = (char)data;
      }

      echo = false;
    }
    else
    {
    /* 兼容文本模式：App 没开 HEX 时发的是字符 '1'/'0'/'3'/'5'，
       统一折算成控制字节，免得“指令发了却没反应” */
    uint8_t cmd = data;

    if      (data=='0') { cmd = 0x02U; }   /* 文本 '0' → 关 */
    else if (data=='1') { cmd = 0x01U; }   /* 文本 '1' → 开 */
    else if (data=='3') { cmd = 0x03U; }   /* 文本 '3' → 自检 */
    else if (data=='5') { cmd = 0x05U; }   /* 文本 '5' → 扫描 */
    else if ((data=='z') || (data=='Z')) { cmd = 0x06U; }  /* 文本 'z' → 空载零点校准 */
    else if ((data=='r') || (data=='R')) { cmd = 0x07U; }  /* 文本 'r' → 回原始寄存器值 */
    else if ((data=='v') || (data=='V')) { cmd = 0x08U; }  /* 文本 'v' → 回电压 ADC 原始值 */

    if (cmd==0x01U)
    {
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);   //开
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    }
    else if (cmd==0x02U)
    {
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);  //关
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    }
    else if (cmd==0x03U)
    {
      /* 通信自检：只置请求标志，I²C 探测放到主循环，中断里不能阻塞 */
      meter_diag_request = true;
    }
    else if (cmd==0x05U)
    {
      /* 地址扫描：同样只在主循环里执行 */
      meter_scan_request = true;
    }
    else if (cmd==0x06U)
    {
      /* 空载零点校准：采样也在主循环里做 */
      meter_zero_request = true;
    }
    else if (cmd==0x07U)
    {
      /* 回原始值：同样只置标志 */
      meter_raw_request = true;
    }
    else if (cmd==0x08U)
    {
      /* 回电压原始值：同样只置标志 */
      meter_vraw_request = true;
    }
    }

    /* 重新启动，准备接收下一个字节。
       这里绝不能再调用 Error_Handler()：它 __disable_irq()+while(1)，
       一旦 HAL 返回 HAL_BUSY（比如主循环的阻塞发送正占着串口），
       整机就静默死掉，表现为"发 0x01/0x02 没反应、数据也停了"。 */
    if (HAL_UART_Receive_IT(&huart2, &bluetooth_rx_byte, 1) != HAL_OK)
    {
      bluetooth_rx_rearm = true;
      meter_uart_error_count++;
    }

    /* 回显收到的指令，主循环负责打印；行命令的每个字节不回显，避免刷屏 */
    if (echo)
    {
      bluetooth_echo_byte = data;
    }

    bluetooth_last_byte = data;
    bluetooth_rx_count++;

    /* 在这里添加简单的字节处理，不能使用 HAL_Delay() */
  }
}



void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    /* USART 最后一个字节发完后，才释放对应缓冲区 */
    if (meter_tx_active!=0U)
    {
      meter_tx_pending[meter_tx_active-1U] = false;
      meter_tx_active = 0;
    }

    /* 上一条发完，立即接着发送另一条待发数据 */
    Meter_TxStart();
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    meter_uart_error_count++;

    /* DMA 出错时保留待发内容，交给主循环重试 */
    if ((huart->ErrorCode & HAL_UART_ERROR_DMA)!=0U && huart->gState==HAL_UART_STATE_READY)
    {
      meter_tx_active = 0;
    }

    /* 接收溢出等错误结束接收后，重新接收控制命令 */
    if (huart->RxState==HAL_UART_STATE_READY)
    {
      if (HAL_UART_Receive_IT(&huart2, &bluetooth_rx_byte, 1) != HAL_OK)
      {
        meter_uart_error_count++;
      }
    }
  }
}

static void Meter_TxStart(void)
{
  /* 选一个待发槽位并在临界区里登记；真正的发送放到临界区外，
     否则阻塞发送期间中断被关着，会丢接收到的指令字节 */
  uint8_t slot = 0xFFU;

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  if (meter_tx_active==0U && huart2.gState==HAL_UART_STATE_READY)
  {
    for (uint8_t i=0; i<METER_TX_SLOTS; i++)
    {
      if (meter_tx_pending[i])
      {
        slot = i;
        meter_tx_active = i+1U;
        meter_tx_start_tick = HAL_GetTick();
        break;
      }
    }
  }

  /* 恢复进入函数前的中断状态 */
  __set_PRIMASK(primask);

  if (slot==0xFFU)
  {
    return;
  }

#if (METER_TX_USE_DMA != 0U)
  if (HAL_UART_Transmit_DMA(&huart2,(uint8_t *)meter_tx_text[slot],meter_tx_length[slot]) != HAL_OK)
  {
    /* 启动失败时不清除 pending，下一次继续重试 */
    meter_tx_active = 0;
    meter_uart_error_count++;
  }
#else
  /* 阻塞发送，并且按行拆开发：每行都以 \n 结尾、长度控制在 20 字节以内，
     保证一条 BLE 通知就能装下一行，不会再出现跨包截断 */
  {
    char *text = meter_tx_text[slot];
    uint16_t total = meter_tx_length[slot];
    uint16_t start = 0U;
    bool ok = true;

    for (uint16_t i=0U; i<total; i++)
    {
      if (text[i]=='\n')
      {
        uint16_t n = (uint16_t)(i+1U-start);

        if (HAL_UART_Transmit(&huart2,(uint8_t *)&text[start],n,50U) != HAL_OK)
        {
          ok = false;
        }

        start = (uint16_t)(i+1U);
      }
    }

    /* 末尾没有换行的残余部分也发出去 */
    if (start<total)
    {
      if (HAL_UART_Transmit(&huart2,(uint8_t *)&text[start],(uint16_t)(total-start),50U) != HAL_OK)
      {
        ok = false;
      }
    }

    if (ok)
    {
      meter_tx_pending[slot] = false;
    }
    else
    {
      meter_uart_error_count++;
    }
  }

  meter_tx_active = 0;
#endif
}

/* 结束一个槽位的文本后发布待发标记；长度越界则丢弃，不发送半截内容 */
static void Meter_TxPublish(uint8_t index,char *end)
{
  char *text = meter_tx_text[index];
  int length;

  *end = '\0';
  length = (int)(end-text);

  if (length > 0 && length < (int)sizeof(meter_tx_text[index]))
  {
    meter_tx_length[index] = (uint16_t)length;
    /* 内容和长度写好后再发布待发标记，供中断回调接续发送 */
    __DMB();
    meter_tx_pending[index] = true;
    Meter_TxStart();
  }
}

//INA226 都要求高字节在前。 先传高8位，再传低8位
//向芯片读
bool INA226_ReadReg(I2C_HandleTypeDef *bus,uint8_t reg,uint16_t *value)
{
  uint8_t data[2];
  HAL_StatusTypeDef status = HAL_I2C_Mem_Read(bus,INA226_ADDR_RT,reg,I2C_MEMADD_SIZE_8BIT,data,sizeof(data),100);

  if (status != HAL_OK) {
    return false;
  }
  *value = ((uint16_t)data[0] << 8) | data[1];
  return true;
}

//向芯片写
bool INA226_WriteReg(I2C_HandleTypeDef *bus,uint8_t reg,uint16_t value)
{
  uint8_t data[2];
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
  HAL_StatusTypeDef status = HAL_I2C_Mem_Write(bus,INA226_ADDR_RT,reg,I2C_MEMADD_SIZE_8BIT,data,sizeof(data),30);
  return status == HAL_OK;
}

/* 试探某个 7 位地址上是不是 INA226：有应答且厂商 ID 正确
   超时给得很短（3ms/10ms），扫 16 个地址最坏约 50ms */
static bool INA226_ProbeAddr(I2C_HandleTypeDef *bus,uint8_t addr7)
{
  uint8_t data[2];
  uint16_t address = (uint16_t)(addr7<<1);

  if (HAL_I2C_IsDeviceReady(bus,address,1,3) != HAL_OK)
  {
    return false;
  }

  if (HAL_I2C_Mem_Read(bus,address,INA226_REG_MANUFACTURER,I2C_MEMADD_SIZE_8BIT,data,sizeof(data),10) != HAL_OK)
  {
    return false;
  }

  return ((((uint16_t)data[0]<<8)|data[1]) == INA226_MANUFACTURER_ID);
}

/*
先按上次的地址确认应答与制造商 ID。
地址对不上时在 0x40~0x4F 里扫一遍，认到就记住（兼容 A0/A1 不同接法）。
最后配置芯片开始连续测量。
*/
bool INA226_Init(I2C_HandleTypeDef *bus)
{
  if (!INA226_ProbeAddr(bus,meter_ina_addr7))
  {
    bool found = false;

    for (uint8_t addr7=INA226_ADDR7; addr7<=(INA226_ADDR7+0x0FU); addr7++)
    {
      if (addr7!=meter_ina_addr7 && INA226_ProbeAddr(bus,addr7))
      {
        meter_ina_addr7 = addr7;
        found = true;
        break;
      }
    }

    if (!found)
    {
      return false;
    }
  }

  /* 当前 INA226_Measure=0x0725：64 次平均、每次 1.1ms，一组约 70.4ms */
  if (!INA226_WriteReg(bus, INA226_REG_CONFIG, INA226_Measure))
  {
    return false;
  }

  /* 等第一组平均结果产生（70.4ms），取 80ms 留足余量；仅初始化和重试时执行 */
  HAL_Delay(80);

  return true;
}


/**
 * @brief  读取 INA226 的分流电压，算出电流写入 result->current_A
 * @param  bus     该 INA226 所连接的 I²C 句柄
 * @param  result  保存测量结果的地址，必须指向有效的 MeterData 变量
 * @return true：读取成功；false：读取失败，result 原有内容不变
 */
bool Meter_ReadCurrent(I2C_HandleTypeDef *bus, MeterData *result)
{
  uint16_t raw_shunt; // 分流电压寄存器的原始位模式，计算时按有符号数解释

  // 读取采样电阻两端的压差；通信失败则退出
  if (!INA226_ReadReg(bus, INA226_REG_SHUNT, &raw_shunt))
  {
    return false;
  }

  // 减掉空载零点偏移（±10µV 量级的失调，折算到 2mΩ 上是 ±5mA）
  int32_t raw_signed = (int32_t)(int16_t)raw_shunt - (int32_t)shunt_offset_lsb;

  /* 饱和到 int16 范围，防止减法溢出 */
  if (raw_signed > 32767)
  {
    raw_signed = 32767;
  }
  else if (raw_signed < -32768)
  {
    raw_signed = -32768;
  }

  // 分流电压每位代表 2.5μV，算出采样电阻两端的压差
  float shunt_voltage_V = (float)raw_signed * 0.0000025f;

  // 根据 I = U / R 计算电流，单位 A
  result->current_A = shunt_voltage_V / SHUNT_RESISTANCE_OHM;

  return true;
}


/* 空载零点校准：间隔取 SHUNT_ZERO_SAMPLES 组分流电压求平均，作为偏移量。
   INA226 现在每 70.4ms 才更新一次平均值，所以每组之间要等 80ms，
   否则连读会拿到同一个结果，平均等于没平均。总共约 0.7s。
   返回 false：一次都没读到（I²C 不通） */
static bool Meter_ZeroCalibrate(void)
{
  uint16_t raw;
  int32_t sum = 0;
  uint8_t ok = 0U;

  for (uint8_t i=0U; i<SHUNT_ZERO_SAMPLES; i++)
  {
    if (INA226_ReadReg(&hi2c1, INA226_REG_SHUNT, &raw))
    {
      sum += (int16_t)raw;
      ok++;
    }

    HAL_Delay(80);
  }

  if (ok==0U)
  {
    return false;
  }

  /* 四舍五入到最近的整数 LSB（负数向远离 0 的方向进位） */
  int32_t average = (sum>=0) ? ((sum+(int32_t)ok/2)/(int32_t)ok)
                             : ((sum-(int32_t)ok/2)/(int32_t)ok);

  shunt_offset_lsb = (int16_t)average;

  return true;
}


/* 读 ADC1 的两个规则通道各一次：rank1 = PA0(IN0)，rank2 = PA1(IN1)
   返回 false：启动失败或转换超时 */
static bool Meter_ReadAdc(uint16_t *raw0,uint16_t *raw1)
{
  if (HAL_ADC_Start(&hadc1) != HAL_OK)
  {
    return false;
  }

  for (uint8_t i=0U; i<2U; i++)
  {
    if (HAL_ADC_PollForConversion(&hadc1,10U) != HAL_OK)
    {
      HAL_ADC_Stop(&hadc1);
      return false;
    }

    uint16_t value = (uint16_t)HAL_ADC_GetValue(&hadc1);

    if (i==0U)
    {
      *raw0 = value;
    }
    else
    {
      *raw1 = value;
    }
  }

  HAL_ADC_Stop(&hadc1);

  return true;
}

/* ADC 原始值 → 被测电压：线性校准 V = K × raw + B（K/B 运行期可改） */
static float Meter_AdcVoltage(uint16_t raw)
{
  return ((float)raw * meter_k) + meter_b;
}

/* 把一次 PA0 采样推进滑动窗口；窗口满后覆盖最老的点，并同步更新全局平均值 */
static void Meter_AdcPush(uint16_t raw)
{
  if (adc_ring_count<ADC_AVG_WINDOW)
  {
    adc_ring_count++;
  }
  else
  {
    adc_ring_sum -= adc_ring[adc_ring_index];
  }

  adc_ring[adc_ring_index] = raw;
  adc_ring_sum += raw;
  adc_ring_index = (uint8_t)((adc_ring_index+1U) % ADC_AVG_WINDOW);

  /* 四舍五入的平均值，存进全局变量 */
  adc_raw_avg = (uint16_t)((adc_ring_sum + (uint32_t)(adc_ring_count/2U)) / adc_ring_count);
}


/* 只拼接所需文本，避免完整 printf 格式化代码占满 32KB Flash */
static char *Meter_WriteText(char *text,const char *value)
{
  while (*value!='\0')
  {
    *text++ = *value++;
  }

  return text;
}

/* 将有符号整数写成十进制，支持负电流和负功率 */
static char *Meter_WriteInt(char *text,int32_t value)
{
  char digits[10];
  uint8_t count = 0;
  uint32_t magnitude = (uint32_t)value;

  if (value<0)
  {
    *text++ = '-';
    /* 用无符号减法取绝对值，INT32_MIN 也不会溢出 */
    magnitude = 0U-magnitude;
  }

  do
  {
    digits[count++] = (char)('0'+magnitude%10U);
    magnitude /= 10U;
  } while (magnitude!=0U);

  while (count>0U)
  {
    *text++ = digits[--count];
  }

  return text;
}

/* 写定点小数，例如 Meter_WriteFixed(p,1.23456f,5) -> "1.23456"，
   0.5f/5 -> "0.50000"，-0.00125f/5 -> "-0.00125"；decimals 取 0~5 */
static char *Meter_WriteFixed(char *text,float value,uint8_t decimals)
{
  static const int32_t pow10[6] = {1,10,100,1000,10000,100000};
  int32_t scale;
  float scaled_f;
  int32_t scaled;
  bool negative;

  if (decimals>5U)
  {
    decimals = 5U;
  }

  scale = pow10[decimals];

  scaled_f = value * (float)scale;
  negative = (scaled_f < 0.0f);

  if (negative)
  {
    scaled_f = -scaled_f;
  }

  /* 防止 float 溢出后转 int32 出未定义行为 */
  if (scaled_f > 2000000000.0f)
  {
    scaled_f = 2000000000.0f;
  }

  scaled = (int32_t)(scaled_f + 0.5f);

  if (negative && (scaled!=0))
  {
    *text++ = '-';
  }

  /* 整数部分 */
  text = Meter_WriteInt(text,scaled/scale);

  /* 小数部分：按位取，保证位数固定（含前导 0） */
  if (decimals>0U)
  {
    int32_t fraction = scaled % scale;

    *text++ = '.';

    for (int32_t div=scale/10; div>0; div/=10)
    {
      *text++ = (char)('0'+((fraction/div)%10));
    }
  }

  return text;
}


/* 写 digits 位大写十六进制（digits=4 打厂商 ID，2 打地址） */
static char *Meter_WriteHex(char *text,uint16_t value,uint8_t digits)
{
  static const char hex_digits[] = "0123456789ABCDEF";
  int8_t shift = (int8_t)((digits-1U)*4U);

  do
  {
    *text++ = hex_digits[(value>>shift) & 0x0FU];
    shift -= 4;
  } while (shift >= 0);

  return text;
}

/* 第一轮逐段跟踪：只打 "Tn t=<tick>"，用来定位采集流程卡在哪一步 */
static void Meter_Trace(char step)
{
  char msg[20];
  char *p = msg;

  *p++ = 'T';
  *p++ = step;
  *p++ = ' ';
  *p++ = 't';
  *p++ = '=';
  p = Meter_WriteInt(p,(int32_t)HAL_GetTick());
  *p++ = '\r';
  *p++ = '\n';

  HAL_UART_Transmit(&huart2,(uint8_t *)msg,(uint16_t)(p-msg),100);
}

/* 探测一路 INA226：只读不写，不会打断正在测量的通道
 * 返回 0：地址无应答；1：厂商 ID 正确；2：有应答但 ID 不符；3：应答后读寄存器失败 */
static uint8_t INA226_Probe(I2C_HandleTypeDef *bus,uint16_t *manufacturer)
{
  *manufacturer = 0U;

  if (HAL_I2C_IsDeviceReady(bus, INA226_ADDR_RT, 2, 30) != HAL_OK)
  {
    return 0U;
  }

  if (!INA226_ReadReg(bus, INA226_REG_MANUFACTURER, manufacturer))
  {
    return 3U;
  }

  return (*manufacturer == INA226_MANUFACTURER_ID) ? 1U : 2U;
}

/* 拼一路自检结果：CHn=OK(5449) / CHn=BAD(FFFF) / CHn=NACK / CHn=ERR */
static char *Meter_WriteDiagChannel(char *text,uint8_t channel,uint8_t state,uint16_t manufacturer)
{
  char *end = Meter_WriteText(text,"CH");

  *end++ = (char)('0'+channel);
  *end++ = '=';

  if (state==1U)
  {
    end = Meter_WriteText(end,"OK(");
    end = Meter_WriteHex(end,manufacturer,4U);
    *end++ = ')';
  }
  else if (state==0U)
  {
    end = Meter_WriteText(end,"NACK");
  }
  else if (state==2U)
  {
    end = Meter_WriteText(end,"BAD(");
    end = Meter_WriteHex(end,manufacturer,4U);
    *end++ = ')';
  }
  else
  {
    end = Meter_WriteText(end,"ERR");
  }

  return end;
}

/* 回应 0x03：回一帧 I²C 与串口自检结果，例如
 * DIAG CH2=OK(5449) ADDR=40 OFF=-10 RDY=1 ERR=0
 * 只探 INA226（U9，走 I2C1），ADDR 是当前在用的 7 位地址，OFF 是零点偏移（LSB）
 * 返回 false：上一帧自检结果还在发送，本次不处理，下一轮重试 */
static bool Meter_DiagSend(void)
{
  /* 自检帧仍在发送时不能改它的缓冲区 */
  if (meter_tx_pending[METER_TX_DIAG])
  {
    return false;
  }

  uint16_t manufacturer;

  uint8_t state = INA226_Probe(&hi2c1,&manufacturer);

  char *text = meter_tx_text[METER_TX_DIAG];
  char *end = Meter_WriteText(text,"DIAG ");

  end = Meter_WriteDiagChannel(end,2U,state,manufacturer);

  end = Meter_WriteText(end," ADDR=");
  end = Meter_WriteHex(end,(uint16_t)meter_ina_addr7,2U);

  end = Meter_WriteText(end," OFF=");
  end = Meter_WriteInt(end,(int32_t)shunt_offset_lsb);

  end = Meter_WriteText(end," RDY=");
  *end++ = meter_ready ? '1' : '0';

  end = Meter_WriteText(end," ERR=");
  end = Meter_WriteInt(end,(int32_t)meter_uart_error_count);
  end = Meter_WriteText(end,"\r\n");

  /* 最长 "DIAG CH2=BAD(FFFF) ADDR=4F RDY=1 ERR=4294967295\r\n" 约 49 字节 */
  Meter_TxPublish(METER_TX_DIAG,end);

  return true;
}

/* 扫描 0x40~0x4F：返回应答地址个数，前 4 个写进 out
 * 超时给 3ms、只试 1 次，16 个地址最坏约 48ms */
static uint8_t INA226_Scan(I2C_HandleTypeDef *bus,uint8_t *out)
{
  uint8_t found = 0;

  for (uint8_t addr = 0x40U; addr <= 0x4FU; addr++)
  {
    if (HAL_I2C_IsDeviceReady(bus,(uint16_t)((uint16_t)addr<<1),1,3) == HAL_OK)
    {
      if (found < 4U)
      {
        out[found] = addr;
      }

      found++;
    }
  }

  return found;
}

/* 拼一路扫描结果：label=40 / label=40,41 / label=NONE */
static char *Meter_WriteScan(char *text,const char *label,const uint8_t *addr,uint8_t count)
{
  char *end = Meter_WriteText(text,label);

  *end++ = '=';

  if (count==0U)
  {
    return Meter_WriteText(end,"NONE");
  }

  uint8_t shown = (count>4U) ? 4U : count;

  for (uint8_t i=0; i<shown; i++)
  {
    if (i>0U)
    {
      *end++ = ',';
    }

    end = Meter_WriteHex(end,addr[i],2U);
  }

  if (count>4U)
  {
    *end++ = '+';
  }

  return end;
}

/* 回应 0x05：扫描 I2C1 上的 INA226 地址（0x40~0x4F），例如
 * SCAN CH2/I2C1=44
 * CH2=U9 接 I2C1；NONE 表示该总线一个地址都没应答
 * 返回 false：上一帧扫描结果还在发送，本次不处理，下一轮重试 */
static bool Meter_ScanSend(void)
{
  if (meter_tx_pending[METER_TX_SCAN])
  {
    return false;
  }

  uint8_t addr[4];

  uint8_t count = INA226_Scan(&hi2c1,addr);

  char *text = meter_tx_text[METER_TX_SCAN];
  char *end = Meter_WriteText(text,"SCAN ");

  end = Meter_WriteScan(end,"CH2/I2C1",addr,count);
  end = Meter_WriteText(end,"\r\n");

  /* 最长 "SCAN CH2/I2C1=40,41,42,43+\r\n" 约 26 字节 */
  Meter_TxPublish(METER_TX_SCAN,end);

  return true;
}

//串口蓝牙发送（只有第二片 INA226，帧头固定为 CH2）
void Meter_Send(const MeterData *data,bool valid)
{
  if (data==NULL)
  {
    return;
  }

  uint8_t index = METER_TX_MEASURE;

  /* 上一条尚未发送完成，不能修改 DMA 仍在使用的内容 */
  if (meter_tx_pending[index])
  {
    return;
  }

  char *text = meter_tx_text[index];
  char *end = text;

  /* 四行最坏约 4×19=76 字节，96 字节缓冲区足够 */
  if (valid)
  {
#if (METER_FRAME_STYLE != 0U)
    /* 可读四行，各一行一条 BLE 通知；帧头固定 CH2（U9/I2C1）
       V 保留 3 位小数（1mV），I 保留 5 位小数（10µA），P 保留 3 位小数（1mW），
       最后一行是电压 ADC 原始值，手机端导出历史数据要用 */
    end = Meter_WriteText(end,"CH2 V ");
    end = Meter_WriteFixed(end,data->voltage_V,3U);
    end = Meter_WriteText(end,"V\r\n");

    end = Meter_WriteText(end,"CH2 I ");
    end = Meter_WriteFixed(end,data->current_A,5U);
    end = Meter_WriteText(end,"A\r\n");

    end = Meter_WriteText(end,"CH2 P ");
    end = Meter_WriteFixed(end,data->power_W,3U);
    end = Meter_WriteText(end,"W\r\n");

    end = Meter_WriteText(end,"CH2 R ");
    end = Meter_WriteInt(end,(int32_t)adc_raw_avg);
    end = Meter_WriteText(end,"\r\n");
#else
    /* 紧凑单行：C2,<V>,<A>,<W> */
    *end++ = 'C';
    *end++ = '2';
    *end++ = ',';
    end = Meter_WriteFixed(end,data->voltage_V,3U);
    *end++ = ',';
    end = Meter_WriteFixed(end,data->current_A,5U);
    *end++ = ',';
    end = Meter_WriteFixed(end,data->power_W,3U);
    end = Meter_WriteText(end,"\r\n");
#endif
  }
  else
  {
#if (METER_FRAME_STYLE != 0U)
    end = Meter_WriteText(end,"CH2 ERR\r\n");
#else
    *end++ = 'C';
    *end++ = '2';
    end = Meter_WriteText(end,",ERR\r\n");
#endif
  }

  Meter_TxPublish(index,end);
}




/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
