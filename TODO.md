# TODO

Lista de trabajo pendiente de BoredomOS: funcionalidades por implementar y cambios
sobre las existentes. Cada entrada se define aquí antes de empezar a programarla.

## Formato de una entrada

```markdown
### Título corto de la funcionalidad

**Estado:** propuesta | definida | en curso | hecha
**Ámbito:** ficheros o módulos afectados (`src/*.cpp`, `lib/*`, ...)

Qué debe hacer y por qué. Si añade una tarea o una cola, indicar prioridad
(`include/Priority.h`), tamaño de pila en palabras y quién libera la memoria
de los punteros que viajan por la cola.
```

## Por implementar

### Mover el enlace MAVLink a `Serial1` y dejar el USB como consola de depuración

**Estado:** definida
**Ámbito:** `src/serial.cpp`, `src/mavlink.cpp`, `src/main.cpp`, `src/hooks.cpp`, `platformio.ini`, `README.md`

Hoy MAVLink y la depuración comparten el mismo puerto: `Serial` (USB CDC) transporta
las tramas binarias, así que el monitor serie es ilegible y cualquier `print` de
depuración corrompería el enlace. La funcionalidad separa ambos usos:

- El enlace MAVLink pasa al UART hardware `Serial1` (pines D0 `RX` / D1 `TX` del
  UNO R4 Minima), que es donde se conectará la radio de telemetría.
- `Serial` (USB) queda libre como consola de texto: depuración, salida de Unity en
  `pio test` y el mensaje de desbordamiento de pila de `src/hooks.cpp`.

Puntos a resolver al implementarla:

- El puerto del enlace se elige en **un solo sitio** (alias o `#define` en
  `include/`, no `Serial1` repetido en cada `.cpp`), para poder volver a USB sin
  tocar la lógica de protocolo.
- Velocidad del enlace: `Serial1.begin(...)` con la baudrate de la radio (las
  telemetrías MAVLink suelen ir a 57600, no a 115200). Queda por decidir si es fija
  o un `build_flag`.
- Los `while (!Serial)` de `src/serial.cpp` y el `waitSerial()` de `src/mavlink.cpp`
  existen porque el CDC USB no está listo hasta que el host abre el puerto. En un
  UART hardware ese guard es un no-op, así que hay que quitarlo o sustituirlo por la
  espera que corresponda, sin dejar tareas girando en vacío.
- `Serial.begin()` sigue en `setup()` para la consola, pero **ninguna tarea puede
  bloquearse esperando a `Serial`**: la placa tiene que funcionar en vuelo sin USB
  conectado.
- Lo que se escriba en la consola no debe hacerlo desde varias tareas sin control;
  decidir si se accede directamente o vía una cola, coherente con el resto del
  firmware.
- Documentar en `README.md` el cambio de puerto en el lado de tierra: `mavproxy.py
  --master=<puerto de la radio>` en lugar de `/dev/ttyACM0`.
- Revisar `test/test_main.cpp`: al liberar el USB, la salida de Unity deja de
  mezclarse con las tramas MAVLink.

### Añadir un sensor de temperatura

**Estado:** propuesta
**Ámbito:** `src/sensors.cpp` (nuevo), `src/main.cpp`, `include/Data.h`,
`src/logger.cpp`, `src/sdwrite.cpp`, `src/mavlink.cpp`, `platformio.ini`

Medir la temperatura a bordo y exponerla por los dos caminos que ya existen: el
registro de mantenimiento en la SD y la telemetría MAVLink hacia tierra. Es dato
crítico para un CubeSat: la LiPo y la SD tienen rango de operación estrecho y hoy
no hay forma de saber a qué temperatura vuela la placa.

Por decidir antes de implementar:

- **Qué sensor.** El sensor interno del RA4M1 no necesita hardware pero mide el
  die, no el ambiente. Un I2C externo colgado del bus que ya usa el DS1307 no
  añade cableado nuevo. Elegir uno u otro fija la dependencia en `platformio.ini`
  y si hace falta una librería en `lib/` al estilo de `lib/Battery`.
- **Cuántos puntos de medida.** Un solo sensor, o varios (batería, exterior)
  cambia la forma del dato en `Data`.
- **Cadencia y caché.** `lib/Battery` cachea 125 ms; la temperatura cambia mucho
  más despacio, así que el muestreo puede ser bastante más lento que el 1 Hz de
  `src/logger.cpp`.

Puntos de implementación:

- `src/main.cpp:44` ya declara `[[noreturn]] extern void TaskSensors(...)` sin
  implementación ni `xTaskCreate`: es el hueco previsto para esto. Hay que crear
  `src/sensors.cpp` y darle prioridad de `include/Priority.h` y pila en palabras.
- Añadir el campo a `include/Data.h`, rellenarlo en `src/logger.cpp` y volcarlo en
  `src/sdwrite.cpp`. Al tocar `Data` cambia el esquema de los `.mpk`: decidir si
  los ficheros antiguos siguen siendo legibles.
- Si la lectura no la hace el propio `TaskLogger`, el valor tiene que llegarle sin
  romper el protocolo de colas: **punteros en heap, `vPortFree` si el `xQueueSend`
  no devuelve `pdPASS`, y el consumidor libera**.
- Mensaje MAVLink de salida: elegir uno estándar (`SCALED_PRESSURE.temperature` en
  centigrados x100, o `HYGROMETER_SENSOR`) y emitirlo con la misma tripleta de
  identidad que el resto: sistema `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Añadir la comprobación a `test/test_main.cpp`, que corre solo sobre hardware
  real. Si el sensor se inicializa con `configASSERT` en `setup()`, su ausencia
  colgará la placa igual que hoy hacen RTC y SD.
- Vigilar los high-water marks tras añadir la tarea: el margen de RAM es escaso.

### Detectar cuándo la batería está cargando

**Estado:** propuesta
**Ámbito:** `lib/Battery`, `src/mavlink.cpp`, `include/Data.h`, `src/logger.cpp`,
`src/sdwrite.cpp`

Ahora mismo el firmware solo sabe *qué tensión* tiene la batería, no si está
entrando corriente de los paneles. `src/mavlink.cpp` refleja esa carencia: envía
`MAV_BATTERY_CHARGE_STATE_UNDEFINED` fijo y `current_battery = -1`. Desde tierra no
se distingue una batería al 60 % subiendo al sol de una al 60 % bajando en eclipse,
que es justo la diferencia que importa para planificar el consumo.

Por decidir antes de implementar:

- **De dónde sale la señal.** `lib/Battery` solo tiene `SolarCharger` sobre `A0`, y
  esa librería únicamente expone `readVoltage()`. Hay dos caminos:
  - *Hardware:* leer el pin `STAT`/`CHG` del cargador en un GPIO. Es fiable e
    inmediato, pero añade cableado, y hoy el pinout está fijo en el código
    (SD `CS` en 9, batería en `A0`, DS1307 en I2C) — habría que documentarlo ahí.
  - *Software:* inferirlo de la tendencia de la tensión. No toca hardware, pero
    obliga a guardar historial y a fijar umbral y ventana temporal para no
    confundir el ruido del ADC con carga real.
- **Qué estados se distinguen.** Basta con cargando / no cargando, o interesa
  además *cargada* (fin de carga) y *descargando*. Esto fija qué valores de
  `MAV_BATTERY_CHARGE_STATE` se emiten.

Puntos de implementación:

- La lectura vive en `lib/Battery`, junto a `voltage()` y `remaining()`, con la
  misma política de caché que ya usa (125 ms) si la fuente lo requiere.
- Sustituir el `MAV_BATTERY_CHARGE_STATE_UNDEFINED` fijo de `sendBatteryStatus()`
  por el estado real. Misma tripleta de identidad que el resto: sistema `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Añadir el estado a `Energy` en `include/Data.h` y volcarlo en `src/sdwrite.cpp`,
  para poder reconstruir después los ciclos de carga desde los `.mpk`. Igual que
  con el sensor de temperatura, esto cambia el esquema de los ficheros de la SD.
- Si se elige la vía software, el historial no puede crecer: buffer de tamaño fijo,
  sin `malloc` por muestra.
- Comprobación en `test/test_main.cpp`, que corre sobre hardware real y por tanto
  puede validar el estado con la placa enchufada (cargando) y sin ella.

## Por modificar

### Comprobar el resultado de `pvPortMalloc` en los cuatro sitios que no lo hacen

**Estado:** definida
**Ámbito:** `src/mavlink.cpp`

El protocolo de colas del proyecto se cumple a medias en `src/mavlink.cpp`: todos
los `xQueueSend` liberan con `vPortFree` cuando no devuelven `pdPASS`, pero cuatro
reservas no comprueban que `pvPortMalloc` haya devuelto algo antes de usar el
puntero. Se lo pasan directamente a `mavlink_msg_*_pack`, que escribe en él:

- `src/mavlink.cpp:20` — `sendHeartbeat()`
- `src/mavlink.cpp:41` — `SYSTEM_TIME`
- `src/mavlink.cpp:104` — `sendBatteryStatus()`
- `src/mavlink.cpp:201` — respuesta a `TIMESYNC`

Con el heap agotado, `pvPortMalloc` devuelve `NULL` y el `pack` escribe en la
dirección 0. El patrón correcto ya está en el mismo fichero, en `src/mavlink.cpp:61`
(`STATUSTEXT`), y en `src/logger.cpp:42` y `src/serial.cpp:50`: envolver desde la
reserva hasta el `xQueueSend` en `if (msg != NULL) { ... }`.

Importa más de lo que parece porque los tres primeros son emisores **periódicos**:
un heap momentáneamente lleno no da un fallo puntual, lo repite en cada ciclo.

Por decidir: si al no poder reservar conviene dejar rastro (contador en `Data`
hacia el log de la SD) o basta con saltarse el envío en silencio.

## Hecho

_Vacío por ahora._
