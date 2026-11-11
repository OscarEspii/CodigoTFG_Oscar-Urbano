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
   the following, then **reboot** the computer:

   ```
   sudo usermod -aG dialout $USER
   ```

   Logging out and back in is often not enough: many desktops keep parts
   of the session running, so the new group is only picked up after a
   reboot. Run `groups` afterwards to confirm `dialout` is listed. Adding a
   user to `dialout` needs an administrator account, but the user does not
   need administrator rights to use the port afterwards.

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

- **Signal tabs:** Sine, Triangle, Pulses (voltage pulses) and EC Pulses
  (`OLD` mode, where the EC pin itself is pulsed and the DAC stays at 0 V).
- **Config:** memristor selection, current mirror range and the K
  calibration of each range. To calibrate, replace the device under test
  with a known resistor and press **Calibrate**. The calibration is stored
  in the browser.
- **Commands:** a reference of every frame the Arduino accepts, its
  replies, the `DATA` format and the console commands. Each example has a
  **Use** button that copies it to the console. See [Commands](#commands).
- **Results:** plots of the acquired channels per cycle, and export to
  `.xlsx`.
- **Console:** sends raw frames such as `TYPE:SIN,F:100,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:1,NPTS:170`
  directly to the Arduino, plus a few commands handled by the page (see
  [Web console commands](#web-console-commands)).
- **Debug tab:** hidden by default; type `debug` in the console to show
  it. It contains a live oscilloscope of CH0 and a THD-vs-frequency sweep
  (5 Hz to 5 kHz).

## Commands

The web page is only a front end. Everything it does, it does by sending
the text frames below over the serial port and reading the replies. The
bench can be driven the same way from a serial terminal, MATLAB, Python or
any other program that can open a serial port (see
[Using the bench without the web page](#using-the-bench-without-the-web-page)).
The **Commands** tab in the web page shows the same reference.

### Serial settings

| Setting | Value |
| --- | --- |
| Baud rate | 2,000,000 |
| Format | 8 data bits, no parity, 1 stop bit (8N1), no flow control |
| Encoding | Plain ASCII text |
| Frames sent to the Arduino | One per line, ending in `\n` (LF). A `\r` before it is ignored. 127 characters at most; anything longer is cut off. |
| Replies from the Arduino | One per line, ending in `\r\n` (CR LF) |

Opening the port resets the Arduino (as with any Arduino Mega). It sends
`OK:READY` about 2 seconds later, when it is ready to receive frames.

### How a measurement works

1. Send one `TYPE:` frame. There is no separate start command: the frame
   itself starts the measurement.
2. The Arduino replies with zero or more `WARN:` lines, then `OK:<TYPE>`,
   and starts generating the signal straight away. If the type is not
   recognised it replies `ERR:UNKNOWN_TYPE` and measures nothing.
3. It generates `NCYC` cycles back to back, sampling both ADC channels as
   it goes. It does not read the serial port during this time, so do not
   send anything until step 4.
4. It sends all the samples in a single `DATA:` line ending in `END`, then
   waits for the next frame.

### Frame format

```
TYPE:SIN,F:10.00,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:3,NPTS:170
```

- `KEY:value` fields separated by commas, with no spaces. Fields can go in
  any order.
- A field that is left out keeps its value from the previous frame.
- Decimal numbers use a point (`0.50`), not a comma.
- Value types, as the firmware reads them:
  - **float**: a 32-bit decimal number (`-0.5`, `10`, `10.00`);
  - **integer**: a 32-bit signed whole number (`500`);
  - **text**: letters or digits.

#### Fields common to all signals

| Field | Type | Unit | Valid values | Meaning |
| --- | --- | --- | --- | --- |
| `TYPE` | text | — | `SIN`, `TRI`, `PUL`, `OLD` | Signal type. Only the first letter is checked. |
| `MEM` | integer | — | `0` or `1` | Memristor: `0` = memristor 1 (SM1 low), `1` = memristor 2 (SM1 high). |
| `INT` | text, 2 digits | — | `00`, `01`, `10`, `11` | Current mirror range. First digit M0, second M1: `00` = ×1/8, `01` = ×1/4, `10` = ×1/2, `11` = ×1. |
| `NCYC` | integer | cycles | ≥ 1 | Number of cycles. In `OLD`, the number of pulses (max. 512). |
| `NPTS` | integer | samples per cycle | 1 to 512 / `NCYC` | Samples per cycle. If `NCYC × NPTS` > 512, `NPTS` is reduced; below 1, 1 is used. Not used in `OLD`, but send it anyway. |

`MEM` and `INT` are applied as soon as the frame is read, even if `TYPE`
turns out to be unknown.

#### `SIN`: sine

| Field | Type | Unit | Valid values | Meaning |
| --- | --- | --- | --- | --- |
| `F` | float | Hz | ≥ 1 | Frequency. Below 1 Hz, 1 Hz is used. The sign is ignored. |
| `AMP` | float | V | \|`OFF`\| + `AMP` ≤ 1 | Peak amplitude. |
| `OFF` | float | V | −1 to 1 | Offset. The output is `OFF + AMP · sin(2π · F · t)`. |

```
TYPE:SIN,F:10.00,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:3,NPTS:170
```

#### `TRI`: triangle

| Field | Type | Unit | Valid values | Meaning |
| --- | --- | --- | --- | --- |
| `F` | float | Hz | ≥ 1 | Frequency. Below 1 Hz, 1 Hz is used. |
| `VMAX` | float | V | −1 to 1 | Maximum voltage, reached at half period. |
| `VMIN` | float | V | −1 to 1 | Minimum voltage, at the start and end of each period. |

```
TYPE:TRI,F:10.00,VMAX:0.50,VMIN:-0.50,MEM:0,INT:11,NCYC:3,NPTS:170
```

#### `PUL`: voltage pulses

| Field | Type | Unit | Valid values | Meaning |
| --- | --- | --- | --- | --- |
| `VHI` | float | V | −1 to 1 | Pulse voltage, during the first `DUR` µs of each period. |
| `VLO` | float | V | −1 to 1 | Rest voltage, for the rest of the period. |
| `DUR` | integer | µs | ≥ 1 | Pulse duration. |
| `PER` | integer | ms | ≥ 1 | Period. Note the unit: milliseconds, while `DUR` is in microseconds. |

```
TYPE:PUL,VHI:0.50,VLO:0.00,DUR:5000,PER:10,MEM:0,INT:11,NCYC:3,NPTS:170
```

#### `OLD`: EC pulses

The DAC stays at 0 V and the TK042's EC pin itself is switched on and off
to make the pulses. One sample is taken per pulse, at the falling edge of
EC.

| Field | Type | Unit | Valid values | Meaning |
| --- | --- | --- | --- | --- |
| `T1` | integer | µs | 1 to `T2` | Time EC stays high in each period. Below about 2000 µs the real pulse is noticeably shorter than requested, because the firmware loop takes about 350 µs. |
| `T2` | integer | ms | ≥ 1 | Period. |

```
TYPE:OLD,T1:5000,T2:10,MEM:0,INT:11,NCYC:10,NPTS:170
```

All output voltages are clipped to ±1 V by the firmware, whatever the
frame asks for, to protect the TK042's control input.

### Replies

| Reply | When it is sent |
| --- | --- |
| `OK:READY` | At power-up or reset, including when the port is opened. |
| `WARN:NPTS_CLIPPED_TO_MAX` | `NCYC × NPTS` > 512, so `NPTS` was reduced. In `OLD`, `NCYC` > 512 and `NCYC` was reduced. |
| `WARN:NPTS_CLIPPED_TO_MIN` | `NPTS` < 1 in `SIN`, `TRI` or `PUL`; 1 is used. |
| `WARN:FREQ_CLIPPED_TO_MIN` | `F` < 1 Hz in `SIN` or `TRI`; 1 Hz is used. |
| `WARN:VOLTAGE_CLIP_1V` | The requested signal goes beyond ±1 V; its peaks will be clipped. |
| `OK:SIN`, `OK:TRI`, `OK:PUL`, `OK:OLD` | The frame was accepted and the measurement starts now. Any warnings come before this line. |
| `ERR:UNKNOWN_TYPE` | `TYPE` was not recognised. Nothing is measured. |
| `DATA:...;END` | The results, once all cycles are done. |

The `WARN:` lines are only warnings: the measurement still runs.

### The `DATA` line

```
DATA:t,ch0,ch1;t,ch0,ch1;...;END
```

For example:

```
DATA:1012,0.1245,0.0610;2004,0.2490,0.1221;...;END
```

`DATA:` is followed by one group per sample, each ending in `;`, and the
line ends with `END`. Each group has three comma-separated values:

| Value | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `t` | unsigned 32-bit integer, decimal | µs | Time since the start of the signal, counted across all cycles. |
| `ch0` | decimal number, 4 decimals | V | MCP3304 channel 0: node V01 of the TK042. |
| `ch1` | decimal number, 4 decimals | V | MCP3304 channel 1: node V02 of the TK042. |

- `ch0` and `ch1` come from 12-bit ADC readings (0 to 4095 counts),
  converted as `counts × 5 / 4095`. One count is about 1.22 mV, and the
  range is −5 to +5 V.
- They are negative while the requested voltage is negative, that is,
  while the current is reversed.
- There are at most 512 samples per line, so the line can be about 14 KB
  long.
- In `OLD` there is one sample per pulse.
- With fast signals there may be fewer samples per cycle than `NPTS`,
  because the firmware loop takes about 350 µs. The `t` values are always
  the real sample times.
- The Arduino sends all cycles together. To split them, use the number of
  samples per cycle, or the signal period and `t`.

### Using the bench without the web page

Any program that follows the steps above can run measurements.

**Serial terminal.** With `picocom` on Linux:

```
picocom -b 2000000 --omap crlf --echo /dev/ttyACM0
```

`--omap crlf` makes the Enter key send the `\n` the firmware expects, and
`--echo` shows what you type. Type a frame and press Enter. To exit, press
Ctrl-A then Ctrl-X. In the Arduino IDE's Serial Monitor, set the speed to
2000000 baud and the line ending to "Newline".

**MATLAB** (R2019b or later):

```matlab
s = serialport("/dev/ttyACM0", 2000000);   % "COM3" or similar on Windows
configureTerminator(s, "CR/LF", "LF");     % replies end in \r\n, frames in \n
s.Timeout = 30;                            % seconds, longer than the slowest measurement
pause(2);                                  % opening the port resets the Arduino
disp(readline(s))                          % OK:READY

writeline(s, "TYPE:SIN,F:10,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:2,NPTS:50");
line = readline(s);
while ~startsWith(line, "DATA:")
    disp(line)                             % WARN:... lines and OK:SIN
    line = readline(s);
end
body = extractBetween(line, "DATA:", ";END");
vals = str2double(split(split(body, ";"), ","));   % N x 3 matrix
t_us = vals(:,1); ch0 = vals(:,2); ch1 = vals(:,3);
plot(t_us/1000, ch0, t_us/1000, ch1);
xlabel("t (ms)"); ylabel("V"); legend("CH0 (V01)", "CH1 (V02)");
```

**Python** (with `pyserial`):

```python
import serial, time

s = serial.Serial("/dev/ttyACM0", 2000000, timeout=30)   # "COM3" or similar on Windows
time.sleep(2)                                            # opening the port resets the Arduino
print(s.readline().decode().strip())                     # OK:READY

s.write(b"TYPE:SIN,F:10,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:2,NPTS:50\n")
while True:
    line = s.readline().decode().strip()
    if line.startswith("DATA:"):
        break
    print(line)                                          # WARN:... lines and OK:SIN

samples = [tuple(map(float, p.split(","))) for p in line[5:].split(";") if p and p != "END"]
# samples = [(t_us, ch0_V, ch1_V), ...]
```

### Web console commands

The console under the log in the web page also accepts these commands.
The page handles them itself; they are not sent to the Arduino. Any line
starting with `TYPE:` is sent to the Arduino as it is, with decimal commas
(`0,5`) changed to points.

| Command | What it does |
| --- | --- |
| `help` | Lists the console commands. |
| `status` | Shows whether the page is connected. |
| `clear` | Clears the log. |
| `light`, `dark` | Switches between light and dark mode. |
| `play`, `pause` | Starts or stops the signal preview animation. |
| `debug` | Shows the hidden Debug tab (oscilloscope and THD sweep). |

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
