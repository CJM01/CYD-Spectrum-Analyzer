/*
 * ============================================================
 *  INMP441 Microphone Diagnostic Tool
 *  Board  : Hosyond 4" ESP32-3248S040 CYD
 *  Mic    : INMP441 (I2S)
 *           SCK -> IO32   WS -> IO25   SD -> IO35   L/R -> GND
 *
 *  PURPOSE: Diagnose raw mic data BEFORE building the spectrum
 *  analyzer. Open Serial Monitor at 115200 baud. Also displays
 *  live status on the TFT so you don't need a laptop connected.
 *
 *  WHAT IT REPORTS (every 500ms):
 *    - Raw min/max sample values from the I2S buffer
 *    - RMS amplitude
 *    - Peak-to-peak swing
 *    - Estimated dBFS (dB relative to full scale)
 *    - A simple ASCII bar in the serial monitor
 *    - Whether the mic looks dead, noisy, or healthy
 *
 *  Libraries: TFT_eSPI (Bodmer)   [arduinoFFT NOT needed here]
 * ============================================================
 */

#include <driver/i2s.h>
#include <TFT_eSPI.h>
#include <math.h>

// ─── I2S pins ────────────────────────────────────────────────
#define I2S_PORT    I2S_NUM_0
#define I2S_SCK     32
#define I2S_WS      25
#define I2S_SD      35

// ─── How many 32-bit samples per test block ───────────────────
// 2048 samples @ 10 kHz = ~205 ms per snapshot
#define SAMPLE_COUNT  2048
#define SAMPLE_RATE   10000

// ─── TFT ─────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ─── Sample buffer ────────────────────────────────────────────
static int32_t samples[SAMPLE_COUNT];

// ─────────────────────────────────────────────────────────────
void i2s_init() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_SCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD
    };
    esp_err_t r1 = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    esp_err_t r2 = i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);

    Serial.print("I2S driver install: ");
    Serial.println(r1 == ESP_OK ? "OK" : esp_err_to_name(r1));
    Serial.print("I2S set pin:        ");
    Serial.println(r2 == ESP_OK ? "OK" : esp_err_to_name(r2));
}

// ─── Draw a full-screen TFT status panel ─────────────────────
void tftReport(int32_t rawMin, int32_t rawMax, float rms,
               float peakToPeak, float dbfs, const char* verdict,
               uint16_t verdictColor) {

    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextFont(4);
    tft.setCursor(10, 8);
    tft.print("INMP441 Mic Test");

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    int y = 50;
    int lineH = 26;

    // Raw range
    tft.setCursor(10, y);
    tft.printf("Raw min : %ld", (long)rawMin);
    y += lineH;
    tft.setCursor(10, y);
    tft.printf("Raw max : %ld", (long)rawMax);
    y += lineH;

    // After right-shift (24-bit normalised)
    tft.setCursor(10, y);
    tft.printf("Norm min: %ld", (long)(rawMin >> 8));
    y += lineH;
    tft.setCursor(10, y);
    tft.printf("Norm max: %ld", (long)(rawMax >> 8));
    y += lineH;

    // RMS / P2P / dBFS
    tft.setCursor(10, y);
    tft.printf("RMS     : %.1f", rms);
    y += lineH;
    tft.setCursor(10, y);
    tft.printf("Pk-Pk   : %.1f", peakToPeak);
    y += lineH;
    tft.setCursor(10, y);
    tft.printf("dBFS    : %.1f dB", dbfs);
    y += lineH + 4;

    // Visual level bar
    float fraction = constrain((dbfs + 90.0f) / 60.0f, 0.0f, 1.0f); // map -90..-30 to 0..1
    int barW = (int)(fraction * 460);
    uint16_t barCol = (fraction < 0.5f) ? TFT_GREEN
                    : (fraction < 0.8f) ? TFT_YELLOW
                    : TFT_RED;
    tft.fillRect(10, y, barW, 18, barCol);
    tft.fillRect(10 + barW, y, 460 - barW, 18, TFT_DARKGREY);
    y += 26;

    // Verdict
    tft.setTextFont(4);
    tft.setTextColor(verdictColor, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print(verdict);
}

// ─── ASCII bar for serial monitor ────────────────────────────
void serialBar(float fraction, int width = 40) {
    int filled = (int)(fraction * width);
    Serial.print("[");
    for (int i = 0; i < width; i++) Serial.print(i < filled ? '#' : '-');
    Serial.print("]");
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  INMP441 Microphone Diagnostic Tool");
    Serial.println("  SCK=IO32  WS=IO25  SD=IO35  L/R=GND");
    Serial.println("========================================");

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextFont(4);
    tft.setCursor(40, 140);
    tft.print("Starting mic test...");

    i2s_init();
    delay(200);   // let DMA settle
    Serial.println("Listening... (snapshot every 500 ms)\n");
}

// ─────────────────────────────────────────────────────────────
void loop() {
    // ── 1. Read a block of samples ──
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, portMAX_DELAY);
    int n = bytesRead / sizeof(int32_t);

    // ── 2. Statistics ──
    int32_t rawMin =  2147483647L;
    int32_t rawMax = -2147483648LL;
    double  sumSq  = 0.0;

    for (int i = 0; i < n; i++) {
        int32_t s = samples[i];
        if (s < rawMin) rawMin = s;
        if (s > rawMax) rawMax = s;
        // Normalise to 24-bit before squaring to avoid overflow
        double norm = (double)(s >> 8);
        sumSq += norm * norm;
    }

    float rms        = (n > 0) ? (float)sqrt(sumSq / n) : 0.0f;
    float peakToPeak = (float)(rawMax - rawMin);
    // dBFS relative to 24-bit full scale (8388608)
    float dbfs       = (rms > 0.5f) ? 20.0f * log10f(rms / 8388608.0f) : -99.0f;

    // ── 3. Verdict ──
    const char* verdict;
    uint16_t    verdictColor;

    bool allZero   = (rawMin == 0 && rawMax == 0);
    bool stuck     = (rawMax - rawMin) < 500;        // almost nothing moving
    bool dcOnly    = (abs((int64_t)rawMin + rawMax) > (int64_t)(peakToPeak * 0.9f));
    bool healthy   = (rms > 200.0f && dbfs > -80.0f);

    if (allZero) {
        verdict      = "DEAD: all zeros - check wiring";
        verdictColor = TFT_RED;
    } else if (stuck) {
        verdict      = "STUCK: tiny swing - check SD/SCK/WS";
        verdictColor = TFT_ORANGE;
    } else if (dcOnly) {
        verdict      = "DC OFFSET only - check L/R pin = GND";
        verdictColor = TFT_ORANGE;
    } else if (healthy) {
        verdict      = "OK - mic responding";
        verdictColor = TFT_GREEN;
    } else {
        verdict      = "WEAK - very low signal";
        verdictColor = TFT_YELLOW;
    }

    // ── 4. Serial report ──
    Serial.println("─────────────────────────────────────────");
    Serial.printf("Samples read  : %d\n", n);
    Serial.printf("Raw min       : %ld\n",   (long)rawMin);
    Serial.printf("Raw max       : %ld\n",   (long)rawMax);
    Serial.printf("Norm min (>>8): %ld\n",   (long)(rawMin >> 8));
    Serial.printf("Norm max (>>8): %ld\n",   (long)(rawMax >> 8));
    Serial.printf("RMS (norm)    : %.2f\n",  rms);
    Serial.printf("Peak-to-peak  : %.0f\n",  peakToPeak);
    Serial.printf("dBFS          : %.1f dB\n", dbfs);
    Serial.print("Level         : ");
    float fraction = constrain((dbfs + 90.0f) / 60.0f, 0.0f, 1.0f);
    serialBar(fraction);
    Serial.println();
    Serial.print("Verdict       : ");
    Serial.println(verdict);

    // First 16 raw values so you can spot stuck bits or wild DC
    Serial.print("First 16 raw  : ");
    for (int i = 0; i < 16 && i < n; i++) {
        Serial.printf("%ld ", (long)samples[i]);
    }
    Serial.println("\n");

    // ── 5. TFT update ──
    tftReport(rawMin, rawMax, rms, peakToPeak, dbfs, verdict, verdictColor);

    delay(500);
}
