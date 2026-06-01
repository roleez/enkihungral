// HR. 2026.Q2.
// ════════════════════════════════════════════════════════════════
//  Enki Hungrál – ESP32-C3 SuperMini
//  RGB LED vezérlő deep-sleep alapú működéssel, WiFi AP + OTA
//
//  Javítások (review alapján):
//   C1 – ESP_SLEEP_WAKEUP_GPIO (nem EXT0) az ESP32-C3-on
//   C2 – Jumper-figyelés operátor-precedencia bug javítva
//   C3 – Visszaalvásnál gomb-elengedés megvárása (végtelen ébresztési hurok ellen)
//   C4 – Race condition: SemaphoreHandle_t mutex a megosztott állapot körül
//   S1 – buildStatusJson(): static buf → lokális, mutex védelem
//   S2 – WS adatframe: String((const char*)data, len) – null-terminátor fix
//   S3 – SAVE parancs: NVS írás mutex-en kívül (stack overflow megelőzés)
//   S4 – ws.cleanupClients() periodikusan (heap fragmentáció ellen)
//   S5 – gpio_hold_en() a PWM lábakra deep sleep előtt
//   E3 – WiFi leállítás goToDeepSleep()-ben
//   E4 – PIN_WIFIEN pull-up kikapcsolása sleep előtt (áramfelvétel csökkentés)
//   R2 – Alacsony akkufeszültség figyelés + kényszeres deep sleep
//   R4 – Szoftveres gomb-debounce (30 ms)
//   R6 – #undef / #ifndef TESZTWIFI redundancia megszüntetve
//   R7 – strncpy után null-terminátor garantálása
//
// ─────────────────────────────────────────────────────────────────
//  ÚJ: Két fordítási mód közötti váltás egyetlen #define-val
//
//  RÉGI MÓD  (alapértelmezett, ha a define nincs bekapcsolva):
//    → Deep-sleep alapú működés, minden az eredetivel kompatibilis.
//
//  ÚJ MÓD   (ha az alábbi define aktív):
//    → Az ESP32 soha nem megy deep sleep-be.
//    → WiFi/AP csak boot-kor dönt (PIN_WIFIEN állapota), utána fix.
//    → Timeout esetén csak a LED-ek kapcsolnak ki, az MCU fut tovább.
//    → Gomb normál eseménykezelés (nem ébresztés).
//    → FreeRTOS blink task a beépített LED-en.
//
//  A define engedélyezéséhez távolítsd el a komment jelet:
// ─────────────────────────────────────────────────────────────────
//#define USE_ALWAYS_RUNNING_MODE
#define WIFI_LED_VILLOGAS
//#define URESC3
// ════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>
#include "webpage.h"
#include "gamma10.h"
#include "RgbFader.h"

// ─────────────────────────────────────────────
//  Hardver konfiguráció
// ─────────────────────────────────────────────
#define AKKUFESZ    A0          // Analóg bemenet, feszültségosztón át
#define PIEZO       1           // Aktív buzzer kimenet
#define NYOMOGOMB   3           // Nyomógomb (INPUT_PULLUP, aktív = LOW)
#define PIN_R       5           // PWM piros csatorna
#define PIN_G       6           // PWM zöld csatorna
#define PIN_B       7           // PWM kék csatorna
#define PIN_WIFIEN  10          // WiFi engedélyező jumper (INPUT_PULLUP, aktív = LOW)

// ── [NEW ALWAYS RUNNING MODE] Beépített LED (ESP32-C3 SuperMini: GPIO8) ──
#define PIN_BUILTIN_LED  8

#define PWM_RES     10           // 8 bit felbontás → 0–255
#define MAX_COLORS  25          // Maximum tárolható szín
#define BEEPDB      3           // hányszor csipogjon

// Alacsony akku küszöb: ez alatt kényszeres deep sleep
// (brownout ~2.44 V, Li-ion védett leállás ~3.0 V)
#define BATT_LOW_V  3.1f

// Gomb debounce idő (ms)
#define BTN_DEBOUNCE_MS  30
#define BTN_EBRED        1500

#define DEFAULT_COLORS_COUNT 7

// TESZTWIFI: Kommenteld be fejlesztéshez (WiFi mindig indul)
// Csak az OLD MODE-ban van hatása; az új módban a PIN_WIFIEN boot-kori
// állapota dönt, a TESZTWIFI nem releváns.
#define TESZTWIFI

// ─────────────────────────────────────────────
//  WiFi AP konfiguráció
// ─────────────────────────────────────────────
const char* AP_SSID   = "ENKILED";
const char* MDNS_HOST = "enkiled";
const char* TAGMAIN   = "ENKI";

// ─────────────────────────────────────────────
//  Adatstruktúra egy színhez
// ─────────────────────────────────────────────
struct ColorEntry {
    char    name[32];
    uint8_t r, g, b;
    uint16_t freq;
};

// ─────────────────────────────────────────────
//  Buzzer task paraméter struktúra
// ─────────────────────────────────────────────
struct BuzzerParams {
  uint32_t ms;
  bool isError; // true = buzzerError (folytonos), false = buzzerBeep (tört)
};

// ─────────────────────────────────────────────
//  Alapértelmezett 7 szín (ha még nincs flash adat)
//  Megjegyzés: PROGMEM ESP32-C3-on NOP (egységes memóriabusz),
//  eltávolítva a félreértések elkerüléséért.
// ─────────────────────────────────────────────
static const ColorEntry DEFAULT_COLORS[7] = {
    { "Vörös",    255, 0,   0,   396 },
    { "Narancs",  255, 165, 0,   417 },
    { "Sárga",    255, 255, 0,   528 },
    { "Zöld",     0,   128, 0,   639 },
    { "Kék",      0,   0,   255, 741 },
    { "Indigó",   75,  0,   130, 852 },
    { "Ibolya",   238, 130, 238, 963 },
};

// ─────────────────────────────────────────────
//  Globális állapot
// ─────────────────────────────────────────────
ColorEntry colors[MAX_COLORS];
int        colorCount  = 0;
int        activeIndex = 0;     // Aktív szín indexe
bool lastJumperState;
bool voltFeszHiba = false;  // Akkufeszültség hiba állapotának követése
bool pendingWsUpdate = false;   // Jelzi, hogy küldeni kell a státuszt a klienseknek
bool pendingNvsSave   = false;   // Jelzi, hogy menteni kell az indexet a Flash-be
uint32_t lastChangeMs = 0;       // Késleltetett mentéshez
bool g_skipBtnUntilRelease = false;

// Mutex: védi a colors[], colorCount, activeIndex, sleepMinutes
// változókat a loop task és az async_tcp (WS/HTTP) task között.
// Mindig LOCK_STATE / UNLOCK_STATE makrókkal érd el!
static SemaphoreHandle_t g_stateMutex = nullptr;
TaskHandle_t g_blinkTaskHandle = nullptr;

#define LOCK_STATE()   xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(250))
#define UNLOCK_STATE() xSemaphoreGive(g_stateMutex)

Preferences    prefs;
AsyncWebServer server(80);
DNSServer      dnsServer;
AsyncWebSocket ws("/ws");

IPAddress local_ip(192, 168, 99, 9);
IPAddress gateway (192, 168, 99, 9);
IPAddress subnet  (255, 255, 255, 0);

// WiFi állapot követése
bool     wifiRunning  = false;
bool     otaStarted   = false;
bool g_ledsOn = false;
uint32_t wifiStartMs  = 0;

// Ébresztési timer (percben, 5–60)
uint8_t sleepMinutes = 10;

// Aktív üzemmód időzítő
uint32_t activeStartMs = 0;

// Akkufeszültség – csak a loop taskból frissítjük (ADC nem thread-safe)
static float    g_battVoltage    = 0.0f;
static uint32_t g_battLastReadMs = 0;

// ── [NEW ALWAYS RUNNING MODE] Állapotváltozók ────────────────────
#ifdef USE_ALWAYS_RUNNING_MODE

// true  = LED-ek világítanak (aktív szín ki van rakva)
// false = LED-ek ki vannak kapcsolva (timeout lejárt)
//static bool g_ledsOn = false;

// A blink task handle-je (opcionálisan leállítható, ha szükséges)
//static TaskHandle_t g_blinkTaskHandle = nullptr;

String buildStatusJson();

#endif // USE_ALWAYS_RUNNING_MODE
// ─────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────
//  Feszültségosztó konstans
//  V_akku = V_adc * (33k + 100k) / 100k = V_adc * 1.33
//  V_adc  = ADC_érték / 4095 * 3.3
//  FIGYELEM: Csak a loop taskból hívd (ADC nem thread-safe)!
// ─────────────────────────────────────────────
float readBatteryVoltage() {
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(AKKUFESZ);
        vTaskDelay(2);
    }
    float adc_v = (sum / 8.0f) / 4095.0f * 3.3f;
    return adc_v * 1.4103f;
}

// ─────────────────────────────────────────────
//  PWM kimenet beállítása
// ─────────────────────────────────────────────
void applyPWM(uint8_t r, uint8_t g, uint8_t b, uint16_t freq) {
    ledcChangeFrequency(0, freq, PWM_RES);
    ledcChangeFrequency(1, freq, PWM_RES);
    ledcChangeFrequency(2, freq, PWM_RES);
    /*ledcWrite(0, r);
    ledcWrite(1, g);
    ledcWrite(2, b);*/
    ledcWrite(0, pgm_read_word(&gamma10[r]));
    ledcWrite(1, pgm_read_word(&gamma10[g]));
    ledcWrite(2, pgm_read_word(&gamma10[b]));
}

// ─────────────────────────────────────────────
//  Buzzer one-shot task
//  heap-ről veszi a paramétert, végén free() + vTaskDelete()
// ─────────────────────────────────────────────
static void buzzerTask(void *pvParam) {
  BuzzerParams *p = static_cast<BuzzerParams *>(pvParam);

  if (p->isError) {
    // Folytonos hang (eredeti buzzerError logika)
    digitalWrite(PIEZO, HIGH);
    vTaskDelay(pdMS_TO_TICKS(p->ms));
    digitalWrite(PIEZO, LOW);
  } else {
    // Tört hang (eredeti buzzerBeep logika)
    uint32_t onOff = p->ms / BEEPDB / 2;
    for (int i = 0; i < BEEPDB; i++) {
      digitalWrite(PIEZO, HIGH);
      vTaskDelay(pdMS_TO_TICKS(onOff));
      digitalWrite(PIEZO, LOW);
      vTaskDelay(pdMS_TO_TICKS(onOff));
    }
  }

  free(p);
  vTaskDelete(nullptr);
}

void buzzerError(uint32_t ms) {
  digitalWrite(PIEZO, HIGH);
  vTaskDelay(pdMS_TO_TICKS(ms));
  digitalWrite(PIEZO, LOW);
}

void buzzerBeep(uint32_t ms) {
  uint32_t onOff = ms / BEEPDB / 2;
  for (int i = 0; i < BEEPDB; i++) {
    digitalWrite(PIEZO, HIGH);
    vTaskDelay(pdMS_TO_TICKS(onOff));
    digitalWrite(PIEZO, LOW);
    vTaskDelay(pdMS_TO_TICKS(onOff));
  }
}
// ─────────────────────────────────────────────
//  Buzzer indítás – nem blokkoló, azonnal visszatér
//  A loop() (vagy WS handler) ebből hívja a hangokat.
// ─────────────────────────────────────────────
static void buzzerBeepAsync(uint32_t ms) {
  BuzzerParams *p = static_cast<BuzzerParams *>(malloc(sizeof(BuzzerParams)));
  if (!p)
    return;
  p->ms = ms;
  p->isError = false;
  if (xTaskCreate(buzzerTask, "buzzer", 2048, p, 2, nullptr) != pdPASS) {
    free(p);
    ESP_LOGE(TAGMAIN, "buzzerTask letrehozas sikertelen!");
  } else {
    ESP_LOGI(TAGMAIN, "Buzzer beep task inditva: %u ms", ms);
  }
}

static void buzzerErrorAsync() {
  BuzzerParams *p = static_cast<BuzzerParams *>(malloc(sizeof(BuzzerParams)));
  if (!p)
    return;
  p->ms = 5000;
  p->isError = true;
  if (xTaskCreate(buzzerTask, "buzzer", 512, p, 2, nullptr) != pdPASS) {
    free(p);
    ESP_LOGE(TAGMAIN, "buzzerTask letrehozas sikertelen!");
  } else {
    ESP_LOGI(TAGMAIN, "Buzzer error task inditva: folytonos hang 5 mp-ig");
  }
}

// ─────────────────────────────────────────────
//  PWM lábak biztonságos leállítása sleep előtt
//  FIX S5: gpio_hold_en() megelőzi a lebegő GPIO állapotot
//   1. Duty → 0
//   2. GPIO explicit LOW
//   3. PWM lecsatolás
//   4. GPIO hold (állapot megőrzése deep sleep alatt)
// ─────────────────────────────────────────────
static void detachAndHoldPWM() {
    ledcWrite(0, 0);
    ledcWrite(1, 0);
    ledcWrite(2, 0);
    gpio_set_level((gpio_num_t)PIN_R, 0);
    gpio_set_level((gpio_num_t)PIN_G, 0);
    gpio_set_level((gpio_num_t)PIN_B, 0);
    ledcDetachPin(PIN_R);
    ledcDetachPin(PIN_G);
    ledcDetachPin(PIN_B);
    gpio_hold_en((gpio_num_t)PIN_R);
    gpio_hold_en((gpio_num_t)PIN_G);
    gpio_hold_en((gpio_num_t)PIN_B);
}

// ─────────────────────────────────────────────
//  GPIO hold feloldása – setup() elején kell hívni,
//  hogy az előző deep sleep hold-ja ne blokkolja a PWM-et
// ─────────────────────────────────────────────
static void releasePWMHold() {
    gpio_hold_dis((gpio_num_t)PIN_R);
    gpio_hold_dis((gpio_num_t)PIN_G);
    gpio_hold_dis((gpio_num_t)PIN_B);
}

// ─────────────────────────────────────────────
//  Mélyálomba megy
//  [OLD MODE only] – USE_ALWAYS_RUNNING_MODE-ban soha nem hívódik
// ─────────────────────────────────────────────
#ifndef USE_ALWAYS_RUNNING_MODE
void goToDeepSleep() {
    ESP_LOGI(TAGMAIN, "Melyalvas indul...");

    // FIX E3: WiFi leállítása – különben RF modul aktív marad sleep alatt
    if (wifiRunning) {
        ws.closeAll();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        wifiRunning = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    RgbFader::fadeOut();
    while (RgbFader::isFading()) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    // LED ki, buzzer, majd GPIO hold
    applyPWM(0, 0, 0, 1000);
    //buzzerBeep(3000);
    detachAndHoldPWM();

    // Buzzer GPIO hold (LOW állapotból indulunk)
    digitalWrite(PIEZO, LOW);
    gpio_hold_en((gpio_num_t)PIEZO);

    // FIX E4: PIN_WIFIEN pull-up kikapcsolása
    // GPIO10 nem RTC GPIO, a belső pull-up sleep alatt nem aktív,
    // de explicit kikapcsolás megelőzi a bizonytalan állapotot
    gpio_pullup_dis((gpio_num_t)PIN_WIFIEN);
    gpio_pulldown_dis((gpio_num_t)PIN_WIFIEN);

    // FIX C1: ESP32-C3-on GPIO wakeup (nem ext0/ext1)
    esp_deep_sleep_enable_gpio_wakeup(1 << NYOMOGOMB, ESP_GPIO_WAKEUP_GPIO_LOW);
    //Serial.flush();
    delay(100);
    esp_deep_sleep_start();
}
#endif // ifndef USE_ALWAYS_RUNNING_MODE

// ─────────────────────────────────────────────
//  [NEW ALWAYS RUNNING MODE] LED timeout kikapcsolás
//  A deep sleep helyett csak a LED-eket kapcsolja ki.
//  Az MCU, WiFi, WebSocket, OTA mind fut tovább.
// ─────────────────────────────────────────────
#ifdef USE_ALWAYS_RUNNING_MODE
static void turnOffLeds() {
  buzzerBeepAsync(3000);
  applyPWM(0, 0, 0, 1000);
  g_ledsOn = false;
  ESP_LOGI(TAGMAIN, "[ARM] LED-ek lekapcsolva (timeout). MCU fut tovabb.");
  // Státusz frissítése WebSocket kliensek felé (opcionális, de kompatibilis)
  //ws.textAll(buildStatusJson()); // előre deklarálva – lásd lejjebb
}
#endif // USE_ALWAYS_RUNNING_MODE

// ─────────────────────────────────────────────
//  [NEW ALWAYS RUNNING MODE] FreeRTOS Blink Task
//  Beépített LED-et villogtatja 500 ms ON / 500 ms OFF ciklusban.
//  Folyamatosan fut, nem blokkolja a loop()-ot.
// ─────────────────────────────────────────────
#ifdef WIFI_LED_VILLOGAS
static void blinkTask(void* pvParameters) {
    (void)pvParameters;  // Nem használt paraméter – figyelmeztetés elkerülése

    pinMode(PIN_BUILTIN_LED, OUTPUT);
    ESP_LOGI(TAGMAIN, "[ARM] Blink task elindult (GPIO%d).", PIN_BUILTIN_LED);

    while (true) {
        if (wifiRunning) {
            digitalWrite(PIN_BUILTIN_LED, LOW);
            vTaskDelay(pdMS_TO_TICKS(150));
            digitalWrite(PIN_BUILTIN_LED, HIGH);
            vTaskDelay(pdMS_TO_TICKS(750));
        }
    }
    // Ide soha nem jutunk, de formálisan helyes:
    vTaskDelete(nullptr);
}
#endif // USE_ALWAYS_RUNNING_MODE

// ─────────────────────────────────────────────
//  Preferences – betöltés
// ─────────────────────────────────────────────
void loadFromPreferences() {
    bool opened = prefs.begin("enki", true);
    if (!opened) {
        ESP_LOGE(TAGMAIN, "NVS megnyitas sikertelen – alapertekek betoltve");
        colorCount   = 7;
        activeIndex  = 0;
        sleepMinutes = 10;
        for (int i = 0; i < 7; i++) colors[i] = DEFAULT_COLORS[i];
        return;
    }

    activeIndex  = prefs.getInt("idx",    0);
    sleepMinutes = prefs.getUChar("sleepm", 10);
    colorCount   = prefs.getInt("count",  0);

    if (colorCount == 0) {
        colorCount = 7;
        for (int i = 0; i < 7; i++) colors[i] = DEFAULT_COLORS[i];
    } else {
        colorCount = min(colorCount, MAX_COLORS);
        for (int i = 0; i < colorCount; i++) {
            char key[16];
            snprintf(key, sizeof(key), "n%d", i);
            prefs.getString(key, colors[i].name, sizeof(colors[i].name));
            colors[i].name[sizeof(colors[i].name) - 1] = '\0';  // null-terminátor garantálása
            snprintf(key, sizeof(key), "r%d", i);  colors[i].r    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "g%d", i);  colors[i].g    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "b%d", i);  colors[i].b    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "f%d", i);  colors[i].freq = (uint16_t)prefs.getInt(key, 1000);
        }
    }

    prefs.end();

    if (activeIndex >= colorCount) activeIndex = 0;
    sleepMinutes = constrain(sleepMinutes, 5, 60);
}

// ─────────────────────────────────────────────
//  Preferences – teljes mentés
//  FIGYELEM: Lassú (flash írás). Csak mutex-en kívül hívd,
//  az állapotot előtte lokális változókba másold!
// ─────────────────────────────────────────────
void saveToPreferences() {
    prefs.begin("enki", false);
    prefs.putInt("idx",      activeIndex);
    prefs.putUChar("sleepm", sleepMinutes);
    prefs.putInt("count",    colorCount);
    for (int i = 0; i < colorCount; i++) {
        char key[16];
        snprintf(key, sizeof(key), "n%d", i);  prefs.putString(key, colors[i].name);
        snprintf(key, sizeof(key), "r%d", i);  prefs.putInt(key, colors[i].r);
        snprintf(key, sizeof(key), "g%d", i);  prefs.putInt(key, colors[i].g);
        snprintf(key, sizeof(key), "b%d", i);  prefs.putInt(key, colors[i].b);
        snprintf(key, sizeof(key), "f%d", i);  prefs.putInt(key, colors[i].freq);
    }
    prefs.end();
    ESP_LOGI(TAGMAIN, "Preferences mentve.");
}

// ─────────────────────────────────────────────
//  Csak az index + sleepMinutes mentése (gyors)
// ─────────────────────────────────────────────
void saveIndexAndTimer() {
    prefs.begin("enki", false);
    prefs.putInt("idx",      activeIndex);
    prefs.putUChar("sleepm", sleepMinutes);
    prefs.end();
}

// ─────────────────────────────────────────────
//  JSON állapot (HTTP /status és WS push)
//  FIX S1: lokális buffer (nem static) + mutex → thread-safe
// ─────────────────────────────────────────────
String buildStatusJson() {
    char buf[3200];   // lokális → minden task saját példányon dolgozik
    int pos = 0;

    // g_battVoltage-t a loop task írja, csak olvasás itt → elfogadható
    float batt = g_battVoltage;

    if (LOCK_STATE() != pdTRUE) {
        return "{\"error\":\"busy\"}";
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"batt\":%.2f,\"sleepm\":%d,\"activeIndex\":%d,\"count\":%d,\"colors\":[",
        batt, sleepMinutes, activeIndex, colorCount);

    for (int i = 0; i < colorCount; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"index\":%d,\"name\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,\"freq\":%d}",
            i, colors[i].name, colors[i].r, colors[i].g, colors[i].b, colors[i].freq);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");

    UNLOCK_STATE();
    return String(buf);
}

// ─────────────────────────────────────────────
//  WebSocket üzenet feldolgozása
//
//  SET:<idx>:<r>:<g>:<b>:<freq>  → PWM élő frissítés
//  NAME:<idx>:<nev>              → Szín neve
//  SLEEP:<perc>                  → Alvási idő beállítás
//  SAVE                          → Minden mentés flash-be
//  ADD                           → Új szín
//  DEL:<idx>                     → Szín törlése
// ─────────────────────────────────────────────
void handleWsMessage(AsyncWebSocketClient* client, const String& msg) {

    if (msg.startsWith("SET:")) {
        int idx, r, g, b, freq;
        if (sscanf(msg.c_str(), "SET:%d:%d:%d:%d:%d", &idx, &r, &g, &b, &freq) == 5) {
            uint8_t  cr, cg, cb;
            uint16_t cf;
            bool valid = false;
            if (LOCK_STATE() == pdTRUE) {
                if (idx >= 0 && idx < colorCount) {
                    colors[idx].r    = (uint8_t)constrain(r,    0, 255);
                    colors[idx].g    = (uint8_t)constrain(g,    0, 255);
                    colors[idx].b    = (uint8_t)constrain(b,    0, 255);
                    colors[idx].freq = (uint16_t)constrain(freq, 1, 40000);
                    activeIndex = idx;
                    cr = colors[idx].r; cg = colors[idx].g;
                    cb = colors[idx].b; cf = colors[idx].freq;
                    valid = true;
                }
                UNLOCK_STATE();
            }
            if (valid) {
                applyPWM(cr, cg, cb, cf);   // PWM hívás mutex-en kívül

                // [NEW ALWAYS RUNNING MODE] SET parancsnál a LED-ek bekapcsolnak,
                // az activeStartMs frissül, hogy a timeout újra induljon.
#ifdef USE_ALWAYS_RUNNING_MODE
                g_ledsOn = true;
                activeStartMs = millis();
#endif

                if (client) client->text(buildStatusJson());
            }
        }
    }
    else if (msg.startsWith("NAME:")) {
        int idx; char name[32];
        if (sscanf(msg.c_str(), "NAME:%d:%31[^\n]", &idx, name) == 2) {
            if (LOCK_STATE() == pdTRUE) {
                if (idx >= 0 && idx < colorCount) {
                    strncpy(colors[idx].name, name, sizeof(colors[idx].name));
                    colors[idx].name[sizeof(colors[idx].name) - 1] = '\0';
                }
                UNLOCK_STATE();
            }
        }
    }
    else if (msg.startsWith("SLEEP:")) {
        int m;
        if (sscanf(msg.c_str(), "SLEEP:%d", &m) == 1) {
            if (LOCK_STATE() == pdTRUE) {
                sleepMinutes = (uint8_t)constrain(m, 5, 60);
                UNLOCK_STATE();
            }
        }
    }
    else if (msg == "SAVE") {
        // FIX S3: Az állapotot mutex-en belül másoljuk ki,
        // majd az NVS írás (lassú) mutex-en kívül fut →
        // nem blokkolja az async_tcp task stackjét
        uint8_t  cr = 0, cg = 0, cb = 0;
        uint16_t cf = 1000;
        if (LOCK_STATE() == pdTRUE) {
            if (activeIndex >= 0 && activeIndex < colorCount) {
                cr = colors[activeIndex].r;
                cg = colors[activeIndex].g;
                cb = colors[activeIndex].b;
                cf = colors[activeIndex].freq;
            }
            activeStartMs = millis();
            UNLOCK_STATE();
        }
        applyPWM(cr, cg, cb, cf);

        // [NEW ALWAYS RUNNING MODE] SAVE parancs is bekapcsolja a LED-eket
        // és újraindítja a timeoutot
#ifdef USE_ALWAYS_RUNNING_MODE
        g_ledsOn = true;
#endif

        saveToPreferences();                // lassú – mutex-en kívül
        if (client) client->text(buildStatusJson());
    }
    else if (msg == "ADD") {
        if (LOCK_STATE() == pdTRUE) {
            if (colorCount < MAX_COLORS) {
                strncpy(colors[colorCount].name, "Uj szin", sizeof(colors[colorCount].name));
                colors[colorCount].name[sizeof(colors[colorCount].name) - 1] = '\0';
                colors[colorCount].r    = 128;
                colors[colorCount].g    = 128;
                colors[colorCount].b    = 128;
                colors[colorCount].freq = 1000;
                colorCount++;
            }
            UNLOCK_STATE();
        }
    }
    else if (msg.startsWith("DEL:")) {
        int idx;
        if (sscanf(msg.c_str(), "DEL:%d", &idx) == 1) {
            if (LOCK_STATE() == pdTRUE) {
                if (idx >= 0 && idx < colorCount) {
                    for (int i = idx; i < colorCount - 1; i++) colors[i] = colors[i + 1];
                    colorCount--;
                    if (activeIndex >= colorCount) activeIndex = colorCount - 1;
                    if (activeIndex < 0)           activeIndex = 0;
                }
                UNLOCK_STATE();
            }
        }
    }
}

// ─────────────────────────────────────────────
//  WebSocket eseménykezelő
// ─────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        ESP_LOGI(TAGMAIN, "WS kliens csatlakozott: %u", client->id());
        client->text(buildStatusJson());
    }
    else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGI(TAGMAIN, "WS kliens lecsatlakozott: %u", client->id());
    }
    else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            // FIX S2: len-alapú konstruktor – nem feltételezi a null-terminátort
            String msg = String((const char*)data, len);
            handleWsMessage(client, msg);
        }
    }
}

// ─────────────────────────────────────────────
//  Captive portal átirányítás
// ─────────────────────────────────────────────
static void send_redirect(AsyncWebServerRequest* r) {
    char buf[32];
    snprintf(buf, sizeof(buf), "http://%s", WiFi.softAPIP().toString().c_str());
    r->redirect(buf);
}

class CaptiveRequestHandler : public AsyncWebHandler {
public:
    // FIX R8: override jelölése – fordítási idejű virtuális függvény ellenőrzés
    bool canHandle(AsyncWebServerRequest* req) {
        if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) return false;
        String host   = req->host();
        String myIP   = WiFi.softAPIP().toString();
        String myMDNS = String(MDNS_HOST) + ".local";
        if (host == myIP || host.equalsIgnoreCase(myMDNS)) return false;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* req) override {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    }
};

// ─────────────────────────────────────────────
//  HTTP útvonalak
// ─────────────────────────────────────────────
void setupRoutes() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", INDEX_HTML);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildStatusJson());
    });

    // Captive portal URL-ek átirányítása
    server.on("/canonical.html",   HTTP_ANY, [](AsyncWebServerRequest* r){ send_redirect(r); });
    server.on("/success.txt",      HTTP_ANY, [](AsyncWebServerRequest* r){ r->send(200); });
    server.on("/ncsi.txt",         HTTP_ANY, [](AsyncWebServerRequest* r){ send_redirect(r); });
    server.on("/connecttest.txt",  HTTP_ANY, [](AsyncWebServerRequest* r){ send_redirect(r); });
    server.on("/generate_204",     HTTP_ANY, [](AsyncWebServerRequest* r){ send_redirect(r); });
    server.on("/redirect",         HTTP_ANY, [](AsyncWebServerRequest* r){ send_redirect(r); });

    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Nem talalhato");
    });

    static CaptiveRequestHandler captiveHandler;
    server.addHandler(&captiveHandler);
}

// ─────────────────────────────────────────────
//  WiFi AP + szolgáltatások indítása
// ─────────────────────────────────────────────
void startWifi() {
    if (wifiRunning) return;

    ESP_LOGI(TAGMAIN, "WiFi AP indul...");

    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP(AP_SSID, nullptr, 1, 0, 2);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAGMAIN, "AP IP: %s", WiFi.softAPIP().toString().c_str());

    dnsServer.setTTL(300);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());

    if (MDNS.begin(MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        ESP_LOGI(TAGMAIN, "mDNS: %s.local", MDNS_HOST);
    }

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    setupRoutes();

    ElegantOTA.begin(&server);
    ElegantOTA.onStart([]() {
        otaStarted = true;
        ESP_LOGI(TAGMAIN, "OTA frissites megkezdve");
    });
    ElegantOTA.onEnd([](bool success) {
        ESP_LOGI(TAGMAIN, "OTA vege, siker: %d", success);
    });

    server.begin();
    wifiRunning  = true;
    wifiStartMs  = millis();
    ESP_LOGI(TAGMAIN, "HTTP szerver elindult.");
}

// ─────────────────────────────────────────────
//  WiFi leállítása és újraindítás
//  [OLD MODE only] – az új módban nincs WiFi újraindítás
// ─────────────────────────────────────────────
#ifndef USE_ALWAYS_RUNNING_MODE
void stopWifiAndRestart() {
    ESP_LOGI(TAGMAIN, "WiFi leallitas, ujraindulas...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(200);
    ESP.restart();
}
#endif // ifndef USE_ALWAYS_RUNNING_MODE

// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────
SET_LOOP_TASK_STACK_SIZE(6144);

void setup() {
    //Serial.begin(115200);
    //delay(1000);

#ifdef USE_ALWAYS_RUNNING_MODE
    ESP_LOGW(TAGMAIN, "Indul... [ALWAYS RUNNING MODE]");
#else
    ESP_LOGW(TAGMAIN, "Indul... [DEEP SLEEP MODE]");
#endif

    // ── Mutex létrehozása – minden más előtt ──
    g_stateMutex = xSemaphoreCreateMutex();
    configASSERT(g_stateMutex);

    // ── GPIO hold feloldása (előző deep sleep maradványa) ──
    releasePWMHold();
    gpio_hold_dis((gpio_num_t)PIEZO);

    // ── GPIO beállítások ──────────────────────
    pinMode(NYOMOGOMB,  INPUT_PULLUP);
    pinMode(PIN_WIFIEN, INPUT_PULLUP);
    pinMode(PIEZO,      OUTPUT);
    digitalWrite(PIEZO, LOW);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // ── PWM csatornák inicializálása ──────────
    ledcSetup(0, 1000, PWM_RES);
    ledcSetup(1, 1000, PWM_RES);
    ledcSetup(2, 1000, PWM_RES);
    ledcAttachPin(PIN_R, 0);
    ledcAttachPin(PIN_G, 1);
    ledcAttachPin(PIN_B, 2);

    lastJumperState = digitalRead(PIN_WIFIEN);

    RgbFader::init(gamma10);

    // ── Flash adatok betöltése ─────────────────
    loadFromPreferences();
    ESP_LOGI(TAGMAIN, "%d szin betoltve, aktiv: %d, alvasi ido: %d perc",
             colorCount, activeIndex, sleepMinutes);

// ════════════════════════════════════════════════════════════════
//  [OLD MODE] – Deep-sleep alapú setup logika
// ════════════════════════════════════════════════════════════════
#ifndef USE_ALWAYS_RUNNING_MODE
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // Ha NEM gombnyomásra ébredt (pl. tápfeszültség rákapcsolás vagy reset)
  if (cause != ESP_SLEEP_WAKEUP_GPIO) {
    ESP_LOGI(TAGMAIN, "Nem gombnyomásos ébredés -> azonnali alvás.");
    goToDeepSleep();
  }

  // Ha gombnyomásra ébredt, megvárjuk az 5 másodpercet (BTN_EBRED)
  ESP_LOGI(TAGMAIN, "Ébredés gombnyomásra, ellenőrzés (%d ms)...", BTN_EBRED);
  uint32_t pressStart = millis();
  bool longPressOk = true;

  while (millis() - pressStart < BTN_EBRED) {
    if (digitalRead(NYOMOGOMB) == HIGH) { // Ha elengedte a gombot idő előtt
      longPressOk = false;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!longPressOk) {
    ESP_LOGI(TAGMAIN, "Túl rövid ébresztési kísérlet -> visszaalvás.");
    // Megvárjuk, amíg elengedi, hogy ne ébredjen vissza azonnal
    while (digitalRead(NYOMOGOMB) == LOW) {
      vTaskDelay(10);
    }
    goToDeepSleep();
  }

  // Ha ide eljut, akkor megvolt az 5 mp nyomás!
  ESP_LOGI(TAGMAIN, "Sikeres ébredés, rendszer indul.");

  g_skipBtnUntilRelease = true;

  vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS)); // debounce

  buzzerBeepAsync(500); // Visszajelzés a sikeres bekapcsolásról
  g_ledsOn = true; // Az új módban ez a flag vezérli a timeout logikát

  // Ébredéskor fade-in (fekete → aktív szín)
  RgbFader::fadeIn(colors[activeIndex].r, colors[activeIndex].g,
                   colors[activeIndex].b, colors[activeIndex].freq);
  //  applyPWM(colors[activeIndex].r, colors[activeIndex].g,
  //  colors[activeIndex].b,
  //           colors[activeIndex].freq);
  activeStartMs = millis();

#else // USE_ALWAYS_RUNNING_MODE

  // ── Első akku mérés ───────────────────────
  g_battVoltage = readBatteryVoltage();
  g_battLastReadMs = millis();

  // ── Aktív szín beállítása PWM-re ──────────
  // Induláskor az utolsó aktív szín azonnal bekapcsol.
  applyPWM(0, 0, 0, 1000);
  g_ledsOn = false;

  /*    applyPWM(colors[activeIndex].r, colors[activeIndex].g,
               colors[activeIndex].b, colors[activeIndex].freq);
      g_ledsOn = true;*/

  // ── Alvási időzítő indítása ───────────────
  activeStartMs = millis();

// ── WiFi döntés (CSAK boot-kor, jumper alapján, fix) ─────────
// A boot utáni PIN_WIFIEN állapot dönt. Később a jumper állapotát
// NEM figyeljük, NEM indítjuk újra a WiFit, NEM állítjuk le.
#endif // USE_ALWAYS_RUNNING_MODE

#ifdef URESC3
  bool isC3 = HIGH;
#else
  bool isC3 = LOW;
#endif
  if (digitalRead(PIN_WIFIEN) == isC3) {
    ESP_LOGI(TAGMAIN, "[ARM] PIN_WIFIEN LOW -> WiFi AP indul.");
    startWifi();
    wifiRunning = true;
  } else {
    ESP_LOGI(TAGMAIN, "[ARM] PIN_WIFIEN HIGH -> WiFi nem indul.");
  }

#ifdef WIFI_LED_VILLOGAS
    // ── FreeRTOS Blink Task indítása ──────────
    // Stack mérete: 1024 szó (4096 byte) elegendő egy egyszerű blink taskhoz.
    // Prioritás: 1 (minimális, hogy ne konkuráljon a loop taskkal).
    // Core: 0-ra pinelve (ESP32-C3-on csak 1 core van, ezért itt nem kritikus,
    //       de xTaskCreatePinnedToCore tskNO_AFFINITY is megfelelő).
    if (wifiRunning) {
        ESP_LOGI(TAGMAIN, "[ARM] Blink task inditasa...");
    BaseType_t blinkResult = xTaskCreatePinnedToCore(
        blinkTask,              // Task függvény
        "blinkTask",            // Debug név
        2048,                   // Stack méret (szavakban)
        nullptr,                // Paraméter
        1,                      // Prioritás (alacsony)
        &g_blinkTaskHandle,     // Handle (opcionálisan leállítható)
        tskNO_AFFINITY          // Core affinity (ESP32-C3: single core)
    );

    if (blinkResult != pdPASS) {
        ESP_LOGE(TAGMAIN, "[ARM] Blink task letrehozasa sikertelen!");
    } else {
        ESP_LOGI(TAGMAIN, "[ARM] Blink task elindult.");
    }
    } else {
        ESP_LOGI(TAGMAIN, "[ARM] WiFi nem indul, blink task nem inditva.");
    }
#endif // WIFI_LED_VILLOGAS

    if (g_ledsOn) {
//      applyPWM(colors[activeIndex].r, colors[activeIndex].g,
//               colors[activeIndex].b, colors[activeIndex].freq);
    RgbFader::fadeIn(colors[activeIndex].r, colors[activeIndex].g,
                 colors[activeIndex].b, colors[activeIndex].freq);
    }
    activeStartMs = millis();
}

// ─────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────
void loop() {

    // ── ElegantOTA és DNS kiszolgálás ─────────
    if (wifiRunning) {
        ElegantOTA.loop();
        dnsServer.processNextRequest();
    }

    // ── WS cleanup: elszakadt kliensek törlése ─
    // FIX S4: Hosszú üzemidő alatt megelőzi a heap fragmentációt
    static uint32_t lastWsCleanupMs = 0;
    if (wifiRunning && (millis() - lastWsCleanupMs > 30000)) {
        ws.cleanupClients();
        lastWsCleanupMs = millis();
    }

    // ── Akkufeszültség mérés (percenként) ─────
    // Alacsony akku esetén is csak LED kikapcsolás, az MCU fut tovább.
    // Ha a feszültség extrém alacsony (brownout közel), a hardver is véd.
    static uint32_t lastBattPushMs = 0;
    if (millis() - g_battLastReadMs > 60000) {
        g_battVoltage = readBatteryVoltage();
        g_battLastReadMs = millis();
        ESP_LOGI(TAGMAIN, "[ARM] Akku: %.2f V", g_battVoltage);

        if (g_battVoltage < BATT_LOW_V && g_battVoltage > 1.0f) {
            ESP_LOGW(TAGMAIN, "[ARM] Alacsony akku (%.2f V), LED-ek lekapcsolva.",
                     g_battVoltage);
            if (g_ledsOn) {
                /*buzzerErrorAsync();
                applyPWM(0, 0, 0, 1000);
                g_ledsOn = false;
                voltFeszHiba = true;
                ws.textAll(buildStatusJson());*/
                buzzerError(4000);
#ifndef USE_ALWAYS_RUNNING_MODE
                goToDeepSleep();
#endif
            }
        }
    }

    if (wifiRunning && (millis() - lastBattPushMs > BTN_EBRED)) {
      lastBattPushMs = millis();
      g_battVoltage = readBatteryVoltage(); // ← friss mérés push előtt
      ws.textAll(buildStatusJson());
    }

    // ── Gomb: felfutó élre színváltás / 5mp tartva → azonnal deep sleep ──
    {
      static bool lastRaw = HIGH;
      static uint32_t lastDebounceMs = 0;
      static bool debouncedBtn = HIGH;
      static bool btnWasPressed = false;
      static uint32_t btnPressMs = 0;

      if (g_skipBtnUntilRelease) {
        // Ébresztési gombnyomás "lenyelése" – várjuk az elengedést
        if (digitalRead(NYOMOGOMB) == HIGH) {
          g_skipBtnUntilRelease = false;
          lastRaw = false;
          debouncedBtn = false;
          btnWasPressed = false;
        }
      } else {
        bool rawBtn = (digitalRead(NYOMOGOMB) == LOW);
        if (rawBtn != lastRaw) {
          lastDebounceMs = millis();
          lastRaw = rawBtn;
        }
        bool stableBtn = ((millis() - lastDebounceMs) >= BTN_DEBOUNCE_MS)
                             ? rawBtn
                             : debouncedBtn;
        debouncedBtn = stableBtn;

        // Lenyomás észlelése
        if (stableBtn && !btnWasPressed) {
          btnWasPressed = true;
          btnPressMs = millis();
        }

        // ── 5mp tartva → AZONNAL deep sleep, nem kell elengedni ───────
        if (btnWasPressed && stableBtn && (millis() - btnPressMs >= 3000)) {
          ESP_LOGI(TAGMAIN, "[BTN] 5mp+ tartva -> azonnal deep sleep");
#ifndef USE_ALWAYS_RUNNING_MODE
          buzzerError(500);
          goToDeepSleep();
#endif
      }

      // Elengedés
      if (!stableBtn && btnWasPressed) {
        uint32_t dur = millis() - btnPressMs;
        btnWasPressed = false;

        if (dur <= 1000) {
          // ── max 1 mp → színváltás ────────────────────────────────
          uint8_t cr, cg, cb;
          uint16_t cf;
          if (LOCK_STATE() == pdTRUE) {
            activeIndex++;
            if (activeIndex >= colorCount)
              activeIndex = 0;
            cr = colors[activeIndex].r;
            cg = colors[activeIndex].g;
            cb = colors[activeIndex].b;
            cf = colors[activeIndex].freq;
            UNLOCK_STATE();
          }
          //applyPWM(cr, cg, cb, cf);
          RgbFader::crossFade(cr, cg, cb, cf);
          g_ledsOn = true;
          activeStartMs = millis();
          pendingWsUpdate = true;
          pendingNvsSave = true;
          lastChangeMs = millis();

          // ── Rövid csippanás ──────────────────────────────────────
          digitalWrite(PIEZO, HIGH);
          vTaskDelay(pdMS_TO_TICKS(150));
          digitalWrite(PIEZO, LOW);
          // ─────────────────────────────────────────────────────────

          ESP_LOGI(TAGMAIN, "[BTN] Szin leptes -> %d (dur=%lums)", activeIndex,
                   dur);
        }
        // 1–5 mp közötti nyomás: ignorálva
       }
      }
    }

    // Az MCU, WiFi, WebSocket, OTA mind fut tovább!
    // Gombnyomásra újra bekapcsol (nem ébresztés, normál esemény).
    if (g_ledsOn) {
        uint32_t elapsed = millis() - activeStartMs;
        uint32_t limitMs = (uint32_t)sleepMinutes * 60UL * 1000UL;

        if (elapsed >= limitMs) {
#ifdef USE_ALWAYS_RUNNING_MODE
          // Always Running mód: csak lekapcsoljuk a LED-et, MCU marad
          ESP_LOGI(TAGMAIN, "[ARM] LED timeout (%d perc), MCU marad.",
                   sleepMinutes);
          applyPWM(0, 0, 0, 1000);
          buzzerBeepAsync(3000);
          g_ledsOn = false;
          ws.textAll(buildStatusJson());
#else
          // Deep Sleep mód: teljes kikapcsolás
          ESP_LOGI(TAGMAIN, "Timeout (%d perc) -> Mélyalvás.", sleepMinutes);
          buzzerBeep(3000);
          goToDeepSleep();
#endif
        }
    }

    if ((pendingWsUpdate || pendingNvsSave) &&
        (millis() - lastChangeMs > 300)) {
      if (pendingWsUpdate && wifiRunning) {
        ws.textAll(buildStatusJson());
        pendingWsUpdate = false;
      }
      if (pendingNvsSave) {
        saveIndexAndTimer(); // Most már ráér menteni
        pendingNvsSave = false;
      }
    }

    // Kis szünet, CPU kímélése (mindkét módban)
    vTaskDelay(pdMS_TO_TICKS(20));
}
