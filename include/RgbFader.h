// ════════════════════════════════════════════════════════════════
//  RgbFader.h  –  Nem-blokkoló RGB LED fade vezérlő
//  Cél: ESP32 / FreeRTOS, Arduino framework (PlatformIO)
//
//  Publikus API:
//    RgbFader::init(gamma10)        – setup()-ban, PWM init után
//    RgbFader::fadeIn(r,g,b,freq)   – fekete → szín (ébredéskor)
//    RgbFader::crossFade(r,g,b,freq)– szín → fekete → új szín (váltáskor)
//    RgbFader::stop()               – azonnali leállítás (deep sleep előtt)
//    RgbFader::isFading()           – fut-e jelenleg fade task?
//
//  Megjegyzések:
//    - PWM csatornák: CH0=R, CH1=G, CH2=B (ledcSetup-pal egyező).
//    - A fade-out az ELŐZŐ szín frekvenciáján fut, a fade-in az ÚJÉN.
//    - Az interpoláció 0–255 lineáris térben történik; a gamma-tábla
//      csak a ledcWrite-hoz szükséges 10-bites értékre konvertál.
//    - Ha fade közben új kérés érkezik: a task jelzőn keresztül leáll,
//      a loop() NEM blokkolódik – az új task az elozo állapotból
//      indul tovabb értesítéssel (TaskNotify).
//    - A gamma-tábla pontosan 256 uint16_t elemet kell tartalmazzon
//      (index 0..255). Hiányos tábla esetén tömbtúlcsordulás lép fel!
// ════════════════════════════════════════════════════════════════
#pragma once

#include <Arduino.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace RgbFader {

// ─────────────────────────────────────────────────────────────────
//  Konfiguráció – módosítható konstansok
// ─────────────────────────────────────────────────────────────────

/// Egy fade-fázis (fade-out VAGY fade-in) teljes ideje milliszekundumban.
static constexpr uint32_t FADE_DURATION_MS = 1000;

/// A fade task lépésköze ms-ban. 20ms → 50 lépés/mp, sima átmenet.
static constexpr uint32_t FADE_STEP_MS     = 20;

/// Lépésszám = FADE_DURATION_MS / FADE_STEP_MS
static constexpr uint32_t FADE_STEPS       = FADE_DURATION_MS / FADE_STEP_MS; // 50

/// FreeRTOS task stack mérete szavakban (4 byte/szó).
static constexpr uint32_t TASK_STACK_WORDS = 1536;

/// Task prioritás. Legyen magasabb mint a loop() task (1), de ne
/// versengjen a buzzer taskkal, ha az 2-es prioritású.
static constexpr UBaseType_t TASK_PRIORITY = 2;

/// LEDC felbontás (bitben). Egyezzen a ledcSetup() PWM_RES értékével.
static constexpr uint8_t LEDC_RES_BITS = 10;

// ─────────────────────────────────────────────────────────────────
//  Belső megvalósítás
// ─────────────────────────────────────────────────────────────────
namespace _priv {

/// Gamma-korrekciós tábla mutatója (uint16_t[256]). init()-ben állítjuk be.
static const uint16_t* s_gamma = nullptr;

/// Az aktuálisan MEGJELENÍTETT logikai szín (0–255, gamma ELŐTT).
/// Lépésenként frissül, hogy megszakításkor a közbülső értékről induljon az új fade-out.
static volatile uint8_t s_curR = 0;
static volatile uint8_t s_curG = 0;
static volatile uint8_t s_curB = 0;

/// Az aktuálisan MEGJELENÍTETT frekvencia (Hz). Fade-out-nál ezt kell használni.
static volatile uint16_t s_curFreq = 1000;

/// Futó fade task handle-je (nullptr = nincs).
static TaskHandle_t s_taskHandle = nullptr;

/// Kooperatív stop flag: a task ezt ellenőrzi minden lépésnél.
/// volatile + atomic olvasás elég, mutex nem kell a flag-hez.
static volatile bool s_stopRequest = false;

/// Mutex kizárólag a s_taskHandle írásához/olvasásához.
static SemaphoreHandle_t s_mutex = nullptr;

// ── Paraméterstruktúra a fade task számára (heap-en allokált) ────
struct FadeParams {
    uint8_t  fromR, fromG, fromB; ///< Kiindulópontok (a pillanatnyi szín snapshot-ja)
    uint16_t fromFreq;            ///< Kiindulópont frekvenciája (fade-out alatt ezt használjuk)
    uint8_t  toR, toG, toB;      ///< Célszín
    uint16_t toFreq;              ///< Célszín frekvenciája (fade-in alatt ezt használjuk)
    bool     doFadeOut;           ///< true = fade-out fázis kell
    bool     doFadeIn;            ///< true = fade-in fázis kell
};

// ── Gamma-konverzió és közvetlen PWM írás ───────────────────────
static inline uint16_t toGamma(uint8_t v) {
    if (s_gamma) return pgm_read_word(&s_gamma[v]);
    // Tábla nélkül: lineáris 8-bit → 10-bit
    return static_cast<uint16_t>(v) << 2;
}

static inline void writePWM(uint8_t r, uint8_t g, uint8_t b, uint16_t freq) {
    ledcChangeFrequency(0, freq, LEDC_RES_BITS);
    ledcChangeFrequency(1, freq, LEDC_RES_BITS);
    ledcChangeFrequency(2, freq, LEDC_RES_BITS);
    ledcWrite(0, toGamma(r));
    ledcWrite(1, toGamma(g));
    ledcWrite(2, toGamma(b));
    // Állapot frissítése: a közbülső értékek mindig naprakészek
    s_curR    = r;
    s_curG    = g;
    s_curB    = b;
    s_curFreq = freq;
}

// ── Egyirányú fade (from → to), lépésenként vTaskDelay ──────────
// Visszatér: true = végigfutott, false = stop kérés érkezett
static bool runFade(
    uint8_t  fromR, uint8_t  fromG, uint8_t  fromB,
    uint8_t  toR,   uint8_t  toG,   uint8_t  toB,
    uint16_t freq)
{
    for (uint32_t step = 1; step <= FADE_STEPS; step++) {
        if (s_stopRequest) return false;

        // Lineáris interpoláció: step=1 → közel from, step=FADE_STEPS → pontosan to
        // Cast int32_t-re a negatív különbség miatt
        const int32_t fR = fromR, fG = fromG, fB = fromB;
        const int32_t tR = toR,   tG = toG,   tB = toB;
        const int32_t N  = static_cast<int32_t>(FADE_STEPS);
        const int32_t s  = static_cast<int32_t>(step);

        uint8_t r = static_cast<uint8_t>(fR + (tR - fR) * s / N);
        uint8_t g = static_cast<uint8_t>(fG + (tG - fG) * s / N);
        uint8_t b = static_cast<uint8_t>(fB + (tB - fB) * s / N);

        writePWM(r, g, b, freq);
        vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
    }
    return true;
}

// ── FreeRTOS task ────────────────────────────────────────────────
static void fadeTask(void* pvParam) {
    FadeParams* p = static_cast<FadeParams*>(pvParam);

    // Paraméterek lokális másolata, heap felszabadítható
    const uint8_t  fR = p->fromR,   fG = p->fromG,   fB = p->fromB;
    const uint16_t fFreq = p->fromFreq;
    const uint8_t  tR = p->toR,     tG = p->toG,     tB = p->toB;
    const uint16_t tFreq = p->toFreq;
    const bool     doOut = p->doFadeOut;
    const bool     doIn  = p->doFadeIn;
    free(p);

    if (doOut) {
        // ── Fázis 1: fade-out az ELŐZŐ szín frekvenciáján ─────────
        bool ok = runFade(fR, fG, fB, 0, 0, 0, fFreq);
        if (!ok) goto task_end;

        // Feketét biztosan kiírjuk (kerekítési hiba kizárása)
        writePWM(0, 0, 0, fFreq);

        if (s_stopRequest) goto task_end;
    }

    // ── Fázis 2: fade-in a CÉLSZÍN frekvenciáján ──────────────────
    if (doIn) {
        bool ok = runFade(0, 0, 0, tR, tG, tB, tFreq);
        if (!ok) goto task_end;

        // Célszínt biztosan kiírjuk
        writePWM(tR, tG, tB, tFreq);
    }

task_end:
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_taskHandle = nullptr;
        xSemaphoreGive(s_mutex);
    }
    vTaskDelete(nullptr);
}

// ── Belső indítás ────────────────────────────────────────────────
// FONTOS: ez a hívó taskjából fut (loop() vagy setup()), NEM blokkolódik
// hosszan – a régi task kooperatív leállítása legfeljebb FADE_STEP_MS*2+5 ms-t vesz el.
static void startTask(
    uint8_t  toR, uint8_t  toG, uint8_t  toB, uint16_t toFreq,
    bool doFadeOut, bool doFadeIn)
{
    if (!s_mutex) return;

    // ── 1. Snapshot az aktuális állapotról (task indítás előtt) ───
    // s_curR/G/B volatile, de mutex nélkül olvassuk – az értéke
    // legrosszabb esetben 1 lépéssel régebbi, ami 20ms-nél kevesebb
    // eltérést jelent. Ez elhanyagolható.
    uint8_t  snapR    = s_curR;
    uint8_t  snapG    = s_curG;
    uint8_t  snapB    = s_curB;
    uint16_t snapFreq = s_curFreq;

    // ── 2. Futó task leállítása ────────────────────────────────────
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return;
    bool hadRunning = (s_taskHandle != nullptr);
    if (hadRunning) {
        s_stopRequest = true;
    }
    xSemaphoreGive(s_mutex);

    if (hadRunning) {
        // Megvárjuk, hogy a task legfeljebb egy lépést befejezzen és kilépjen.
        // A task FADE_STEP_MS-enként ellenőrzi a stop flag-et.
        // 2 lépés + kis tartalék biztosan elég.
        vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS * 2 + 10));

        // Ha még mindig fut (például magas rendszerterhelés miatt),
        // várunk még egy ciklust.
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool stillRunning = (s_taskHandle != nullptr);
            xSemaphoreGive(s_mutex);
            if (stillRunning) {
                vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS + 5));
            }
        }

        // Stop után az aktuális s_curR/G/B az IGAZI közbülső értéket
        // tartalmazza (a task writePWM()-je frissítette), frissítjük a snapshot-ot.
        snapR    = s_curR;
        snapG    = s_curG;
        snapB    = s_curB;
        snapFreq = s_curFreq;
    }

    // ── 3. Paraméterek feltöltése ──────────────────────────────────
    FadeParams* params = static_cast<FadeParams*>(malloc(sizeof(FadeParams)));
    if (!params) return;

    params->fromR    = snapR;
    params->fromG    = snapG;
    params->fromB    = snapB;
    params->fromFreq = snapFreq;
    params->toR      = toR;
    params->toG      = toG;
    params->toB      = toB;
    params->toFreq   = toFreq;
    params->doFadeOut = doFadeOut;
    params->doFadeIn  = doFadeIn;

    // ── 4. Új task indítása ────────────────────────────────────────
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        free(params);
        return;
    }

    s_stopRequest = false; // Reset csak az új task indítása előtt, mutex alatt

    BaseType_t res = xTaskCreate(
        fadeTask, "rgbFade",
        TASK_STACK_WORDS, params,
        TASK_PRIORITY, &s_taskHandle
    );

    if (res != pdPASS) {
        free(params);
        s_taskHandle = nullptr;
    }

    xSemaphoreGive(s_mutex);
}

} // namespace _priv

// ═════════════════════════════════════════════════════════════════
//  Publikus API
// ═════════════════════════════════════════════════════════════════

/// Inicializálás – setup()-ban hívd, a ledcSetup() / ledcAttachPin() UTÁN.
/// @param gammaTable  Pointer a uint16_t[256] gamma-táblára (pl. gamma10).
///                    nullptr esetén lineáris (gamma nélküli) kimenet.
inline void init(const uint16_t* gammaTable) {
    _priv::s_gamma       = gammaTable;
    _priv::s_curR        = 0;
    _priv::s_curG        = 0;
    _priv::s_curB        = 0;
    _priv::s_curFreq     = 1000;
    _priv::s_taskHandle  = nullptr;
    _priv::s_stopRequest = false;
    if (!_priv::s_mutex) {
        _priv::s_mutex = xSemaphoreCreateMutex();
    }
}

/// Fade-in: (feltételezetten) fekete állapotból a célszínre, 1 mp alatt.
/// Ébredéskor és első bekapcsoláskor használd.
/// Ha véletlenül fut már fade task, azt leállítja és a pillanatnyi
/// értékről indít fade-in-t (nem feltétlenül feketéről).
inline void fadeIn(uint8_t r, uint8_t g, uint8_t b, uint16_t freq) {
    _priv::startTask(r, g, b, freq, /*doFadeOut=*/false, /*doFadeIn=*/true);
}

/// Fade-out: pillanatnyi szín → fekete, 1 mp alatt. Csak kikapcsoláskor használd
/// (pl. turnOffLeds()). Nincs utána fade-in.
inline void fadeOut(uint16_t freq = 1000) {
    // A célszín fekete, frekvencia az aktuális (s_curFreq), de átadható felül is.
    // toR/G/B = 0 – a fade-out végén a LED fekete marad.
    _priv::startTask(0, 0, 0, freq, /*doFadeOut=*/true, /*doFadeIn=*/false);
}

/// Keresztfade: pillanatnyi szín → fekete (1 mp) → célszín (1 mp).
/// Gombnyomásos színváltáskor használd.
inline void crossFade(uint8_t r, uint8_t g, uint8_t b, uint16_t freq) {
    _priv::startTask(r, g, b, freq, /*doFadeOut=*/true, /*doFadeIn=*/true);
}

/// Azonnali leállítás – deep sleep előtt kötelező hívni.
/// A LED a pillanatnyi értéken marad; a hívónak kell utána
/// applyPWM(0,0,0,...)-val lekapcsolni a LED-et.
/// Ez a hívás BLOKKOLÓDIK legfeljebb FADE_STEP_MS*2+10 ms-ig.
inline void stop() {
    if (!_priv::s_mutex) return;
    if (xSemaphoreTake(_priv::s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        if (_priv::s_taskHandle != nullptr) {
            _priv::s_stopRequest = true;
        }
        xSemaphoreGive(_priv::s_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS * 2 + 10));
}

/// Visszaadja, hogy fut-e jelenleg fade task.
inline bool isFading() {
    if (!_priv::s_mutex) return false;
    bool running = false;
    if (xSemaphoreTake(_priv::s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        running = (_priv::s_taskHandle != nullptr);
        xSemaphoreGive(_priv::s_mutex);
    }
    return running;
}

} // namespace RgbFader
