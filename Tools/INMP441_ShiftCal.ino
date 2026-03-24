/*
 * ============================================================
 *  INMP441 Bit-Shift Calibration Tool
 *  Board  : Hosyond 4" ESP32-3248S040 CYD
 *  Mic    : INMP441  SCK=IO32  WS=IO25  SD=IO35  L/R=GND
 *
 *  PURPOSE: The INMP441 packs 24-bit data left-justified into a
 *  32-bit I2S word. The "correct" right-shift to normalise it
 *  varies by driver version and board. This tool tries shifts
 *  6, 7, 8, 9, 10, 11, 12, 13, 14 and reports which gives the
 *  best centered, highest-dynamic-range result.
 *
 *  It also prints a DC-offset check: a good shift has mean ≈ 0.
 *
 *  Open Serial Monitor @ 115200 baud.
 *  TFT shows the winning shift value live.
 *
 *  Libraries: TFT_eSPI (Bodmer)
 * ============================================================
 */

#include <driver/i2s.h>
#include <TFT_eSPI.h>
#include <math.h>

#define I2S_PORT      I2S_NUM_0
#define I2S_SCK       32
#define I2S_WS        25
#define I2S_SD        35
#define SAMPLE_RATE   10000
#define SAMPLE_COUNT  2048

// Shifts to evaluate
const int SHIFTS[]   = {6, 7, 8, 9, 10, 11, 12, 13, 14};
const int NUM_SHIFTS = sizeof(SHIFTS) / sizeof(SHIFTS[0]);

static int32_t samples[SAMPLE_COUNT];

TFT_eSPI tft = TFT_eSPI();

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
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ─────────────────────────────────────────────────────────────
void analyseShift(int shift, int n,
                  float &rmsOut, float &dcOut, float &dbfsOut, float &centreOut) {
    double sum   = 0.0;
    double sumSq = 0.0;
    int32_t mn   =  2147483647L;
    int32_t mx   = -2147483648LL;

    for (int i = 0; i < n; i++) {
        int32_t s = samples[i] >> shift;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        sum   += s;
        sumSq += (double)s * s;
    }

    float mean  = (float)(sum / n);
    float rms   = (float)sqrt(sumSq / n);
    float pp    = (float)(mx - mn);

    // Full scale for this shift: 32-bit range shifted down
    float fs    = (float)(2147483648LL >> shift);

    // "Centre" score: 0 = perfectly symmetric, 1 = all DC
    // Computed as |mean| / rms — low is good
    float centre = (rms > 0.1f) ? fabsf(mean) / rms : 99.0f;

    rmsOut    = rms;
    dcOut     = mean;
    dbfsOut   = (rms > 0.5f) ? 20.0f * log10f(rms / fs) : -99.0f;
    centreOut = centre;
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(400);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextFont(4);
    tft.setCursor(10, 10);
    tft.print("Bit-Shift Calibration");
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 50);
    tft.print("Reading mic... make some noise!");

    i2s_init();
    delay(300);

    Serial.println();
    Serial.println("==============================================");
    Serial.println("  INMP441 Bit-Shift Calibration");
    Serial.println("  Make some noise while this runs!");
    Serial.println("==============================================\n");
}

// ─────────────────────────────────────────────────────────────
void loop() {
    // Read block
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, portMAX_DELAY);
    int n = bytesRead / sizeof(int32_t);

    Serial.println("──────────────────────────────────────────────");
    Serial.printf("%-6s %-12s %-12s %-10s %-8s %s\n",
                  "Shift", "RMS", "DC mean", "dBFS", "DC/RMS", "Verdict");
    Serial.println("──────────────────────────────────────────────");

    float   bestScore = 1e9f;
    int     bestShift = 8;
    float   bestDbfs  = -99.0f;

    struct Result {
        int   shift;
        float rms, dc, dbfs, centre;
    } results[NUM_SHIFTS];

    for (int si = 0; si < NUM_SHIFTS; si++) {
        int sh = SHIFTS[si];
        analyseShift(sh, n,
                     results[si].rms, results[si].dc,
                     results[si].dbfs, results[si].centre);
        results[si].shift = sh;

        // Score = DC/RMS ratio (lower = better centred = better shift)
        // Tiebreak toward stronger signal (higher RMS)
        float score = results[si].centre;
        if (score < bestScore ||
            (fabsf(score - bestScore) < 0.05f && results[si].dbfs > bestDbfs)) {
            bestScore = score;
            bestShift = sh;
            bestDbfs  = results[si].dbfs;
        }
    }

    for (int si = 0; si < NUM_SHIFTS; si++) {
        bool winner = (results[si].shift == bestShift);
        Serial.printf(">> %2d   %-12.1f %-12.1f %-10.1f %-8.3f %s\n",
                      results[si].shift,
                      results[si].rms,
                      results[si].dc,
                      results[si].dbfs,
                      results[si].centre,
                      winner ? "<-- BEST" : "");
    }

    Serial.println();
    Serial.printf("RECOMMENDATION: Use >> %d  (DC/RMS=%.3f, dBFS=%.1f)\n\n",
                  bestShift, bestScore, bestDbfs);

    // ── TFT summary ──────────────────────────────────────────
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(4);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 8);
    tft.print("Bit-Shift Calibration");

    // Table header
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(10, 48);
    tft.print("SHIFT    RMS        dBFS   DC/RMS");
    tft.drawFastHLine(10, 58, 460, TFT_DARKGREY);

    int y = 62;
    for (int si = 0; si < NUM_SHIFTS; si++) {
        bool winner = (results[si].shift == bestShift);
        uint16_t col = winner ? TFT_GREEN : TFT_WHITE;
        tft.setTextColor(col, TFT_BLACK);
        tft.setCursor(10, y);
        tft.printf("  >> %2d   %9.0f   %5.1f    %.3f %s",
                   results[si].shift,
                   results[si].rms,
                   results[si].dbfs,
                   results[si].centre,
                   winner ? " <--" : "    ");
        y += 14;
    }

    // Big winner announcement
    tft.setTextFont(4);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, y + 8);
    tft.printf("Use shift: >> %d", bestShift);

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, y + 40);
    tft.print("Keep making noise for best result!");

    delay(800);
}
