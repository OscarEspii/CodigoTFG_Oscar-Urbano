// ═══════════════════════════════════════════════════════════════
//  Signal generator for the memristor measurement bench
//  Arduino Mega 2560 + 8-bit R-2R ladder + TK042
//
//  The web app sends a text frame over the serial port at 2 Mbaud,
//  the Arduino generates the requested signal, reads the ADC while it
//  is generating it, and when it finishes it sends back all the samples
//  at once.
//
//  Pins
//    22-29    R-2R ladder (the whole of Port A, written in one go)
//    32 SM1   active memristor
//    33 SM2   current direction
//    34 EC    current enable
//    35 M0    current mirror, together with M1
//    36 M1      00 = x1/8, 01 = x1/4, 10 = x1/2, 11 = x1
//    53 CS    MCP3304 chip select (the rest of the SPI is hardware)
//
//  Serial port
//    2 000 000 baud, 8N1. One frame per line, terminated by '\n' ('\r' is
//    ignored). 127 characters max; anything beyond that is silently dropped.
//
//  Startup
//    On power-up (or on reset; opening the port resets the Mega) it sends
//      OK:READY
//    Everything is left idle: DAC at 0, EC low (no current), SM1 and SM2 high.
//
//  How a measurement is started
//    There is no separate START command: the TYPE:... frame itself is the
//    order to start. As soon as it is received and validated, the Arduino
//    generates NCYC consecutive cycles, sends DATA:...END and goes back to
//    idle, waiting for another frame.
//    It is one-shot: to repeat, send the frame again.
//    While measuring it does not read the serial port, so a measurement
//    cannot be stopped halfway, and nothing should be sent until END arrives.
//
//  Frames it understands
//    TYPE:SIN,F:10.00,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:TRI,F:10.00,VMAX:1.00,VMIN:-1.00,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:PUL,VHI:1.00,VLO:0.00,DUR:500,PER:10,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:OLD,T1:2000,T2:10,MEM:0,INT:11,NCYC:10,NPTS:170
//
//    KEY:value fields separated by commas, in any order (see findKey()).
//    If a field is missing, the value from the previous frame is reused.
//
//    Common to all of them
//      TYPE   SIN | TRI | PUL | OLD (only the first letter counts)
//      MEM    1 = SM1 high, 0 = SM1 low (which memristor is active)
//      INT    two digits M0 M1, the current mirror (see the pin table)
//      NCYC   number of cycles (min. 1). In OLD, number of pulses (max. 512)
//      NPTS   samples per cycle (min. 1). NCYC x NPTS <= 512. Not used in OLD,
//             but sent anyway so the format stays the same
//    SIN    F    frequency in Hz (min. 1, the absolute value is taken)
//           AMP  peak amplitude in V
//           OFF  offset in V
//    TRI    F    frequency in Hz (min. 1)
//           VMAX maximum voltage in V
//           VMIN minimum voltage in V
//    PUL    VHI  pulse voltage in V
//           VLO  rest voltage in V
//           DUR  pulse duration in us
//           PER  period in ms
//    OLD    T1   EC pulse duration in us (min. 1, at most T2)
//           T2   period in ms (min. 1)
//    All voltages are clipped to +-1 V (TK042_VMAX) when output.
//
//  Replies to a frame, in this order
//    WARN:NPTS_CLIPPED_TO_MAX   NCYC x NPTS > 512, NPTS has been reduced
//                               (in OLD, NCYC > 512 and NCYC has been reduced)
//    WARN:NPTS_CLIPPED_TO_MIN   NPTS < 1 in SIN/TRI/PUL, 1 is used
//    WARN:FREQ_CLIPPED_TO_MIN   F < 1 Hz in SIN/TRI, 1 Hz is used
//    WARN:VOLTAGE_CLIP_1V       the signal goes beyond 1 V, peaks will be clipped
//    OK:SIN | OK:TRI | OK:PUL | OK:OLD   frame accepted, measurement starts
//    ERR:UNKNOWN_TYPE           TYPE not recognised, nothing is measured
//    WARNs are only warnings: the measurement starts anyway.
//
//  Data, sent once when all cycles are done
//    DATA:t0,ch0,ch1;t1,ch0,ch1;...;END
//    t        us since the start of the signal (accumulated across cycles)
//    ch0,ch1  MCP3304 voltage in V, channels 0 and 1, with 4 decimals.
//             Negative sign during negative half-cycles (see
//             storeSample()). In OLD there is one sample per pulse.
//    Each cycle also has a 10 s timeout (PERIOD_TIMEOUT_US).
//
//  Test mode (TEST_MODE)
//    Uncommenting "#define TEST_MODE" (below) skips reading the MCP3304: each
//    sample is made up from the voltage requested from the DAC plus random
//    noise (ch1 at 75 % of ch0). Useful for testing the frames and the web
//    app without the chip. It is enabled at compile time, not by a frame,
//    and it is off by default.
//    Not to be confused with the chip's own test mode.
//
//  Things I tend to forget :)
//   · The buffer holds 512 samples. NCYC x NPTS cannot exceed that; if it
//     does, NPTS is reduced and WARN:NPTS_CLIPPED_TO_MAX is sent.
//   · The TK042 cannot take more than 1.5 V on its control input. The web
//     app already limits it, but a frame typed by hand in the console skips
//     that filter, so dacWrite() clips to 1 V on its own (0.5 V of margin
//     for safety).
//   · OLD is the mode used with an older version of the chip.
//     In OLD, EC doesn't enable anything, it IS the switch making the pulses.
//     It is fully explained in case 'O' of generateAndSample().
//   · The loop takes about 350 us per iteration and that conditions
//     everything: how many samples can be requested, how short an EC pulse
//     can be, and the instant at which each sample is taken.
//   · The web app's oscilloscope has no protocol of its own, it sends
//     small normal frames in a loop.
// ═══════════════════════════════════════════════════════════════

#include <math.h>
#include <SPI.h>

// ── Test mode ────────────────────────────────────────────────
// This test mode is not the test mode built into the chip
// (I know, I could have picked another name). During development
// I needed to be able to try things out, so this TEST_MODE produced
// "random" data to check whether the frames arrived correctly.
// I would delete it, but it has been very useful, so it stays as a
// keepsake.

// Uncomment this line to use simulated data instead of the real ADC
// #define TEST_MODE

// ── Hardware configuration ───────────────────────────────────
#define DAC_PORT PORTA
#define DAC_DDR DDRA
#define PIN_SM1 32
#define PIN_SM2 33
#define PIN_EC 34
#define PIN_M0 35
#define PIN_M1 36
#define ADC_CS 53
#define BAUD 2000000

// ── DAC range ────────────────────────────────────────────────
#define DAC_MIN 0
#define DAC_MAX 255
#define V_REF 5.0

// ── TK042 limit ──────────────────────────────────────────────
// Ceiling for the control input. dacWrite() clips here no matter
// what, even if the frame asks for more.
// As said above, it is 1 V for safety; 1.5 V would be pushing our luck
#define TK042_VMAX 1.0

// ── Acquisition ──────────────────────────────────────────────
// Total samples of one signal, adding up all its cycles.
// 512 x 8 bytes = 4 KB, half of the Mega's SRAM.
#define MAX_SAMPLES 512

// ═══════════════════════════════════════════════════════════════
//  GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════════
char sigType = ' ';

// Sine
float sin_freq = 1.0;
float sin_amp = 2.0;
float sin_off = 0.0;

// Triangle
float tri_freq = 1.0;
float tri_vmax = 1.0;
float tri_vmin = -1.0;

// Pulses
float pul_vhi = 4.0;
float pul_vlo = 0.0;
unsigned long pul_dur = 500;
unsigned long pul_per = 10000;

// EC pulses (old chip) (it also works on the new one; "old" just stuck as
// the name).
// There is no voltage of any kind here: the DAC stays at zero and the
// signal is PIN_EC itself switching. Both times are stored in us, as in
// pulses, even though T2 arrives in ms.
unsigned long old_t1 = 500;
unsigned long old_t2 = 10000;

// Previous EC state, to catch its falling edge, which is where the sample
// is taken. Reset in loop() when an 'O' signal starts.
bool old_ecPrev = false;

// micros() of the last sample. Used to timestamp the next one, because
// here the gap between samples is a whole T2, not one loop iteration.
unsigned long old_lastSampleUs = 0;

// Config
int sel_mem = 1;
char cfg_int[3] = "11";
int n_cycles = 1;
int n_points = 512;  // Number of points per cycle (arrives as NPTS in the frame)

// Time
float phaseAccum = 0.0;
unsigned long lastMicros = 0;
unsigned long signalStart = 0;
unsigned long sampleTimeAccum = 0;  // sum of the dt values, acts as the signal clock
unsigned long lastSampleUs = 0;     // micros() of the last sample (SIN/TRI/PUL); reset in loop()
unsigned long DTMax = 0;            // gap between samples (us) = period / n_points

// Serial buffer
char rxBuf[128];
int rxIdx = 0;

// Last reading of each channel, in ADC counts and signed (-4095..4095).
// The sign is set by storeSample() during negative half-cycles.
int16_t adc_ch0 = 0;
int16_t adc_ch1 = 0;

// Samples from all cycles accumulate here, and nothing is sent until the
// end. They are stored in counts, not volts, so that the sampling routine
// is as short as possible; the division is done by sendData(), once per
// sample and with no hurry.
struct Sample {
  unsigned long t;
  int16_t ch0;
  int16_t ch1;
};
Sample samples[MAX_SAMPLES];
int sampleCount = 0;

// ═══════════════════════════════════════════════════════════════
//  dacWrite()
//  Outputs a voltage through the ladder. The sign goes separately, on SM2,
//  because the ladder can only produce positive voltages.
//    SM2 high  = forward direction  (MEM1/MEM2 -> MEMC)
//    SM2 low   = reverse direction  (MEMC -> MEM1/MEM2)
//
//  The +0.5 is needed: when converting to uint8_t, C++ does not round, it
//  drops the decimals, so 51.9 and 51.1 both ended up as 51 and the ladder
//  output was on average half a step too low, about 9.8 mV.
//  Adding 0.5 before truncating, sometimes it's over and sometimes under,
//  and the average stays at zero. It matters a lot, because the app
//  computes K with the voltage it requested, not the one that came out, and
//  that bias lowered K by 1.8 %. Now it stays at 0.03 %.
//
//  The byte cannot overflow: with the 1 V clip the highest code is 51.
// ═══════════════════════════════════════════════════════════════
uint8_t dacWrite_val;

void dacWrite(float volts) {
  if (volts < 0) digitalWrite(PIN_SM2, LOW);
  else digitalWrite(PIN_SM2, HIGH);
  volts = fabs(volts);
  volts = constrain(volts, 0.0, TK042_VMAX);
  dacWrite_val = (uint8_t)((volts / V_REF) * DAC_MAX + 0.5);
  DAC_PORT = dacWrite_val;
}

// ═══════════════════════════════════════════════════════════════
//  PARSER
// The parser is what searches the frame for each piece of data
// and stores it in its corresponding variable.
// (To be honest I had no idea it was called that, thanks Rodrigo I guess)
// ═══════════════════════════════════════════════════════════════
// findKey() returns a pointer to the key's value, or NULL if it is absent.
// It only matches the key as a whole field: it must be at the start of the
// frame or right after a comma, and followed by ':'. Previously "F:" was
// searched for with plain strstr(), which also matched inside "OFF:", so
// if OFF came before F the offset was read as the frequency.
const char* findKey(const char* frame, const char* key) {
  size_t n = strlen(key);
  const char* p = frame;
  while ((p = strstr(p, key)) != NULL) {
    if ((p == frame || p[-1] == ',') && p[n] == ':') return p + n + 1;
    p++;
  }
  return NULL;
}

#define PARSE_FLOAT(frame, key, var) \
  { \
    const char* p = findKey(frame, key); \
    if (p) var = atof(p); \
  }

#define PARSE_LONG(frame, key, var) \
  { \
    const char* p = findKey(frame, key); \
    if (p) sscanf(p, "%ld", &var); \
  }

#define PARSE_STR(frame, key, var) \
  { \
    const char* p = findKey(frame, key); \
    if (p) sscanf(p, "%2s", var); \
  }

long memVal = 0;
char type[8] = "";
long dur = 500, per = 10;
long oldT1 = 500, oldT2 = 10;
long cyc = 1;
long pts = 512;

void parseFrame(const char* frame) {
  PARSE_LONG(frame, "MEM", memVal);
  sel_mem = (int)memVal;
  PARSE_STR(frame, "INT", cfg_int);
  digitalWrite(PIN_SM1, sel_mem == 1 ? HIGH : LOW);
  // M0/M1 select the current mirror. They are only touched when a new
  // frame arrives, never inside the sampling loop.
  digitalWrite(PIN_M0, cfg_int[0] == '1' ? HIGH : LOW);
  digitalWrite(PIN_M1, cfg_int[1] == '1' ? HIGH : LOW);

  PARSE_STR(frame, "TYPE", type);

  // All four signals carry NCYC and NPTS, so they are read and validated
  // here once rather than in every case.
  PARSE_LONG(frame, "NCYC", cyc);
  n_cycles = (int)cyc;
  PARSE_LONG(frame, "NPTS", pts);
  n_points = (int)pts;

  if (n_cycles < 1) n_cycles = 1;  // otherwise there is a division by zero further down
  // Same for NPTS: DTMax = period / n_points below. OLD doesn't use NPTS,
  // so it isn't checked (or warned about) there.
  if (type[0] != 'O' && n_points < 1) {
    n_points = 1;
    Serial.println("WARN:NPTS_CLIPPED_TO_MIN");
  }
  if (type[0] == 'O') {
    // Here there is one sample per pulse, so what limits the buffer is
    // directly the number of pulses. NPTS is read so as not to break the
    // frame format, but it is not used for this signal.
    if (n_cycles > MAX_SAMPLES) {
      n_cycles = MAX_SAMPLES;
      Serial.println("WARN:NPTS_CLIPPED_TO_MAX");
    }
  } else if ((long)n_cycles * (long)n_points > MAX_SAMPLES) {
    // If they don't fit, instead of rejecting the signal the points per
    // cycle are reduced, a warning is sent and it starts anyway.
    n_points = MAX_SAMPLES / n_cycles;
    if (n_points < 1) n_points = 1;
    Serial.println("WARN:NPTS_CLIPPED_TO_MAX");
  }

  switch (type[0]) {

    case 'S':
      {
        PARSE_FLOAT(frame, "F", sin_freq);
        PARSE_FLOAT(frame, "AMP", sin_amp);
        PARSE_FLOAT(frame, "OFF", sin_off);
        sin_freq = fabs(sin_freq);
        if (sin_freq < 1.0) {
          sin_freq = 1.0;  // 1 Hz minimum, like the web app's slider
          Serial.println("WARN:FREQ_CLIPPED_TO_MIN");
        }
        // if it goes over 1 V, dacWrite() will clip the peaks. Let it be known.
        if (fabs(sin_off) + sin_amp > TK042_VMAX) {
          Serial.println("WARN:VOLTAGE_CLIP_1V");
        }
        // the period in us is 1e6/freq, and this splits it among the points
        DTMax = (unsigned long)((1e6 / sin_freq) / n_points);
        sigType = 'S';
        Serial.println("OK:SIN");
        break;
      }

    case 'T':
      {
        PARSE_FLOAT(frame, "F", tri_freq);
        PARSE_FLOAT(frame, "VMAX", tri_vmax);
        PARSE_FLOAT(frame, "VMIN", tri_vmin);
        tri_freq = fabs(tri_freq);
        if (tri_freq < 1.0) {
          tri_freq = 1.0;  // 1 Hz minimum, like the web app's slider
          Serial.println("WARN:FREQ_CLIPPED_TO_MIN");
        }
        // same as above, if it goes beyond 1 V it gets clipped
        if (tri_vmax > TK042_VMAX || tri_vmin < -TK042_VMAX) {
          Serial.println("WARN:VOLTAGE_CLIP_1V");
        }
        DTMax = (unsigned long)((1e6 / tri_freq) / n_points);
        sigType = 'T';
        Serial.println("OK:TRI");
        break;
      }

    case 'P':
      {
        PARSE_FLOAT(frame, "VHI", pul_vhi);
        PARSE_FLOAT(frame, "VLO", pul_vlo);
        PARSE_LONG(frame, "DUR", dur);
        PARSE_LONG(frame, "PER", per);
        pul_dur = (unsigned long)dur;
        pul_per = (unsigned long)per * 1000;
        // same as above, if it goes beyond 1 V it gets clipped
        if (pul_vhi > TK042_VMAX || pul_vlo < -TK042_VMAX) {
          Serial.println("WARN:VOLTAGE_CLIP_1V");
        }
        // here the period is already in us, no conversion needed
        DTMax = pul_per / n_points;
        sigType = 'P';
        Serial.println("OK:PUL");
        break;
      }

    // Old chip: the signal is made by EC, not the DAC. TYPE:OLD and not
    // something starting with 'P' or 'S' because the switch dispatches on
    // type[0] and those letters are already taken by PUL and SIN. So calling
    // it "old" suits us perfectly haha
    case 'O':
      {
        PARSE_LONG(frame, "T1", oldT1);
        PARSE_LONG(frame, "T2", oldT2);
        if (oldT1 < 1) oldT1 = 1;
        if (oldT2 < 1) oldT2 = 1;              // T2 min. 1 ms: a zero period gives not even one iteration
        old_t1 = (unsigned long)oldT1;         // T1 arrives in µs
        old_t2 = (unsigned long)oldT2 * 1000;  // T2 arrives in ms
        if (old_t1 > old_t2) old_t1 = old_t2;  // the pulse cannot last longer than the period
        // No voltages are read: this signal has none. Nor is the
        // WARN:VOLTAGE_CLIP_1V warning needed, because the DAC is not touched.
        // DTMax is NOT used for this signal: periodic sampling is disabled
        // and the sample is triggered by EC's falling edge (one per pulse).
        // It is set to zero to make clear that no value inherited from the
        // previous signal is controlling anything.
        DTMax = 0;
        sigType = 'O';
        Serial.println("OK:OLD");
        break;
      }

    default:
      Serial.println("ERR:UNKNOWN_TYPE");
      break;
  }
}

// ═══════════════════════════════════════════════════════════════
//  readSerial()
// ═══════════════════════════════════════════════════════════════
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      rxBuf[rxIdx] = '\0';
      if (rxIdx > 0) parseFrame(rxBuf);
      rxIdx = 0;
    } else if (c != '\r' && rxIdx < (int)sizeof(rxBuf) - 1) {
      rxBuf[rxIdx++] = c;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  generateAndSample()
//  Writes one point of the signal on every call, always. The ADC, on the
//  other hand, is only read every DTMax microseconds, which is the gap
//  needed for n_points samples to be spread over the period.
//
//  Writing on every iteration but not sampling on every one is what allows
//  higher frequencies without spoiling the waveform. Even if samples are
//  lost, the signal the chip sees is still the right one.
//
//  Samples are taken at the START of each DTMax slot of the period
//  (t = 0, T/N, 2T/N... with DT preloaded to DTMax in runOnePeriod()), and
//  the grid restarts with every period, so each period gets exactly N
//  samples (if the loop is fast enough) and its first one is at phase 0,
//  which is what the web app assumes when it splits the cycles. Taking them
//  at the end of each slot put the last one on the period boundary, where
//  the loop may already have stopped: PUL lost the last sample of the
//  signal and shifted every cycle by one sample.
// ═══════════════════════════════════════════════════════════════
unsigned long DT = 0;
int periodSamples = 0;   // samples taken so far in the current period
unsigned long now;
unsigned long dt;
float outVolts;
float range;
float phaseFrac;
unsigned long tInPeriod;
bool old_ecNow;  // level EC should have in this iteration ('O' signal only)

void generateAndSample() {
  now = micros();
  dt = now - lastMicros;
  outVolts = 0.0;

  // lastMicros is always updated, whatever the signal type
  lastMicros = now;

  switch (sigType) {

      // phaseAccum only grows; 1 is never subtracted when it wraps. Its
      // fractional part is used for the voltage and that's it. Subtracting 1
      // is what hung the Arduino at high frequencies, because if one loop
      // iteration advances more than a whole period, subtracting 1 once is
      // not enough and runOnePeriod() keeps waiting for an end of period
      // that never comes.
      // I don't even know why, but Rodrigo changed it and it works, so DO NOT TOUCH

    case 'S':
      {
        phaseAccum += sin_freq * (dt / 1e6);
        phaseFrac = phaseAccum - (long)phaseAccum;
        outVolts = sin_off + sin_amp * sin(2.0 * PI * phaseFrac);
        break;
      }

    case 'T':
      {
        phaseAccum += tri_freq * (dt / 1e6);
        phaseFrac = phaseAccum - (long)phaseAccum;
        range = tri_vmax - tri_vmin;
        if (phaseFrac < 0.5)
          outVolts = tri_vmin + (phaseFrac / 0.5) * range;
        else
          outVolts = tri_vmax - ((phaseFrac - 0.5) / 0.5) * range;
        break;
      }

    case 'P':
      {
        tInPeriod += dt;
        if (tInPeriod >= pul_per) tInPeriod -= pul_per;
        outVolts = (tInPeriod < pul_dur) ? pul_vhi : pul_vlo;
        break;
      }

    // Keeps time the same way as 'P', but what switches is EC and there is
    // no voltage to output. One sample per pulse, on the falling edge, which
    // is what the manufacturer asks for. Since the loop does one iteration
    // every ~350 us, the sample lands on the iteration after the edge, with
    // that delay.
    case 'O':
      {
        tInPeriod += dt;
        if (tInPeriod >= old_t2) tInPeriod -= old_t2;
        old_ecNow = (tInPeriod < old_t1);
        digitalWrite(PIN_EC, old_ecNow ? HIGH : LOW);
        if (old_ecPrev && !old_ecNow) {
          // 0 V because there is no voltage here, and the dt is the time
          // elapsed since the previous sample, a whole T2, not this loop
          // iteration's.
          storeSample(0.0, now - old_lastSampleUs);
          old_lastSampleUs = now;
        }
        old_ecPrev = old_ecNow;
        break;
      }

    default:
      outVolts = 0.0;
      break;
  }

  // In 'O' the DAC is not written. loop() set it to zero before starting
  // and it stays there for the whole generation.
  if (sigType != 'O') dacWrite(outVolts);

  // DTMax-based sampling applies to the three DAC signals. In 'O' it is
  // off, since there the sample is triggered by the edge inside the case.
  // The time passed to storeSample() is the real time since the previous
  // sample, not this iteration's dt: several iterations go by between two
  // samples, and passing only the last dt made the timestamps run about
  // DTMax / 350 us times too slow.
  if (sigType != 'O') {
    DT += dt;
    if (DT >= DTMax && periodSamples < n_points) {
      storeSample(outVolts, now - lastSampleUs);
      lastSampleUs = now;
      periodSamples++;
      DT -= DTMax;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  MCP3304
//  Three bytes per reading, aligned according to Figure 6-4 of DS21697F.
//  Careful, the MCP3208 frame does not work here: this one returns one more
//  bit and its data window starts earlier, so with the 3208 one you get half.
//
//   cycle  1 2 3 4   5      6     7  8 │ 9 10  11    12   13..16 │ 17..24
//   MOSI   0 0 0 0 START SGL/DIFF D2 D1│D0  X   X     X     X    │   X
//   MISO   ? ? ? ?   ?      ?     ?  ? │ ?  ?  NULL   SB  B11..B8│ B7..B0
// ═══════════════════════════════════════════════════════════════
uint8_t mcp3304Read_b0;
uint8_t mcp3304Read_b1;
uint8_t mcp3304Read_hi;
uint8_t mcp3304Read_lo;
uint16_t mcp3304Read_raw;

int mcp3304Read(uint8_t channel) {
  // 0x0C = 0000 1100, i.e. the zeros, the start bit and single-ended mode
  mcp3304Read_b0 = 0x0C;
  // the channel bit goes in bit 7 of the second byte
  mcp3304Read_b1 = (channel == 1) ? 0x80 : 0x00;
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(mcp3304Read_b0);
  mcp3304Read_hi = SPI.transfer(mcp3304Read_b1);
  mcp3304Read_lo = SPI.transfer(0x00);
  digitalWrite(ADC_CS, HIGH);
  mcp3304Read_raw = ((uint16_t)(mcp3304Read_hi & 0x0F) << 8) | mcp3304Read_lo;
  return mcp3304Read_raw;
}


// ═══════════════════════════════════════════════════════════════
//  storeSample()
//  Stores the reading in counts, without converting to volts, so this is
//  as fast as possible. sendData() does the division at the end.
//
//  V01 and V02 are fixed pins of the chip, so reversing the current does
//  not change which node each channel sees, only which one ends up higher.
//  The sign takes care of that, and each channel always comes from its own
//  reading.
// ═══════════════════════════════════════════════════════════════
int storeSample_noise0;
int storeSample_noise1;
int16_t storeSample_dacRaw;
int16_t storeSample_raw0;
int16_t storeSample_raw1;
bool storeSample_neg;

void storeSample(float dacVolts, unsigned long dt) {
  // accumulated, so the timestamp is measured from the start of the signal
  sampleTimeAccum += dt;

  storeSample_neg = (dacVolts < 0);

#ifdef TEST_MODE
  // Simulate a RAW ADC value (0-4095) from the DAC voltage, with integer
  // noise so the cycles look different from each other.
  // (fabs: the physical node always sees the magnitude, the sign goes separately)
  // As said, we leave it here, but it was done purely for debugging
  storeSample_dacRaw = (int16_t)((fabs(dacVolts) / V_REF) * 4095.0);
  storeSample_noise0 = random(-250, 250);
  storeSample_noise1 = random(-330, 330);
  storeSample_raw0 = constrain((int)storeSample_dacRaw + storeSample_noise0, 0, 4095);
  storeSample_raw1 = constrain((int)(storeSample_dacRaw * 0.75) + storeSample_noise1, 0, 4095);
#else
  storeSample_raw0 = mcp3304Read(0);
  storeSample_raw1 = mcp3304Read(1);
#endif

  // each channel, its own reading. The only thing that changes is the sign.
  adc_ch0 = storeSample_neg ? -storeSample_raw0 : storeSample_raw0;
  adc_ch1 = storeSample_neg ? -storeSample_raw1 : storeSample_raw1;

  if (sampleCount < MAX_SAMPLES) {
    samples[sampleCount].t = sampleTimeAccum;
    samples[sampleCount].ch0 = adc_ch0;
    samples[sampleCount].ch1 = adc_ch1;
    sampleCount++;
  }
}

// ═══════════════════════════════════════════════════════════════
//  sendData()
//  Sends all the samples at once, DATA:t,ch0,ch1;...;END
//  Counts are converted to volts here, when there is no longer any hurry.
// ═══════════════════════════════════════════════════════════════
int sendData_i;
float sendData_v0;
float sendData_v1;

void sendData() {
  Serial.print("DATA:");
  for (sendData_i = 0; sendData_i < sampleCount; sendData_i++) {
    sendData_v0 = (samples[sendData_i].ch0 / 4095.0) * V_REF;
    sendData_v1 = (samples[sendData_i].ch1 / 4095.0) * V_REF;
    Serial.print(samples[sendData_i].t);
    Serial.print(',');
    Serial.print(sendData_v0, 4);
    Serial.print(',');
    Serial.print(sendData_v1, 4);
    Serial.print(';');
  }
  Serial.println("END");
}

// ═══════════════════════════════════════════════════════════════
//  runOnePeriod()
//  One period of the signal, with its samples. The period ends when the
//  phase reaches 1, and since the phase only grows the loop always exits,
//  even if one iteration advances more than a whole period. The ten-second
//  timeout is the last safeguard so the Arduino never stops listening to
//  the serial port, whatever frame it has been sent.
// ═══════════════════════════════════════════════════════════════
#define PERIOD_TIMEOUT_US 10000000UL

int runOnePeriod_n0;  // sampleCount on entering the period ('O' signal only)

void runOnePeriod() {
  signalStart = micros();
  lastMicros = signalStart;
  phaseAccum = 0.0;
  tInPeriod = 0;
  DT = DTMax;          // first sample of the period right at its start
  periodSamples = 0;

  if (sigType == 'S' || sigType == 'T') {
    while (phaseAccum < 1.0) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
  } else if (sigType == 'P') {
    // For pulses the period is PER, no phases or anything
    while (micros() - signalStart < pul_per) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
  } else if (sigType == 'O') {
    // Here one period is one pulse, i.e. T2. Each one leaves exactly one
    // sample, the falling-edge one, so at the end of the train there are NCYC.
    runOnePeriod_n0 = sampleCount;
    while (micros() - signalStart < old_t2) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
    // Safety net. If the period ends with EC still high, the edge was never
    // seen and that pulse would be left without a sample.
    if (old_ecPrev && sampleCount == runOnePeriod_n0) {
      digitalWrite(PIN_EC, LOW);
      old_ecPrev = false;
      storeSample(0.0, micros() - old_lastSampleUs);
      old_lastSampleUs = micros();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════
void setup() {
  DAC_DDR = 0xFF;
  DAC_PORT = 0x00;
  pinMode(PIN_SM1, OUTPUT);
  pinMode(PIN_SM2, OUTPUT);
  digitalWrite(PIN_SM1, HIGH);
  digitalWrite(PIN_SM2, HIGH);  // at rest, forward direction
  pinMode(PIN_EC, OUTPUT);
  digitalWrite(PIN_EC, LOW);  // no current until a frame is received
  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  pinMode(ADC_CS, OUTPUT);
  digitalWrite(ADC_CS, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  Serial.begin(BAUD);
  lastMicros = micros();
  Serial.println("OK:READY");
}

void loop() {
  readSerial();

  if (sigType == ' ') return;

  // n_cycles consecutive periods, accumulating samples without sending anything
  sampleCount = 0;
  sampleTimeAccum = 0;
  // For the three DAC signals, EC is the enable and stays high for the whole
  // signal. Not in 'O', where EC IS the signal: forcing it high here would
  // leave it stuck.
  // SM2 does need to be set by hand, because the only one that writes it is
  // dacWrite() and it isn't called here. Otherwise it keeps whatever the
  // previous signal left and the train goes out with the current reversed
  // without warning.
  if (sigType == 'O') {
    DAC_PORT = 0x00;
    digitalWrite(PIN_SM2, HIGH);
    // EC low, so the first edge seen is the one from the first pulse
    digitalWrite(PIN_EC, LOW);
    old_ecPrev = false;
    old_lastSampleUs = micros();  // time origin of the train
  } else {
    digitalWrite(PIN_EC, HIGH);
    lastSampleUs = micros();  // time origin of the signal
  }
  for (int c = 0; c < n_cycles; c++) {
    runOnePeriod();
  }

  // All cycles are done. The DAC is switched off, the current is cut, and
  // only then is everything sent.
  DAC_PORT = 0x00;
  digitalWrite(PIN_EC, LOW);
  sendData();

  sigType = ' ';  // wait for the next frame
}
