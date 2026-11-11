# Memristor characterisation tool

A signal generator and acquisition system for characterising memristors
with the TK042 chip from IC-Malaga. The TK042 is a memristor measurement
system that drives a given current through the memristor, set by a control
voltage. This project adds the electronics around it. An Arduino Mega 2560
produces the control voltage through an 8-bit R-2R digital-to-analog
converter (DAC), and reads the memristor's two terminal voltages with an
MCP3304 analog-to-digital converter (ADC). A web interface running in the
browser configures each measurement, talks to the Arduino over USB serial,
and plots and exports the results.

Developed as part of Oscar Urbano's bachelor's thesis (TFG).

## Repository contents

| File | Description |
| --- | --- |
| `SignalGenFunciona40.ino` | Arduino Mega firmware: signal generation, sampling and the serial protocol |
| `signalgen25.html` | Web interface (a single self-contained HTML file; the UI is in Spanish) |

## Hardware

The system has three parts around an Arduino Mega 2560: the TK042, a DAC
that sets its control voltage, and an ADC that reads back the memristor's
voltages.

### TK042 (IC-Malaga)

The TK042 drives a current through the selected memristor. The current is
proportional to the control voltage coming from the DAC: I = K · Vctrl. The
web interface measures K for each current mirror range during calibration.
The Arduino sets the chip's digital inputs directly:

| Arduino pin | TK042 signal | Function |
| --- | --- | --- |
| 32 | SM1 | Active memristor |
| 33 | SM2 | Current direction (high = forward, MEM1/MEM2 → MEMC; low = reverse) |
| 34 | EC | Current enable (in OLD mode, the pulse signal itself) |
| 35, 36 | M0, M1 | Current mirror range: `00` = x1/8, `01` = x1/4, `10` = x1/2, `11` = x1 |

The TK042 control input must not exceed 1.5 V. The firmware clips every
output voltage to ±1 V as a safety margin, whatever the frame asks for.

### DAC: 8-bit R-2R ladder

The control voltage comes from an 8-bit R-2R resistor ladder on Arduino
pins 22–29. These eight pins are port A of the ATmega2560, with bit 0
(least significant) on pin 22 and bit 7 (most significant) on pin 29:

| Arduino pin | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Port A bit | PA0 (LSB) | PA1 | PA2 | PA3 | PA4 | PA5 | PA6 | PA7 (MSB) |

The firmware treats the eight pins as one 8-bit word, not as eight separate
bits. Instead of calling `digitalWrite()` once per pin, it writes the whole
byte to the port register in a single instruction (`PORTA = value`, see
`dacWrite()`), so all eight pins change at the same instant. Setting the
bits one at a time would be slower, and the ladder would briefly output
wrong intermediate codes while some bits had changed and others had not.
Going from 127 (`01111111`) to 128 (`10000000`), for example, changes
every bit, and the output could pass through 0 or 255 on the way.

With a 5 V reference, each step is 5 V / 255 ≈ 19.6 mV, so the ±1 V limit
uses codes 0 to 51. The ladder only produces positive voltages. The sign
of the signal is set separately with SM2, which reverses the current
direction.

### ADC: MCP3304

The memristor's voltages are read with a Microchip MCP3304. It is a single
chip with 8 inputs, used here in single-ended mode on two channels: CH0
reads node V01 of the TK042 and CH1 reads node V02. Every sample reads
both channels, one after the other. The MCP3304 is connected to the Mega's
hardware SPI bus (1 MHz, SPI mode 0):

| MCP3304 pin | Function | Connection |
| --- | --- | --- |
| 1 | CH0 | TK042 V01 |
| 2 | CH1 | TK042 V02 |
| 3–8 | CH2–CH7 | Unused |
| 9 | DGND | Ground |
| 10 | CS/SHDN | Arduino pin 53 |
| 11 | DIN | Arduino pin 51 (MOSI) |
| 12 | DOUT | Arduino pin 50 (MISO) |
| 13 | CLK | Arduino pin 52 (SCK) |
| 14 | AGND | Ground |
| 15 | VREF | 5 V (the firmware assumes `V_REF = 5.0`) |
| 16 | VDD | 5 V |

Pin numbers are for the 16-pin PDIP/SOIC package, from the Microchip
datasheet DS21697 (the firmware's read routine follows its Figure 6-4).
Readings are 12-bit (0–4095). The firmware adds the sign from SM2 and
converts them to volts before sending them.

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
from disk. On Linux, check the [Linux setup](#linux-setup) section first.

## Linux setup

The web interface works on Linux, but the computer needs a few things set
up before the browser can open the Arduino's serial port. Go through these
in order.

1. **Serial port permission.** Only members of the `dialout` group can
   open serial ports. Check with `groups`. If `dialout` is not listed, run
   the following, then log out and back in (or reboot):

   ```
   sudo usermod -aG dialout $USER
   ```

   Without this, Chrome shows an error such as "Failed to open serial
   port" after you pick the port.

2. **The browser.** Use Google Chrome, Chromium or Microsoft Edge; Firefox
   does not support Web Serial. Browsers installed as Snap packages (such
   as Ubuntu's default Chromium) usually cannot access serial ports: the
   port list stays empty or opening fails. Install Google Chrome from its
   official `.deb` package instead.

3. **ModemManager.** It probes new `/dev/ttyACM*` devices as if they were
   modems, which can block the port for a few seconds or send stray bytes
   to the Arduino. If the computer does not use a mobile modem, disable it:

   ```
   sudo systemctl disable --now ModemManager
   ```

4. **brltty (clone boards only).** Some clone Megas use a CH340 USB-serial
   chip and appear as `/dev/ttyUSB0` instead of `/dev/ttyACM0`. On recent
   Ubuntu versions, the `brltty` braille service grabs these chips and the
   port disappears right after plugging the board in. If no braille
   display is used, remove it and replug the board:

   ```
   sudo apt remove brltty
   ```

5. **Port already in use.** Only one program can open the port at a time.
   Close the Arduino IDE's Serial Monitor, or any other serial terminal,
   before pressing **Connect**.

### Diagnosing a connection problem

How the page fails tells you where to look:

- **The port list is empty:** the browser (item 2) or brltty (item 4).
- **The port is listed but opening fails:** permissions (item 1) or
  another program holding the port (items 3 and 5).
- **It connects but no data arrives:** the board is not running this
  project's firmware, or it is not an Arduino Mega 2560.

With the board plugged in, this script prints everything needed to tell
these cases apart:

```bash
echo "groups: $(id -nG)"; ls -l /dev/ttyACM* /dev/ttyUSB* 2>&1
for b in google-chrome chromium chromium-browser microsoft-edge; do p=$(command -v $b) && echo "$b -> $(readlink -f $p)"; done
snap list 2>/dev/null | grep -iE "chrom|edge"
systemctl is-active ModemManager brltty 2>&1
lsusb | grep -iE "2341|2a03|1a86|0403|10c4"
sudo fuser -v /dev/ttyACM* /dev/ttyUSB* 2>&1
```

What to look for in its output:

- `dialout` is in the groups;
- the port exists and belongs to the `dialout` group;
- the browser path is not under `/snap`;
- ModemManager and brltty are `inactive`;
- the board appears in `lsusb` (`2341:0042` is an Arduino Mega 2560 R3);
- no other process holds the port.

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

## License

© 2026 Oscar Urbano and contributors.

This project is licensed under the
[Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
See [`LICENSE`](LICENSE) for the full text. You may use, modify and
distribute it for any purpose, including commercially. If you redistribute
it or a modified version, you must include the license, keep the copyright
and attribution notices, state the changes you made, and include the
attribution in the [`NOTICE`](NOTICE) file.
