/*
 * 最小验证程序：先跑通这个，再玩复杂效果
 *
 * 现象：红 -> 绿 -> 蓝 -> 白，每颗依次点亮，然后全灭重来
 * 这一步就能暴露接线 / 供电 / 引脚配置问题，跟效果代码无关。
 */
#include <FastLED.h>

#define DATA_PIN 4
#define NUM_LEDS 60

CRGB leds[NUM_LEDS];

void setup() {
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(60);        // 先低亮度，保护电源
  FastLED.show();                   // 上电先全灭，避免随机亮
}

void loop() {
  const CRGB colors[] = { CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White };

  for (uint8_t c = 0; c < 4; c++) {
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      leds[i] = colors[c];
      FastLED.show();
      delay(30);
    }
    delay(600);
  }

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(800);
}
