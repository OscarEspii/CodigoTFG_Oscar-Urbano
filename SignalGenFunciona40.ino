// ═══════════════════════════════════════════════════════════════
//  Generador de señales para el banco de medida de memristores
//  Arduino Mega 2560 + escalera R-2R de 8 bits + TK042
//
//  La app web manda una trama de texto por el puerto serie a 2 Mbaud,
//  el Arduino genera la señal pedida, va leyendo el ADC mientras la
//  genera, y al acabar devuelve todas las muestras juntas.
//
//  Pines
//    22-29    escalera R-2R (Puerto A entero, se escribe de una vez)
//    32 SM1   memristor activo
//    33 SM2   sentido de la corriente
//    34 EC    habilita la corriente
//    35 M0    espejo de corriente, junto con M1
//    36 M1      00 = x1/8, 01 = x1/4, 10 = x1/2, 11 = x1
//    53 CS    chip select del MCP3304 (el resto del SPI, por hardware)
//
//  Puerto serie
//    2 000 000 baud, 8N1. Una trama por línea, terminada en '\n' (el '\r'
//    se ignora). Máximo 127 caracteres; lo que sobre se descarta sin aviso.
//
//  Arranque
//    Al encender (o al resetear, que abrir el puerto resetea el Mega) manda
//      OK:READY
//    Todo queda en reposo: DAC a 0, EC bajo (sin corriente), SM1 y SM2 altos.
//
//  Cómo se lanza una medida
//    No hay comando START aparte: la propia trama TYPE:... es la orden de
//    empezar. En cuanto se recibe y se valida, el Arduino genera NCYC ciclos
//    seguidos, manda el DATA:...END y vuelve a reposo esperando otra trama.
//    Es de un solo disparo: para repetir hay que volver a mandar la trama.
//    Mientras mide no lee el puerto serie, así que no se puede parar a medias
//    ni conviene mandar nada hasta recibir el END.
//
//  Tramas que entiende
//    TYPE:SIN,F:10.00,AMP:0.50,OFF:0.00,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:TRI,F:10.00,VMAX:1.00,VMIN:-1.00,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:PUL,VHI:1.00,VLO:0.00,DUR:500,PER:10,MEM:0,INT:11,NCYC:3,NPTS:170
//    TYPE:OLD,T1:2000,T2:10,MEM:0,INT:11,NCYC:10,NPTS:170
//
//    Campos CLAVE:valor separados por comas. Si falta un campo se reutiliza
//    el valor de la trama anterior. OJO: la búsqueda es por texto, y "F:"
//    también aparece dentro de "OFF:", así que en SIN el F tiene que ir
//    antes que el OFF (como en el ejemplo) o leerá el offset como frecuencia.
//
//    Comunes a todas
//      TYPE   SIN | TRI | PUL | OLD (solo cuenta la primera letra)
//      MEM    1 = SM1 alto, 0 = SM1 bajo (qué memristor está activo)
//      INT    dos dígitos M0 M1, el espejo de corriente (tabla de pines)
//      NCYC   número de ciclos (mín. 1). En OLD, número de pulsos (máx. 512)
//      NPTS   muestras por ciclo. NCYC x NPTS <= 512. En OLD no se usa,
//             pero se manda igual para no romper el formato
//    SIN    F    frecuencia en Hz (mín. 1, se toma el valor absoluto)
//           AMP  amplitud de pico en V
//           OFF  offset en V
//    TRI    F    frecuencia en Hz (mín. 1)
//           VMAX tensión máxima en V
//           VMIN tensión mínima en V
//    PUL    VHI  tensión del pulso en V
//           VLO  tensión de reposo en V
//           DUR  duración del pulso en us
//           PER  periodo en ms
//    OLD    T1   duración del pulso de EC en us (mín. 1, como mucho T2)
//           T2   periodo en ms (mín. 1)
//    Todas las tensiones se recortan a +-1 V (TK042_VMAX) al sacarlas.
//
//  Respuestas a una trama, en este orden
//    WARN:NPTS_CLIPPED_TO_MAX   NCYC x NPTS > 512, se ha bajado NPTS
//                               (en OLD, NCYC > 512 y se ha bajado NCYC)
//    WARN:FREQ_CLIPPED_TO_MIN   F < 1 Hz en SIN/TRI, se usa 1 Hz
//    WARN:VOLTAGE_CLIP_1V       la señal pasa de 1 V, se recortarán los picos
//    OK:SIN | OK:TRI | OK:PUL | OK:OLD   trama aceptada, empieza la medida
//    ERR:UNKNOWN_TYPE           TYPE no reconocido, no se mide nada
//    Los WARN son solo avisos: la medida arranca igual.
//
//  Datos, una sola vez al terminar todos los ciclos
//    DATA:t0,ch0,ch1;t1,ch0,ch1;...;END
//    t        us desde el inicio de la señal (acumulado entre ciclos)
//    ch0,ch1  tensión en V del MCP3304, canales 0 y 1, con 4 decimales.
//             Llevan signo negativo en los semiciclos negativos (ver
//             storeSample()). En OLD hay una muestra por pulso.
//    Cada ciclo tiene además un timeout de 10 s (PERIOD_TIMEOUT_US).
//
//  Modo test (TEST_MODE)
//    Descomentando "#define TEST_MODE" (más abajo) no se lee el MCP3304: cada
//    muestra se inventa a partir de la tensión pedida al DAC más ruido
//    aleatorio (ch1 al 75 % de ch0). Sirve para probar las tramas y la web
//    sin el chip. Se activa al compilar, no por trama, y viene desactivado.
//    No confundir con el modo test del propio integrado.
//
//  Cosas que se me suelen olvidar :)
//   · El buffer son 512 muestras. NCYC x NPTS no puede pasar de ahí; si se
//     pasa, se recorta NPTS y con aviso WARN:NPTS_CLIPPED_TO_MAX.
//   · El TK042 no aguanta más de 1.5 V en la entrada de control. La web ya lo
//     limita, pero una trama escrita a mano por la consola se salta ese
//     filtro, así que dacWrite() clippea por su cuenta a 1V (-0.5V por seguridad).
//   · OLD es el modo que se usaba en una version antigua del chip viejo.
//     En PUL_OLD EC no habilita nada, ES como el interruptor haciendo los pulsos.
//     Está explicado entero en el case 'O' de generateAndSample().
//   · El bucle tarda unos 350 us por vuelta y eso condiciona todo: cuántas
//     muestras se pueden pedir, lo corto que puede ser un pulso de EC y el
//     instante en que cae cada muestra.
//   · El osciloscopio de la web no tiene protocolo propio, manda tramas
//     normales pequeñas en bucle.
// ═══════════════════════════════════════════════════════════════

#include <math.h>
#include <SPI.h>

// ── Modo test ────────────────────────────────────────────────
// Este modo test no es el el modo test que hay en el integrado
// (lo sé, podria haber buscado otro nombre). Durante el desarollo
// del proyecto tenia que poder ir pobando las cosas entonces este
// TEST_MODE ejecutaba un modo "random" de datos para poder verificar
// si las tramas llegaban bien o no. Lo borraria, pero me ha servido mucho
// asi que se queda para el recuerdo.

// Comenta esta línea para usar el ADC real
// #define TEST_MODE

// ── Configuración hardware ───────────────────────────────────
#define DAC_PORT PORTA
#define DAC_DDR DDRA
#define PIN_SM1 32
#define PIN_SM2 33
#define PIN_EC 34
#define PIN_M0 35
#define PIN_M1 36
#define ADC_CS 53
#define BAUD 2000000

// ── Rango del DAC ────────────────────────────────────────────
#define DAC_MIN 0
#define DAC_MAX 255
#define V_REF 5.0

// ── Límite del TK042 ──────────────────────────────────────────
// Tope de la entrada de control. dacWrite() recorta aquí pase lo que
// pase, aunque la trama pida más.
// Lo dicho, esta en 1V por seguridad, poner 1.5V seria jugarsela
#define TK042_VMAX 1.0

// ── Adquisición ──────────────────────────────────────────────
// Muestras totales de una señal, sumando todos sus ciclos.
// 512 x 8 bytes = 4 KB, la mitad de la SRAM del Mega.
#define MAX_SAMPLES 512

// ═══════════════════════════════════════════════════════════════
//  VARIABLES GLOBALES
// ═══════════════════════════════════════════════════════════════
char sigType = ' ';

// Sinusoidal
float sin_freq = 1.0;
float sin_amp = 2.0;
float sin_off = 0.0;

// Triangular
float tri_freq = 1.0;
float tri_vmax = 1.0;
float tri_vmin = -1.0;

// Pulsos
float pul_vhi = 4.0;
float pul_vlo = 0.0;
unsigned long pul_dur = 500;
unsigned long pul_per = 10000;

// Pulsos por EC (chip antiguo)(Tambien funciona en el nuevo, lo de antiguo se ha quedado por el nombre). 
// Aquí no hay tensión de ningún tipo, el DAC se
// queda a cero y la señal es el propio PIN_EC conmutando. Los dos tiempos se
// guardan en us, igual que en pulsos, aunque T2 llegue en ms.
unsigned long old_t1 = 500;
unsigned long old_t2 = 10000;

// Estado anterior de EC, para pillar su flanco de bajada, que es donde se
// toma la muestra. Se reinicia en loop() al arrancar una señal 'O'.
bool old_ecPrev = false;

// micros() de la última muestra. Sirve para "datar"/"fechar" (no se como explicarlo)
// la siguiente, porque aquí
// el hueco entre muestras es un T2 entero y no una vuelta del bucle.
unsigned long old_lastSampleUs = 0;

// Config
int sel_mem = 1;
char cfg_int[3] = "11";
int n_cycles = 1;
int n_points = 512;  // Número de puntos por ciclo (llega como NPTS en la trama)

// Tiempo
float phaseAccum = 0.0;
unsigned long lastMicros = 0;
unsigned long signalStart = 0;
unsigned long sampleTimeAccum = 0;  // suma de los dt, hace de reloj de la señal
unsigned long DTMax = 0;            // hueco entre muestras (us) = periodo / n_points

// Buffer serie
char rxBuf[128];
int rxIdx = 0;

// Última lectura de cada canal, en cuentas del ADC y con signo (-4095..4095).
// El signo lo pone storeSample() en los semiciclos negativos.
int16_t adc_ch0 = 0;
int16_t adc_ch1 = 0;

// Aquí se van acumulando las muestras de todos los ciclos, y no se manda nada
// hasta el final. Se guardan en cuentas y no en voltios para que la rutina de
// muestreo sea lo más corta posible; la división la hace sendData(), una vez
// por muestra y ya sin prisa.
struct Sample {
  unsigned long t;
  int16_t ch0;
  int16_t ch1;
};
Sample samples[MAX_SAMPLES];
int sampleCount = 0;

// ═══════════════════════════════════════════════════════════════
//  dacWrite()
//  Saca un voltaje por la escalera. El signo va aparte, en SM2, porque la
//  escalera solo sabe hacer tensiones positivas.
//    SM2 alto  = sentido directo  (MEM1/MEM2 -> MEMC)
//    SM2 bajo  = sentido inverso  (MEMC -> MEM1/MEM2)
//
//  El +0.5 es necesario: Al convertir a uint8_t, C++ no
//  redondea sino que tira los decimales, así que 51.9 y 51.1 acababan los dos
//  en 51 y la escalera sacaba de media medio escalón de menos, unos 9.8 mV.
//  Sumando 0.5 antes de truncar, unas veces sobra y otras falta, y la media
//  se queda en cero. Importa bastante, porque la app calcula K con la
//  tensión que pidió y no con la que salió, y ese sesgo le bajaba la K un
//  1.8 %. Ahora se queda en el 0.03 %.
//
//  El byte no puede desbordar: con el recorte a 1 V el código más alto es 51.
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
// El Parser es como se le llama a la busqueda de datos de la trama
// Para guardar X dato en su variable correspondiente.
// (La verdad es que no tenia ni idea de que se llamaba asi, gracias Rodrigo supongo)
// ═══════════════════════════════════════════════════════════════
#define PARSE_FLOAT(frame, key, var) \
  { \
    const char* p = strstr(frame, key ":"); \
    if (p) var = atof(p + strlen(key) + 1); \
  }

#define PARSE_LONG(frame, key, var) \
  { \
    const char* p = strstr(frame, key ":"); \
    if (p) sscanf(p, key ":%ld", &var); \
  }

#define PARSE_STR(frame, key, var) \
  { \
    const char* p = strstr(frame, key ":"); \
    if (p) sscanf(p, key ":%2s", var); \
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
  // M0/M1 eligen el espejo de corriente. Solo se tocan al llegar una trama
  // nueva, nunca dentro del bucle de muestreo.
  digitalWrite(PIN_M0, cfg_int[0] == '1' ? HIGH : LOW);
  digitalWrite(PIN_M1, cfg_int[1] == '1' ? HIGH : LOW);

  PARSE_STR(frame, "TYPE", type);

  // NCYC y NPTS los llevan las cuatro señales, así que se leen y se validan
  // aquí una vez y no en cada case.
  PARSE_LONG(frame, "NCYC", cyc);
  n_cycles = (int)cyc;
  PARSE_LONG(frame, "NPTS", pts);
  n_points = (int)pts;

  if (n_cycles < 1) n_cycles = 1;  // si no, más abajo se divide entre cero
  if (type[0] == 'O') {
    // Aquí sale una muestra por pulso, así que lo que limita el buffer es
    // directamente el número de pulsos. NPTS se lee para no romper el formato
    // de la trama, pero en esta señal no se usa.
    if (n_cycles > MAX_SAMPLES) {
      n_cycles = MAX_SAMPLES;
      Serial.println("WARN:NPTS_CLIPPED_TO_MAX");
    }
  } else if ((long)n_cycles * (long)n_points > MAX_SAMPLES) {
    // Si no caben, en vez de rechazar la señal se recortan los puntos por
    // ciclo, se avisa y se arranca igual.
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
          sin_freq = 1.0;  // mínimo 1 Hz, como el deslizador de la web
          Serial.println("WARN:FREQ_CLIPPED_TO_MIN");
        }
        // si se pasa de 1 V, dacWrite() va a recortar los picos. Que se sepa.
        if (fabs(sin_off) + sin_amp > TK042_VMAX) {
          Serial.println("WARN:VOLTAGE_CLIP_1V");
        }
        // el periodo en us es 1e6/freq, y esto lo reparte entre los puntos
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
          tri_freq = 1.0;  // mínimo 1 Hz, como el deslizador de la web
          Serial.println("WARN:FREQ_CLIPPED_TO_MIN");
        }
        // igual que arriba, si se sale de 1 V se recorta
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
        // igual que arriba, si se sale de 1 V se recorta
        if (pul_vhi > TK042_VMAX || pul_vlo < -TK042_VMAX) {
          Serial.println("WARN:VOLTAGE_CLIP_1V");
        }
        // aquí el periodo ya está en us, no hay que convertirlo
        DTMax = pul_per / n_points;
        sigType = 'P';
        Serial.println("OK:PUL");
        break;
      }

    // Chip antiguo: la señal la hace EC, no el DAC. TYPE:OLD y no algo que
    // empiece por 'P' o 'S' porque el switch despacha por type[0] y esas
    // letras ya están cogidas por PUL y SIN. Asi que nos viene genial llamarle 
    // viejo jaja
    case 'O':
      {
        PARSE_LONG(frame, "T1", oldT1);
        PARSE_LONG(frame, "T2", oldT2);
        if (oldT1 < 1) oldT1 = 1;
        if (oldT2 < 1) oldT2 = 1;              // T2 mínimo 1 ms: un periodo de cero no da ni una iteración
        old_t1 = (unsigned long)oldT1;         // T1 llega en µs
        old_t2 = (unsigned long)oldT2 * 1000;  // T2 llega en ms
        if (old_t1 > old_t2) old_t1 = old_t2;  // el pulso no puede durar más que el periodo
        // No se leen tensiones: esta señal no las tiene. Tampoco hace falta el
        // aviso WARN:VOLTAGE_CLIP_1V, porque el DAC no se toca.
        // DTMax NO se usa en esta señal: el muestreo periódico está desactivado
        // y la muestra la dispara el flanco de bajada de EC (una por pulso). Se
        // pone a cero para dejar claro que no queda ningún valor heredado de la
        // señal anterior gobernando nada.
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
//  Escribe un punto de la señal en cada llamada, siempre. El ADC en cambio
//  solo se lee cada DTMax microsegundos, que es el hueco que hace falta para
//  que salgan n_points muestras repartidas por el periodo.
//
//  Que la escritura vaya en todas las vueltas y el muestreo no es lo que
//  permite subir de frecuencia sin estropear la forma de onda. Aunque se pierden
//  muestras, la señal que ve el chip sigue siendo la buena.
// ═══════════════════════════════════════════════════════════════
unsigned long DT = 0;
unsigned long now;
unsigned long dt;
float outVolts;
float range;
float phaseFrac;
unsigned long tInPeriod;
bool old_ecNow;  // nivel que le toca a EC en esta iteración (solo señal 'O')

void generateAndSample() {
  now = micros();
  dt = now - lastMicros;
  outVolts = 0.0;

  // lastMicros se actualiza siempre, sea cual sea el tipo de señal
  lastMicros = now;

  switch (sigType) {

      // phaseAccum solo crece, nunca se le resta 1 al pasarse. Para el voltaje
      // se usa su parte decimal y ya está. Restarle 1 es lo que colgaba el
      // Arduino a frecuencias altas, porque si una vuelta del bucle avanza más
      // de un periodo entero, restar 1 una vez no basta y runOnePeriod() se
      // queda esperando un fin de periodo que ya no llega.
      // No se ni porque, pero Rodrigo lo cambio y funciona asi que NO TOCAR

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

    // Cuenta el tiempo igual que 'P', pero lo que conmuta es EC y no hay
    // tensión que sacar. Una muestra por pulso, en el flanco de bajada, que
    // es lo que pide el fabricante. Como el bucle da una vuelta cada ~350 us,
    // la muestra cae en la vuelta siguiente al flanco, con ese retardo.
    case 'O':
      {
        tInPeriod += dt;
        if (tInPeriod >= old_t2) tInPeriod -= old_t2;
        old_ecNow = (tInPeriod < old_t1);
        digitalWrite(PIN_EC, old_ecNow ? HIGH : LOW);
        if (old_ecPrev && !old_ecNow) {
          // 0 V porque aquí no hay tensión, y el dt es el que ha corrido desde
          // la muestra anterior, un T2 entero, no el de esta vuelta del bucle.
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

  // En 'O' el DAC no se escribe. loop() lo ha dejado a cero antes de empezar
  // y ahí se queda toda la generación.
  if (sigType != 'O') dacWrite(outVolts);

  // El muestreo por DTMax vale para las tres señales del DAC. En 'O' está
  // apagado, que allí la muestra la dispara el flanco dentro del case.
  if (sigType != 'O') {
    DT += dt;
    if (DT >= DTMax) {
      storeSample(outVolts, dt);
      DT -= DTMax;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  MCP3304
//  Tres bytes por lectura, alineados según la Figura 6-4 del DS21697F.
//  Ojo, la trama del MCP3208 aquí no vale: este devuelve un bit más y su
//  ventana de datos empieza antes, así que con la del 3208 sale la mitad.
//
//   ciclo  1 2 3 4   5      6     7  8 │ 9 10  11    12   13..16 │ 17..24
//   MOSI   0 0 0 0 START SGL/DIFF D2 D1│D0  X   X     X     X    │   X
//   MISO   ? ? ? ?   ?      ?     ?  ? │ ?  ?  NULL   SB  B11..B8│ B7..B0
// ═══════════════════════════════════════════════════════════════
uint8_t mcp3304Read_b0;
uint8_t mcp3304Read_b1;
uint8_t mcp3304Read_hi;
uint8_t mcp3304Read_lo;
uint16_t mcp3304Read_raw;

int mcp3304Read(uint8_t channel) {
  // 0x0C = 0000 1100, o sea los ceros, el start bit y el modo single-ended
  mcp3304Read_b0 = 0x0C;
  // el bit de canal va en el bit 7 del segundo byte
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
//  Guarda la lectura en cuentas, sin pasarla a voltios, para que esto sea lo
//  más rápido posible. Ya lo divide sendData() al final.
//
//  V01 y V02 son pines fijos del chip, así que invertir la corriente no
//  cambia qué nodo ve cada canal, solo cuál queda más alto. De eso se encarga
//  el signo, y cada canal sale siempre de su propia lectura.
// ═══════════════════════════════════════════════════════════════
int storeSample_noise0;
int storeSample_noise1;
int16_t storeSample_dacRaw;
int16_t storeSample_raw0;
int16_t storeSample_raw1;
bool storeSample_neg;

void storeSample(float dacVolts, unsigned long dt) {
  // acumulado, para que la marca de tiempo sea desde el inicio de la señal
  sampleTimeAccum += dt;

  storeSample_neg = (dacVolts < 0);

#ifdef TEST_MODE
  // Simulamos un valor RAW de ADC (0-4095) a partir del voltaje del DAC,
  // con ruido entero para que los ciclos se vean distintos entre sí.
  // (fabs: el nodo físico siempre ve la magnitud, el signo va aparte)
  // Lo dicho, lo dejamos aqui, pero era algo hecho por puro debuggeo
  storeSample_dacRaw = (int16_t)((fabs(dacVolts) / V_REF) * 4095.0);
  storeSample_noise0 = random(-250, 250);
  storeSample_noise1 = random(-330, 330);
  storeSample_raw0 = constrain((int)storeSample_dacRaw + storeSample_noise0, 0, 4095);
  storeSample_raw1 = constrain((int)(storeSample_dacRaw * 0.75) + storeSample_noise1, 0, 4095);
#else
  storeSample_raw0 = mcp3304Read(0);
  storeSample_raw1 = mcp3304Read(1);
#endif

  // cada canal, su lectura. Lo único que cambia es el signo.
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
//  Suelta todas las muestras de golpe, DATA:t,ch0,ch1;...;END
//  Aquí se pasa de cuentas a voltios, que a estas alturas ya no corre prisa.
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
//  Un periodo de la señal, con sus muestras. El periodo se acaba cuando la
//  fase llega a 1, y como la fase solo crece el bucle sale siempre, aunque
//  una vuelta avance más de un periodo entero. El timeout de diez segundos
//  es el último cortafuegos para que el Arduino no se quede sordo al puerto
//  serie pase lo que pase con la trama que le hayan mandado.
// ═══════════════════════════════════════════════════════════════
#define PERIOD_TIMEOUT_US 10000000UL

int runOnePeriod_n0;  // sampleCount al entrar en el periodo (solo señal 'O')

void runOnePeriod() {
  signalStart = micros();
  lastMicros = signalStart;
  phaseAccum = 0.0;
  tInPeriod = 0;

  if (sigType == 'S' || sigType == 'T') {
    while (phaseAccum < 1.0) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
  } else if (sigType == 'P') {
    // En pulsos el periodo es PER, sin fases ni nada
    while (micros() - signalStart < pul_per) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
  } else if (sigType == 'O') {
    // Aquí un periodo es un pulso, o sea T2. Cada uno deja exactamente una
    // muestra, la del flanco de bajada, así que al final del tren hay NCYC.
    runOnePeriod_n0 = sampleCount;
    while (micros() - signalStart < old_t2) {
      generateAndSample();
      if (micros() - signalStart > PERIOD_TIMEOUT_US) break;
    }
    // Red de seguridad. Si el periodo se acaba con EC todavía alto, el flanco
    // no se ha llegado a ver y ese pulso se quedaría sin muestra.
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
  digitalWrite(PIN_SM2, HIGH);  // en reposo, sentido directo
  pinMode(PIN_EC, OUTPUT);
  digitalWrite(PIN_EC, LOW);  // sin corriente hasta recibir una trama
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

  // n_cycles periodos seguidos, acumulando muestras y sin mandar nada
  sampleCount = 0;
  sampleTimeAccum = 0;
  // En las tres del DAC, EC es la habilitación y va alto toda la señal. En 'O'
  // no, que ahí EC ES la señal: forzarlo alto aquí lo dejaría clavado.
  // SM2 sí hay que fijarlo a mano, porque el único que lo escribe es dacWrite()
  // y aquí no se llama. Si no, se queda con lo que dejara la señal anterior y
  // el tren sale con la corriente al revés sin avisar.
  if (sigType == 'O') {
    DAC_PORT = 0x00;
    digitalWrite(PIN_SM2, HIGH);
    // EC en bajo, para que el primer flanco que se vea sea el del primer pulso
    digitalWrite(PIN_EC, LOW);
    old_ecPrev = false;
    old_lastSampleUs = micros();  // origen de tiempos del tren
  } else digitalWrite(PIN_EC, HIGH);
  for (int c = 0; c < n_cycles; c++) {
    runOnePeriod();
  }

  // Ya están todos los ciclos. Se apaga el DAC, se corta la corriente y solo
  // entonces se manda todo.
  DAC_PORT = 0x00;
  digitalWrite(PIN_EC, LOW);
  sendData();

  sigType = ' ';  // a esperar la siguiente trama
}
