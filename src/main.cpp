// ════════════════════════════════════════════════════════════════
//  Enki Hungrál – ESP32-C3 SuperMini
//  RGB LED vezérlő deep-sleep alapú működéssel, WiFi AP + OTA
// ════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include "webpage.h"

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

#define PWM_RES     8           // 8 bit felbontás → 0–255
#define MAX_COLORS  25          // Maximum tárolható szín

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
//  Alapértelmezett 3 szín (ha még nincs flash adat)
// ─────────────────────────────────────────────
static const ColorEntry DEFAULT_COLORS[3] PROGMEM = {
    { "Meleg fehér",  255, 180,  80, 1000 },
    { "Kék",            0,   0, 255, 1000 },
    { "Zöld",           0, 200,  50, 1000 },
};

// ─────────────────────────────────────────────
//  Globális állapot
// ─────────────────────────────────────────────
ColorEntry colors[MAX_COLORS];
int        colorCount  = 0;
int        activeIndex = 0;     // Aktív szín indexe

Preferences    prefs;
AsyncWebServer server(80);
DNSServer      dnsServer;

IPAddress local_ip(192, 168, 99, 9);
IPAddress gateway (192, 168, 99, 9);
IPAddress subnet  (255, 255, 255, 0);

// WiFi állapot követése
bool wifiRunning  = false;
bool otaStarted   = false;      // Volt-e OTA folyamat
uint32_t wifiStartMs = 0;

// Ébresztési timer (percben, 5–60)
uint8_t sleepMinutes = 10;

// Aktív üzemmód időzítő
uint32_t activeStartMs = 0;

// Gomb lenyomás időpontja (szín léptető)
uint32_t btnPressMs    = 0;
bool     btnWasPressed = false;

// ─────────────────────────────────────────────
//  Feszültségosztó konstans
//  V_akku = V_adc * (33k + 100k) / 100k = V_adc * 1.33
//  V_adc  = ADC_érték / 4095 * 3.3
// ─────────────────────────────────────────────
float readBatteryVoltage() {
    // Több minta átlaga a zajszűréshez
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(AKKUFESZ);
        delay(2);
    }
    float adc_v = (sum / 8.0f) / 4095.0f * 3.3f;
    return adc_v * 1.33f;  // feszültségosztó kompenzáció
}

// ─────────────────────────────────────────────
//  PWM kimenet beállítása
// ─────────────────────────────────────────────
void applyPWM(uint8_t r, uint8_t g, uint8_t b, uint16_t freq) {
    ledcChangeFrequency(PIN_R, freq, PWM_RES);
    ledcChangeFrequency(PIN_G, freq, PWM_RES);
    ledcChangeFrequency(PIN_B, freq, PWM_RES);
    ledcWrite(PIN_R, r);
    ledcWrite(PIN_G, g);
    ledcWrite(PIN_B, b);
}

// ─────────────────────────────────────────────
//  Buzzer: adott ideig szól
// ─────────────────────────────────────────────
void buzzerBeep(uint32_t ms) {
    digitalWrite(PIEZO, HIGH);
    delay(ms);
    digitalWrite(PIEZO, LOW);
}

// ─────────────────────────────────────────────
//  Mélysomba menetel
// ─────────────────────────────────────────────
void goToDeepSleep() {
    ESP_LOGI(TAGMAIN, "Mélyalvás indul...");

    // LED-ek kikapcsolása
    applyPWM(0, 0, 0, 1000);

    // Buzzer: 2 mp szól, majd alvás
    buzzerBeep(2000);

    // PWM csatornák lecsatolása (GPIO lebegő legyen alvás alatt)
    ledcDetachPin(PIN_R);
    ledcDetachPin(PIN_G);
    ledcDetachPin(PIN_B);

    // NYOMOGOMB lefutó élre ébresztés (GPIO3 = RTC GPIO)
    //esp_sleep_enable_ext0_wakeup((gpio_num_t)NYOMOGOMB, 0); // 0 = LOW szint
    esp_deep_sleep_enable_gpio_wakeup(1 << NYOMOGOMB, ESP_GPIO_WAKEUP_GPIO_LOW);

    esp_deep_sleep_start();
}

// ─────────────────────────────────────────────
//  Preferences – betöltés
// ─────────────────────────────────────────────
void loadFromPreferences() {
    prefs.begin("enki", true);  // read-only

    // Aktív szín index
    activeIndex  = prefs.getInt("idx",  0);
    // Alvási idő percben
    sleepMinutes = prefs.getUChar("sleepm", 10);

    // Szín lista mérete
    colorCount = prefs.getInt("count", 0);

    if (colorCount == 0) {
        // Első indulás: alapértelmezett 3 szín betöltése PROGMEM-ből
        colorCount = 3;
        for (int i = 0; i < 3; i++) {
            memcpy_P(&colors[i], &DEFAULT_COLORS[i], sizeof(ColorEntry));
        }
    } else {
        colorCount = min(colorCount, MAX_COLORS);
        for (int i = 0; i < colorCount; i++) {
            char key[16];
            snprintf(key, sizeof(key), "n%d", i);
            prefs.getString(key, colors[i].name, sizeof(colors[i].name));
            snprintf(key, sizeof(key), "r%d", i);  colors[i].r    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "g%d", i);  colors[i].g    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "b%d", i);  colors[i].b    = (uint8_t)prefs.getInt(key, 128);
            snprintf(key, sizeof(key), "f%d", i);  colors[i].freq = (uint16_t)prefs.getInt(key, 1000);
        }
    }

    prefs.end();

    // Határon belül tartás
    if (activeIndex >= colorCount) activeIndex = 0;
    sleepMinutes = constrain(sleepMinutes, 5, 60);
}

// ─────────────────────────────────────────────
//  Preferences – teljes mentés
// ─────────────────────────────────────────────
void saveToPreferences() {
    prefs.begin("enki", false); // read-write
    prefs.putInt("idx",    activeIndex);
    prefs.putUChar("sleepm", sleepMinutes);
    prefs.putInt("count",  colorCount);
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
    prefs.putInt("idx",    activeIndex);
    prefs.putUChar("sleepm", sleepMinutes);
    prefs.end();
}

// ─────────────────────────────────────────────
//  JSON állapot (HTTP /status végponthoz)
// ─────────────────────────────────────────────
String buildStatusJson() {
    static char buf[3200];
    int pos = 0;

    float batt = readBatteryVoltage();

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
            if (idx >= 0 && idx < colorCount) {
                colors[idx].r    = (uint8_t)constrain(r,    0, 255);
                colors[idx].g    = (uint8_t)constrain(g,    0, 255);
                colors[idx].b    = (uint8_t)constrain(b,    0, 255);
                colors[idx].freq = (uint16_t)constrain(freq, 1, 40000);
                activeIndex = idx;
                applyPWM(colors[idx].r, colors[idx].g, colors[idx].b, colors[idx].freq);
            }
        }
    }
    else if (msg.startsWith("NAME:")) {
        int idx; char name[32];
        if (sscanf(msg.c_str(), "NAME:%d:%31[^\n]", &idx, name) == 2) {
            if (idx >= 0 && idx < colorCount) {
                strncpy(colors[idx].name, name, sizeof(colors[idx].name));
            }
        }
    }
    else if (msg.startsWith("SLEEP:")) {
        int m;
        if (sscanf(msg.c_str(), "SLEEP:%d", &m) == 1) {
            sleepMinutes = (uint8_t)constrain(m, 5, 60);
        }
    }
    else if (msg == "SAVE") {
        // PWM frissítés az aktív színre
        if (activeIndex >= 0 && activeIndex < colorCount) {
            applyPWM(colors[activeIndex].r, colors[activeIndex].g,
                     colors[activeIndex].b, colors[activeIndex].freq);
        }
        saveToPreferences();
        // Visszajelzés az időzítő újraindításával
        activeStartMs = millis();
        // Visszaküldjük a friss állapotot
        if (client) client->text(buildStatusJson());
    }
    else if (msg == "ADD") {
        if (colorCount < MAX_COLORS) {
            strncpy(colors[colorCount].name, "Új szín", sizeof(colors[colorCount].name));
            colors[colorCount].r    = 128;
            colors[colorCount].g    = 128;
            colors[colorCount].b    = 128;
            colors[colorCount].freq = 1000;
            colorCount++;
        }
    }
    else if (msg.startsWith("DEL:")) {
        int idx;
        if (sscanf(msg.c_str(), "DEL:%d", &idx) == 1) {
            if (idx >= 0 && idx < colorCount) {
                for (int i = idx; i < colorCount - 1; i++) colors[i] = colors[i + 1];
                colorCount--;
                if (activeIndex >= colorCount) activeIndex = colorCount - 1;
                if (activeIndex < 0) activeIndex = 0;
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
        // Azonnal küldjük az aktuális állapotot
        client->text(buildStatusJson());
    }
    else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGI(TAGMAIN, "WS kliens lecsatlakozott: %u", client->id());
    }
    else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            String msg = String((char*)data).substring(0, len);
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
    bool canHandle(AsyncWebServerRequest* req) {
        if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) return false;
        String host   = req->host();
        String myIP   = WiFi.softAPIP().toString();
        String myMDNS = String(MDNS_HOST) + ".local";
        if (host == myIP || host.equalsIgnoreCase(myMDNS)) return false;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* req) {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    }
};

// ─────────────────────────────────────────────
//  HTTP útvonalak
// ─────────────────────────────────────────────
void setupRoutes() {
    // Főoldal
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", INDEX_HTML);
    });

    // JSON státusz (oldalbetöltéskor is lekérhető)
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
    WiFi.softAP(AP_SSID, nullptr, 1, 0, 4);  // nyílt AP, max 4 kliens
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAGMAIN, "AP IP: %s", WiFi.softAPIP().toString().c_str());

    // DNS szerver: minden kérés az AP IP-re mutat (captive portal)
    dnsServer.setTTL(300);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());

    // mDNS: enkiled.local
    if (MDNS.begin(MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        ESP_LOGI(TAGMAIN, "mDNS: %s.local", MDNS_HOST);
    }

    // WebSocket
    static AsyncWebSocket ws("/ws");
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // HTTP útvonalak
    setupRoutes();

    // ElegantOTA (aszinkron mód)
    ElegantOTA.begin(&server);
    ElegantOTA.onStart([]() {
        otaStarted = true;
        ESP_LOGI(TAGMAIN, "OTA frissítés megkezdve");
    });
    ElegantOTA.onEnd([](bool success) {
        ESP_LOGI(TAGMAIN, "OTA vége, siker: %d", success);
        // Az ESP újraindul – boot után a jumper ellenőrzés megtörténik
    });

    server.begin();
    wifiRunning = true;
    wifiStartMs = millis();
    ESP_LOGI(TAGMAIN, "HTTP szerver elindult.");
}

// ─────────────────────────────────────────────
//  WiFi leállítása és újraindítás
// ─────────────────────────────────────────────
void stopWifiAndRestart() {
    ESP_LOGI(TAGMAIN, "WiFi leállítás, újraindítás...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    ESP.restart();
}

// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────
SET_LOOP_TASK_STACK_SIZE(6144);

void setup() {
    Serial.begin(115200);
    ESP_LOGW(TAGMAIN, "Indul...");

    // ── GPIO beállítások ──────────────────────
    pinMode(NYOMOGOMB,  INPUT_PULLUP);
    pinMode(PIN_WIFIEN, INPUT_PULLUP);
    pinMode(PIEZO,      OUTPUT);
    digitalWrite(PIEZO, LOW);

    // ── PWM csatornák inicializálása ──────────
    ledcSetup(0, 1000, PWM_RES);
    ledcSetup(1, 1000, PWM_RES);
    ledcSetup(2, 1000, PWM_RES);
    ledcAttachPin(PIN_R, 0);
    ledcAttachPin(PIN_G, 1);
    ledcAttachPin(PIN_B, 2);

    // ── Flash adatok betöltése ─────────────────
    loadFromPreferences();
    ESP_LOGI(TAGMAIN, "%d szin betoltve, aktiv: %d, alvasi ido: %d perc",
             colorCount, activeIndex, sleepMinutes);

    // ── Ébresztési ok vizsgálata ──────────────
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        // GPIO ébresztő – ellenőrizzük, hogy legalább 5 mp-ig nyomva marad-e
        ESP_LOGI(TAGMAIN, "Ebreszes gombra. 5 masodpercet varok...");

        uint32_t pressStart = millis();
        bool longPress = false;

        while (millis() - pressStart < 5000) {
            if (digitalRead(NYOMOGOMB) == HIGH) {
                // Elengedték a gombot 5 mp előtt → vissza az alvásba
                ESP_LOGI(TAGMAIN, "Rovid nyomas, vissza alvásba.");
                break;
            }
            delay(50);
        }

        if (digitalRead(NYOMOGOMB) == LOW) {
            // 5 mp-ig nyomva volt → folytatás
            longPress = true;
        }

        if (!longPress) {
            // PWM csatornák lecsatolása mielőtt alvásba megy
            ledcDetachPin(PIN_R);
            ledcDetachPin(PIN_G);
            ledcDetachPin(PIN_B);
            //esp_sleep_enable_ext0_wakeup((gpio_num_t)NYOMOGOMB, 0);
            esp_deep_sleep_enable_gpio_wakeup(1 << NYOMOGOMB, ESP_GPIO_WAKEUP_GPIO_LOW);
            esp_deep_sleep_start();
        }

        ESP_LOGI(TAGMAIN, "Hosszu nyomas OK, aktiv uzemmod.");
    }
    // else: első indulás (tápfeszültség, USB reset) → normál boot, nincs 5 mp várakozás

    // ── Aktív szín beállítása PWM-re ──────────
    applyPWM(colors[activeIndex].r, colors[activeIndex].g,
             colors[activeIndex].b, colors[activeIndex].freq);

    // ── Alvási időzítő indítása ───────────────
    activeStartMs = millis();

    // ── WiFi döntés: jumper zárt ÉS lefutó él volt ──
    // Az első boot után a jumper zárt állapota elegendő az AP indításhoz,
    // de csak ha gombot is nyomtak (ébresztés után jártunk).
    // OTA utáni újrainduláskor (cause != EXT0) a jumper vizsgálata elegendő.
    if (digitalRead(PIN_WIFIEN) == LOW) {
        if (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
            // Lefutó él volt (gomb) ÉS jumper zárt → WiFi indítás
            startWifi();
        }
    }
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

    // ── Jumper kiesés figyelés (felfutó él) ───
    // Ha a WiFi fut, de a jumperet kihúzták és nincs OTA folyamat → restart
    static bool lastJumperState = LOW;
    bool currentJumper = digitalRead(PIN_WIFIEN);

    if (wifiRunning && !otaStarted &&
        ((millis() - wifiStartMs) > 4UL * 60UL * 1000UL) &&
        lastJumperState == LOW && currentJumper == HIGH) {
        // Felfutó él: jumper kihúzva
        ESP_LOGW(TAGMAIN, "Jumper kihuzva, ujraindulas...");
        delay(50);  // pergésmentesítés
        if (digitalRead(PIN_WIFIEN) == HIGH) {
            stopWifiAndRestart();
        }
    }
    lastJumperState = currentJumper;

    // ── Gomb: szín léptető (0.5–1.0 s nyomás) ──
    bool btnDown = (digitalRead(NYOMOGOMB) == LOW);

    if (btnDown && !btnWasPressed) {
        // Gomb lenyomásának kezdete
        btnPressMs    = millis();
        btnWasPressed = true;
    }

    if (!btnDown && btnWasPressed) {
        // Gomb elengedve – mérjük a nyomásidőt
        uint32_t pressDuration = millis() - btnPressMs;
        btnWasPressed = false;

        if (pressDuration >= 500 && pressDuration <= 1000) {
            // 0.5–1.0 s → következő szín
            activeIndex = (activeIndex + 1) % colorCount;
            applyPWM(colors[activeIndex].r, colors[activeIndex].g,
                     colors[activeIndex].b, colors[activeIndex].freq);
            saveIndexAndTimer();
            // Időzítő újraindítása
            activeStartMs = millis();
            ESP_LOGI(TAGMAIN, "Szin leptetés -> %d (%s)", activeIndex, colors[activeIndex].name);
        }
    }

    // ── Alvási időzítő lejárat ────────────────
    uint32_t elapsed = millis() - activeStartMs;
    uint32_t limitMs = (uint32_t)sleepMinutes * 60UL * 1000UL;

    if (elapsed >= limitMs) {
        ESP_LOGI(TAGMAIN, "Idozito lejart (%d perc), alvás.", sleepMinutes);
        goToDeepSleep();
    }

    // Kis szünet, CPU kímélése
    vTaskDelay(pdMS_TO_TICKS(20));
}
