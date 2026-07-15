// ============================================================================
//  OfficeAir - ESP32 office presence & climate monitor
//
//  Hardware: Heltec WiFi LoRa 32 V3 (ESP32-S3) + SHT21 sensor + 18650 cell
//  Backend:  Google Sheets via Apps Script webhook
//
//  Features:
//   - 4-pass ARP sweep device counting (occupancy proxy), thread-safe lwIP access
//   - SHT21 temperature/humidity logging every 10 minutes
//   - RAM FIFO buffer with idempotent, ACK-based upload (survives outages)
//   - Battery voltage telemetry (onboard divider)
//   - Crash forensics: reset-reason tracking, RTC breadcrumbs, core dump
//     summary, all persisted in NVS and reported to the sheet
//   - Hardware task watchdog + self-recovery restarts
//   - Physiological heartbeat LED: HRV (RSA + jitter), asymmetric HR kinetics,
//     CPU-load-driven BPM via an IRAM-safe tick-hook sampling profiler
//
//  Firmware version: V12.4
// ============================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <vector>
#include <time.h>
#include <Wire.h>
#include "SHT21.h"
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Preferences.h>
#include <esp_core_dump.h>       // reading the flash-stored crash dump
#include <esp_freertos_hooks.h>  // tick hook for the CPU load sampler

// ==========================================
// ⚙️ HARDWARE & NETWORK CONFIGURATION
// ==========================================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const String googleScriptUrl = "https://script.google.com/macros/s/YOUR_DEPLOYMENT_ID/exec";

// Heltec V3 pinout
#define LED_PIN 35
#define BUTTON_PIN 0        // USER/PRG button (active LOW, internal pull-up)
#define MY_SDA 41
#define MY_SCL 42
#define Vext 36

// Battery voltage measurement (Heltec V3 onboard voltage divider)
#define ADC_CTRL 37         // divider enable pin
#define VBAT_PIN 1          // ADC input for the divided cell voltage
// NOTE: the ADC_CTRL polarity may be inverted depending on the board revision!
// If vbat reads a nonsensical ~0.3 V, swap LOW/HIGH in readVBat().
const float VBAT_DIVIDER = 4.9f;  // 390k/100k divider ratio - verify with a multimeter

// LEDC (12-bit gamma-corrected PWM, Arduino core 2.x API)
const int LEDC_CH   = 0;
const int LEDC_RES  = 12;
const int LEDC_FREQ = 5000;

// PHYSIOLOGICAL HEART MODEL PARAMETERS
const float IDLE_BPM        = 50.0f;   // resting heart rate (athletic adult)
const float ACTIVITY_FLOOR_BPM = 110.0f; // minimum BPM while working
const float MAX_BPM         = 160.0f;  // sprints toward this at full CPU load
const float HEARTBEAT_MAX   = 0.40f;   // peak brightness at rest (perceptual)
const float ACTIVITY_MAX    = 0.55f;   // peak brightness under load (slightly brighter)
const float HRV_RSA_AMP     = 0.06f;   // respiratory sinus arrhythmia depth at rest (±6%)
const float HRV_RSA_AMP_HI  = 0.015f;  // HRV narrows under load (sympathetic tone!)
const float HRV_JITTER      = 0.02f;   // beat-to-beat random component (±2%)
const float BREATH_PERIOD_MS = 5000.0f; // breathing cycle (12 breaths/min)
const float HR_RAMP_UP      = 0.35f;   // acceleration per beat (onset of load)
const float HR_RAMP_DOWN    = 0.08f;   // cool-down per beat (slow recovery)

// CPU LOAD SAMPLER
const uint32_t CPU_SAMPLE_MS = 500;    // sampling window
const float CPU_LOAD_SMOOTH  = 0.4f;   // EMA smoothing (0..1, higher = snappier)

const float ACTIVITY_LEVEL = 0.45f;    // setup-phase blink brightness

// LED modes - the main thread only sets this, the LED task does the work
enum LedMode : uint8_t {
  LED_MODE_HEARTBEAT = 0,  // resting lub-dub with HRV
  LED_MODE_ACTIVITY  = 1,  // sport mode (scan/upload)
  LED_MODE_BLINK     = 2   // setup phase: WiFi/NTP connection blink
};
volatile uint8_t ledMode = LED_MODE_BLINK;

// Watchdog and error handling
const int WDT_TIMEOUT_S = 90;
const int MAX_CONSECUTIVE_FAILS = 10;

SHT21 sht21;
Preferences prefs;

// ==========================================
// 📊 STRUCT & BUFFER DEFINITIONS
// ==========================================
struct Measurement {
  float temperature;
  float humidity;
  unsigned long timestamp;
  int deviceCount;
  float vbat;              // battery voltage at measurement time
};

std::vector<Measurement> dataBuffer;
const int MAX_BUFFER_SIZE = 144;

unsigned long lastMeasurementTime = 0;
const unsigned long MEASUREMENT_INTERVAL = 10UL * 60UL * 1000UL;
int lastKnownDeviceCount = 0;
int consecutiveFails = 0;

// Button and debug state
volatile bool forceMeasurement = false;
unsigned long lastButtonPress = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 500;

unsigned long lastStatusPrint = 0;
const unsigned long STATUS_PRINT_INTERVAL = 60UL * 1000UL;

// Lifetime statistics loaded from NVS
uint32_t bootCount = 0;
uint32_t wdtResetCount = 0;
uint32_t swResetCount = 0;
uint32_t panicResetCount = 0;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;

// ==========================================
// 🍞 BREADCRUMB SYSTEM (RTC memory)
// Survives WDT/panic/SW resets (but not power loss).
// The code marks each phase transition; after a crash we read back
// on boot which phase the firmware died in.
// ==========================================
#define CP_MAGIC 0xC0FFEE42
RTC_NOINIT_ATTR uint32_t cpMagic;
RTC_NOINIT_ATTR uint32_t cpPhase;
RTC_NOINIT_ATTR uint32_t cpDetail;   // e.g. scan pass number, HTTP phase

enum Checkpoint : uint32_t {
  CP_IDLE = 1,        // main loop, doing nothing
  CP_SENSOR,          // SHT21 read
  CP_SCAN,            // ARP scan (detail = pass number)
  CP_HTTP_BEGIN,      // HTTP connection setup (TLS!)
  CP_HTTP_GET,        // request sent, waiting for response
  CP_HTTP_RESP,       // response processing
  CP_NVS_WRITE        // flash write
};

inline void setCheckpoint(uint32_t phase, uint32_t detail = 0) {
  cpPhase  = phase;
  cpDetail = detail;
  cpMagic  = CP_MAGIC;
}

const char* checkpointToString(uint32_t cp) {
  switch (cp) {
    case CP_IDLE:       return "IDLE";
    case CP_SENSOR:     return "SENSOR";
    case CP_SCAN:       return "SCAN";
    case CP_HTTP_BEGIN: return "HTTP_BEGIN/TLS";
    case CP_HTTP_GET:   return "HTTP_GET";
    case CP_HTTP_RESP:  return "HTTP_RESP";
    case CP_NVS_WRITE:  return "NVS_WRITE";
    default:            return "?";
  }
}

// Description of the previous run's death - uploaded to the sheet with the
// first successful transmission
String crashInfo = "";

// ==========================================
// 🔋 BATTERY VOLTAGE MEASUREMENT
// ==========================================
float readVBat() {
  pinMode(ADC_CTRL, OUTPUT);
  digitalWrite(ADC_CTRL, LOW);    // enable divider (revision-dependent polarity!)
  delay(10);
  uint32_t mv = analogReadMilliVolts(VBAT_PIN);
  digitalWrite(ADC_CTRL, HIGH);   // disable divider (don't drain the battery)
  return (mv * VBAT_DIVIDER) / 1000.0f;
}

// ==========================================
// 💡 12-BIT GAMMA-CORRECTED LEDC DRIVER
// ==========================================
void ledInit() {
  ledcSetup(LEDC_CH, LEDC_FREQ, LEDC_RES);
  ledcAttachPin(LED_PIN, LEDC_CH);
}

void ledSet(float perceived) {
  perceived = constrain(perceived, 0.0f, 1.0f);
  float linear = powf(perceived, 2.8f);
  ledcWrite(LEDC_CH, (uint32_t)(linear * 4095.0f));
}

// ==========================================
// 📈 CPU LOAD - TICK-HOOK SAMPLING PROFILER
// The FreeRTOS tick interrupt fires at a fixed 1000 Hz (it wakes the core
// even from WFI). On every tick we check which task is running on the given
// core: if it's the idle task, the core was free; otherwise it was working.
//   load = 1 - idle_ticks / total_ticks
// Time-based, calibration-free, a true ratio - no running maximum, no
// WiFi-interrupt artifacts. Resolution: 1 ms (statistical sampling).
// ==========================================
volatile uint32_t tickCount[2] = {0, 0};
volatile uint32_t idleTicks[2] = {0, 0};
static TaskHandle_t idleTaskHandle[2] = {NULL, NULL};
volatile float cpuLoadPct = 0.0f;  // smoothed, combined load (0..100)

// Tick hooks - these run in ISR context!
// CRASH-FIX: IRAM_ATTR IS MANDATORY. The tick interrupt keeps firing during
// flash writes (e.g. WiFi calibration data being saved to NVS) while the
// flash cache is disabled - if the hook lives in flash, you get an instant
// "Cache disabled but cached memory region accessed" panic. IRAM is
// accessible even without the cache.
static void IRAM_ATTR tickHook0(void) {
  tickCount[0]++;
  if (xTaskGetCurrentTaskHandleForCPU(0) == idleTaskHandle[0]) idleTicks[0]++;
}
static void IRAM_ATTR tickHook1(void) {
  tickCount[1]++;
  if (xTaskGetCurrentTaskHandleForCPU(1) == idleTaskHandle[1]) idleTicks[1]++;
}

// Called by the LED task every CPU_SAMPLE_MS
float sampleCpuLoad() {
  static uint32_t lastTick[2] = {0, 0};
  static uint32_t lastIdle[2] = {0, 0};
  static float smoothed = 0.0f;

  float worstLoad = 0.0f;
  for (int c = 0; c < 2; c++) {
    uint32_t dTick = tickCount[c] - lastTick[c];
    uint32_t dIdle = idleTicks[c] - lastIdle[c];
    lastTick[c] = tickCount[c];
    lastIdle[c] = idleTicks[c];

    if (dTick == 0) continue;
    float load = 1.0f - (float)dIdle / (float)dTick;
    if (load < 0.0f) load = 0.0f;
    if (load > worstLoad) worstLoad = load;  // the harder-working core wins
  }

  smoothed += (worstLoad - smoothed) * CPU_LOAD_SMOOTH;  // EMA smoothing
  cpuLoadPct = smoothed * 100.0f;
  return smoothed;
}

// ==========================================
// 💓 PHYSIOLOGICAL HEART MODEL - IN ITS OWN FreeRTOS TASK
// Keeps beating even while the main thread blocks (scan, TLS).
// Modeled physiology:
//  - RSA: heart rate oscillates with "breathing" (inhale: faster, exhale: slower)
//  - beat-to-beat jitter: every beat is slightly different in length
//  - HRV narrows under load (sympathetic dominance)
//  - asymmetric HR kinetics: fast ramp-up, slow cool-down
// ==========================================

// Envelope of a single beat, with timing scaled to the period
float beatEnvelope(float t, float period) {
  float scale = period / 1200.0f;               // reference: a 50 BPM beat
  float dubDelay = 0.25f * period;              // "dub" at a quarter of the cycle
  float attack   = fminf(40.0f, 0.08f * period); // rise (shortens at high HR)
  float decay    = fmaxf(60.0f, 180.0f * scale); // fall-off

  // lub (strong)
  float lub = 0.0f;
  if (t < attack) lub = t / attack;
  else lub = expf(-(t - attack) / decay);

  // dub (weaker, delayed)
  float dub = 0.0f;
  if (t >= dubDelay) {
    float td = t - dubDelay;
    if (td < attack) dub = 0.55f * (td / attack);
    else dub = 0.55f * expf(-(td - attack) / decay);
  }
  return fmaxf(lub, dub);
}

void ledTask(void *param) {
  float currentPeriod = 60000.0f / IDLE_BPM;   // current "physiological" period (smoothed)
  float beatPeriod    = currentPeriod;         // actual length of the running beat (with HRV)
  float cpuLoad       = 0.0f;                  // 0..1, smoothed
  uint32_t beatStart  = millis();
  uint32_t lastCpuSample = 0;

  for (;;) {
    uint32_t now = millis();

    // --- CPU load sampling at a fixed interval ---
    if (now - lastCpuSample >= CPU_SAMPLE_MS) {
      lastCpuSample = now;
      cpuLoad = sampleCpuLoad();
    }

    // --- Setup phase: simple blinking ---
    if (ledMode == LED_MODE_BLINK) {
      ledSet(((now / 250) % 2) ? ACTIVITY_LEVEL : 0.0f);
      vTaskDelay(pdMS_TO_TICKS(20));
      beatStart = now;  // start with a fresh beat when the mode changes
      continue;
    }

    float t = (float)(now - beatStart);

    // --- Beat boundary: new beat, compute the new period ---
    if (t >= beatPeriod) {
      beatStart = now;
      t = 0.0f;

      // 1) Target BPM = mode floor + real CPU load pushing on top.
      //    Idle: starts from 50 BPM, but creeps up if something sweats
      //    in the background.
      //    Work: minimum 110 BPM, panting toward 160 during the TLS
      //    crypto sprint.
      float baseBpm = (ledMode == LED_MODE_ACTIVITY) ? ACTIVITY_FLOOR_BPM : IDLE_BPM;
      float targetBpm = baseBpm + (MAX_BPM - baseBpm) * cpuLoad;
      float targetPeriod = 60000.0f / targetBpm;

      float alpha = (targetPeriod < currentPeriod) ? HR_RAMP_UP : HR_RAMP_DOWN;
      currentPeriod += (targetPeriod - currentPeriod) * alpha;

      // 2) HRV: RSA (breathing modulation) + random jitter.
      //    RSA amplitude narrows under load - just like in real life.
      float rsaAmp = (ledMode == LED_MODE_ACTIVITY) ? HRV_RSA_AMP_HI : HRV_RSA_AMP;
      float rsa = sinf(2.0f * PI * (float)(now % (uint32_t)BREATH_PERIOD_MS) / BREATH_PERIOD_MS);
      float jitter = (((int)(esp_random() % 2001)) - 1000) / 1000.0f * HRV_JITTER;

      beatPeriod = currentPeriod * (1.0f + rsaAmp * rsa + jitter);
    }

    // --- Draw the envelope ---
    float maxBright = (ledMode == LED_MODE_ACTIVITY) ? ACTIVITY_MAX : HEARTBEAT_MAX;
    ledSet(beatEnvelope(t, beatPeriod) * maxBright);

    vTaskDelay(pdMS_TO_TICKS(10));  // ~100 Hz refresh, plenty for smooth ramps
  }
}

// ==========================================
// 🔎 ARP TABLE HARVESTER (thread-safe)
// ==========================================
void harvestArpTable(std::vector<uint32_t>& discoveredIPs, const char* label) {
  LOCK_TCPIP_CORE();
  for (int j = 0; j < ARP_TABLE_SIZE; j++) {
    ip4_addr_t *ipaddr;
    struct netif *iface;
    struct eth_addr *ethaddr;

    if (etharp_get_entry(j, &ipaddr, &iface, &ethaddr) == 1) {
      uint32_t rawIP = ipaddr->addr;

      bool alreadySaved = false;
      for (uint32_t savedIP : discoveredIPs) {
        if (savedIP == rawIP) { alreadySaved = true; break; }
      }

      if (!alreadySaved) {
        discoveredIPs.push_back(rawIP);
        Serial.printf("    [+ %s] IP: %s | MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      label, ip4addr_ntoa(ipaddr),
                      ethaddr->addr[0], ethaddr->addr[1], ethaddr->addr[2],
                      ethaddr->addr[3], ethaddr->addr[4], ethaddr->addr[5]);
      }
    }
  }
  UNLOCK_TCPIP_CORE();
}

// ==========================================
// 🔎 4-PASS MULTI-SWEEP ARP SCANNER (thread-safe)
// Multiple passes + union of results defeats WiFi frame loss; the pauses
// between passes give power-saving mobile clients a chance to wake up
// and answer. lwIP calls are wrapped in the TCP/IP core lock - without it,
// a race against the lwIP thread eventually corrupts the ARP table
// (manifests as a random freeze after hours of fine operation).
// ==========================================
int countActiveDevices() {
  ledMode = LED_MODE_ACTIVITY;  // sport mode - it's visibly working, not frozen!

  std::vector<uint32_t> discoveredIPs;
  IPAddress localIP = WiFi.localIP();
  IPAddress targetIP = localIP;
  struct netif *nif = netif_default;

  if (nif == NULL) {
    Serial.println("❌ [ARP] No active network interface, scan skipped.");
    return lastKnownDeviceCount;
  }

  Serial.println("\n🔎 Starting 4-pass multi-sweep ARP scan...");
  unsigned long start = millis();

  for (int pass = 0; pass < 4; pass++) {
    Serial.printf("  -> [Pass %d/4] Subnet sweep...\n", pass + 1);
    esp_task_wdt_reset();
    setCheckpoint(CP_SCAN, pass + 1);   // 🍞 breadcrumb: scan, pass N

    for (int i = 1; i < 255; i++) {
      targetIP[3] = i;
      if (targetIP == localIP) continue;

      ip4_addr_t target_lwip;
      target_lwip.addr = (uint32_t)targetIP;

      LOCK_TCPIP_CORE();
      etharp_request(nif, &target_lwip);
      UNLOCK_TCPIP_CORE();

      if (i % 4 == 0 || i == 254) {
        delay(45);
        harvestArpTable(discoveredIPs, "Device Found");
      }
      delay(2);
    }

    if (pass < 3) {
      Serial.println("  -> Sleeping 1500 ms (mobile power-save cycles)...");
      delay(1500);
    }
  }

  Serial.println("  -> Waiting for late responses (500 ms)...");
  delay(500);
  harvestArpTable(discoveredIPs, "Late Device Found");

  int totalDevices = discoveredIPs.size() + 1;  // +1 for ourselves
  Serial.printf("⏱️ ARP scan finished in %lu ms. Unique online devices: %d\n",
                millis() - start, totalDevices);
  return totalDevices;
}

// ==========================================
// 🌡️ SENSOR & CLOCK FUNCTIONS
// ==========================================
float readTemperature() { return sht21.getTemperature(); }
float readHumidity()    { return sht21.getHumidity(); }
unsigned long getRtcTimestamp() { return (unsigned long)time(NULL); }

// ==========================================
// 📟 UART DEBUG STATUS LINE
// ==========================================
void printStatusLine() {
  Serial.printf("💠 [STATUS] up:%lum | heap:%u B | maxblock:%u B | vbat:%.2f V | cpu:%.0f%% | buffer:%d | dev:%d | boots:%u | wdt:%u | sw:%u | panic:%u | fails:%d\n",
                millis() / 60000UL,
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap(),
                readVBat(),
                cpuLoadPct,
                (int)dataBuffer.size(),
                lastKnownDeviceCount,
                bootCount, wdtResetCount, swResetCount, panicResetCount,
                consecutiveFails);
}

// Short code for the sheet (URL-friendly)
const char* resetReasonShort(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "UNKNOWN";
  }
}

// Verbose decoding for the boot log
const char* resetReasonToString(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (power-on / hard reset)";
    case ESP_RST_SW:        return "SW (ESP.restart - fail counter or setup timeout)";
    case ESP_RST_PANIC:     return "PANIC (crash/exception!)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog!)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (our 90 s watchdog fired!)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dip!)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wakeup";
    default:                return "OTHER/UNKNOWN";
  }
}

// ==========================================
// 🚀 INITIALIZATION
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- OfficeAir ESP32 (SHT21 + Buffer + Multi-Arp V12.4 + IRAM-safe Tick Sampler) Initializing ---");

  // Read reset reason and update lifetime statistics (NVS flash, one write per boot)
  esp_reset_reason_t rr = esp_reset_reason();
  bootResetReason = rr;
  prefs.begin("officeair", false);
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  wdtResetCount   = prefs.getUInt("wdt", 0);
  swResetCount    = prefs.getUInt("sw", 0);
  panicResetCount = prefs.getUInt("panic", 0);
  if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) {
    wdtResetCount++;
    prefs.putUInt("wdt", wdtResetCount);
  } else if (rr == ESP_RST_SW) {
    swResetCount++;
    prefs.putUInt("sw", swResetCount);
  } else if (rr == ESP_RST_PANIC || rr == ESP_RST_BROWNOUT) {
    panicResetCount++;
    prefs.putUInt("panic", panicResetCount);
  }

  Serial.printf("🔁 [Boot] Startup #%u | Last reset reason: %s\n", bootCount, resetReasonToString(rr));
  Serial.printf("📈 [Lifetime] WDT resets: %u | SW resets: %u | Panic/Brownout: %u\n",
                wdtResetCount, swResetCount, panicResetCount);

  // ==========================================
  // 🕵️ CRASH FORENSICS - NVS-persistent, reboot-storm proof
  // Records the reason of SW resets too (FAILCNT / WIFI_TIMEOUT / NTP_TIMEOUT),
  // and un-uploaded forensics accumulate in flash until they make it up.
  // ==========================================
  bool abnormalReset = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
                        rr == ESP_RST_WDT || rr == ESP_RST_PANIC ||
                        rr == ESP_RST_BROWNOUT);

  String thisCrash = "";

  if (rr == ESP_RST_SW) {
    // SW restart: our own code requested it - the reason was written to NVS
    // right before the restart
    String swReason = prefs.getString("swr", "?");
    prefs.remove("swr");
    thisCrash = "SW@" + swReason;
  } else if (abnormalReset && cpMagic == CP_MAGIC) {
    // 1) Breadcrumb: which phase the code was in at the moment of death
    thisCrash = String(resetReasonShort(rr)) + "@" + checkpointToString(cpPhase);
    if (cpDetail > 0) thisCrash += "/" + String(cpDetail);

    // 2) Core dump: on panic/WDT the IDF wrote the backtrace to flash
    esp_core_dump_summary_t *summary =
        (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (summary != NULL) {
      if (esp_core_dump_get_summary(summary) == ESP_OK) {
        Serial.printf("🕵️ [CoreDump] Crash task: %s | PC: 0x%08x\n",
                      summary->exc_task, (unsigned)summary->exc_pc);
        thisCrash += "|PC:0x" + String((unsigned)summary->exc_pc, HEX);
        Serial.print("🕵️ [CoreDump] Backtrace:");
        thisCrash += "|BT:";
        for (int d = 0; d < summary->exc_bt_info.depth && d < 8; d++) {
          Serial.printf(" 0x%08x", (unsigned)summary->exc_bt_info.bt[d]);
          if (d > 0) thisCrash += ",";
          thisCrash += "0x" + String((unsigned)summary->exc_bt_info.bt[d], HEX);
        }
        Serial.println(summary->exc_bt_info.corrupted ? " (TRUNCATED!)" : "");
        esp_core_dump_image_erase();
      } else {
        Serial.println("🕵️ [CoreDump] No readable core dump (normal for brownout).");
      }
      free(summary);
    }
  } else if (abnormalReset) {
    thisCrash = String(resetReasonShort(rr)) + "@NO_BREADCRUMB";
  }

  // Chain with previously un-uploaded forensics (reboot storm!)
  String prevPending = prefs.getString("pcrash", "");
  if (thisCrash.length() > 0) {
    crashInfo = prevPending.length() > 0 ? (prevPending + "_" + thisCrash) : thisCrash;
    // URL length guard: keep only the freshest ~220 characters
    if (crashInfo.length() > 220) {
      crashInfo = crashInfo.substring(crashInfo.length() - 220);
    }
    prefs.putString("pcrash", crashInfo);
    Serial.printf("🕵️ [Forensics] Pending upload: %s\n", crashInfo.c_str());
  } else {
    crashInfo = prevPending;  // leftovers from a previous storm, if not yet uploaded
    if (crashInfo.length() > 0) {
      Serial.printf("🕵️ [Forensics] Previously un-uploaded forensics: %s\n", crashInfo.c_str());
    }
  }
  // Fresh magic + initial state for the current run
  setCheckpoint(CP_IDLE);

  ledInit();
  // Register the tick-hook CPU sampling profiler.
  // Discover the idle task handles first, then arm the hooks.
  idleTaskHandle[0] = xTaskGetIdleTaskHandleForCPU(0);
  idleTaskHandle[1] = xTaskGetIdleTaskHandleForCPU(1);
  esp_register_freertos_tick_hook_for_cpu(tickHook0, 0);
  esp_register_freertos_tick_hook_for_cpu(tickHook1, 1);
  // The LED lives in its own FreeRTOS task - the heart keeps beating even
  // while the main thread is blocking
  xTaskCreatePinnedToCore(ledTask, "ledTask", 3072, NULL, 1, NULL, 0);
  ledMode = LED_MODE_BLINK;  // during setup: connection blink

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(100);

  Wire.begin(MY_SDA, MY_SCL);
  Serial.println("[Sensor] I2C bus initialized.");

  // First battery voltage reading right at boot
  Serial.printf("🔋 [VBat] Battery voltage: %.2f V\n", readVBat());

  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.print("[Wi-Fi] Connecting");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    if (millis() - wifiStart > 60000) {
      Serial.println("\n❌ [Wi-Fi] Could not connect within 60 s. Restarting...");
      prefs.putString("swr", "WIFI_TIMEOUT");   // record the SW restart reason
      ESP.restart();
    }
  }
  Serial.println("\n✅ [Wi-Fi] Connected!");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("[System] NTP time sync");
  unsigned long ntpStart = millis();
  while (time(NULL) < 1000000000) {
    delay(250);
    Serial.print(".");
    if (millis() - ntpStart > 60000) {
      Serial.println("\n❌ [NTP] No time sync within 60 s. Restarting...");
      prefs.putString("swr", "NTP_TIMEOUT");   // record the SW restart reason
      ESP.restart();
    }
  }
  Serial.printf("\n✅ [System] Time set! Unix epoch: %lu\n", (unsigned long)time(NULL));

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.printf("🐕 [WDT] Watchdog armed: %d s timeout.\n", WDT_TIMEOUT_S);
  Serial.println("🔘 [Button] Press the USER button (GPIO0) for an immediate measurement + upload.");

  ledMode = LED_MODE_HEARTBEAT;  // setup done, switch to resting heart rate

  printStatusLine();
}

// ==========================================
// 🔄 MAIN LOOP
// ==========================================
void loop() {
  esp_task_wdt_reset();
  unsigned long currentMillis = millis();

  // --- 0. USER BUTTON POLLING (debounced) ---
  if (digitalRead(BUTTON_PIN) == LOW &&
      (currentMillis - lastButtonPress) > BUTTON_DEBOUNCE_MS) {
    lastButtonPress = currentMillis;
    forceMeasurement = true;
    Serial.println("\n🔘 [Button] Manual measurement requested! Starting measurement + upload...");
  }

  // --- 1. MEASUREMENT & SCAN CYCLE (EVERY 10 MIN or ON BUTTON) ---
  if (currentMillis - lastMeasurementTime >= MEASUREMENT_INTERVAL ||
      lastMeasurementTime == 0 ||
      forceMeasurement) {
    lastMeasurementTime = currentMillis;
    forceMeasurement = false;

    setCheckpoint(CP_SENSOR);   // 🍞 breadcrumb
    float temp = readTemperature();
    float hum  = readHumidity();
    unsigned long timestamp = getRtcTimestamp();
    float vbat = readVBat();   // battery voltage at measurement time

    if (!isnan(temp) && !isnan(hum) && temp > -40.0) {
      lastKnownDeviceCount = countActiveDevices();

      if (dataBuffer.size() >= MAX_BUFFER_SIZE) {
        dataBuffer.erase(dataBuffer.begin());
      }

      Measurement m = {temp, hum, timestamp, lastKnownDeviceCount, vbat};
      dataBuffer.push_back(m);

      Serial.printf("📊 Data buffered -> Temp: %.1f C | Humidity: %.1f %% | Devices: %d | VBat: %.2f V | In buffer: %d\n",
                    temp, hum, lastKnownDeviceCount, vbat, (int)dataBuffer.size());
    } else {
      Serial.println("[Sensor] ERROR! Failed to read the SHT21 sensor.");
    }
  }

  // --- 2. DATA TRANSMISSION CYCLE ---
  if (!dataBuffer.empty() && WiFi.status() == WL_CONNECTED) {
    esp_task_wdt_reset();
    ledMode = LED_MODE_ACTIVITY;  // sport mode during upload too

    Measurement oldestData = dataBuffer.front();

    HTTPClient http;
    // Telemetry parameters:
    //   heap     = total free heap
    //   maxblock = largest single allocatable block (TLS needs ~40 kB!)
    //   uptime   = minutes since boot (drops to zero = a silent reboot happened)
    //   crash    = forensics of the previous run's death (empty if none)
    String fullUrl = googleScriptUrl +
                     "?temperature=" + String(oldestData.temperature, 1) +
                     "&humidity=" + String(oldestData.humidity, 1) +
                     "&time=" + String(oldestData.timestamp) +
                     "&buffer=" + String((int)dataBuffer.size()) +
                     "&devices=" + String(oldestData.deviceCount) +
                     "&heap=" + String(ESP.getFreeHeap()) +
                     "&maxblock=" + String(ESP.getMaxAllocHeap()) +
                     "&uptime=" + String(millis() / 60000UL) +
                     "&boots=" + String(bootCount) +
                     "&wdt=" + String(wdtResetCount) +
                     "&rr=" + String(resetReasonShort(bootResetReason)) +
                     "&vbat=" + String(oldestData.vbat, 2) +
                     "&crash=" + crashInfo;

    Serial.println("🌐 Transmitting to Google Sheets...");
    setCheckpoint(CP_HTTP_BEGIN);   // 🍞 breadcrumb: TLS handshake coming up
    http.begin(fullUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    // Explicit timeouts - without these a stuck TLS connection can block
    // the loop FOREVER
    http.setConnectTimeout(10000);
    http.setTimeout(15000);

    setCheckpoint(CP_HTTP_GET);     // 🍞 breadcrumb: request going out
    int httpCode = http.GET();
    setCheckpoint(CP_HTTP_RESP);    // 🍞 breadcrumb: response processing
    if (httpCode == 200) {
      String response = http.getString();
      if (response == "OK") {
        Serial.println("✅ [Sheets] Saved (OK ACK). Popping entry from the buffer.");
        dataBuffer.erase(dataBuffer.begin());
        consecutiveFails = 0;
        if (crashInfo.length() > 0) {
          prefs.remove("pcrash");   // forensics made it up, safe to clear from NVS
          crashInfo = "";
        }
        delay(2000);
      } else {
        consecutiveFails++;
        Serial.printf("⚠️ [Sheets] Rejected (consecutive failure #%d): %s\n",
                      consecutiveFails, response.c_str());
        Serial.println("⏱️ 15-second penalty wait...");
        delay(15000);
      }
    } else {
      consecutiveFails++;
      Serial.printf("❌ [Network] HTTP error: %d (consecutive failure #%d). Waiting 15 s...\n",
                    httpCode, consecutiveFails);
      delay(15000);
    }
    http.end();

    if (consecutiveFails >= MAX_CONSECUTIVE_FAILS) {
      Serial.println("💀 [Recovery] Too many consecutive failures. Self-restarting in 3 s...");
      prefs.putString("swr", "FAILCNT");   // record the SW restart reason
      delay(3000);
      ESP.restart();
    }
  }

  // --- 3. UART STATUS LINE ONCE A MINUTE ---
  if (currentMillis - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
    lastStatusPrint = currentMillis;
    printStatusLine();
  }

  // --- 4. SWITCH BACK TO RESTING HEARTBEAT (IDLE) ---
  // The LED is driven by its own task; here we only signal that work is done.
  // The "cool-down" (slow return from sport mode to 50 BPM) happens in the
  // task by itself.
  setCheckpoint(CP_IDLE);
  ledMode = LED_MODE_HEARTBEAT;

  // BUSY-WAIT FIX - without this the loop spins with no delay, the idle task
  // never runs on core 1, and the CPU meter rightfully shows 100% (it was!).
  // The 10 ms yield hands time to the idle task; button polling is still 100 Hz.
  vTaskDelay(pdMS_TO_TICKS(10));
}
