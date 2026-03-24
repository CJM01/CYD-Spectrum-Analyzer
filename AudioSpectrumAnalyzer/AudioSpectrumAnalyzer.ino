/*
 * ============================================================
 *  Audio Spectrum Analyzer for Guitar (80 Hz – 5 kHz)
 *  Board  : Hosyond 4" ESP32-3248S040 CYD
 *  Display: ST7796 via TFT_eSPI
 *  Mic    : INMP441  SCK=IO32  WS=IO25  SD=IO35  L/R=GND
 *
 *  KEY FIX vs v1: DC offset is removed per-frame with a
 *  high-pass IIR filter BEFORE the FFT. This eliminates the
 *  spectral leakage that was swamping all frequency bins.
 *
 *  Libraries:
 *    - TFT_eSPI   (Bodmer)
 *    - arduinoFFT v2.x (Enrique Condes)
 * ============================================================
 */

#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <TFT_eSPI.h>

// ─── I2S / Mic ────────────────────────────────────────────────
#define I2S_PORT        I2S_NUM_0
#define I2S_SCK_PIN     32
#define I2S_WS_PIN      25
#define I2S_SD_PIN      35

// ─── FFT / Sampling ──────────────────────────────────────────
#define SAMPLE_RATE     10000       // Nyquist = 5 kHz
#define FFT_SIZE        1024        // Power of 2
#define FREQ_MIN         80.0f
#define FREQ_MAX       5000.0f

// ─── Display ─────────────────────────────────────────────────
#define SCREEN_W        480
#define SCREEN_H        320
#define NUM_BARS         48

// ─── Level tuning ────────────────────────────────────────────
// Adjust DB_FLOOR up if bars dance in silence; down if too quiet
// Adjust DB_SCALE down if bars never reach the top; up if always maxed
#define DB_FLOOR        50.0f       // Magnitude (log units) at bar=0
#define DB_SCALE        80.0f       // Magnitude range above floor = full bar

// ─── Animation ───────────────────────────────────────────────
#define FALL_SPEED       4          // px per frame drop
#define PEAK_HOLD_FRAMES 25

// ─── DC removal IIR coefficient ──────────────────────────────
// Lower = faster DC tracking. 0.98 converges in ~50 samples
// (~5ms at 10kHz) which handles the INMP441's wandering offset.
// Don't go below 0.95 or it starts eating low guitar frequencies.
#define DC_ALPHA        0.98f

// ─── Temporal bar smoothing ───────────────────────────────────
// Each frame: smoothed = smoothed * SMOOTH_UP + new * (1-SMOOTH_UP)
// SMOOTH_UP   applies when bar is rising  (snappy attack)
// SMOOTH_DOWN applies when bar is falling (slower, natural decay)
// Both are 0.0 (instant) → 1.0 (never moves)
#define SMOOTH_UP       0.3f
#define SMOOTH_DOWN     0.6f

// ─── Globals ─────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

static double  vReal[FFT_SIZE];
static double  vImag[FFT_SIZE];
ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, (double)SAMPLE_RATE);

static int16_t barHeight[NUM_BARS];
static int16_t peakY[NUM_BARS];
static uint8_t peakTimer[NUM_BARS];
static int16_t barX[NUM_BARS];
static int16_t barW;
static int16_t plotH;
static int16_t plotY;

// DC removal state
static float   dcEstimate = 0.0f;

// Temporally smoothed bar magnitudes (persists between frames)
static float   smoothMag[NUM_BARS] = {0};

// ─── Colour: dark purple (bottom) → yellow (top) ─────────────
uint16_t barColour(float f) {
    f = constrain(f, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(80  + f * 175);    // 80  → 255
    uint8_t g = (uint8_t)(0   + f * 210);    // 0   → 210
    uint8_t b = (uint8_t)(120 * (1.0f - f)); // 120 → 0
    return tft.color565(r, g, b);
}

// ─── I2S init ────────────────────────────────────────────────
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
        .bck_io_num   = I2S_SCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD_PIN
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ─── Read mic + remove DC ─────────────────────────────────────
//  The INMP441 outputs left-justified 24-bit data in a 32-bit
//  word. We right-shift by 8 to get a signed 24-bit value, then
//  run a first-order high-pass IIR to strip the DC bias that the
//  mic naturally has. Without this, FFT bin 0 is enormous and
//  bleeds into every guitar-range bin via spectral leakage.
void readMic() {
    int32_t raw[FFT_SIZE];
    size_t  bytesRead = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY);

    int n = bytesRead / sizeof(int32_t);
    for (int i = 0; i < FFT_SIZE; i++) {
        float s = (i < n) ? (float)(raw[i] >> 8) : 0.0f;

        // High-pass IIR: y[n] = x[n] - dc,  dc += α*(x[n]-dc)
        dcEstimate += DC_ALPHA * (s - dcEstimate);
        float filtered = s - dcEstimate;

        vReal[i] = (double)filtered;
        vImag[i] = 0.0;
    }
}

// ─── Map bin → bar (log-spaced) ───────────────────────────────
int binToBar(int bin) {
    double freq = (double)bin * SAMPLE_RATE / FFT_SIZE;
    if (freq < FREQ_MIN || freq > FREQ_MAX) return -1;
    double logMin  = log10((double)FREQ_MIN);
    double logMax  = log10((double)FREQ_MAX);
    double logFreq = log10(freq);
    int bar = (int)((logFreq - logMin) / (logMax - logMin) * NUM_BARS);
    return constrain(bar, 0, NUM_BARS - 1);
}

// ─── Accumulate per-bar magnitudes ────────────────────────────
void computeBars(float barMag[NUM_BARS]) {
    float accum[NUM_BARS] = {0};
    int   count[NUM_BARS] = {0};
    int   maxBin = FFT_SIZE / 2;

    for (int b = 1; b < maxBin; b++) {
        int bar = binToBar(b);
        if (bar < 0) continue;
        float mag = (float)sqrt(vReal[b]*vReal[b] + vImag[b]*vImag[b]);
        accum[bar] += mag;
        count[bar]++;
    }
    for (int i = 0; i < NUM_BARS; i++) {
        barMag[i] = (count[i] > 0) ? accum[i] / count[i] : 0.0f;
    }
}

// ─── Draw chrome ──────────────────────────────────────────────
void drawChrome() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(4, 4);
    tft.print("Analyzing  80Hz - 5kHz Spectrum");
    tft.drawFastHLine(0, plotY + 1, SCREEN_W, TFT_DARKGREY);

    // Frequency landmarks
    const char*  labels[]  = {"100","200","500","1k","2k","5k"};
    const float  freqs[]   = {100, 200, 500, 1000, 2000, 5000};
    double logMin = log10((double)FREQ_MIN);
    double logMax = log10((double)FREQ_MAX);
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    for (int i = 0; i < 6; i++) {
        int x = (int)((log10((double)freqs[i]) - logMin) /
                      (logMax - logMin) * SCREEN_W);
        if (x >= 0 && x < SCREEN_W - 14) {
            // Tick mark
            tft.drawFastVLine(x, plotY - 4, 6, TFT_DARKGREY);
            tft.setCursor(x + 2, plotY + 4);
            tft.print(labels[i]);
        }
    }
}

// ─── Incremental bar render ───────────────────────────────────
void updateBars(int16_t newH[NUM_BARS]) {
    for (int i = 0; i < NUM_BARS; i++) {
        int16_t oldH = barHeight[i];
        int16_t h    = newH[i];
        int16_t x    = barX[i];
        int16_t w    = barW - 1;

        if (h > oldH) {
            uint16_t col = barColour((float)h / plotH);
            tft.fillRect(x, plotY - h, w, h - oldH, col);
        } else if (h < oldH) {
            tft.fillRect(x, plotY - oldH, w, oldH - h, TFT_BLACK);
            if (h > 0) {
                uint16_t col = barColour((float)h / plotH);
                tft.fillRect(x, plotY - h, w, h, col);
            }
        }
        barHeight[i] = h;

        // Peak dot
        int16_t newPeakY = plotY - h;
        if (newPeakY < peakY[i]) {
            tft.drawFastHLine(x, peakY[i], w, TFT_BLACK);
            peakY[i]     = newPeakY;
            peakTimer[i] = PEAK_HOLD_FRAMES;
        }
        if (peakTimer[i] > 0) {
            peakTimer[i]--;
            tft.drawFastHLine(x, peakY[i], w, TFT_WHITE);
        } else {
            tft.drawFastHLine(x, peakY[i], w, TFT_BLACK);
            if (peakY[i] < plotY) peakY[i]++;
        }
    }
}

// ─── Setup ───────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    plotH = SCREEN_H - 40;
    plotY = SCREEN_H - 20;
    barW  = SCREEN_W / NUM_BARS;

    for (int i = 0; i < NUM_BARS; i++) {
        barX[i]      = i * barW;
        barHeight[i] = 0;
        peakY[i]     = plotY;
        peakTimer[i] = 0;
    }

    drawChrome();
    i2s_init();

    // Warm up the DC filter — discard first 10 frames so the
    // IIR estimate converges before we start drawing
    Serial.println("Warming up DC filter...");
    int32_t warmup[FFT_SIZE];
    for (int w = 0; w < 10; w++) {
        size_t b = 0;
        i2s_read(I2S_PORT, warmup, sizeof(warmup), &b, portMAX_DELAY);
        int n = b / sizeof(int32_t);
        for (int i = 0; i < n; i++) {
            float s = (float)(warmup[i] >> 8);
            dcEstimate += DC_ALPHA * (s - dcEstimate);
        }
    }
    Serial.printf("DC estimate after warmup: %.1f\n", dcEstimate);
    Serial.println("Running.");
}

// ─── Loop ────────────────────────────────────────────────────
void loop() {
    // 1. Read + DC-filter samples into vReal
    readMic();

    // 2. Window + FFT
    FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    // Note: do NOT call complexToMagnitude() — we accumulate
    // per-bar magnitudes manually to keep log-spaced binning.

    // 3. Bin magnitudes → per-bar averages
    float barMag[NUM_BARS] = {0};
    computeBars(barMag);

    // 4. Temporal smoothing + log magnitude → pixel height
    int16_t targetH[NUM_BARS];
    for (int i = 0; i < NUM_BARS; i++) {
        float mag = barMag[i];

        // Lerp: rise fast, fall slow — eliminates single-frame spikes
        float alpha = (mag > smoothMag[i]) ? SMOOTH_UP : SMOOTH_DOWN;
        smoothMag[i] = smoothMag[i] * alpha + mag * (1.0f - alpha);

        // Convert to dB-like log scale
        float db = 20.0f * log10f(smoothMag[i] + 1.0f);

        // Map [DB_FLOOR .. DB_FLOOR+DB_SCALE] → [0 .. plotH]
        float fraction = (db - DB_FLOOR) / DB_SCALE;
        fraction = constrain(fraction, 0.0f, 1.0f);
        int16_t h = (int16_t)(fraction * plotH);

        // Gravity (still applied on top of smoothing for peak drops)
        if (h < barHeight[i] - FALL_SPEED)
            h = barHeight[i] - FALL_SPEED;

        targetH[i] = h;
    }

    // 5. Draw
    updateBars(targetH);

    // 6. Serial debug — print peak bar magnitude once per second
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 1000) {
        lastPrint = millis();
        float maxMag = 0;
        int   maxBar = 0;
        for (int i = 0; i < NUM_BARS; i++) {
            if (smoothMag[i] > maxMag) { maxMag = smoothMag[i]; maxBar = i; }
        }
        float peakFreq = FREQ_MIN * pow(FREQ_MAX / FREQ_MIN,
                                        (float)maxBar / NUM_BARS);
        float db = 20.0f * log10f(maxMag + 1.0f);
        Serial.printf("Peak bar=%2d (~%.0f Hz)  mag=%.1f  db=%.1f  DC=%.1f\n",
                      maxBar, peakFreq, maxMag, db, dcEstimate);
    }
}
