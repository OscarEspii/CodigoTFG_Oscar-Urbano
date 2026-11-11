# Memristor characterisation tool

A signal generator and acquisition system for characterising memristors with
the TK042 chip. An Arduino Mega 2560 generates the test signal through an
8-bit R-2R ladder and samples the response with an MCP3304 ADC. A web
interface running in the browser configures each measurement, talks to the
Arduino over USB serial, and plots and exports the results.

Developed as part of Oscar Urbano's bachelor's thesis (TFG).

## Repository contents

| File | Description |
| --- | --- |
| `SignalGenFunciona40.ino` | Arduino Mega firmware: signal generation, sampling and the serial protocol |
| `signalgen25.html` | Web interface (a single self-contained HTML file; the UI is in Spanish) |

## Hardware

- Arduino Mega 2560
- 8-bit R-2R ladder on pins 22–29 (port A) as the DAC
- MCP3304 ADC on the hardware SPI bus, chip select on pin 53
- TK042 memristor chip, controlled through:

| Pin | Signal | Function |
| --- | --- | --- |
| 32 | SM1 | Active memristor |
| 33 | SM2 | Current direction |
| 34 | EC | Current enable (in OLD mode, the pulse signal itself) |
| 35, 36 | M0, M1 | Current mirror range: `00` = x1/8, `01` = x1/4, `10` = x1/2, `11` = x1 |

The TK042 control input must not exceed 1.5 V. The firmware clips every
output voltage to ±1 V as a safety margin, whatever the frame asks for.

## Getting started

### 1. Flash the firmware

1. Open `SignalGenFunciona40.ino` in the Arduino IDE (1.8.x or 2.x). The IDE
   will ask to move it into a folder with the same name; accept.
2. Select **Tools → Board → Arduino Mega or Mega 2560** and the right port.
3. Upload. No external libraries are needed (only the built-in `SPI`).

### 2. Open the web interface

1. Open `signalgen25.html` in **Chrome or Edge** on a desktop computer. It
   uses the [Web Serial API](https://caniuse.com/web-serial), which Firefox
   and Safari do not support.
2. Click **Conectar** and choose the Arduino's serial port. The connection
   runs at 2,000,000 baud.
3. Choose a signal in one of the tabs, set its parameters and start the
   measurement.

No server or installation is needed; the page works when opened directly
from disk.

## Web interface features

- **Signal tabs:** Sinusoidal, Triangular, Pulsos (voltage pulses) and
  Pulsos EC (OLD mode, where the EC pin itself is pulsed and the DAC stays
  at 0 V).
- **Config:** memristor selection, current direction, current mirror range
  and the K calibration of each range. To calibrate, replace the device
  under test with a known resistor and press **Calibrar**. The calibration
  is stored in the browser.
- **Results:** plots of the acquired channels per cycle, and export to
  `.xlsx`.
- **Console:** sends raw frames such as `TYPE:SIN,F:100,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:1,NPTS:170`
  directly to the Arduino. Other commands: `help`, `status`, `clear`,
  `light`, `dark`, `play`, `pause`.
- **Debug tab:** hidden by default; type `debug` in the console to show
  it. It contains a live oscilloscope of CH0 and a THD-vs-frequency sweep
  (5 Hz to 5 kHz).

## Serial protocol

The protocol is fully documented in the header of `SignalGenFunciona40.ino`.
In short:

- There is no separate start command. Each frame is one text line ending
  in `\n`, and the frame itself starts a one-shot measurement:

  ```
  TYPE:SIN,F:10.00,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:3,NPTS:170
  TYPE:TRI,F:10.00,VMAX:1.00,VMIN:-1.00,MEM:0,INT:11,NCYC:3,NPTS:170
  TYPE:PUL,VHI:1.00,VLO:0.00,DUR:500,PER:10,MEM:0,INT:11,NCYC:3,NPTS:170
  TYPE:OLD,T1:2000,T2:10,MEM:0,INT:11,NCYC:10,NPTS:170
  ```

- Fields can be given in any order. A missing field keeps its value from
  the previous frame.
- Units: frequencies in Hz, voltages in V, `DUR` and `T1` in µs, `PER` and
  `T2` in ms.
- The Arduino replies with optional `WARN:...` lines, then `OK:<TYPE>` (or
  `ERR:UNKNOWN_TYPE`). When all cycles are done, it sends every sample in
  one line:

  ```
  DATA:t0,ch0,ch1;t1,ch0,ch1;...;END
  ```

  `t` is in µs from the start of the signal, and `ch0` and `ch1` are in
  volts, signed.
- The buffer holds 512 samples, so `NCYC × NPTS` must not exceed 512.
  Otherwise `NPTS` is reduced and a warning is sent.

## Test mode

Uncomment `#define TEST_MODE` in the firmware to replace the ADC readings
with simulated data: the requested DAC voltage plus random noise. This is
useful for testing the protocol and the web interface without the chip
connected. It is off by default.

## Authors

- **Oscar Urbano** ([@OscarEspii](https://github.com/OscarEspii)), author
- **Rodrigo Picos Gaya** ([@rpicos-uib](https://github.com/rpicos-uib)), contributor
