/*
 * WS2812B 灯带控制器 —— ESP32-S3 (N16R8)
 * ---------------------------------------------------------------------------
 * 数据线接 GPIO 4，颜色顺序 GRB
 *
 * 【灯珠数量可以随时改，改完立刻生效，而且断电也记得】
 *   串口上发命令  n11  →  「11 颗」
 *                 n24  →  「24 颗」   （范围 1~300）
 *   桌面工具「灯带控制台」就是干这个的，不用重新编译。
 *
 * 【芯片的小限制怎么绕过去】
 *   ESP32-S3 的 RMT 硬件一次最多装下 4 块内存 = 192 个数据元素，
 *   而 1 颗灯珠要 24 个元素，所以一包最多 8 颗。
 *   这里按 6 颗一包连发。包与包之间只隔几微秒，
 *   而灯带要连续 50 微秒以上的低电平才会「锁存」，所以它察觉不到断点。
 *
 * 【接线】
 *   灯带 V+  → 5V 电源正极
 *   灯带 GND → 5V 电源负极（必须和开发板的 GND 接在一起，共地）
 *   灯带 DIN → 开发板 GPIO 4
 *
 * 【手机控制】板子自己开一个 Wi-Fi 热点，不用联网、不用装 App
 *   手机连 Wi-Fi「灯带遥控」，密码 12345678
 *   浏览器打开 http://192.168.4.1
 *   页面上能调：亮度、单色、灯效、倒计时关灯、每天定时开关、灯珠数量
 *
 * 【串口命令】115200 bps，每条命令后面要按回车
 *   n<数字>   灯珠数量，例：n11          （会永久保存）
 *   b<数字>   亮度 0~255，例：b110       （会永久保存）
 *   c<RRGGBB> 单色，例：cFF8000          （会永久保存）
 *   1~6       彩虹 / 扫描 / 呼吸 / 彗星 / 全白 / 整体变色（会永久保存）
 *   7         单色常亮     0 关灯
 *   t         自检：红 → 绿 → 蓝 → 白
 *   w         看 Wi-Fi 热点信息
 *   p<0~3>    数据波形档（排查闪烁用）p0 基准 / p1 慢速 / p2 高电平加长 / p3 半速
 *   d<0~3>    数据脚驱动电流档 5 / 10 / 20 / 40 mA
 *   ?         打印帮助
 */
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "web_page.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

// ============================== 配置区 ==============================
#define DATA_PIN        4      // 数据线接的 GPIO
#define MAX_LEDS        300    // 灯珠数量上限（真正用几颗随时用 n 命令改）
#define DEF_LEDS        11     // 出厂默认灯珠数量
#define DEF_BRIGHTNESS  110    // 出厂默认亮度 0~255
#define LEDS_PER_CHUNK  6      // 一包发几颗（芯片上限 8 颗，留点余量）
#define CMD_MAX         32     // 串口命令行最长多少个字符
#define AP_SSID         "灯带遥控"      // 手机要连的 Wi-Fi 名字
#define AP_PASS         "12345678"      // 手机要连的 Wi-Fi 密码
// ====================================================================

#define RMT_CH  RMT_CHANNEL_0

// ---------- 排查闪烁用的两个旋钮：波形档 / 驱动电流档 ----------
// 一格 = 25ns。t?h = 高电平格数，t?l = 低电平格数。
struct WaveProfile { uint8_t t0h, t0l, t1h, t1l; const char *name; };
static const WaveProfile gWaves[] = {
  { 14, 36, 28, 22, "P0 基准   800kHz  高电平 350/700ns" },   // 原来一直用的
  { 16, 44, 32, 28, "P1 慢速   667kHz  高电平 400/800ns" },
  { 18, 38, 36, 20, "P2 高电平加长 714kHz 高电平 450/900ns" },
  { 20, 52, 40, 32, "P3 半速   556kHz  高电平 500/1000ns" },
};
static const uint8_t      WAVE_COUNT  = sizeof(gWaves) / sizeof(gWaves[0]);
static uint8_t            gWave       = 0;
static uint8_t            gDrive      = 2;      // 0=5mA 1=10mA 2=20mA 3=40mA
static const char        *gDriveNames[4] = { "5mA", "10mA", "20mA", "40mA" };

static rmt_item32_t gItems[LEDS_PER_CHUNK * 24];
static uint8_t      gWire[MAX_LEDS * 3];    // 待发字节，已按 GRB 排好
static bool         gRmtReady = false;

// ---------- SPI 直发：整帧一口气发完，中间一道缝都没有 ----------
// 为什么加这条路线：RMT 那块硬件一包最多装 8 颗，74 颗得拆成 13 包，
// 包与包之间哪怕只空几十微秒，灯带也会当成「这一帧发完了」，
// 结果只有第一包（6 颗）能生效 —— 实测「只亮 6 颗」就是这么来的。
// 做法：拿 SPI 当"码流发生器"。1 个灯带位 = 4 个 SPI 位
//        0 → 1000      1 → 1100
// 时钟 3.2MHz（每个 SPI 位 312.5ns）算下来正好是标准的 800kHz 波形：
//        0 位 = 高312ns + 低937ns       1 位 = 高625ns + 低625ns
// 整帧（含帧尾低电平）丢给 DMA 一次发完，中间 CPU 忙别的也不影响。
#define SPITRAN_HOST         SPI2_HOST
#define SPI_BYTES_PER_LED    12      // 每颗 3 字节 × 每字节 4 个 SPI 字节
#define SPI_TAIL_BYTES       160     // 帧尾压 400 微秒低电平，让灯带锁存
// 帧头"起跑缓冲"：真正的数据前面先压这么多字节的低电平。
// SPI 起跑的那一下（DMA 供数之前）容易在数据线上留一个多余的高电平，
// 灯带会把它当成一个"1"。垫一段低电平让这一下落在缓冲里，吃不到真数据。
#define SPI_HEAD_BYTES       8
#define SPI_HEAD_MAX         64
// 数据速度：2.5MHz 时 0 位高 400 纳秒、1 位高 800 纳秒，正好是灯带手册的标称值。
// 3.2MHz 的 1 位只有 625 纳秒，比手册下限还短，边缘那颗容易认错。
#define SPI_KHZ_DEF          2500
#define SPI_KHZ_MIN          1500
#define SPI_KHZ_MAX          10000
#define SPI_REPEAT_MAX       5
static spi_device_handle_t gSpiDev    = nullptr;
static bool         gSpiReady = false;
static uint8_t      gEnc[256][4];        // 字节 → 4 个 SPI 字节（开机时生成）
static uint8_t     *gSpiBuf = nullptr;   // 编码后的整帧
static size_t       gSpiBufBytes = 0;
static uint8_t      gSpiHead = SPI_HEAD_BYTES;   // 帧头起跑缓冲长度（可调）
static uint16_t     gSpiKhz  = SPI_KHZ_DEF;      // 数据速度 kHz（可调）
static uint8_t      gRepeat  = 1;                // 一帧连发几遍（可调）
static bool         gSkipFirst = false;          // 首颗启动缓冲：先给最头上那颗一段静止时间，治串色（可调）
static bool         gSwapFirst = false;          // 第一颗的红/绿通道对调（可调）
// ---------- 用电保护：电流上限（多条灯带并联时用） ----------
// 0 = 不限制。设成数字后，每帧先估算要吃多少毫安，超了就整帧等比压暗。
// 只压暗、不改效果本身，所以画面不会变形，也绝不会把电源拉垮。
static uint16_t     gMaxMa    = 0;               // 电流上限（毫安），0 = 不限制
static uint16_t     gLastMa   = 0;               // 上一帧"本来要吃"的用电（毫安），只用来显示
static uint16_t     gLastMaOut = 0;              // 上一帧"实际发出去"的用电（毫安），压暗后就是它
static uint8_t      gTransport = 1;      // 1 = SPI 直发（默认）  0 = 老 RMT 分包
static uint16_t     gNumLeds  = DEF_LEDS;   // 当前灯珠数量（可改）
static uint8_t      gBright   = DEF_BRIGHTNESS;
static int          gChunk    = LEDS_PER_CHUNK;
static uint32_t     gLastPktEnd = 0;   // 上一包发完的时刻（微秒）
// 看病用的计数：包与包之间那条"缝隙"有多长。
// 灯带的规矩是：数据线上低电平超过约 250 微秒，它就认为"这一帧发完了"。
// 所以缝隙一旦超过这个数，半截数据就被当成一整帧，画面会错位一帧。
static uint32_t     gMaxGapUs = 0;     // 开机至今，最长的包间隔（微秒）
static uint32_t     gGapLatch = 0;     // 开机至今，超过 250 微秒的次数
static Preferences  gPrefs;
static WebServer    gServer(80);

// 单色模式用的颜色（也会存进 Flash）
static uint8_t gSolidR = 255, gSolidG = 128, gSolidB = 0;

// 定时相关
static uint32_t gOffAtMs    = 0;       // 倒计时关灯的时刻（millis），0 = 没开
static bool     gDailyOn    = false;   // 每天定时开关是否启用
static uint16_t gOnMin      = 19 * 60; // 每天几点开（0~1439，从 0:00 起算的分钟）
static uint16_t gOffMin     = 23 * 60; // 每天几点关
static bool     gTimeSynced = false;   // 手机有没有给我们对过时
static int64_t  gEpochBase  = 0;       // 对时基准（Unix 时间秒）
static uint32_t gMillisBase = 0;       // 对时时的 millis()
static int      gTzMin      = 480;     // 时区偏移（分钟），默认东八区 +8:00

// ------------------------------------------------------------ 小工具
static inline uint8_t scale8(uint8_t v, uint8_t s) {
  return (uint8_t)(((uint16_t)v * (uint16_t)s) / 255);
}

// HSV → RGB（纯整数运算）。色相用 16 位，段内还要插值，
// 这样 65536 级颜色是一格一格连续变的，不会跳色。
static void hsv2rgb16(uint16_t h, uint8_t s, uint8_t v,
                      uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s == 0) { r = g = b = v; return; }
  const uint16_t SEG = 10923;                       // 65536 ÷ 6，六个色段
  uint8_t region = (uint8_t)(h / SEG);
  if (region > 5) region = 5;
  uint8_t rem = (uint8_t)(((uint32_t)(h - (uint16_t)region * SEG) * 255) / SEG);
  uint8_t p = scale8(v, (uint8_t)(255 - s));
  uint8_t q = scale8(v, (uint8_t)(255 - scale8(s, rem)));
  uint8_t t = scale8(v, (uint8_t)(255 - scale8(s, (uint8_t)(255 - rem))));
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

// 老式 8 位色相入口（1 = 1/256 圈）
static inline void hsv2rgb(uint8_t h, uint8_t s, uint8_t v,
                           uint8_t &r, uint8_t &g, uint8_t &b) {
  hsv2rgb16((uint16_t)h * 257, s, v, r, g, b);
}

// 写一颗灯珠（自动套亮度，并按 GRB 排字节）
// 只做一次"乘完四舍五入"，不要攒小数做平均。
// 攒小数会让每颗粒在两档亮度之间来回跳，肉眼看到的就是高频闪烁。
static inline void putOne(int idx, uint8_t v) {
  gWire[idx] = (uint8_t)(((uint16_t)v * gBright + 127) / 255);   // +127 = 四舍五入
}

static void setLed(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= (int)gNumLeds) return;
  putOne(i * 3 + 0, g);     // WS2812B 要的是 G R B 这个顺序
  putOne(i * 3 + 1, r);
  putOne(i * 3 + 2, b);
}

static void fillAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < (int)gNumLeds; i++) setLed(i, r, g, b);
}

static void fadeAll(uint8_t keep) {
  for (int i = 0; i < (int)gNumLeds * 3; i++) gWire[i] = scale8(gWire[i], keep);
}

// ------------------------------------------------------------ RMT 驱动
static void applyDrive() {
  esp_err_t e = gpio_set_drive_capability((gpio_num_t)DATA_PIN, (gpio_drive_cap_t)gDrive);
  Serial.printf("[数据脚] 驱动电流 = %s（%s）\n",
                gDriveNames[gDrive], e == ESP_OK ? "设置成功" : "设置失败");
}

static bool rmtBegin() {
  rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)DATA_PIN, RMT_CH);
  cfg.clk_div       = 2;     // 80MHz ÷ 2 = 40MHz，一格 25ns
  cfg.mem_block_num = 4;     // 这块芯片单通道最多 4 块
  cfg.tx_config.carrier_en     = false;
  cfg.tx_config.loop_en        = false;
  cfg.tx_config.idle_output_en = true;
  cfg.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

  esp_err_t e1 = rmt_config(&cfg);
  esp_err_t e2 = rmt_driver_install(RMT_CH, 0, 0);
  Serial.printf("[RMT] 初始化 GPIO%d : config=%d  install=%d\n", DATA_PIN, e1, e2);
  applyDrive();          // 要在 RMT 配好之后再设，免得被它覆盖
  return (e1 == ESP_OK && e2 == ESP_OK);
}

// 老路线：RMT 分包发（留着做对照，命令 x0 切回来）
// 估算这一帧要吃多少电流，并算出"要不要压暗、压多暗"。
// 算法：WS2812B 每个通道满 255 约 20mA，所以 电流(mA) = 所有通道值之和 / 255 * 20。
// 返回 255 = 不用压；小于 255 = 整帧按这个系数等比压暗。
static uint8_t currentScale() {
  uint32_t sum = 0;
  const int n = (int)gNumLeds * 3;
  for (int i = 0; i < n; i++) sum += gWire[i];
  uint32_t ma = (sum * 20UL) / 255UL;
  if (ma > 60000UL) ma = 60000UL;
  gLastMa = (uint16_t)ma;
  if (gMaxMa == 0 || ma <= (uint32_t)gMaxMa) {
    gLastMaOut = (uint16_t)ma;
    return 255;
  }
  uint32_t s = ((uint32_t)gMaxMa * 255UL) / ma;
  if (s > 255UL) s = 255UL;
  gLastMaOut = (uint16_t)((ma * s) / 255UL);
  return (uint8_t)s;
}

static void sendFrameRmt() {
  if (!gRmtReady) return;

  const WaveProfile &w = gWaves[gWave];
  const uint8_t lim = currentScale();

  if (gSkipFirst) {                       // 先发一段静止低电平，稳住最边上那颗的振荡器
    for (int i = 0; i < 24; i++) {
      gItems[i].level0 = 1; gItems[i].duration0 = w.t0h;
      gItems[i].level1 = 0; gItems[i].duration1 = w.t0l;
    }
    rmt_write_items(RMT_CH, gItems, 24, true);
    rmt_wait_tx_done(RMT_CH, pdMS_TO_TICKS(50));
  }

  for (int sent = 0; sent < (int)gNumLeds; ) {
    int n = (int)gNumLeds - sent;
    if (n > gChunk) n = gChunk;

    int k = 0;
    for (int i = 0; i < n; i++) {
      const uint8_t *p = &gWire[(sent + i) * 3];
      for (int by = 0; by < 3; by++) {
        int src = by;
        if (gSwapFirst && (sent + i) == 0) {   // 第一颗：红、绿对调
          if      (by == 0) src = 1;
          else if (by == 1) src = 0;
        }
        uint8_t v = p[src];
        if (lim < 255) v = scale8(v, lim);
        for (int bit = 7; bit >= 0; bit--) {
          bool one = (v >> bit) & 1;
          gItems[k].level0    = 1;
          gItems[k].duration0 = one ? w.t1h : w.t0h;   // 高电平
          gItems[k].level1    = 0;
          gItems[k].duration1 = one ? w.t1l : w.t0l;   // 低电平
          k++;
        }
      }
    }

    // 量一下：上一包发完到现在，数据线低电平空了多久
    uint32_t tPkt = micros();
    if (sent > 0) {
      uint32_t gap = tPkt - gLastPktEnd;
      if (gap > gMaxGapUs) gMaxGapUs = gap;
      if (gap > 250)       gGapLatch++;
    }

    esp_err_t e = rmt_write_items(RMT_CH, gItems, k, true);
    if (e != ESP_OK) {
      Serial.printf("[RMT] 发包失败 err=%d（这一包 %d 颗），改小重试\n", e, n);
      if (gChunk > 1) { gChunk /= 2; continue; }
      break;
    }
    rmt_wait_tx_done(RMT_CH, pdMS_TO_TICKS(50));
    gLastPktEnd = micros();
    sent += n;
  }
  delayMicroseconds(320);   // 低电平保持 320µs，灯带才会锁存这一帧
}


// ------------------------------------------------------------ SPI 直发
// 开机时把「字节 → 4 个 SPI 字节」的表算好，发的时候只做搬运，省 CPU。
static void buildEncTable() {
  for (int v = 0; v < 256; v++) {
    for (int k = 0; k < 4; k++) {
      uint8_t outByte = 0;
      for (int b = 0; b < 8; b++) {
        int j  = k * 8 + b;        // 第几个输出位（0 = 最前面）
        int i  = j / 4;            // 对应第几个数据位（0 = 最高位）
        int ph = j % 4;            // 一个数据位摊成 4 个 SPI 位
        bool one = ((v >> (7 - i)) & 1) != 0;
        bool bit = (ph == 0) ? true : (ph == 1 ? one : false);
        if (bit) outByte |= (uint8_t)(0x80 >> b);
      }
      gEnc[v][k] = outByte;
    }
  }
}

static bool spiBegin() {
  if (gSpiBuf == nullptr) {
    gSpiBufBytes = (size_t)MAX_LEDS * SPI_BYTES_PER_LED + SPI_TAIL_BYTES + SPI_HEAD_MAX + SPI_BYTES_PER_LED;
    gSpiBuf = (uint8_t *)heap_caps_malloc(gSpiBufBytes, MALLOC_CAP_DMA);
    if (!gSpiBuf) { Serial.println("  ！内存不够，开不了 SPI 直发"); return false; }
  }

  spi_bus_config_t bus = {};
  bus.mosi_io_num     = DATA_PIN;
  bus.miso_io_num     = -1;
  bus.sclk_io_num     = -1;          // 不用时钟线，只借 MOSI 这一根
  bus.quadwp_io_num   = -1;
  bus.quadhd_io_num   = -1;
  bus.max_transfer_sz = (int)gSpiBufBytes;

  esp_err_t e1 = spi_bus_initialize(SPITRAN_HOST, &bus, SPI_DMA_CH_AUTO);
  if (e1 != ESP_OK) { Serial.printf("[SPI] 总线初始化失败 err=%d\n", (int)e1); return false; }

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = (int)gSpiKhz * 1000;
  dev.mode           = 0;
  dev.spics_io_num   = -1;
  dev.queue_size     = 1;

  esp_err_t e2 = spi_bus_add_device(SPITRAN_HOST, &dev, &gSpiDev);
  if (e2 != ESP_OK) {
    Serial.printf("[SPI] 设备注册失败 err=%d\n", (int)e2);
    spi_bus_free(SPITRAN_HOST);
    return false;
  }
  applyDrive();
  Serial.printf("[SPI] 直发就绪：GPIO%d，数据速度 %u kHz（0 位高 %.0f 纳秒 / 1 位高 %.0f 纳秒）\n",
                DATA_PIN, gSpiKhz, 1000000.0 / gSpiKhz, 2000000.0 / gSpiKhz);
  return true;
}

static void sendFrameSpi() {
  if (!gSpiReady || !gSpiBuf) return;
  const uint8_t lim = currentScale();

  // 帧头先垫一段低电平，把 SPI 起跑那一下的毛刺吃掉。
  memset(gSpiBuf, 0, gSpiHead);
  size_t k = gSpiHead;

  // 首颗启动缓冲：在真数据前面垫一段静止低电平，让最头上那颗芯片的振荡器先稳下来。
  // 注意它不吃灯珠 —— 画面依然从第一颗开始，一颗都不会变暗。
  if (gSkipFirst) { memset(gSpiBuf + k, 0, SPI_BYTES_PER_LED); k += SPI_BYTES_PER_LED; }

  int bytes = (int)gNumLeds * 3;
  for (int i = 0; i < bytes; i++) {
    int src = i;
    if (gSwapFirst && i < 3) {            // 第一颗：红、绿对调
      if      (i == 0) src = 1;
      else if (i == 1) src = 0;
    }
    uint8_t v = gWire[src];
    if (lim < 255) v = scale8(v, lim);
    const uint8_t *e = gEnc[v];
    gSpiBuf[k++] = e[0];
    gSpiBuf[k++] = e[1];
    gSpiBuf[k++] = e[2];
    gSpiBuf[k++] = e[3];
  }
  memset(gSpiBuf + k, 0, SPI_TAIL_BYTES);   // 帧尾低电平 → 灯带锁存
  k += SPI_TAIL_BYTES;

  // 同一帧连发几遍：哪一遍被边上那颗认歪了，后面一遍会把它盖回来。
  int times = (int)gRepeat;
  if (times < 1) times = 1;
  for (int n = 0; n < times; n++) {
    spi_transaction_t t = {};
    t.length    = k * 8;
    t.tx_buffer = gSpiBuf;
    esp_err_t e = spi_device_transmit(gSpiDev, &t);
    if (e != ESP_OK) { Serial.printf("[SPI] 发送失败 err=%d\n", (int)e); return; }
  }
}

// 在线换数据速度。灯带对"1 位有多长"最敏感，慢一点往往就认准了。
static void setSpiKhz(int khz) {
  if (khz < SPI_KHZ_MIN) khz = SPI_KHZ_MIN;
  if (khz > SPI_KHZ_MAX) khz = SPI_KHZ_MAX;
  gSpiKhz = (uint16_t)khz;

  if (gSpiReady && gSpiDev) {
    spi_bus_remove_device(gSpiDev);
    gSpiDev = nullptr;
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = khz * 1000;
    dev.mode           = 0;
    dev.spics_io_num   = -1;
    dev.queue_size     = 1;
    esp_err_t e = spi_bus_add_device(SPITRAN_HOST, &dev, &gSpiDev);
    if (e != ESP_OK) { Serial.printf("！换速度失败 err=%d\n", (int)e); return; }
  }
  Serial.printf(">> 数据速度 = %d kHz（1 位高 %.0f 纳秒，标称 800）\n",
                khz, 2000000.0 / khz);
}

// 把当前这一帧发出去（按当前选的发送方式）
static void sendFrame() {
  if (gTransport == 1) { if (gSpiReady) sendFrameSpi(); }
  else                 { sendFrameRmt(); }
}

// 两种发送方式互相切换：x1 = SPI 直发（默认）  x0 = 老 RMT 分包（对照用）
static void selectTransport(uint8_t t) {
  t = (t == 0) ? 0 : 1;
  if (t == 1) {
    if (gTransport == 1 && gSpiReady) { Serial.println("[发送方式] 已经在用 SPI 直发"); return; }
    if (gRmtReady) { rmt_driver_uninstall(RMT_CH); gRmtReady = false; }
    gpio_reset_pin((gpio_num_t)DATA_PIN);
    gSpiReady  = spiBegin();
    gTransport = gSpiReady ? 1 : 0;
  } else {
    if (gTransport == 0 && gRmtReady) { Serial.println("[发送方式] 已经在用 RMT 分包"); return; }
    if (gSpiReady) {
      if (gSpiDev) { spi_bus_remove_device(gSpiDev); gSpiDev = nullptr; }
      spi_bus_free(SPITRAN_HOST);
      gSpiReady = false;
    }
    gpio_reset_pin((gpio_num_t)DATA_PIN);
    gRmtReady  = rmtBegin();
    gTransport = gRmtReady ? 0 : 1;
  }
  Serial.printf("[发送方式] 现在用 %s\n",
                gTransport == 1 ? "SPI 直发（整帧无缝隙）" : "老 RMT 分包（每 6 颗一道缝）");
}

// ------------------------------------------------------------ 灯效
enum Effect : uint8_t {
  FX_RAINBOW, FX_WIPE, FX_BREATHE, FX_COMET, FX_WHITE, FX_CHASE, FX_OFF, FX_SOLID,
  FX_SOLID_BREATHE, FX_RAINBOW_BREATHE, FX_THEATER, FX_DUO_CHASE, FX_METEOR,
  FX_TWINKLE, FX_FIRE, FX_POLICE, FX_WAVE, FX_ZEBRA, FX_REF, FX_SINGLE, FX_COUNT
};

static Effect  gEffect   = FX_RAINBOW;
static Effect  gLastFx   = FX_RAINBOW;   // 关灯前是什么灯效，定时开灯时接着放
static uint16_t gHue16   = 0;   // 16 位色相：65536 = 转满一圈
static int16_t gWipe     = -1;
static uint16_t gWipeHue16 = 0;
static uint8_t gWipeHold = 0;
static int16_t gComet    = 0;
static int8_t  gCometDir = 1;
static uint8_t gCometTk  = 0;
static uint8_t gBreathPh = 0;
static bool    gBreathWrap = false;              // 这一帧刚好走完一轮（给换色用）
// 呼吸快慢：1 最慢（约 4.1 秒一次），8 最快。每帧把相位往前推多少就看它。
static uint8_t gBreathSpeed = 1;
static const uint8_t kBreathStep[9]  = {0, 1, 2, 3, 4, 6, 8, 12, 16};
static const float   kBreathSec[9]   = {0, 4.1f, 2.05f, 1.37f, 1.02f, 0.68f, 0.51f, 0.34f, 0.26f};
static uint8_t gTheaterPos = 0;
static uint8_t gTheaterTk  = 0;
static int16_t gDuoOff     = 0;
static uint8_t gDuoTk      = 0;
static uint8_t gHeat[MAX_LEDS];        // 篝火用的热量

static const char *fxName(Effect e) {
  switch (e) {
    case FX_RAINBOW: return "彩虹流动";
    case FX_WIPE:    return "逐颗扫描";
    case FX_BREATHE: return "整条呼吸";
    case FX_COMET:   return "彗星来回";
    case FX_WHITE:   return "全白长亮";
    case FX_CHASE:   return "整体变色";
    case FX_OFF:     return "关灯";
    case FX_SOLID:   return "纯色常亮";
    case FX_SOLID_BREATHE:   return "纯色呼吸";
    case FX_RAINBOW_BREATHE: return "彩虹呼吸";
    case FX_THEATER:         return "跑马灯";
    case FX_DUO_CHASE:       return "双色追逐";
    case FX_METEOR:          return "彩虹流星";
    case FX_TWINKLE:         return "星星闪烁";
    case FX_FIRE:            return "篝火";
    case FX_POLICE:          return "警灯";
    case FX_WAVE:            return "彩虹波浪";
    case FX_ZEBRA:           return "斑马纹(排查用)";
    case FX_REF:             return "参考灯(排查用)";
    case FX_SINGLE:          return "单颗定位(排查用)";
    default:         return "?";
  }
}

static void fxRainbow() {
  for (int i = 0; i < (int)gNumLeds; i++) {
    uint8_t r, g, b;
    hsv2rgb16((uint16_t)(gHue16 + (uint32_t)i * 65536UL / gNumLeds),
              255, 255, r, g, b);
    setLed(i, r, g, b);
  }
  // 每帧转一点点。136/65536 ≈ 0.2% 圈，60 帧/秒 → 约 8 秒走完一圈。
  // 这个速度下，一颗粒子每帧正好变 1 级，是 8 位灯带最顺的档位。
  gHue16 += 136;
}

static void fxWipe() {
  static uint32_t last = 0;
  uint32_t now = millis();

  if (gWipe < 0) {
    fillAll(0, 0, 0);
    gWipe = 0;
    gWipeHold = 0;
    last = now;
    return;
  }
  if (gWipe < (int)gNumLeds) {
    if (now - last < 55) return;
    last = now;
    uint8_t r, g, b;
    hsv2rgb16(gWipeHue16, 255, 255, r, g, b);
    setLed(gWipe++, r, g, b);
  } else {
    if (!gWipeHold) { gWipeHold = 1; last = now; return; }
    if (now - last < 900) return;
    gWipeHue16 += 10800;      // 换下一个颜色（≈42/256 圈）
    gWipe = -1;
  }
}

static uint8_t breathLevel();   // 下面才定义，这里先打个招呼

static void fxBreathe() {
  uint8_t v = breathLevel();
  uint8_t r, g, b;
  hsv2rgb16(gHue16, 210, v, r, g, b);
  fillAll(r, g, b);
  if (gBreathWrap) gHue16 += 5000;      // 一轮走完才换下一种颜色
}

static void fxComet() {
  fadeAll(170);
  if (++gCometTk >= 2) {
    gCometTk = 0;
    gComet += gCometDir;
    if (gComet >= (int)gNumLeds) { gComet = (int)gNumLeds - 2; gCometDir = -1; }
    else if (gComet < 0)         { gComet = 1;                 gCometDir = 1;  }
  }
  uint8_t r, g, b;
  hsv2rgb16(gHue16, 255, 255, r, g, b);
  setLed(gComet, r, g, b);
  gHue16 += 400;
}

static void fxWhite() { fillAll(255, 255, 255); }

static void fxChase() {
  uint8_t r, g, b;
  hsv2rgb16(gHue16, 255, 255, r, g, b);
  fillAll(r, g, b);
  gHue16 += 260;
}

static void fxOff() { fillAll(0, 0, 0); }

static void fxSolid() { fillAll(gSolidR, gSolidG, gSolidB); }

static void fillAllScaled(uint8_t r, uint8_t g, uint8_t b, uint8_t v) {
  fillAll(scale8(r, v), scale8(g, v), scale8(b, v));
}

// 呼吸用的波形：0 → 255 → 0。
// 先走一个三角波，再过一遍"两头放缓"的曲线（两头斜率为 0），
// 所以最亮和最暗的地方都是软着陆，不会一顿一顿的。
static uint8_t breathLevel() {
  uint8_t sp = gBreathSpeed;
  if (sp < 1) sp = 1;
  if (sp > 8) sp = 8;
  uint16_t next = (uint16_t)gBreathPh + kBreathStep[sp];
  gBreathWrap = (next >= 256);          // 这一帧跨过了起点 = 一轮结束
  gBreathPh = (uint8_t)next;

  uint8_t t = (gBreathPh < 128) ? (uint8_t)(gBreathPh * 2)
                                : (uint8_t)((255 - gBreathPh) * 2);
  // 两头放缓：t=0 和 t=255 处斜率为 0，画面到顶/到底都是软着陆
  return (uint8_t)(((uint32_t)t * t * (765UL - 2UL * (uint32_t)t)) / 65025UL);
}

static void fxSolidBreathe() {
  fillAllScaled(gSolidR, gSolidG, gSolidB, breathLevel());
}

static void fxRainbowBreathe() {
  uint8_t v = breathLevel();
  for (int i = 0; i < (int)gNumLeds; i++) {
    uint8_t r, g, b;
    hsv2rgb16((uint16_t)(gHue16 + (uint32_t)i * 65536UL / gNumLeds), 255, v, r, g, b);
    setLed(i, r, g, b);
  }
  gHue16 += 128;
}

// 跑马灯：每 3 颗亮 1 颗，整段往前跑
static void fxTheater() {
  uint8_t r, g, b;
  hsv2rgb16(gHue16, 255, 255, r, g, b);
  for (int i = 0; i < (int)gNumLeds; i++) {
    if (((i + gTheaterPos) % 3) == 0) setLed(i, r, g, b);
    else                              setLed(i, 0, 0, 0);
  }
  if (++gTheaterTk >= 5) { gTheaterTk = 0; gTheaterPos = (uint8_t)((gTheaterPos + 1) % 3); }
  gHue16 += 500;
}

// 双色追逐：你选的颜色 和 它的反色 交替成块往前跑
static void fxDuoChase() {
  for (int i = 0; i < (int)gNumLeds; i++) {
    if (((i + gDuoOff) / 6) % 2) setLed(i, (uint8_t)(255 - gSolidR),
                                           (uint8_t)(255 - gSolidG),
                                           (uint8_t)(255 - gSolidB));
    else                         setLed(i, gSolidR, gSolidG, gSolidB);
  }
  if (++gDuoTk >= 4) { gDuoTk = 0; gDuoOff = (int16_t)((gDuoOff + 1) % 12); }
}

// 彩虹流星：3 颗流星拖着尾巴跑，颜色各自不同
static void fxMeteor() {
  fadeAll(205);
  for (int k = 0; k < 3; k++) {
    int pos = (int)((millis() / 35 + (uint32_t)k * (gNumLeds / 3 + 1)) % gNumLeds);
    uint8_t r, g, b;
    hsv2rgb16((uint16_t)(gHue16 + k * 21845), 255, 255, r, g, b);
    setLed(pos, r, g, b);
  }
  gHue16 += 400;
}

// 星星闪烁：随机几颗亮一下，然后慢慢暗下去
static void fxTwinkle() {
  fadeAll(218);
  int n = (int)gNumLeds;
  for (int k = 0; k < 2; k++) {
    int i = (int)random(n);
    uint8_t r, g, b;
    hsv2rgb16((uint16_t)random(65536), 200, 255, r, g, b);
    setLed(i, r, g, b);
  }
}

// 篝火：底下不断冒火苗，往上飘、逐渐变暗
static void fxFire() {
  int n = (int)gNumLeds;
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)random(0, 14);
    gHeat[i] = (gHeat[i] > c) ? (uint8_t)(gHeat[i] - c) : 0;
  }
  for (int i = n - 1; i >= 2; i--) {
    gHeat[i] = (uint8_t)(((uint16_t)gHeat[i - 1] + gHeat[i - 2] + gHeat[i - 2]) / 3);
  }
  int sparks = 1 + n / 16;
  for (int s = 0; s < sparks; s++) {
    int i = n - 1 - (int)random(0, 3);
    if (i < 0) i = 0;
    int v = (int)gHeat[i] + (int)random(60, 140);
    gHeat[i] = (v > 255) ? 255 : (uint8_t)v;
  }
  for (int i = 0; i < n; i++) {
    uint8_t h = gHeat[i];
    uint8_t r, g, b;
    if      (h < 85)  { r = (uint8_t)(h * 3);                 g = 0;                      b = 0; }
    else if (h < 170) { r = 255;                              g = (uint8_t)((h - 85) * 3); b = 0; }
    else              { r = 255;                              g = 255;   b = (uint8_t)((h - 170) * 3 / 2); }
    setLed(i, r, g, b);
  }
}

// 警灯：红蓝轮流闪
static void fxPolice() {
  static uint32_t last = 0;
  static uint8_t  phase = 0;
  uint32_t now = millis();
  if (now - last >= 240) { last = now; phase ^= 1; }
  if (phase) fillAll(255, 0, 0);
  else       fillAll(0, 0, 255);
}

// 彩虹波浪：颜色流动 + 亮度像水波一样起伏
static void fxWave() {
  float t = (float)millis() * 0.006f;
  for (int i = 0; i < (int)gNumLeds; i++) {
    int v = 120 + (int)(125.0f * sinf((float)i * 0.55f - t));
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    uint8_t r, g, b;
    hsv2rgb16((uint16_t)(gHue16 + (uint32_t)i * 1028), 255, (uint8_t)v, r, g, b);
    setLed(i, r, g, b);
  }
  gHue16 += 200;
}

// 排查用：静态斑马纹。每 4 颗亮 1 颗、其余全黑，而且每一帧发出去的数据
// 一模一样（完全不动）。黑的那些灯珠如果还会闪，就跟灯效算法无关了。
// 排查用：前四颗固定成 红/绿/蓝/白，其余全灭。不吃亮度，方便认色。
static void refOne(int idx, uint8_t r, uint8_t g, uint8_t b) {
  if (idx < 0 || idx >= (int)gNumLeds) return;
  gWire[idx * 3 + 0] = g;
  gWire[idx * 3 + 1] = r;
  gWire[idx * 3 + 2] = b;
}

static void fxRef() {
  for (int i = 0; i < (int)gNumLeds; i++) setLed(i, 0, 0, 0);
  refOne(0, 255,   0,   0);   // 第 1 颗 = 红
  refOne(1,   0, 255,   0);   // 第 2 颗 = 绿
  refOne(2,   0,   0, 255);   // 第 3 颗 = 蓝
  refOne(3, 255, 255, 255);   // 第 4 颗 = 白
}

// 排查用：只点亮一颗（默认第 1 颗），红色，其余全灭。
static int     gSingleIdx = 0;
static uint8_t gSingleR = 255, gSingleG = 0, gSingleB = 0;   // 单颗定位用的颜色（命令 a 改）

static void fxSingle() {
  for (int i = 0; i < (int)gNumLeds; i++) setLed(i, 0, 0, 0);
  refOne(gSingleIdx, gSingleR, gSingleG, gSingleB);
}

static void fxZebra() {
  for (int i = 0; i < (int)gNumLeds; i++) {
    if ((i % 4) == 0) setLed(i, 255, 255, 255);
    else              setLed(i, 0, 0, 0);
  }
}

static void renderFrame() {
  switch (gEffect) {
    case FX_RAINBOW: fxRainbow(); break;
    case FX_WIPE:    fxWipe();    break;
    case FX_BREATHE: fxBreathe(); break;
    case FX_COMET:   fxComet();   break;
    case FX_WHITE:   fxWhite();   break;
    case FX_CHASE:   fxChase();   break;
    case FX_SOLID:   fxSolid();   break;
    case FX_SOLID_BREATHE:   fxSolidBreathe();   break;
    case FX_RAINBOW_BREATHE: fxRainbowBreathe(); break;
    case FX_THEATER:         fxTheater();        break;
    case FX_DUO_CHASE:       fxDuoChase();       break;
    case FX_METEOR:          fxMeteor();         break;
    case FX_TWINKLE:         fxTwinkle();        break;
    case FX_FIRE:            fxFire();           break;
    case FX_POLICE:          fxPolice();         break;
    case FX_WAVE:            fxWave();           break;
    case FX_ZEBRA:           fxZebra();          break;
    case FX_REF:             fxRef();            break;
    case FX_SINGLE:          fxSingle();         break;
    case FX_OFF:     fxOff();     break;
    default: break;
  }
  sendFrame();
}

static void setEffect(Effect e, bool save = true) {
  if (e != FX_OFF) gLastFx = e;
  gEffect   = e;
  gHue16    = 0;
  gWipe     = -1;
  gWipeHue16 = 0;
  gWipeHold = 0;
  gComet    = 0;
  gCometDir = 1;
  gCometTk  = 0;
  gBreathPh = 0;
  gTheaterPos = 0;
  gTheaterTk  = 0;
  gDuoOff     = 0;
  gDuoTk      = 0;
  memset(gHeat, 0, sizeof(gHeat));
  if (save) gPrefs.putUChar("fx", (uint8_t)e);
  fillAll(0, 0, 0);
  sendFrame();
}

// ------------------------------------------------------------ 自检
static void selfTest() {
  const char   *names[4]     = { "红", "绿", "蓝", "白" };
  const uint8_t colors[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,255} };
  for (int i = 0; i < 4; i++) {
    Serial.printf("  自检：%u 颗一起亮【%s】\n", gNumLeds, names[i]);
    fillAll(colors[i][0], colors[i][1], colors[i][2]);
    sendFrame();
    delay(400);
  }
  fillAll(0, 0, 0);
  sendFrame();
  delay(120);
}

// ------------------------------------------------------------ 串口命令
static void printWifiInfo();

static void printHelp() {
  Serial.println();
  Serial.println("=========== WS2812B 控制台 ===========");
  Serial.println("  n<数字>  灯珠数量，例 n11   （永久保存）");
  Serial.println("  b<数字>  亮度 0~255，例 b110（永久保存）");
  Serial.println("  c<RRGGBB> 单色，例 cFF8000   （永久保存）");
  Serial.println("  f<0~16>  直接选灯效，例 f8 （永久保存）");
  Serial.println("  快捷：1 彩虹流动  2 逐颗扫描  3 整条呼吸  4 彗星来回");
  Serial.println("        5 全白长亮  6 整体变色  7 纯色常亮  0 关灯");
  Serial.println("  更多：8 纯色呼吸   9 彩虹呼吸  10 跑马灯   11 双色追逐");
  Serial.println("        12 彩虹流星 13 星星闪烁  14 篝火     15 警灯");
  Serial.println("        16 彩虹波浪");
  Serial.println("        17 斑马纹（排查闪烁用，画面完全不动）");
  Serial.println("        18 参考灯（前四颗=红绿蓝白，排查颜色用）");
  Serial.println("  z<数字>  只点亮第几颗，排查定位用，例 z0");
  Serial.println("  a<RRGGBB> 改 z 用的颜色，例 a00FF00");
  Serial.println("  t 自检   w Wi-Fi 热点   ? 显示本帮助");
  Serial.printf("  当前：效果=%s  亮度=%u  灯珠=%u 颗  数据脚=GPIO%d\n",
                fxName(gEffect), gBright, gNumLeds, DATA_PIN);
  Serial.printf("  波形档=%s   数据脚驱动=%s\n", gWaves[gWave].name, gDriveNames[gDrive]);
  Serial.println("  发送方式：x1 = SPI 直发（默认，整帧一次发完、无缝隙）");
  Serial.println("            x0 = 老 RMT 分包（每 6 颗一道缝，只做对照用）");
  Serial.println("  h<0~64>  SPI 帧头起跑缓冲长度（治第一颗灯颜色不对）");
  Serial.printf("  y<1500~10000> 数据速度 kHz，当前 %u（慢一点更稳，默认 2500）\n", gSpiKhz);
  Serial.printf("  r<1~5>   同一帧连发几遍，当前 %u\n", (unsigned)gRepeat);
  Serial.printf("  k<0/1>   首颗启动缓冲，治最头上那颗串色（不吃灯珠），当前 %s\n", gSkipFirst ? "开" : "关");
  Serial.printf("  g<0/1>   第一颗红绿对调（整条红、第一颗发绿就开它），当前 %s\n", gSwapFirst ? "开" : "关");
  if (gMaxMa == 0)
    Serial.println("  m<毫安>  电流上限，例 m8000；0 = 不限制。多条灯带并联时设它保护电源。当前 不限制");
  else
    Serial.printf("  m<毫安>  电流上限，例 m8000；0 = 不限制。多条灯带并联时设它保护电源。当前 %u 毫安\n", gMaxMa);
  Serial.printf("  s<1~8>   呼吸快慢，s1 最慢（约 4.1 秒一次），s8 最快。当前 %u（约 %.2f 秒一次）\n",
                (unsigned)gBreathSpeed, kBreathSec[gBreathSpeed]);
  Serial.println("  其它：d0~d3 换驱动电流   p0~p3 只在老 RMT 方式下有用");
  Serial.printf("  当前颜色=%02X%02X%02X  手机热点=%s  每天定时=%s\n",
                gSolidR, gSolidG, gSolidB, AP_SSID, gDailyOn ? "开" : "关");
  Serial.println("======================================");
}

static void setLedCount(int v) {
  if (v < 1 || v > MAX_LEDS) {
    Serial.printf("！灯珠数量要在 1~%d 之间\n", MAX_LEDS);
    return;
  }
  gNumLeds = (uint16_t)v;
  gPrefs.putUShort("n", gNumLeds);
  gWipe  = -1;
  gComet = 0;
  fillAll(0, 0, 0);
  sendFrame();
  Serial.printf(">> 灯珠数量 = %u 颗（已保存）\n", gNumLeds);
}

static void setBrightness(int v) {
  if (v < 0)   v = 0;
  if (v > 255) v = 255;
  gBright = (uint8_t)v;
  gPrefs.putUChar("b", gBright);
  Serial.printf(">> 亮度 = %u（已保存）\n", gBright);
}

// 波形档：查闪烁用，改完立刻生效
static void setWave(int v) {
  if (v < 0 || v >= (int)WAVE_COUNT) {
    Serial.printf("！波形档要 0~%d\n", (int)WAVE_COUNT - 1);
    return;
  }
  gWave = (uint8_t)v;
  gPrefs.putUChar("wp", gWave);
  Serial.printf(">> 波形档：%s（已保存）\n", gWaves[gWave].name);
}

// 数据脚驱动电流档：查闪烁用
static void setDrive(int v) {
  if (v < 0 || v > 3) { Serial.println("！驱动档要 0~3（5 / 10 / 20 / 40 mA）"); return; }
  gDrive = (uint8_t)v;
  gPrefs.putUChar("dv", gDrive);
  applyDrive();
}

// "FF8000" → 三个字节
static bool parseHex6(const char *s, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (!s || strlen(s) != 6) return false;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  long v = strtol(s, nullptr, 16);
  r = (uint8_t)((v >> 16) & 0xFF);
  g = (uint8_t)((v >> 8)  & 0xFF);
  b = (uint8_t)(v & 0xFF);
  return true;
}

static void setSolidColor(uint8_t r, uint8_t g, uint8_t b, bool save = true) {
  gSolidR = r; gSolidG = g; gSolidB = b;
  if (save) {
    gPrefs.putUChar("cr", r);
    gPrefs.putUChar("cg", g);
    gPrefs.putUChar("cb", b);
  }
  setEffect(FX_SOLID, true);            // 选颜色 = 直接切成纯色常亮
  Serial.printf(">> 颜色 = %02X%02X%02X（已保存）\n", r, g, b);
}

static void applyCommand(char *s) {
  while (*s == ' ') s++;
  int len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\r')) s[--len] = 0;
  if (len == 0) return;

  char c0 = s[0];

  if (c0 == 'n' || c0 == 'N') { setLedCount(atoi(s + 1)); return; }
  if (c0 == 'b' || c0 == 'B') { setBrightness(atoi(s + 1)); return; }
  if (c0 == 'c' || c0 == 'C') {
    uint8_t r, g, b;
    if (parseHex6(s + 1, r, g, b)) setSolidColor(r, g, b);
    else Serial.println("！颜色要写成 cRRGGBB，例：cFF8000");
    return;
  }
  if (c0 == 'f' || c0 == 'F') {
    int id = atoi(s + 1);
    if (id >= 0 && id < (int)FX_COUNT) {
      setEffect((Effect)id);
      Serial.printf(">> 效果：%s\n", fxName(gEffect));
    } else {
      Serial.printf("！灯效编号要在 0~%d 之间\n", (int)FX_COUNT - 1);
    }
    return;
  }
  if (c0 == 'w' || c0 == 'W') { printWifiInfo(); return; }
  if (c0 == 's' || c0 == 'S') {          // s<1~8> 呼吸快慢，1 最慢
    int v = atoi(s + 1);
    if (v < 1 || v > 8) { Serial.println("！呼吸快慢要写 1~8，例：s1（1 最慢，8 最快）"); return; }
    gBreathSpeed = (uint8_t)v;
    gPrefs.putUChar("bs", gBreathSpeed);
    Serial.printf(">> 呼吸快慢 = %d（一次约 %.2f 秒，已保存）\n", v, kBreathSec[v]);
    return;
  }
  if (c0 == 'p' || c0 == 'P') { setWave(atoi(s + 1));  return; }
  if (c0 == 'd' || c0 == 'D') { setDrive(atoi(s + 1)); return; }
  if (c0 == 'x' || c0 == 'X') { selectTransport(atoi(s + 1)); return; }
  if (c0 == 'h' || c0 == 'H') {
    int v = atoi(s + 1);
    if (v < 0 || v > SPI_HEAD_MAX) {
      Serial.printf("！起跑缓冲要在 0~%d 字节之间\n", SPI_HEAD_MAX);
      return;
    }
    gSpiHead = (uint8_t)v;
    Serial.printf(">> 起跑缓冲 = %d 字节（约 %.0f 微秒低电平）\n",
                  (int)gSpiHead, gSpiHead * 8.0 * 1000.0 / gSpiKhz);
    return;
  }
  if (c0 == 'y' || c0 == 'Y') { setSpiKhz(atoi(s + 1)); return; }
  if (c0 == 'r' || c0 == 'R') {
    int v = atoi(s + 1);
    if (v < 1 || v > SPI_REPEAT_MAX) {
      Serial.printf("！连发遍数要在 1~%d 之间\n", SPI_REPEAT_MAX);
      return;
    }
    gRepeat = (uint8_t)v;
    Serial.printf(">> 一帧连发 %d 遍\n", (int)gRepeat);
    return;
  }
  if (c0 == 'z' || c0 == 'Z') {
    int v = atoi(s + 1);
    if (v < 0) v = 0;
    if (v > (int)gNumLeds - 1) v = (int)gNumLeds - 1;
    gSingleIdx = v;
    setEffect(FX_SINGLE, false);
    Serial.printf(">> 只点亮第 %d 颗（红色）\n", gSingleIdx + 1);
    return;
  }
  if (c0 == 'a' || c0 == 'A') {          // a<RRGGBB> 换"单颗定位"的颜色，不动其它设置
    uint8_t r, g, b;
    if (!parseHex6(s + 1, r, g, b)) { Serial.println("！颜色要写成 aRRGGBB，例：a00FF00"); return; }
    gSingleR = r; gSingleG = g; gSingleB = b;
    Serial.printf(">> 单颗定位颜色 = %02X%02X%02X\n", r, g, b);
    return;
  }
  if (c0 == 'g' || c0 == 'G') {
    gSwapFirst = (atoi(s + 1) != 0);
    gPrefs.putUChar("sf", gSwapFirst ? 1 : 0);
    Serial.printf(">> 第一颗红绿对调：%s（已保存）\n", gSwapFirst ? "开" : "关");
    return;
  }
  if (c0 == 'k' || c0 == 'K') {
    gSkipFirst = (atoi(s + 1) != 0);
    gPrefs.putUChar("sk", gSkipFirst ? 1 : 0);
    Serial.printf(">> 首颗启动缓冲：%s（已保存）\n", gSkipFirst ? "开" : "关");
    return;
  }
  if (c0 == 'm' || c0 == 'M') {          // m<毫安数>，0 = 不限制
    long v = atol(s + 1);
    if (v < 0) v = 0;
    if (v > 60000L) v = 60000L;
    gMaxMa = (uint16_t)v;
    gPrefs.putUShort("ma", gMaxMa);
    if (gMaxMa == 0)
      Serial.println(">> 电流上限：不限制（已保存）");
    else
      Serial.printf(">> 电流上限 = %u 毫安（已保存），超了自动整条压暗\n", gMaxMa);
    return;
  }

  if (len == 1) {
    switch (c0) {
      case '1': setEffect(FX_RAINBOW); Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '2': setEffect(FX_WIPE);    Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '3': setEffect(FX_BREATHE); Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '4': setEffect(FX_COMET);   Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '5': setEffect(FX_WHITE);   Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '6': setEffect(FX_CHASE);   Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '7': setEffect(FX_SOLID);   Serial.printf(">> 效果：%s\n", fxName(gEffect)); return;
      case '0': setEffect(FX_OFF);     Serial.println(">> 关灯");                      return;
      case 't': case 'T': selfTest();  return;
      case '?': printHelp();           return;
      default: break;
    }
  }
  if (strcmp(s, "test") == 0) { selfTest();  return; }
  if (strcmp(s, "help") == 0) { printHelp(); return; }

  Serial.printf("！看不懂这条命令：%s  （发 ? 看帮助）\n", s);
}

static void handleSerial() {
  static char    buf[CMD_MAX];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      buf[len] = 0;
      if (len > 0) applyCommand(buf);
      len = 0;
      continue;
    }
    if (len < CMD_MAX - 1) buf[len++] = c;
    else                   len = 0;      // 太长就当乱码丢掉
  }
}

// ------------------------------------------------------------ 定时
static int64_t nowEpoch() {
  return gEpochBase + (int64_t)((millis() - gMillisBase) / 1000);
}

static int localMinuteOfDay() {
  int64_t local = nowEpoch() + (int64_t)gTzMin * 60;
  int m = (int)((local / 60) % 1440);
  if (m < 0) m += 1440;
  return m;
}

static void saveDaily() {
  gPrefs.putUChar("dEn", gDailyOn ? 1 : 0);
  gPrefs.putUShort("dOn", gOnMin);
  gPrefs.putUShort("dOff", gOffMin);
  gPrefs.putInt("tz", gTzMin);
  Serial.printf(">> 每天定时：%s  %02u:%02u 开  %02u:%02u 关\n",
                gDailyOn ? "已启用" : "已关闭",
                gOnMin / 60, gOnMin % 60, gOffMin / 60, gOffMin % 60);
}

// 从 from 分钟走到 to 分钟，有没有经过 target 这一分钟（跨 0 点也算）
static bool minCrossed(int from, int to, int target) {
  if (from < 0) return false;                 // 刚开机/刚对时，先不触发
  if (from <= to) return (from < target && target <= to);
  return (target > from) || (target <= to);
}

static void dailyTick() {
  static int lastMin = -1;
  if (!gDailyOn || !gTimeSynced) { lastMin = -1; return; }

  int m = localMinuteOfDay();
  if (m == lastMin) return;

  if (minCrossed(lastMin, m, (int)gOnMin) && gEffect == FX_OFF) {
    Serial.println(">> 每天定时：到开灯时间了");
    setEffect(gLastFx, true);
  }
  if (minCrossed(lastMin, m, (int)gOffMin) && gEffect != FX_OFF) {
    Serial.println(">> 每天定时：到关灯时间了");
    setEffect(FX_OFF, true);
  }
  lastMin = m;
}

static void timerTick() {
  if (gOffAtMs && (int32_t)(millis() - gOffAtMs) >= 0) {
    gOffAtMs = 0;
    Serial.println(">> 倒计时到，已关灯");
    setEffect(FX_OFF, true);
  }
}

// ------------------------------------------------------------ 手机网页
static void sendState() {
  long leftMin = 0;
  if (gOffAtMs) {
    long d = (long)(gOffAtMs - millis());
    leftMin = (d > 0) ? (d + 59999) / 60000 : 0;
  }
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"n\":%u,\"bri\":%u,\"fxid\":%u,\"fx\":\"%s\","
           "\"color\":\"%02X%02X%02X\",\"offLeft\":%ld,\"daily\":%d,"
           "\"onMin\":%u,\"offMin\":%u,\"synced\":%d,\"ap\":\"%s\"}",
           gNumLeds, gBright, (unsigned)gEffect, fxName(gEffect),
           gSolidR, gSolidG, gSolidB, leftMin, gDailyOn ? 1 : 0,
           gOnMin, gOffMin, gTimeSynced ? 1 : 0, AP_SSID);
  gServer.send(200, "application/json; charset=utf-8", buf);
}

static void handleSet() {
  if (gServer.hasArg("n"))   setLedCount(gServer.arg("n").toInt());
  if (gServer.hasArg("bri")) setBrightness(gServer.arg("bri").toInt());
  if (gServer.hasArg("color")) {
    uint8_t r, g, b;
    if (parseHex6(gServer.arg("color").c_str(), r, g, b)) setSolidColor(r, g, b);
  }
  if (gServer.hasArg("fx")) {
    int f = gServer.arg("fx").toInt();
    if (f >= 0 && f < (int)FX_COUNT) setEffect((Effect)f, true);
  }
  sendState();
}

static void handleTimer() {
  if (gServer.hasArg("off")) {
    long m = gServer.arg("off").toInt();
    if (m <= 0) {
      gOffAtMs = 0;
      Serial.println(">> 取消倒计时关灯");
    } else {
      if (m > 1440) m = 1440;
      gOffAtMs = millis() + (uint32_t)m * 60000UL;
      Serial.printf(">> 倒计时：%ld 分钟后关灯\n", m);
    }
  }
  sendState();
}

static void handleDaily() {
  if (gServer.hasArg("en"))  gDailyOn = gServer.arg("en").toInt() != 0;
  if (gServer.hasArg("on"))  gOnMin  = (uint16_t)constrain(gServer.arg("on").toInt(),  0, 1439);
  if (gServer.hasArg("off")) gOffMin = (uint16_t)constrain(gServer.arg("off").toInt(), 0, 1439);
  saveDaily();
  sendState();
}

static void handleTimeSync() {
  if (gServer.hasArg("epoch")) {
    gEpochBase  = strtoll(gServer.arg("epoch").c_str(), nullptr, 10);
    gMillisBase = millis();
    if (gServer.hasArg("tz")) gTzMin = constrain(gServer.arg("tz").toInt(), -720, 840);
    if (!gTimeSynced) Serial.println(">> 手机帮我们对好时间了，每天定时可以用");
    gTimeSynced = true;
  }
  sendState();
}

static void printWifiInfo() {
  Serial.printf("  手机 Wi-Fi：%s   密码：%s\n", AP_SSID, AP_PASS);
  Serial.printf("  网页地址：http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("  现在连着几台手机：%d\n", WiFi.softAPgetStationNum());
}

static void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);          // 别省电，灯效才稳
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[Wi-Fi] 热点「%s」%s\n", AP_SSID, ok ? "已经开了" : "没开起来，重启一下板子");
  printWifiInfo();
}

static void setupWeb() {
  gServer.on("/", []() { gServer.send_P(200, "text/html; charset=utf-8", PAGE_HTML); });
  gServer.on("/api/state",  sendState);
  gServer.on("/api/set",    handleSet);
  gServer.on("/api/timer",  handleTimer);
  gServer.on("/api/daily",  handleDaily);
  gServer.on("/api/time",   handleTimeSync);
  gServer.on("/favicon.ico", []() { gServer.send(204); });
  gServer.onNotFound([]() { gServer.send(404, "text/plain; charset=utf-8", "没有这个地址"); });
  gServer.begin();
  Serial.println("[网页] 手机页面准备好了，连上热点打开 192.168.4.1 就行");
}

// ------------------------------------------------------------ 主流程
void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)esp_random());   // 星星闪烁/篝火要真随机
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);

  Serial.println();
  Serial.println("====================================================");
  Serial.println("  WS2812B 灯带控制器 —— 灯珠数量/效果随时可改");
  Serial.println("====================================================");

  gPrefs.begin("strip", false);
  gNumLeds = gPrefs.getUShort("n", DEF_LEDS);
  gBright  = gPrefs.getUChar("b", DEF_BRIGHTNESS);
  uint8_t fx = gPrefs.getUChar("fx", (uint8_t)FX_RAINBOW);
  gWave    = gPrefs.getUChar("wp", 0);
  gDrive   = gPrefs.getUChar("dv", 2);
  gSolidR  = gPrefs.getUChar("cr", 255);
  gSolidG  = gPrefs.getUChar("cg", 128);
  gSolidB  = gPrefs.getUChar("cb", 0);
  // 有些批次的灯带，最边上那颗芯片是 RGB 顺序（其余是 GRB），
  // 表现就是"整条红色、第一颗发绿，但蓝/白/黑都正常"。
  // 这两个开关要断电也记得，所以存 Flash。
  gSwapFirst = gPrefs.getUChar("sf", 0) != 0;
  gSkipFirst = gPrefs.getUChar("sk", 0) != 0;
  gMaxMa     = gPrefs.getUShort("ma", 0);
  gBreathSpeed = gPrefs.getUChar("bs", 1);
  if (gBreathSpeed < 1 || gBreathSpeed > 8) gBreathSpeed = 1;
  gDailyOn = gPrefs.getUChar("dEn", 0) != 0;
  gOnMin   = gPrefs.getUShort("dOn",  19 * 60);
  gOffMin  = gPrefs.getUShort("dOff", 23 * 60);
  gTzMin   = gPrefs.getInt("tz", 480);
  if (gNumLeds < 1 || gNumLeds > MAX_LEDS) gNumLeds = DEF_LEDS;
  if (fx >= (uint8_t)FX_COUNT) fx = (uint8_t)FX_RAINBOW;
  if (gOnMin  > 1439) gOnMin  = 19 * 60;
  if (gOffMin > 1439) gOffMin = 23 * 60;
  if (gWave  >= WAVE_COUNT) gWave = 0;
  if (gDrive > 3)           gDrive = 2;
  gEffect = (Effect)fx;
  gLastFx = (gEffect == FX_OFF) ? FX_RAINBOW : gEffect;

  Serial.printf("  板子里记着：%u 颗灯珠，亮度 %u，效果 %s\n",
                gNumLeds, gBright, fxName(gEffect));

  setupWiFi();
  setupWeb();
  if (gDailyOn) {
    Serial.println("  每天定时已开着，但要手机打开网页对一次时才准（板子刚断电过）");
  }

  buildEncTable();
  gSpiReady  = spiBegin();                 // 优先用 SPI 直发（整帧无缝隙）
  gTransport = gSpiReady ? 1 : 0;
  if (!gSpiReady) {
    Serial.println("  ！SPI 直发没起来，退回老的 RMT 分包方式");
    gRmtReady = rmtBegin();
    if (!gRmtReady) Serial.println("  ！RMT 也初始化失败：灯带不会有反应");
  }

  // 上电直接接着跑上次保存的灯效，不闪自检 —— 想自检就发命令 t
  fillAll(0, 0, 0);
  sendFrame();
  setEffect(gEffect, false);
  printHelp();
}

void loop() {
  handleSerial();

  static uint32_t last = 0;
  static uint32_t lastBeat = 0;
  static uint32_t frames = 0;
  static uint32_t worstUs = 0;
  uint32_t now = millis();

  gServer.handleClient();
  timerTick();
  dailyTick();

  // 每 16 毫秒画一帧 ≈ 60 帧/秒（一帧只花 3 毫秒，来得及）
  if (now - last >= 16) {
    last = now;
    uint32_t t0 = micros();
    renderFrame();
    uint32_t dt = micros() - t0;          // 这一帧花了多少微秒
    if (dt > worstUs) worstUs = dt;
    frames++;
  }

  // 每 5 秒报一次「我还活着 + 刷新率 + 数据线缝隙」。
  // 只有电脑连着串口、而且发送缓冲区有空位时才打印 —— 免得打印把灯效卡住。
  if (now - lastBeat >= 5000) {
    lastBeat = now;
    if (Serial && Serial.availableForWrite() > 200) {
      Serial.printf("[心跳] 效果=%s  亮度=%u  %u 颗灯珠  约 %.1f 帧/秒  最慢一帧 %lu 毫秒\n",
                    fxName(gEffect), gBright, gNumLeds,
                    frames / 5.0f, (unsigned long)(worstUs / 1000));
      Serial.printf("       [当前设置] 波形档=%s  驱动=%s  呼吸快慢=%u/8\n",
                    gWaves[gWave].name, gDriveNames[gDrive], (unsigned)gBreathSpeed);
      if (gMaxMa == 0)
        Serial.printf("       [用电] 这一帧约 %u 毫安；电流上限没开\n", gLastMaOut);
      else if (gLastMa > gMaxMa)
        Serial.printf("       [用电] 这一帧约 %u 毫安（本来要 %u 毫安，超上限已自动压暗）；上限 %u 毫安\n",
                      gLastMaOut, gLastMa, gMaxMa);
      else
        Serial.printf("       [用电] 这一帧约 %u 毫安；上限 %u 毫安（没超）\n",
                      gLastMaOut, gMaxMa);
      if (gTransport == 1) {
        Serial.printf("       [数据线] SPI 直发：整帧一次发完，无缝隙；%u kHz；帧头缓冲 %d 字节；连发 %u 遍；首颗缓冲 %s；首颗换序 %s\n",
                      gSpiKhz, (int)gSpiHead, (unsigned)gRepeat,
                      gSkipFirst ? "开" : "关", gSwapFirst ? "开" : "关");
      } else {
        Serial.printf("       [数据线] RMT 分包：一帧拆 %d 包；包间隔最大 %lu 微秒；超过 250 微秒 %lu 次（开机至今）\n",
                      (int)((gNumLeds + gChunk - 1) / gChunk),
                      (unsigned long)gMaxGapUs, (unsigned long)gGapLatch);
      }
    }
    frames  = 0;
    worstUs = 0;
  }
}
