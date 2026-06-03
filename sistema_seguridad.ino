/**
 * @file sistema_seguridad_doxygen.ino
 * @brief Sistema de seguridad embebido con FreeRTOS para Arduino Mega.
 *
 * @details
 * Sistema de control de acceso y monitoreo ambiental basado en FreeRTOS.
 * Integra los siguientes módulos de hardware:
 * * Teclado matricial 4×4 para ingreso de clave numérica.
 * * Pantalla LCD 16×2 para retroalimentación visual al usuario.
 * * Servo-motor como mecanismo de cerradura electrónica.
 * * Lector RFID MFRC522 para autenticación por tarjeta.
 * * Sensor KY-013 (NTC) para temperatura ambiente.
 * * Sensor KY-018 (LDR) para nivel de iluminación.
 * * Sensor KY-037 (micrófono) para detección de ruido.
 * * Sensor KY-035 (efecto Hall) para detección de movimiento.
 *
 * La lógica se modela como una Máquina de Estados Finita (FSM).
 * Todas las esperas utilizan ticks de FreeRTOS; no se usa `millis()` ni
 * `delay()` fuera de contextos de tarea.
 *
 * @dot
 * digraph FSM {
 *   rankdir=LR;
 *   node [shape=rectangle, style=filled, fillcolor=lightblue, fontname="Helvetica"];
 *
 *   INICIO           [label="EST_INICIO\nEspera clave/RFID"];
 *   ABRIENDO         [label="EST_ABRIENDO\nServo 90° (5 s)"];
 *   CONFIG           [label="EST_CONFIG\nCambio clave/RFID"];
 *   MON_AMB          [label="EST_MONITOR_AMBIENTAL\nTemp + Luz (5 s)"];
 *   MON_INT          [label="EST_MONITOR_INTRUSOS\nMovimiento+Ruido (2 s)"];
 *   ALARMA           [label="EST_ALARMA\nBuzzer + LED (2 s)", fillcolor=orange];
 *   BLOQUEO          [label="EST_BLOQUEO\nSistema bloqueado", fillcolor=red, fontcolor=white];
 *
 *   INICIO   -> ABRIENDO  [label="Clave OK / RFID OK"];
 *   INICIO   -> BLOQUEO   [label="3 intentos fallidos"];
 *   ABRIENDO -> MON_AMB   [label="5 s transcurridos"];
 *   MON_AMB  -> MON_INT   [label="5 s transcurridos"];
 *   MON_INT  -> MON_AMB   [label="2 s transcurridos"];
 *   MON_AMB  -> ALARMA    [label="Temp < 20°C / Luz baja"];
 *   MON_INT  -> ALARMA    [label="Ruido / Movimiento / Puerta"];
 *   MON_AMB  -> CONFIG    [label="Tecla *"];
 *   MON_INT  -> CONFIG    [label="Tecla #"];
 *   ALARMA   -> MON_AMB   [label="2 s (< 3 alarmas consec.)"];
 *   ALARMA   -> BLOQUEO   [label="3 alarmas consecutivas"];
 *   BLOQUEO  -> INICIO    [label="Botón físico"];
 *   BLOQUEO  -> CONFIG    [label="Clave admin OK"];
 *   CONFIG   -> MON_AMB   [label="Tecla *"];
 * }
 * @enddot
 *
 * @author  Equipo de desarrollo
 * @date    2025
 * @version 1.0
 *
 * @defgroup sistema_seguridad Sistema de Seguridad
 * @brief    Módulo raíz del sistema de seguridad embebido.
 * @{
 * @defgroup hardware      Hardware y periféricos
 * @defgroup fsm           Máquina de Estados Finita (FSM)
 * @defgroup tareas        Tareas FreeRTOS
 * @defgroup sensores      Lectura de sensores
 * @defgroup actuadores    Control de actuadores
 * @defgroup config        Configuración y constantes
 * @}
 */

#include <Arduino_FreeRTOS.h>
#include <Keypad.h>
#include <LiquidCrystal.h>
#include <Servo.h>
#include <SPI.h>
#include <MFRC522.h>

// ============================================================
/// @addtogroup hardware
/// @{
// ============================================================

/** @brief Objeto LCD de 16×2. Pines: RS=12, EN=11, D4=5, D5=4, D6=3, D7=2. */
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

/** @brief Número de filas del teclado matricial. */
const byte ROWS = 4;

/** @brief Número de columnas del teclado matricial. */
const byte COLS = 4;

/**
 * @brief Mapa de caracteres del teclado matricial 4×4.
 * @code{.cpp}
 * {'1','2','3','A'},
 * {'4','5','6','B'},
 * {'7','8','9','C'},
 * {'*','0','#','D'}
 * @endcode
 */
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

/** @brief Pines de Arduino conectados a las filas del teclado. */
byte rowPins[ROWS] = {33, 35, 37, 39};

/** @brief Pines de Arduino conectados a las columnas del teclado. */
byte colPins[COLS]  = {41, 43, 45, 47};

/** @brief Objeto teclado matricial. */
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/** @brief Objeto lector RFID MFRC522. */
MFRC522 rfid(53, 9);   // SS_PIN=53, RST_PIN=9

/** @brief Servo-motor que actúa como cerradura electrónica. */
Servo puertaServo;

/// @} // hardware

// ============================================================
/// @addtogroup config
/// @{
// ============================================================

/**
 * @name Pines de entrada/salida
 * @{
 */
#define BUZZER     7   /**< @brief Pin digital del buzzer pasivo. */
#define LED_VERDE  6   /**< @brief Pin del LED verde (acceso permitido). */
#define LED_ROJO   13  /**< @brief Pin del LED rojo (alarma / bloqueo). */
#define PIN_SERVO  10  /**< @brief Pin PWM del servo-motor (cerradura). */
#define PIN_BOTON  8   /**< @brief Pin del botón de desbloqueo de emergencia (INPUT_PULLUP). */
#define PIN_HALL   A1  /**< @brief Pin analógico sensor Hall KY-035. */
#define PIN_MIC    A2  /**< @brief Pin analógico sensor micrófono KY-037. */
#define PIN_TEMP   A3  /**< @brief Pin analógico sensor temperatura KY-013. */
#define PIN_LUZ    A4  /**< @brief Pin analógico sensor luz KY-018. */
#define SS_PIN     53  /**< @brief Pin SPI Slave Select del MFRC522. */
#define RST_PIN    9   /**< @brief Pin de reset del MFRC522. */
/** @} */

/**
 * @name Temporización (1 tick = PASO_MS = 100 ms)
 * @{
 */
#define PASO_MS            100  /**< @brief Duración de cada tick de control en ms. */
#define CICLOS_AMBIENTAL    50  /**< @brief Duración del modo ambiental en ticks (5 s). */
#define CICLOS_INTRUSOS     20  /**< @brief Duración del modo intrusos en ticks (2 s). */
#define TICKS_ALARMA        20  /**< @brief Duración de la alarma en ticks (2 s). */
#define TICKS_VENTANA      120  /**< @brief Ventana de alarmas consecutivas en ticks (12 s). */
#define TICKS_PUERTA        50  /**< @brief Tiempo de puerta abierta en ticks (5 s). */
#define TICKS_MSG_ERROR     15  /**< @brief Duración del mensaje de error en ticks (1.5 s). */
#define TICKS_DEBOUNCE_BTN   1  /**< @brief Anti-rebote del botón en ticks (100 ms). */
/** @} */

/**
 * @name Parpadeo de LEDs
 * @{
 */
#define TICKS_PARP_ON       1  /**< @brief Ticks encendido en ALARMA (100 ms). */
#define TICKS_PARP_OFF      5  /**< @brief Ticks apagado en ALARMA (500 ms). */
#define TICKS_PARP_BLQ_ON   3  /**< @brief Ticks encendido en BLOQUEO (300 ms). */
#define TICKS_PARP_BLQ_OFF  7  /**< @brief Ticks apagado en BLOQUEO (700 ms). */
/** @} */

/**
 * @name Umbrales de sensores
 * @{
 */
#define UMBRAL_LUZ    100     /**< @brief ADC mínimo KY-018; por debajo = oscuridad. */
#define UMBRAL_MIC    600     /**< @brief ADC mínimo KY-037; por encima = ruido. */
#define HALL_REPOSO   512     /**< @brief Valor de reposo del KY-035 (~Vcc/2). */
#define HALL_MARGEN    40     /**< @brief Desviación mínima del Hall para detección. */
#define UMBRAL_TEMP    20     /**< @brief Temperatura mínima en °C antes de alarma. */
#define TERM_R1    10000.0    /**< @brief Resistencia de referencia KY-013 (10 kΩ). */
#define MAX_ALARMAS     3     /**< @brief Alarmas consecutivas para pasar a BLOQUEO. */
/** @} */

/**
 * @name Parámetros del detector de ruido (micrófono)
 * @{
 */
#define MIC_LECTURAS_EVENTO   3   /**< @brief Ticks con ruido para confirmar un evento (300 ms). */
#define MIC_EVENTOS_ALARMA    3   /**< @brief Eventos confirmados que disparan alarma. */
#define MIC_TICKS_SILENCIO    5   /**< @brief Ticks sin ruido para cerrar un evento (500 ms). */
#define MIC_TICKS_VENTANA   150   /**< @brief Ticks sin evento para olvidar acumulados (15 s). */
/** @} */

/// @} // config

// ============================================================
/// @addtogroup fsm
/// @{
// ============================================================

/**
 * @brief Estados de la Máquina de Estados Finita del sistema.
 *
 * @details
 * El diagrama completo de transiciones se encuentra en la documentación
 * del archivo principal (@ref sistema_seguridad_doxygen.ino).
 *
 * * `EST_INICIO`            — Pantalla de bienvenida; espera RFID o clave.
 * * `EST_ABRIENDO`          — Servo abierto; cuenta ticks hasta cerrarse.
 * * `EST_CONFIG`            — Submenú de configuración de clave y RFID.
 * * `EST_MONITOR_INTRUSOS`  — Monitorea movimiento, ruido y puerta abierta.
 * * `EST_MONITOR_AMBIENTAL` — Monitorea temperatura y nivel de luz.
 * * `EST_ALARMA`            — Sirena y LED activos durante N ticks.
 * * `EST_BLOQUEO`           — Sistema bloqueado; solo admin o botón desbloquea.
 */
enum Estado {
  EST_INICIO,
  EST_ABRIENDO,
  EST_CONFIG,
  EST_MONITOR_INTRUSOS,
  EST_MONITOR_AMBIENTAL,
  EST_ALARMA,
  EST_BLOQUEO
};

/** @brief Estado actual de la FSM. `volatile` porque lo modifican varias tareas. */
volatile Estado estadoActual   = EST_INICIO;

/** @brief Estado al que se regresa al finalizar una alarma. @see activarAlarma() */
Estado estadoAnterior = EST_MONITOR_AMBIENTAL;

/** @brief Estado destino al cerrar la puerta tras un acceso exitoso. @see manejarAbriendo() */
Estado estadoPost     = EST_MONITOR_AMBIENTAL;

/// @} // fsm

// ============================================================
// VARIABLES DE CONTROL GLOBAL
// ============================================================

/** @brief Dígitos acumulados desde el teclado durante el ingreso de clave. */
String textoClave = "";

/** @brief Intentos de acceso fallidos (clave o RFID). Se reinicia al desbloquear. */
int intentosFallidos = 0;

/**
 * @brief Estado lógico de la puerta.
 * @details `true` = servo en 90° (abierta); `false` = servo en 0° (cerrada).
 * @see abrirPuerta(), cerrarPuerta(), puertaAb()
 */
bool puertaAbiertaFlag = false;

/** @brief Contador de ticks para parpadeo de LEDs en ALARMA y BLOQUEO. */
int  ticsParpadeo     = 0;

/** @brief Indica si el LED rojo está encendido en el ciclo de parpadeo. */
bool ledOn            = false;

/** @brief Ticks transcurridos desde el inicio de la alarma activa. */
int  ticsAlarma       = 0;

/** @brief Tick global en que ocurrió la última alarma (para ventana consecutiva). */
int  ticsUltimaAlarma = 0;

/** @brief Contador global de ticks procesados por @ref TaskControl. */
int  ticsGlobal       = 0;

/** @brief Ticks desde que la puerta quedó abierta. @see manejarAbriendo() */
int  ticsPuerta       = 0;

/** @brief Número de alarmas consecutivas dentro de la ventana `TICKS_VENTANA`. */
int  alarmasConsec    = 0;

/** @brief Cadena de 16 caracteres con la causa de la alarma activa. @see activarAlarma() */
String razonAlarma    = "";

// ============================================================
/// @addtogroup tareas
/// @{
// ============================================================

/** @brief Handle de la tarea de monitoreo ambiental (@ref TaskAmbiental). */
TaskHandle_t hAmbiental = NULL;

/** @brief Handle de la tarea de monitoreo de intrusos (@ref TaskIntrusos). */
TaskHandle_t hIntrusos  = NULL;

/// @} // tareas

// ============================================================
// PROTOTIPOS
// ============================================================
void TaskControl  (void *pv);
void TaskAmbiental(void *pv);
void TaskIntrusos (void *pv);
void manejarInicio();
void manejarAbriendo();
void manejarConfig();
void manejarAlarma();
void manejarBloqueo();
void irAEstado(Estado nuevo);
void activarAlarma(String razon, Estado origen);
void verificarRFID();

// ============================================================
/// @addtogroup hardware
/// @{
// ============================================================

/**
 * @brief Retorna los bytes libres en el heap de RAM.
 *
 * @details
 * Usa las variables de enlazador `__heap_start` y `__brkval` para
 * estimar la RAM disponible. Útil para diagnóstico en tiempo de ejecución.
 *
 * @par Returns
 *    Número de bytes libres en el heap.
 */
int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

/**
 * @brief Escribe dos líneas de texto en el LCD 16×2.
 *
 * @details
 * Limpia el display y rellena cada línea con espacios hasta 16 caracteres.
 * Si la cadena supera los 16 caracteres, se trunca.
 *
 * @param [in] l1  Texto para la fila 0 (máx. 16 caracteres).
 * @param [in] l2  Texto para la fila 1 (máx. 16 caracteres).
 *
 * @par Returns
 *    Nothing.
 */
void lcdEscribir(String l1, String l2) {
  lcd.clear();
  while (l1.length() < 16) l1 += ' ';
  while (l2.length() < 16) l2 += ' ';
  lcd.setCursor(0, 0); lcd.print(l1.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(l2.substring(0, 16));
}

/// @} // hardware

// ============================================================
/// @addtogroup actuadores
/// @{
// ============================================================

/**
 * @brief Apaga todos los actuadores de señalización.
 *
 * @details
 * Pone en LOW los LEDs `LED_ROJO` y `LED_VERDE`, detiene el buzzer con
 * `noTone()` y reinicia `ledOn`. Debe invocarse antes de cada transición
 * de estado. @see irAEstado()
 *
 * @par Returns
 *    Nothing.
 */
void limpiarSalidas() {
  digitalWrite(LED_ROJO,  LOW);
  digitalWrite(LED_VERDE, LOW);
  noTone(BUZZER);
  ledOn = false;
}

/**
 * @brief Abre la puerta girando el servo a 90°.
 *
 * @details
 * Establece `puertaAbiertaFlag = true`, enciende `LED_VERDE` y reinicia
 * `ticsPuerta`. El cierre automático lo gestiona @ref manejarAbriendo().
 *
 * @par Returns
 *    Nothing.
 */
void abrirPuerta() {
  puertaServo.write(90);
  puertaAbiertaFlag = true;
  digitalWrite(LED_VERDE, HIGH);
  ticsPuerta = 0;
  Serial.println("Puerta: ABIERTA");
}

/**
 * @brief Cierra la puerta girando el servo a 0°.
 *
 * @details
 * Establece `puertaAbiertaFlag = false` y apaga `LED_VERDE`.
 * Normalmente es llamada por @ref manejarAbriendo() al expirar el
 * temporizador `TICKS_PUERTA`, o por @ref TaskIntrusos al detectar
 * la puerta abierta como intrusión.
 *
 * @par Returns
 *    Nothing.
 */
void cerrarPuerta() {
  puertaServo.write(0);
  puertaAbiertaFlag = false;
  digitalWrite(LED_VERDE, LOW);
  Serial.println("Puerta: CERRADA");
}

/**
 * @brief Detecta la pulsación del botón físico con anti-rebote por ticks.
 *
 * @details
 * Debe invocarse una vez por tick (cada 100 ms) desde @ref manejarBloqueo().
 * Acumula ticks con nivel LOW; retorna `true` solo al soltar el botón si
 * estuvo presionado al menos `TICKS_DEBOUNCE_BTN` ticks consecutivos.
 *
 * @retval true  Pulsación válida detectada.
 * @retval false No hubo pulsación válida en este tick.
 */
bool botonPresionado() {
  static int cntBtn = 0;
  if (digitalRead(PIN_BOTON) == LOW) {
    cntBtn++;
  } else {
    if (cntBtn >= TICKS_DEBOUNCE_BTN) { cntBtn = 0; return true; }
    cntBtn = 0;
  }
  return false;
}

/// @} // actuadores

// ============================================================
/// @addtogroup sensores
/// @{
// ============================================================

/**
 * @brief Lee la temperatura con el termistor NTC del KY-013.
 *
 * @details
 * Convierte la lectura ADC del pin `PIN_TEMP` a temperatura mediante la
 * ecuación de Steinhart-Hart de tres coeficientes:
 * @code{.cpp}
 * float R2    = TERM_R1 * (1023.0 / Vo - 1.0);
 * float logR2 = log(R2);
 * float T     = 1.0 / (A + B*logR2 + C*logR2^3);
 * return T - 273.15;
 * @endcode
 * Si el ADC devuelve 0 (circuito abierto), retorna `-999` como centinela.
 * La alarma se dispara cuando el valor supera el umbral @ref UMBRAL_TEMP.
 *
 * @par Returns
 *    Temperatura en grados Celsius, o `-999` si la lectura es inválida.
 *
 * @see TaskAmbiental(), UMBRAL_TEMP
 */
float leerTemp() {
  int Vo = analogRead(PIN_TEMP);
  if (Vo <= 0) return -999;
  float R2    = TERM_R1 * (1023.0 / (float)Vo - 1.0);
  float logR2 = log(R2);
  float T     = 1.0 / (0.001129148 + 0.000234125 * logR2
                        + 0.0000000876741 * logR2 * logR2 * logR2);
  return T - 273.15;
}

/**
 * @brief Lee el nivel de iluminación con el KY-018 (LDR).
 *
 * @details
 * Valores bajos (< `UMBRAL_LUZ`) indican oscuridad y disparan alarma
 * en @ref TaskAmbiental().
 *
 * @par Returns
 *    Valor ADC de 0 a 1023.
 *
 * @see TaskAmbiental(), UMBRAL_LUZ
 */
int leerLuz() { return analogRead(PIN_LUZ); }

/**
 * @brief Lee el nivel de ruido con el KY-037 (micrófono).
 *
 * @details
 * Valores superiores a `UMBRAL_MIC` se consideran ruido. La lógica
 * de eventos acumulados se implementa en @ref TaskIntrusos().
 *
 * @par Returns
 *    Valor ADC de 0 a 1023.
 *
 * @see TaskIntrusos(), UMBRAL_MIC
 */
int leerMic() { return analogRead(PIN_MIC); }

/**
 * @brief Consulta el estado lógico de la puerta.
 *
 * @retval true  La puerta está abierta (servo en 90°).
 * @retval false La puerta está cerrada (servo en 0°).
 *
 * @see puertaAbiertaFlag, abrirPuerta(), cerrarPuerta()
 */
bool puertaAb() { return puertaAbiertaFlag; }

/**
 * @brief Detecta campo magnético con el sensor Hall KY-035.
 *
 * @details
 * Compara la lectura de `PIN_HALL` con `HALL_REPOSO`. Si la desviación
 * absoluta supera `HALL_MARGEN`, se interpreta como presencia de un
 * objeto con imán (movimiento de persona/objeto). Tres lecturas positivas
 * consecutivas en @ref TaskIntrusos() disparan la alarma.
 *
 * @retval true  Campo magnético detectado.
 * @retval false Sensor en reposo; sin detección.
 *
 * @see TaskIntrusos(), HALL_REPOSO, HALL_MARGEN
 */
bool hallDet() { return abs(analogRead(PIN_HALL) - HALL_REPOSO) > HALL_MARGEN; }

/// @} // sensores

// ============================================================
/// @addtogroup fsm
/// @{
// ============================================================

/**
 * @brief Ejecuta la transición de la FSM al estado indicado.
 *
 * @details
 * Función central de la FSM. En cada transición:
 * # Actualiza `estadoActual`.
 * # Llama a @ref limpiarSalidas().
 * # Reinicia `textoClave` y `ticsParpadeo`.
 * # Ejecuta las acciones de entrada del estado destino:
 *
 * | Estado destino           | Acción de entrada                        |
 * | :----------------------- | :--------------------------------------- |
 * | `EST_INICIO`             | Muestra pantalla de bienvenida en LCD    |
 * | `EST_ABRIENDO`           | Llama a @ref abrirPuerta()               |
 * | `EST_MONITOR_INTRUSOS`   | `vTaskResume(hIntrusos)`                 |
 * | `EST_MONITOR_AMBIENTAL`  | `vTaskResume(hAmbiental)`                |
 * | `EST_CONFIG`             | Muestra menú de configuración            |
 * | `EST_ALARMA`             | Activa buzzer (900 Hz) y `LED_ROJO`      |
 * | `EST_BLOQUEO`            | Activa buzzer (600 Hz) y parpadeo rojo   |
 *
 * @param [in] nuevo  Estado destino de la transición.
 *
 * @par Returns
 *    Nothing.
 *
 * @see activarAlarma(), manejarAlarma(), manejarBloqueo()
 */
void irAEstado(Estado nuevo) {
  estadoActual = nuevo;
  limpiarSalidas();
  textoClave   = "";
  ticsParpadeo = 0;
  Serial.print(">> Estado: ");

  switch (nuevo) {
    case EST_INICIO:
      Serial.println("INICIO");
      lcdEscribir("Acerque tarjeta", "o ingrese clave:");
      break;
    case EST_ABRIENDO:
      Serial.println("ABRIENDO");
      lcdEscribir("Acceso permitido", "Puerta abierta  ");
      abrirPuerta();
      break;
    case EST_MONITOR_INTRUSOS:
      Serial.println("MONITOR INTRUSOS");
      vTaskResume(hIntrusos);
      break;
    case EST_MONITOR_AMBIENTAL:
      Serial.println("MONITOR AMBIENTAL");
      vTaskResume(hAmbiental);
      break;
    case EST_CONFIG:
      Serial.println("CONFIG");
      lcdEscribir("CONFIG          ", "1=Clave 2=RFID *");
      break;
    case EST_ALARMA:
      Serial.print("ALARMA: "); Serial.println(razonAlarma);
      ticsAlarma = 0; ticsParpadeo = 0;
      ledOn = true;
      digitalWrite(LED_ROJO, HIGH);
      tone(BUZZER, 900);
      lcdEscribir("!! ALARMA !!    ", razonAlarma);
      break;
    case EST_BLOQUEO:
      Serial.println("BLOQUEO");
      ticsParpadeo = 0; ledOn = true;
      digitalWrite(LED_ROJO, HIGH);
      tone(BUZZER, 600);
      lcdEscribir("SISTEMA         ", "BLOQUEADO       ");
      break;
  }
}

/**
 * @brief Registra y activa una alarma del sistema.
 *
 * @details
 * Guarda el estado de origen en `estadoAnterior` para restaurarlo al
 * finalizar la alarma. Controla el contador de alarmas consecutivas:
 * si la alarma ocurre dentro de `TICKS_VENTANA` ticks desde la anterior,
 * incrementa `alarmasConsec`; de lo contrario lo reinicia a 1. Al alcanzar
 * `MAX_ALARMAS` alarmas consecutivas, @ref manejarAlarma() transicionará
 * a `EST_BLOQUEO` en lugar de regresar al estado anterior.
 *
 * @param [in] razon   Cadena de hasta 16 caracteres con la causa de la alarma.
 * @param [in] origen  Estado desde el cual se dispara la alarma.
 *
 * @par Returns
 *    Nothing.
 *
 * @see irAEstado(), manejarAlarma(), estadoAnterior, alarmasConsec
 */
void activarAlarma(String razon, Estado origen) {
  estadoAnterior = origen;
  while (razon.length() < 16) razon += ' ';
  razonAlarma    = razon.substring(0, 16);
  alarmasConsec  = (ticsGlobal - ticsUltimaAlarma < TICKS_VENTANA)
                   ? alarmasConsec + 1 : 1;
  ticsUltimaAlarma = ticsGlobal;
  Serial.print("Alarmas consec: "); Serial.println(alarmasConsec);
  irAEstado(EST_ALARMA);
}

/// @} // fsm

// ============================================================
/// @addtogroup hardware
/// @{
// ============================================================

/**
 * @brief Lee una tarjeta RFID y autoriza o deniega el acceso.
 *
 * @details
 * Construye el UID en hexadecimal (mayúsculas) y lo compara con
 * `uidAutorizado`:
 *
 * * **UID válido**: reinicia `intentosFallidos`, emite beep de 1500 Hz,
 *   y llama a `irAEstado(EST_ABRIENDO)`.
 * * **UID inválido**: incrementa `intentosFallidos`; al alcanzar 3
 *   llama a `irAEstado(EST_BLOQUEO)`.
 *
 * Usa `vTaskDelay` internamente, por lo que **solo debe invocarse desde
 * una tarea FreeRTOS** (@ref TaskControl vía @ref manejarInicio()).
 *
 * @par Returns
 *    Nothing.
 *
 * @see manejarInicio(), irAEstado(), intentosFallidos
 */
void verificarRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  Serial.print("RFID: "); Serial.println(uid);

  if (uid == uidAutorizado) {
    intentosFallidos = 0;
    tone(BUZZER, 1500, 300);
    vTaskDelay(pdMS_TO_TICKS(300));
    estadoPost = EST_MONITOR_AMBIENTAL;
    irAEstado(EST_ABRIENDO);
  } else {
    lcdEscribir("RFID INVALIDO   ", "Acceso denegado ");
    digitalWrite(LED_ROJO, HIGH);
    tone(BUZZER, 300, 1000);
    intentosFallidos++;
    Serial.print("RFID invalido. Intentos: "); Serial.println(intentosFallidos);
    vTaskDelay(pdMS_TO_TICKS(1500));
    limpiarSalidas();
    if (intentosFallidos >= 3) irAEstado(EST_BLOQUEO);
    else irAEstado(EST_INICIO);
  }
  rfid.PICC_HaltA();
}

/// @} // hardware

// ============================================================
// SETUP
// ============================================================

/**
 * @brief Inicialización del sistema.
 *
 * @details
 * Secuencia de arranque:
 * # Serie a 9600 bps, bus SPI y módulo RFID.
 * # Pines de salida (buzzer, LEDs) y entrada (botón PULLUP).
 * # Servo en posición cerrada (0°).
 * # LCD con pantalla de splash.
 * # Creación de las tres tareas FreeRTOS:
 *   - @ref TaskControl   — stack 256 B, prioridad 1.
 *   - @ref TaskAmbiental — stack 256 B, prioridad 1, suspendida al inicio.
 *   - @ref TaskIntrusos  — stack 256 B, prioridad 1, suspendida al inicio.
 * # Diagnóstico de RAM antes y después de crear las tareas.
 *
 * @par Returns
 *    Nothing.
 */
void setup() {
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();

  pinMode(BUZZER,    OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO,  OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);

  puertaServo.attach(PIN_SERVO);
  puertaServo.write(0);

  lcd.begin(16, 2);
  lcd.setCursor(0, 0); lcd.print("  SISTEMA SEG   ");
  lcd.setCursor(0, 1); lcd.print("  Iniciando...  ");

  Serial.println(F("=== ARRANQUE ==="));
  Serial.print(F("RAM libre antes de tareas: ")); Serial.println(freeRam());

  BaseType_t r1 = xTaskCreate(TaskControl,   "Ctrl", 256, NULL, 1, NULL);
  BaseType_t r2 = xTaskCreate(TaskAmbiental, "Amb",  256, NULL, 1, &hAmbiental);
  BaseType_t r3 = xTaskCreate(TaskIntrusos,  "Int",  256, NULL, 1, &hIntrusos);

  Serial.print(F("Crear tareas (1=ok): "));
  Serial.print(r1); Serial.print(' ');
  Serial.print(r2); Serial.print(' ');
  Serial.println(r3);
  Serial.print(F("RAM libre despues de tareas: ")); Serial.println(freeRam());
  Serial.println(F("=== scheduler arranca ahora ==="));
}

/** @brief Función loop vacía; el scheduler de FreeRTOS toma el control tras `setup()`. */
void loop() { }

// ============================================================
/// @addtogroup tareas
/// @{
// ============================================================

/**
 * @brief Tarea FreeRTOS principal: ciclo de despacho de la FSM.
 *
 * @details
 * Se ejecuta cada `PASO_MS` ms. En cada iteración:
 * # Incrementa `ticsGlobal`.
 * # Despacha al manejador del estado activo:
 *
 * | Estado activo           | Manejador invocado      |
 * | :---------------------- | :---------------------- |
 * | `EST_INICIO`            | @ref manejarInicio()    |
 * | `EST_ABRIENDO`          | @ref manejarAbriendo()  |
 * | `EST_CONFIG`            | @ref manejarConfig()    |
 * | `EST_ALARMA`            | @ref manejarAlarma()    |
 * | `EST_BLOQUEO`           | @ref manejarBloqueo()   |
 *
 * Los estados `EST_MONITOR_INTRUSOS` y `EST_MONITOR_AMBIENTAL` son
 * gestionados por sus propias tareas (@ref TaskIntrusos,
 * @ref TaskAmbiental) que se suspenden y reanudan según sea necesario.
 *
 * @param [in] pv  Parámetro de tarea FreeRTOS (no usado; pásese `NULL`).
 *
 * @par Returns
 *    Nothing.
 */
void TaskControl(void *pv) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  irAEstado(EST_INICIO);

  for (;;) {
    ticsGlobal++;
    switch (estadoActual) {
      case EST_INICIO:   manejarInicio();   break;
      case EST_ABRIENDO: manejarAbriendo(); break;
      case EST_CONFIG:   manejarConfig();   break;
      case EST_ALARMA:   manejarAlarma();   break;
      case EST_BLOQUEO:  manejarBloqueo();  break;
      default: break;
    }
    vTaskDelay(pdMS_TO_TICKS(PASO_MS));
  }
}

/**
 * @brief Tarea FreeRTOS de monitoreo ambiental (temperatura y luz).
 *
 * @details
 * Se suspende al crearse y se reanuda desde @ref irAEstado() cuando el
 * estado destino es `EST_MONITOR_AMBIENTAL`. En cada ciclo de
 * `CICLOS_AMBIENTAL` ticks (5 s):
 *
 * * Lee @ref leerTemp() y @ref leerLuz() cada tick.
 * * Actualiza el LCD cada 10 ticks con valores en tiempo real.
 * * `temp < UMBRAL_TEMP` → llama a @ref activarAlarma() con `"TEMP BAJA <20C"`.
 * * `luz  < UMBRAL_LUZ`  → llama a @ref activarAlarma() con `"LUZ BAJA"`.
 * * Tecla `'*'`          → transiciona a `EST_CONFIG`.
 *
 * Al finalizar el ciclo sin incidencias, cede a `EST_MONITOR_INTRUSOS`
 * y se suspende de nuevo.
 *
 * @param [in] pv  Parámetro de tarea FreeRTOS (no usado; pásese `NULL`).
 *
 * @par Returns
 *    Nothing.
 *
 * @see leerTemp(), leerLuz(), activarAlarma(), TaskIntrusos()
 */
void TaskAmbiental(void *pv) {
  vTaskSuspend(NULL);
  const TickType_t paso = pdMS_TO_TICKS(PASO_MS);

  for (;;) {
    limpiarSalidas();
    estadoActual = EST_MONITOR_AMBIENTAL;
    Serial.println(">> MONITOR AMBIENTAL");
    lcdEscribir("[AMBIENT]   OK  ", "T:--C  L:---    ");

    for (int c = 0; c < CICLOS_AMBIENTAL; c++) {
      float temp = leerTemp();
      int   luz  = leerLuz();
      if (c % 10 == 0) {
        String l2 = "T:" + String((int)temp) + "C  L:" + String(luz) + "   ";
        lcdEscribir("[AMBIENT]   OK  ", l2);
      }
      if (temp < UMBRAL_TEMP) { activarAlarma("TEMP BAJA <20C  ", EST_MONITOR_AMBIENTAL); break; }
      if (luz  < UMBRAL_LUZ ) { activarAlarma("LUZ BAJA        ", EST_MONITOR_AMBIENTAL); break; }
      char k = keypad.getKey();
      if (k == '*') { estadoAnterior = EST_MONITOR_AMBIENTAL; irAEstado(EST_CONFIG); break; }
      vTaskDelay(paso);
    }
    if (estadoActual == EST_MONITOR_AMBIENTAL) irAEstado(EST_MONITOR_INTRUSOS);
    vTaskSuspend(NULL);
  }
}

/**
 * @brief Tarea FreeRTOS de monitoreo de intrusos.
 *
 * @details
 * Se suspende al crearse y se reanuda desde @ref irAEstado() cuando el
 * estado destino es `EST_MONITOR_INTRUSOS`. En cada ciclo de
 * `CICLOS_INTRUSOS` ticks (2 s) evalúa tres fuentes de amenaza:
 *
 * **1. Puerta abierta** (@ref puertaAb())
 * Tres ticks consecutivos positivos disparan `"PUERTA ABIERTA"`.
 *
 * **2. Campo magnético** (@ref hallDet())
 * Tres ticks consecutivos positivos disparan `"MOVIMIENTO DET."`.
 *
 * **3. Micrófono** (@ref leerMic()) — lógica de eventos acumulados:
 * * `MIC_LECTURAS_EVENTO` ticks con ruido → evento confirmado.
 * * Evento termina tras `MIC_TICKS_SILENCIO` ticks sin ruido.
 * * `MIC_EVENTOS_ALARMA` eventos → alarma `"RUIDO DETECTADO"`.
 * * `MIC_TICKS_VENTANA` ticks sin evento → contador reiniciado.
 *
 * Tecla `'#'` transiciona a `EST_CONFIG`. Al finalizar el ciclo sin
 * incidencias, cede a `EST_MONITOR_AMBIENTAL` y se suspende de nuevo.
 *
 * @param [in] pv  Parámetro de tarea FreeRTOS (no usado; pásese `NULL`).
 *
 * @par Returns
 *    Nothing.
 *
 * @see puertaAb(), hallDet(), leerMic(), activarAlarma(), TaskAmbiental()
 */
void TaskIntrusos(void *pv) {
  vTaskSuspend(NULL);
  const TickType_t paso = pdMS_TO_TICKS(PASO_MS);

  static int  cntPta            = 0, cntHall = 0;
  static int  micLecturasConsec = 0, micEventos = 0;
  static bool micEnEvento       = false;
  static int  micTicksSilencio  = 0, micTicksVentana = 0;

  for (;;) {
    limpiarSalidas();
    estadoActual = EST_MONITOR_INTRUSOS;
    Serial.println(">> MONITOR INTRUSOS");
    lcdEscribir("[INTRUSOS]  OK  ", "PTA:cerr mic:ok ");

    for (int c = 0; c < CICLOS_INTRUSOS; c++) {
      bool ruido = leerMic() > UMBRAL_MIC;

      if (ruido) {
        micTicksSilencio = 0; micTicksVentana = 0;
        if (!micEnEvento) {
          micLecturasConsec++;
          if (micLecturasConsec >= MIC_LECTURAS_EVENTO) {
            micLecturasConsec = 0; micEnEvento = true; micEventos++;
            Serial.print("MIC evento #"); Serial.println(micEventos);
            String l2 = "mic:RUIDO #"; l2 += String(micEventos); l2 += "/3          ";
            lcdEscribir("[INTRUSOS]  OK  ", l2);
            if (micEventos >= MIC_EVENTOS_ALARMA) {
              micEventos=0; micLecturasConsec=0; micEnEvento=false;
              micTicksSilencio=0; micTicksVentana=0;
              activarAlarma("RUIDO DETECTADO ", EST_MONITOR_INTRUSOS); break;
            }
          }
        }
      } else {
        micLecturasConsec = 0;
        if (micEnEvento) {
          if (++micTicksSilencio >= MIC_TICKS_SILENCIO) { micEnEvento=false; micTicksSilencio=0; }
        } else {
          if (++micTicksVentana >= MIC_TICKS_VENTANA) {
            micEventos=0; micTicksVentana=0;
            Serial.println("MIC ventana expirada: reset eventos");
          }
        }
      }

      if (c % 8 == 0) {
        String l2 = puertaAb() ? "PTA:ABIERTA " : "PTA:cerr    ";
        l2 += ruido ? "MIC:!" : "mic:ok";
        if (!ruido && !micEnEvento) lcdEscribir("[INTRUSOS]  OK  ", l2);
      }
      if (puertaAb()) { if (++cntPta  >= 3) { cntPta=0;  activarAlarma("PUERTA ABIERTA  ", EST_MONITOR_INTRUSOS); break; } } else cntPta=0;
      if (hallDet())  { if (++cntHall >= 3) { cntHall=0; activarAlarma("MOVIMIENTO DET. ", EST_MONITOR_INTRUSOS); break; } } else cntHall=0;
      char k = keypad.getKey();
      if (k == '#') { estadoAnterior = EST_MONITOR_AMBIENTAL; irAEstado(EST_CONFIG); break; }
      vTaskDelay(paso);
    }
    if (estadoActual == EST_MONITOR_INTRUSOS) irAEstado(EST_MONITOR_AMBIENTAL);
    vTaskSuspend(NULL);
  }
}

/// @} // tareas

// ============================================================
/// @addtogroup fsm
/// @{
// ============================================================

/**
 * @brief Manejador del estado `EST_INICIO`.
 *
 * @details
 * Invocado por @ref TaskControl en cada tick. Llama a @ref verificarRFID()
 * y lee el teclado:
 * * `'*'` — Limpia `textoClave` y remuestra la pantalla de bienvenida.
 * * `'#'` — Evalúa la clave:
 *   - Correcta → beep + @ref irAEstado() a `EST_ABRIENDO`.
 *   - Incorrecta → incrementa `intentosFallidos`; al llegar a 3,
 *     llama a @ref irAEstado() a `EST_BLOQUEO`.
 * * Dígito — Concatena a `textoClave` y muestra asteriscos en el LCD.
 *
 * @par Returns
 *    Nothing.
 *
 * @see verificarRFID(), irAEstado(), intentosFallidos
 */
void manejarInicio() {
  verificarRFID();
  char key = keypad.getKey();
  if (!key) return;

  if (key == '*') {
    textoClave = "";
    lcdEscribir("Acerque tarjeta", "o ingrese clave:");
  } else if (key == '#') {
    if (textoClave == claveCorrecta) {
      intentosFallidos = 0;
      tone(BUZZER, 1200, 300);
      vTaskDelay(pdMS_TO_TICKS(300));
      estadoPost = EST_MONITOR_AMBIENTAL;
      irAEstado(EST_ABRIENDO);
    } else {
      intentosFallidos++;
      Serial.print("Clave incorrecta: "); Serial.println(intentosFallidos);
      lcdEscribir("CLAVE           ", "INCORRECTA  " + String(intentosFallidos) + "/3");
      digitalWrite(LED_ROJO, HIGH);
      tone(BUZZER, 300, 1000);
      vTaskDelay(pdMS_TO_TICKS(TICKS_MSG_ERROR * PASO_MS));
      limpiarSalidas();
      if (intentosFallidos >= 3) irAEstado(EST_BLOQUEO);
      else lcdEscribir("Acerque tarjeta", "o ingrese clave:");
    }
    textoClave = "";
  } else {
    textoClave += key;
    String ast = "";
    for (unsigned int i = 0; i < textoClave.length(); i++) ast += '*';
    lcdEscribir("Digite clave:   ", ast);
  }
}

/**
 * @brief Manejador del estado `EST_ABRIENDO`.
 *
 * @details
 * Incrementa `ticsPuerta` en cada tick. Al alcanzar `TICKS_PUERTA` (5 s),
 * llama a @ref cerrarPuerta() y transiciona al estado en `estadoPost`
 * mediante @ref irAEstado().
 *
 * @par Returns
 *    Nothing.
 *
 * @see cerrarPuerta(), irAEstado(), estadoPost, TICKS_PUERTA
 */
void manejarAbriendo() {
  ticsPuerta++;
  if (ticsPuerta >= TICKS_PUERTA) {
    ticsPuerta = 0;
    cerrarPuerta();
    irAEstado(estadoPost);
  }
}

/**
 * @brief Manejador del estado `EST_ALARMA`.
 *
 * @details
 * Gestiona el parpadeo del LED rojo y la duración total de la alarma
 * usando únicamente contadores de ticks:
 * * LED encendido `TICKS_PARP_ON` ticks → apagado (noTone).
 * * LED apagado  `TICKS_PARP_OFF` ticks → encendido (tone 900 Hz).
 *
 * Al cumplirse `TICKS_ALARMA` ticks totales, limpia salidas y:
 * * `alarmasConsec >= MAX_ALARMAS` → @ref irAEstado() a `EST_BLOQUEO`.
 * * En caso contrario → @ref irAEstado() a `estadoAnterior`.
 *
 * @par Returns
 *    Nothing.
 *
 * @see activarAlarma(), irAEstado(), alarmasConsec, estadoAnterior
 */
void manejarAlarma() {
  ticsAlarma++; ticsParpadeo++;
  if ( ledOn && ticsParpadeo >= TICKS_PARP_ON)  { ledOn=false; ticsParpadeo=0; digitalWrite(LED_ROJO,LOW);  noTone(BUZZER); }
  if (!ledOn && ticsParpadeo >= TICKS_PARP_OFF) { ledOn=true;  ticsParpadeo=0; digitalWrite(LED_ROJO,HIGH); tone(BUZZER,900); }
  if (ticsAlarma >= TICKS_ALARMA) {
    ticsAlarma = 0;
    limpiarSalidas();
    irAEstado(alarmasConsec >= MAX_ALARMAS ? EST_BLOQUEO : estadoAnterior);
  }
}

/**
 * @brief Manejador del estado `EST_BLOQUEO`.
 *
 * @details
 * Parpadea el LED rojo con tiempos distintos al de alarma
 * (`TICKS_PARP_BLQ_ON` / `TICKS_PARP_BLQ_OFF`) para diferenciación visual.
 *
 * El sistema se desbloquea de dos formas:
 * * **Botón físico** — @ref botonPresionado() retorna `true`:
 *   reinicia contadores y llama a @ref irAEstado() a `EST_INICIO`.
 * * **Clave de administrador** — el operador ingresa `claveAdmin` y
 *   confirma con `'#'`:
 *   - Correcta → @ref irAEstado() a `EST_CONFIG`.
 *   - Incorrecta → mensaje de error y regresa a pantalla de bloqueo.
 *
 * @par Returns
 *    Nothing.
 *
 * @see botonPresionado(), irAEstado(), claveAdmin
 */
void manejarBloqueo() {
  ticsParpadeo++;
  if ( ledOn && ticsParpadeo >= TICKS_PARP_BLQ_ON)  { ledOn=false; ticsParpadeo=0; digitalWrite(LED_ROJO,LOW);  noTone(BUZZER); }
  if (!ledOn && ticsParpadeo >= TICKS_PARP_BLQ_OFF) { ledOn=true;  ticsParpadeo=0; digitalWrite(LED_ROJO,HIGH); tone(BUZZER,600); }

  if (botonPresionado()) {
    intentosFallidos = 0; alarmasConsec = 0;
    Serial.println("Desbloqueado: boton");
    limpiarSalidas();
    irAEstado(EST_INICIO);
    return;
  }
  char key = keypad.getKey();
  if (!key) return;

  if (key == '*') {
    textoClave = "";
    lcdEscribir("SISTEMA         ", "BLOQUEADO       ");
  } else if (key == '#') {
    if (textoClave == claveAdmin) {
      intentosFallidos = 0; alarmasConsec = 0;
      limpiarSalidas();
      Serial.println("Desbloqueado: admin");
      estadoAnterior = EST_MONITOR_AMBIENTAL;
      irAEstado(EST_CONFIG);
    } else {
      lcdEscribir("Clave incorrecta", "                ");
      tone(BUZZER, 300, 400);
      vTaskDelay(pdMS_TO_TICKS(1000));
      lcdEscribir("SISTEMA         ", "BLOQUEADO       ");
    }
    textoClave = "";
  } else {
    textoClave += key;
    String ast = "";
    for (unsigned int i = 0; i < textoClave.length(); i++) ast += '*';
    lcdEscribir("Clave admin:    ", ast);
  }
}

/**
 * @brief Manejador del estado `EST_CONFIG`.
 *
 * @details
 * Gestiona un submenú de dos opciones usando la variable estática `submenu`:
 *
 * | `submenu` | Descripción                        | Confirmación |
 * | :-------: | :--------------------------------- | :----------- |
 * | 0         | Menú principal (teclas 1, 2, \*)   | —            |
 * | 1         | Cambio de clave de usuario         | `'#'`        |
 * | 2         | Registro de nueva tarjeta RFID     | Lectura RFID |
 *
 * **Submenú 1 (cambio de clave):**
 * Acumula dígitos; `'#'` guarda si hay ≥4 dígitos en `claveCorrecta`;
 * `'*'` cancela.
 *
 * **Submenú 2 (registro RFID):**
 * Espera lectura del MFRC522 y guarda el UID en `uidAutorizado`;
 * `'*'` cancela.
 *
 * La tecla `'*'` en el menú principal llama a
 * @ref irAEstado() con `estadoAnterior`.
 *
 * @par Returns
 *    Nothing.
 *
 * @see irAEstado(), claveCorrecta, uidAutorizado
 */
void manejarConfig() {
  static int submenu = 0;
  char key = keypad.getKey();

  if (submenu == 1) {
    if (!key) return;
    if (key == '#') {
      if (textoClave.length() >= 4) {
        claveCorrecta = textoClave;
        lcdEscribir("Clave guardada  ", textoClave);
        Serial.print("Nueva clave: "); Serial.println(claveCorrecta);
        vTaskDelay(pdMS_TO_TICKS(1500));
      } else {
        lcdEscribir("Min 4 digitos   ", "                ");
        vTaskDelay(pdMS_TO_TICKS(1200));
      }
      textoClave = ""; submenu = 0;
      lcdEscribir("CONFIG          ", "1=Clave 2=RFID *");
    } else if (key == '*') {
      textoClave = ""; submenu = 0;
      lcdEscribir("CONFIG          ", "1=Clave 2=RFID *");
    } else {
      textoClave += key;
      String ast = "";
      for (unsigned int i = 0; i < textoClave.length(); i++) ast += '*';
      lcdEscribir("Nueva clave:    ", ast);
    }
    return;
  }

  if (submenu == 2) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      uidAutorizado = uid;
      lcdEscribir("RFID guardado:  ", uid);
      Serial.print("Nuevo UID: "); Serial.println(uidAutorizado);
      rfid.PICC_HaltA();
      vTaskDelay(pdMS_TO_TICKS(2000));
      submenu = 0;
      lcdEscribir("CONFIG          ", "1=Clave 2=RFID *");
    }
    if (key == '*') { submenu = 0; lcdEscribir("CONFIG          ", "1=Clave 2=RFID *"); }
    return;
  }

  if (!key) return;
  if      (key == '1') { submenu = 1; textoClave = ""; lcdEscribir("Nueva clave:    ", ""); }
  else if (key == '2') { submenu = 2; lcdEscribir("Acerque nueva   ", "tarjeta RFID... "); }
  else if (key == '*') { submenu = 0; irAEstado(estadoAnterior); }
}

/// @} // fsm
