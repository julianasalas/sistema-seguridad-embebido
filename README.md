# Sistema de Seguridad Embebido

Control de acceso y monitoreo ambiental con FreeRTOS en Arduino Mega.

## Descripción
Sistema embebido que implementa una Máquina de Estados Finita (FSM)
para controlar el acceso físico y monitorear condiciones ambientales.

## Módulos
- **Hardware** — LCD, teclado matricial, servo, RFID MFRC522
- **FSM** — Máquina de estados con 7 estados definidos
- **Tareas FreeRTOS** — TaskControl, TaskAmbiental, TaskIntrusos
- **Sensores** — Temperatura KY-013, luz KY-018, micrófono KY-037, Hall KY-035
- **Actuadores** — Servo-motor, buzzer, LEDs verde y rojo

## Estados del sistema
- `EST_INICIO` — Espera clave o tarjeta RFID
- `EST_ABRIENDO` — Puerta abierta durante 5 segundos
- `EST_MONITOR_AMBIENTAL` — Monitorea temperatura y luz
- `EST_MONITOR_INTRUSOS` — Monitorea movimiento y ruido
- `EST_CONFIG` — Configuración de clave y tarjeta RFID
- `EST_ALARMA` — Alarma activa durante 2 segundos
- `EST_BLOQUEO` — Sistema bloqueado por intentos fallidos

## Hardware utilizado
- Arduino Mega 2560
- Teclado matricial 4x4
- LCD 16x2
- Servo-motor SG90
- Lector RFID MFRC522
- Sensores KY-013, KY-018, KY-035, KY-037