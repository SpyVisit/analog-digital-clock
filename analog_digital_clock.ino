/*
  Аналого-цифровые часы
  Arduino Nano + MCP4728 (4-канальный ЦАП, I2C) + DS1302 (RTC, 3-проводный
  интерфейс CLK/DAT/RST, с батарейкой) + 3 стрелочных вольтметра 0-5В + 3 кнопки

  Подключение:
    I2C (только МСР4728):  A4 = SDA, A5 = SCL
    DS1302:  CLK -> D7
             DAT (I/O) -> D6
             RST (CE)  -> D5
             VCC -> 5V, GND -> GND
    DAC:  канал A -> вольтметр "часы"
          канал B -> вольтметр "минуты"
          канал C -> вольтметр "секунды"
    Кнопки (второй контакт на GND, режим INPUT_PULLUP):
          D2 -> +1 час
          D3 -> +1 минута
          D4 -> сброс секунд

  Библиотеки (Sketch -> Include Library -> Manage Libraries):
    - Adafruit MCP4728
    - Adafruit BusIO (зависимость)
    - Rtc by Makuna   (для DS1302, содержит ThreeWire + RtcDS1302)
*/

#include <Wire.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <Adafruit_MCP4728.h>

const uint8_t RTC_CLK = 7;
const uint8_t RTC_DAT = 6;
const uint8_t RTC_RST = 5;

ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST); // IO, SCLK, CE
RtcDS1302<ThreeWire> Rtc(myWire);

Adafruit_MCP4728 dac;

const uint8_t BTN_HOUR = 2;   // +1 час
const uint8_t BTN_MIN  = 3;   // +1 минута
const uint8_t BTN_SEC  = 4;   // сброс секунд

const uint16_t DAC_MAX = 4095;          // 12 бит
const unsigned long DEBOUNCE_MS = 250;  // защита от дребезга

unsigned long lastPress[3] = {0, 0, 0};
bool lastState[3] = {HIGH, HIGH, HIGH};

void setup() {
  Serial.begin(9600);
  Wire.begin();

  pinMode(BTN_HOUR, INPUT_PULLUP);
  pinMode(BTN_MIN,  INPUT_PULLUP);
  pinMode(BTN_SEC,  INPUT_PULLUP);

  Rtc.Begin();

  RtcDateTime compiled(__DATE__, __TIME__);

  if (!Rtc.IsDateTimeValid()) {
    Serial.println(F("RTC потерял данные, выставляю время компиляции."));
    Rtc.SetDateTime(compiled);
  }

  if (Rtc.GetIsWriteProtected()) {
    Rtc.SetIsWriteProtected(false);
  }

  if (!Rtc.GetIsRunning()) {
    Rtc.SetIsRunning(true);
  }

  if (!dac.begin()) {
    Serial.println(F("MCP4728 не найден! Проверьте подключение I2C."));
    while (1) delay(10);
  }

  // Полный размах 0-5В: опора = питание (VDD), усиление x1
  dac.setChannelValue(MCP4728_CHANNEL_A, 0, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
  dac.setChannelValue(MCP4728_CHANNEL_B, 0, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
  dac.setChannelValue(MCP4728_CHANNEL_C, 0, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
}

// Проверка одной кнопки: idx - индекс в массивах lastPress/lastState
bool buttonPressed(uint8_t pin, uint8_t idx) {
  bool state = digitalRead(pin);
  bool pressed = false;

  if (state == LOW && lastState[idx] == HIGH &&
      millis() - lastPress[idx] > DEBOUNCE_MS) {
    pressed = true;
    lastPress[idx] = millis();
  }

  lastState[idx] = state;
  return pressed;
}

void handleButtons() {
  RtcDateTime now = Rtc.GetDateTime();

  if (buttonPressed(BTN_HOUR, 0)) {
    uint8_t h = (now.Hour() + 1) % 24;
    RtcDateTime newTime(now.Year(), now.Month(), now.Day(),
                         h, now.Minute(), now.Second());
    Rtc.SetDateTime(newTime);
  }

  if (buttonPressed(BTN_MIN, 1)) {
    uint8_t m = (now.Minute() + 1) % 60;
    RtcDateTime newTime(now.Year(), now.Month(), now.Day(),
                         now.Hour(), m, now.Second());
    Rtc.SetDateTime(newTime);
  }

  if (buttonPressed(BTN_SEC, 2)) {
    RtcDateTime newTime(now.Year(), now.Month(), now.Day(),
                         now.Hour(), now.Minute(), 0);
    Rtc.SetDateTime(newTime);
  }
}

void updateGauges() {
  RtcDateTime now = Rtc.GetDateTime();

  // Плавное движение стрелок за счёт учёта младших единиц времени
  float hourFrac = now.Hour() + now.Minute() / 60.0;    // 0..24
  float minFrac  = now.Minute() + now.Second() / 60.0;  // 0..60
  float secVal   = now.Second();                        // 0..60

  uint16_t hVal = (uint16_t)(hourFrac / 24.0 * DAC_MAX);
  uint16_t mVal = (uint16_t)(minFrac  / 60.0 * DAC_MAX);
  uint16_t sVal = (uint16_t)(secVal   / 60.0 * DAC_MAX);

  // Раз в минуту (когда секунды обнуляются) - эффект рывка стрелки секунд:
  // быстрый пробег к почти максимуму и плавный возврат к 0
  if (sVal == 0) {
    for (int i = 4000; i >= 150; i -= 1) {
      delayMicroseconds(100);
      dac.setChannelValue(MCP4728_CHANNEL_C, i, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
    }
  }

  dac.setChannelValue(MCP4728_CHANNEL_A, hVal, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
  dac.setChannelValue(MCP4728_CHANNEL_B, mVal, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
  dac.setChannelValue(MCP4728_CHANNEL_C, sVal, MCP4728_VREF_VDD, MCP4728_GAIN_1X);
}

void printTimeToSerial() {
  static unsigned long lastPrint = 0;

  // Печатаем раз в секунду, чтобы не заваливать порт
  if (millis() - lastPrint < 1000) return;
  lastPrint = millis();

  RtcDateTime now = Rtc.GetDateTime();

  char buf[9];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
           now.Hour(), now.Minute(), now.Second());
  Serial.println(buf);
}

void loop() {
  handleButtons();
  updateGauges();
  printTimeToSerial();
  delay(50);
}
