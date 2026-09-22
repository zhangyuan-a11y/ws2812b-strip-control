/*
 * 诊断固件 v4 —— 只干一件事：量出"灯带那一头到底有没有电"
 * ---------------------------------------------------------------------------
 * 原理：
 *   灯带数据输入脚(Din) 内部有一个对 +5V 的保护二极管。
 *   我们用芯片内部的弱上拉(约45kΩ)去"顶"这根信号线，然后读电压：
 *
 *     灯带有 5V   → 二极管不导通，线被顶到 3.3V            → 读数 ≈ 3300mV
 *     灯带没电    → 二极管导通，把线钳在 0.7V 上下          → 读数 ≈ 700mV
 *                   而且灯带上那些电容会慢慢被顶起来，
 *                   所以 5ms / 50ms / 300ms 三个读数会**一路往上涨**（独有指纹）
 *     线被短路    → 一直是 0mV
 *     线是空的    → 一上来就是 3300mV（没有电容，不涨）
 *
 * 对照：
 *   GPIO6 = 空脚（标准答案：一上来就 3300mV）
 *   GPIO48 = 板载灯那根线（已知那颗灯是亮的，所以也是 3300mV）
 *   GPIO4  = 灯带这根线 ← 要看的就是它
 */

#include <Arduino.h>
#include "driver/rmt.h"
#include "driver/adc.h"
#include "driver/gpio.h"

#define PIN_STRIP    4
#define PIN_ONBOARD  48
#define PIXELS       4
#define CH_TX        RMT_CHANNEL_0

static rmt_item32_t gItems[PIXELS * 24];
static bool         gInst = false;
static int          gOn   = -1;

// ===========================================================================
//  电压测量：上拉去顶这根线，看顶不顶得起来 + 涨不涨
// ===========================================================================
static int rawToMv(int raw) { return (int)((long)raw * 3300 / 4095); }

static void voltProbe(int pin, adc1_channel_t ch, const char *tag) {
  gpio_num_t g = (gpio_num_t)pin;

  gpio_reset_pin(g);
  (void)analogRead(pin);            // 让 Arduino 把焊盘切到模拟通道（只做一次）
  delay(5);

  gpio_set_pull_mode(g, GPIO_FLOATING);
  delay(20);
  int f = adc1_get_raw(ch);

  gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
  delay(5);
  int a = adc1_get_raw(ch);
  delay(45);
  int b = adc1_get_raw(ch);
  delay(250);
  int c = adc1_get_raw(ch);

  gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
  delay(20);
  int d = adc1_get_raw(ch);

  gpio_set_pull_mode(g, GPIO_FLOATING);
  gpio_reset_pin(g);

  Serial.printf("  %-18s 悬空=%4d | 上拉 5ms=%4d  50ms=%4d  300ms=%4d | 下拉=%4d\n",
                tag, f, a, b, c, d);
  Serial.printf("  %-18s            (≈ %d mV  →  %d mV  →  %d mV)\n",
                "", rawToMv(a), rawToMv(b), rawToMv(c));
}

// ===========================================================================
//  基础驱动（用来做变色对照）
// ===========================================================================
static void detach() {
  if (gInst) { rmt_driver_uninstall(CH_TX); gInst = false; gOn = -1; }
}

static bool attach(int pin) {
  if (gOn == pin) return true;
  detach();

  rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)pin, CH_TX);
  cfg.clk_div       = 2;
  cfg.mem_block_num = 4;
  cfg.tx_config.carrier_en     = false;
  cfg.tx_config.loop_en        = false;
  cfg.tx_config.idle_output_en = true;
  cfg.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

  esp_err_t e1 = rmt_config(&cfg);
  esp_err_t e2 = rmt_driver_install(CH_TX, 0, 0);
  Serial.printf("    attach GPIO%-3d : rmt_config=%d  install=%d\n", pin, e1, e2);
  if (e1 != ESP_OK || e2 != ESP_OK) return false;

  gInst = true;
  gOn   = pin;
  return true;
}

static void sendColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!gInst) return;

  const uint8_t grb[3] = { g, r, b };
  int k = 0;
  for (int i = 0; i < PIXELS * 3; i++) {
    uint8_t byte = grb[i % 3];
    for (int bit = 7; bit >= 0; bit--) {
      bool one = (byte >> bit) & 1;
      gItems[k].level0    = 1;
      gItems[k].duration0 = one ? 32 : 14;
      gItems[k].level1    = 0;
      gItems[k].duration1 = one ? 18 : 36;
      k++;
    }
  }

  esp_err_t e = rmt_write_items(CH_TX, gItems, k, true);
  esp_err_t w = rmt_wait_tx_done(CH_TX, pdMS_TO_TICKS(200));
  Serial.printf("    发出一帧 %d 个元素 : write=%d  wait=%d\n", k, e, w);
  delayMicroseconds(600);
}

// ===========================================================================
struct Step { const char *name; uint8_t r, g, b; };
static const Step STEPS[] = {
  { "红", 255,   0,   0 },
  { "绿",   0, 255,   0 },
  { "蓝",   0,   0, 255 },
  { "白", 120, 120, 120 },
  { "灭",   0,   0,   0 },
};

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2500) delay(10);

  Serial.println();
  Serial.println("========================================================");
  Serial.println("  诊断 v4：量电压，看灯带那头有没有电");
  Serial.println("========================================================");

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);  // GPIO4
  adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_DB_11);  // GPIO6
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);  // GPIO1（另一个空脚）

  Serial.println();
  Serial.println("  [1] 电压体检（用内部上拉去顶线，读电压）");
  Serial.println("      上拉 5ms → 50ms → 300ms 三个读数如果一路往上涨，");
  Serial.println("      说明线那头有个二极管在给一堆电容充电 = 灯带没吃到电。");
  Serial.println("      -------------------------------------------------------");
  voltProbe(4,  ADC1_CHANNEL_3, "GPIO4  [灯带]");
  voltProbe(6,  ADC1_CHANNEL_5, "GPIO6  [空脚]");
  voltProbe(1,  ADC1_CHANNEL_0, "GPIO1  [空脚]");
  Serial.println("      -------------------------------------------------------");

  Serial.println();
  Serial.println("  [2] 开始变色（板载灯 + 灯带同步）");
}

void loop() {
  for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
    const Step &s = STEPS[i];
    Serial.printf("\n>> 目标颜色【%s】\n", s.name);

    Serial.println("  · 写给灯带 (GPIO 4)");
    if (attach(PIN_STRIP)) sendColor(s.r, s.g, s.b);

    Serial.println("  · 写给板载灯 (GPIO 48)");
    if (attach(PIN_ONBOARD)) sendColor(s.r, s.g, s.b);

    delay(2000);
  }
  Serial.println("\n--- 一轮结束，重新开始 ---");
}
