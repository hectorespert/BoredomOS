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

### Descargar los ficheros de la SD por MAVLink FTP

**Estado:** propuesta
**Ámbito:** `src/mavlink.cpp`, `lib/SdData`, `src/sdwrite.cpp`, `src/main.cpp`

Hoy el registro de mantenimiento solo se puede recuperar sacando la tarjeta de la
placa: nada lo expone por el enlace. Implementar MAVLink FTP
(`FILE_TRANSFER_PROTOCOL`) permitiría listar y descargar los `data0..N.mpk` e
`index.bin` desde tierra con el GCS de referencia (`ftp list` / `ftp get` en
MAVProxy), que es la única forma realista de leerlos con el satélite montado.

El hueco ya está marcado: `src/mavlink.cpp` tiene un `case
MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:` vacío en el `switch` de `TaskMavlink`.

Puntos a resolver antes de implementar:

- **Alcance del protocolo.** FTP de MAVLink es una máquina de estados con sesiones
  y opcodes (`ListDirectory`, `OpenFileRO`, `ReadFile`, `Terminate`, lecturas en
  ráfaga...). Decidir el subconjunto mínimo: listar y leer en solo lectura cubre el
  caso de uso; escritura y borrado desde tierra son otra discusión, y peligrosa
  sobre el fichero que el firmware tiene abierto.
- **Concurrencia con `TaskSdWrite`.** `lib/SdData` mantiene `_dataFile` abierto
  para escritura mientras el logger vuelca a 1 Hz, y la SD cuelga de SPI con `CS`
  en el pin 9. Dos tareas tocando la tarjeta a la vez es corrupción: hace falta un
  mutex, o que el acceso FTP pase por `TaskSdWrite`, que ya es la dueña del medio.
  Es la decisión de diseño principal de esta funcionalidad.
- **RAM.** Es la restricción de siempre: `mavlink_message_t` ya son ~280 bytes y
  las pilas están ajustadas entre 96 y 256 palabras. Una tarea FTP con su búfer de
  sesión no cabe sin medir; hay que mirar los high-water marks del log antes y
  después.
- **Tiempo de descarga.** Por defecto `SdData` son 4 ficheros de 1 GiB. Sobre un
  enlace de radio, y con los ~239 bytes útiles que mueve cada paquete FTP,
  descargar uno entero no es viable: conviene revisar ese tamaño por defecto, o
  soportar lectura por offset para bajar solo el tramo que interese.
- **Coherencia de lo que se descarga.** El fichero activo se está escribiendo
  mientras se lee. Definir si se sirve tal cual (el receptor puede encontrar un
  registro MessagePack a medias al final) o si solo se ofrecen los ficheros
  cerrados del anillo.
- Mantener la tripleta de identidad del resto del firmware: sistema `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.

### Escribir el documento de arquitectura

**Estado:** propuesta
**Ámbito:** `ARCHITECTURE.md` (nuevo), `README.md`, `CLAUDE.md`

El repositorio no tiene documentación de arquitectura para personas: `README.md`
son cuatro líneas y el comando de MAVProxy, y lo único que describe el diseño es la
sección *Architecture* de `CLAUDE.md`, escrita para Claude Code. Quien llegue nuevo
al proyecto —o el propio autor dentro de seis meses— no tiene dónde ver por qué el
firmware está partido así.

Contenido que debería cubrir:

- El modelo de tareas y colas: `src/main.cpp` como único sitio donde se crean, y
  cada tarea en su unidad de traducción alcanzando los objetos compartidos por
  `extern`.
- El protocolo de propiedad de la memoria, que es la regla que más fácil se rompe:
  las colas llevan punteros a heap, el productor libera si `xQueueSend` no devuelve
  `pdPASS`, el consumidor libera tras usar.
- Las tres tuberías —serie ↔ MAVLink, registro de mantenimiento, tiempo— y qué
  fichero es dueño de qué recurso (`src/serial.cpp` del UART, `src/sdwrite.cpp` de
  la tarjeta).
- Las restricciones que explican el código tal como está: pilas en palabras y
  ajustadas, prioridades de `include/Priority.h`, cableado fijo, `configASSERT` que
  cuelga la placa en vez de degradar.
- Un diagrama de las tuberías y las colas. Mermaid se renderiza en GitHub y se
  versiona como texto.

Por decidir antes de escribirlo:

- **Dónde vive la fuente única.** `CLAUDE.md` ya describe todo esto. Mantener dos
  documentos en paralelo garantiza que uno quede obsoleto: o `CLAUDE.md` pasa a
  apuntar a `ARCHITECTURE.md` y se queda con lo específico de Claude Code
  (comandos, convenciones al editar), o el nuevo documento se limita a lo que
  `CLAUDE.md` no cubre.
- **Qué profundidad.** Documentar decisiones y restricciones envejece bien;
  enumerar funciones y firmas envejece mal y ya está en el código.
- Si conviene registrar además el *porqué* de las decisiones ya tomadas
  (MessagePack en vez de JSON, anillo de ficheros de tamaño fijo, `MAV_TYPE_ROCKET`),
  que es justo lo que no se deduce leyendo los fuentes.

### Compilación debug y release, con traza de MAVLink por consola

**Estado:** propuesta
**Ámbito:** `platformio.ini`, `src/mavlink.cpp`, `src/serial.cpp`, `CLAUDE.md`,
`README.md`

Depurar el protocolo hoy es a ciegas: no hay forma de ver qué mensajes MAVLink
entran y salen sin un GCS al otro lado interpretándolos. La idea es tener dos
perfiles de compilación y que el de depuración vuelque la traza del protocolo por
el puerto de consola, en texto legible.

Depende de *[Mover el enlace MAVLink a `Serial1`...]*: mientras las tramas binarias
sigan yendo por el USB no hay puerto de consola donde escribir la traza.

Qué debería trazar, por cada mensaje: sentido (entrante/saliente), `msgid` —a poder
ser con nombre, no solo el número—, `sysid`/`compid` de origen y longitud. Los dos
puntos de paso obligados ya existen y son los sitios naturales donde engancharlo:
el `switch` de `TaskMavlink` para lo que entra y el drenaje de `TaskSerialWrite`
para lo que sale.

Puntos a resolver antes de implementar:

- **Cómo se separan los perfiles.** Un segundo `[env:...]` en `platformio.ini` que
  herede del actual y añada su `build_flags` es lo idiomático de PlatformIO. Ojo:
  CI ejecuta `pio run` sin `-e`, que compila *todos* los entornos — así el perfil
  de depuración también se comprueba en cada push, que es lo deseable, pero hay
  que asegurarse de que no se convierte en el que se sube a la placa por defecto.
- **La traza no puede existir en release.** Tiene que compilarse fuera con
  `#ifdef`, no quedar tras un `if` en tiempo de ejecución: las cadenas de texto y
  el formateo ocupan flash y RAM, y aquí no sobra ninguna de las dos. El perfil
  release debe generar exactamente el binario de hoy.
- **Escribir en consola no puede bloquear el vuelo.** Si el USB no está conectado o
  su búfer se llena, un `print` puede quedarse esperando y arrastrar a una tarea de
  prioridad `PRIORITY_HIGHEST`. Y varias tareas escribiendo a la vez entrelazan la
  salida. Hay que decidir si se escribe directamente, con mutex, o por una cola
  como el resto del firmware.
- **Pilas.** Formatear texto consume pila, y están ajustadas entre 96 y 256
  palabras. Al activar el perfil de depuración hay que volver a mirar los
  high-water marks del log: es exactamente el caso que provoca el parpadeo a
  0,5 Hz de `src/hooks.cpp`.
- **Qué más entra en el perfil de depuración.** Nivel de detalle configurable
  (solo cabeceras, o volcado hexadecimal), y si se aprovecha para las trazas de
  otros subsistemas o se queda solo en MAVLink.
- Documentar en `README.md` y `CLAUDE.md` cómo compilar y subir cada perfil.

### Añadir un watchdog

**Estado:** propuesta
**Ámbito:** `src/main.cpp`, tareas de `src/*.cpp`, `platformio.ini`

No hay watchdog. Cualquier tarea que se quede colgada —un `xQueueReceive` que no
llega, un I2C esperando al DS1307, el bucle de `src/hooks.cpp`— deja el satélite
inerte hasta un ciclo de alimentación que en vuelo nadie puede dar. Para firmware
pensado para volar es la ausencia más grave del proyecto.

El RA4M1 tiene WDT independiente. Por decidir:

- **Qué tareas lo alimentan.** Un solo `refresh()` desde la tarea de menor
  prioridad detecta inanición pero no que una tarea concreta se haya parado. Un
  esquema donde cada tarea marca su paso y una sola refresca cuando todas han
  pasado detecta mucho más, a costa de estado compartido.
- **Timeout**, contra el ciclo más lento (`TaskSdWrite`, que puede bloquearse en
  SPI escribiendo en la tarjeta).
- **Qué se hace tras un reinicio por watchdog.** Dejar constancia en el log de la
  SD o en un `STATUSTEXT` al arrancar; si no, los reinicios son invisibles desde
  tierra.
- Interacción con `configASSERT`: hoy un fallo de hardware en `setup()` cuelga la
  placa. Con watchdog eso pasa a ser un ciclo de reinicio infinito, que puede ser
  mejor (reintenta) o peor (no llega a emitir nada). Hay que decidirlo a la vez.


### Reflejar el estado real del satélite en el heartbeat

**Estado:** propuesta
**Ámbito:** `src/mavlink.cpp`, `include/Data.h`

`sendHeartbeat()` manda constantes: `MAV_STATE_ACTIVE` y
`MAV_MODE_FLAG_AUTO_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED`, pase lo que pase. Con la
batería al 5 %, la SD sin montar o el RTC perdido, el satélite sigue anunciando por
el enlace que todo va bien — justo cuando tierra necesita enterarse.

Por decidir:

- Qué condiciones elevan el estado a `MAV_STATE_CRITICAL` o `MAV_STATE_EMERGENCY`:
  umbral de batería, fallo de escritura en la SD, hora no válida.
- De dónde sale esa información. Hoy nadie centraliza la salud del sistema; hace
  falta un estado compartido, y llegar a él sin romper el modelo de tareas y colas.
- Si `MAV_STATE_BOOT` durante `setup()` y `MAV_STATE_STANDBY` sin enlace aportan
  algo, o basta con activo/crítico.

Relacionado con *[Detectar cuándo la batería está cargando]*: el estado de carga es
una de las entradas naturales de esta decisión.


### Responder a los mensajes del GCS que hoy se ignoran

**Estado:** propuesta
**Ámbito:** `src/mavlink.cpp`

En el `switch` de `TaskMavlink` hay varios `case` que solo hacen `break`:
`COMMAND_LONG` (incluido `MAV_CMD_GET_HOME_POSITION`), `PARAM_REQUEST_LIST` y
`REQUEST_DATA_STREAM`. El GCS los da por perdidos y reintenta: MAVProxy se queda
esperando un `COMMAND_ACK` que no llega nunca.

Lo mínimo es contestar siempre algo. Un `COMMAND_ACK` con
`MAV_RESULT_UNSUPPORTED` es una respuesta honesta y corta el reintento; callarse no.

Por decidir:

- Qué comandos se soportan de verdad y cuáles se rechazan explícitamente.
- Si se implementa el protocolo de parámetros (`PARAM_REQUEST_LIST` /
  `PARAM_SET`) o se responde con una lista vacía. Tener parámetros ajustables desde
  tierra —umbrales, cadencias— cambiaría bastante el proyecto: hoy todo está fijo
  en el código.
- Si `REQUEST_DATA_STREAM` debe poder cambiar la cadencia de la telemetría, hoy
  clavada en `TaskHeartbeat` y `TaskMavlinkBatteryStatus`.


### Añadir análisis estático a CI

**Estado:** definida
**Ámbito:** `.github/workflows/main.yml`, `platformio.ini`

El workflow solo ejecuta `pio run`: comprueba que compila, nada más. PlatformIO
trae cppcheck integrado en `pio check`, que no cuesta nada añadir al job existente.

No es teórico: la aritmética de punteros de
*[Corregir la aritmética de punteros en el `STATUSTEXT`...]* y los `pvPortMalloc`
sin comprobar son exactamente el tipo de defecto que cppcheck señala.

Por decidir: qué severidades hacen fallar el build. Empezar avisando sin romper el
job, ver el ruido real sobre este código y solo después endurecerlo — si el primer
`pio check` sale con cien avisos y tumba CI, acaba desactivado.

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

### Mejorar la sincronización del reloj

**Estado:** propuesta
**Ámbito:** `lib/SystemTime`, `src/mavlink.cpp`

`lib/SystemTime` mantiene en hora el RTC interno del R4 y el DS1307 externo, pero
la sincronización tiene hoy varias limitaciones que se notan en cuanto el satélite
lleva tiempo encendido o el GCS intenta medir el desfase.

Limitaciones actuales, por orden de impacto:

- **Resolución de un segundo.** `getUnixTimeUsec()` y `getUnixTimeNsec()` son
  `getUnixTime()` multiplicado por 10^6 y 10^9, así que la parte subsegundo es
  siempre cero. Eso va a la respuesta `TIMESYNC` y al `SYSTEM_TIME` que se emite
  cada segundo desde `TaskHeartbeat`: el GCS recibe una marca cuantizada al
  segundo y su estimación de desfase hereda ese error de hasta ±1 s. Combinar el
  RTC (segundos) con `xTaskGetTickCount()` o `micros()` para la fracción es lo que
  da resolución real.
- **Base de tiempo de `TIMESYNC` sin fijar.** La respuesta de `src/mavlink.cpp:201`
  usa `getUnixTimeNsec()`, tiempo de pared. Conviene decidir y documentar si es eso
  lo que espera el GCS de referencia o un tiempo monótono desde arranque, porque si
  no coinciden el desfase calculado en tierra no significa nada.
- **El DS1307 no se corrige si el RTC interno ya está en hora.** `setUnixTime()`
  sale por el `return` temprano al comparar solo contra el reloj interno, así que
  un DS1307 que haya derivado nunca se reajusta desde tierra — y es justo el que
  siembra la hora en el siguiente arranque.
- **No hay resincronización periódica.** `begin()` copia DS1307 → RTC interno una
  vez al arrancar y ahí acaba. Los dos relojes derivan por separado durante toda la
  misión sin que nadie los vuelva a acercar.
- **No se valida lo que llega del GCS.** `MAVLINK_MSG_ID_SYSTEM_TIME` se acepta tal
  cual: un valor corrupto o a cero deja al satélite en 1970 y contamina el
  `unixtime` de todos los registros de la SD, que es la referencia con la que luego
  se leen los `.mpk`.

Por decidir antes de implementar: qué reloj manda cuando discrepan, cada cuánto se
resincronizan entre sí, y qué rango de fechas se considera aceptable en un
`SYSTEM_TIME` entrante.

Al tocar `lib/SystemTime` hay que pasar `pio test`: `test/test_main.cpp` valida
contra el DS1307 real, así que esto no se puede comprobar sin la placa.

### Corregir la aritmética de punteros en el `STATUSTEXT` de mensaje desconocido

**Estado:** definida
**Ámbito:** `src/mavlink.cpp`

En el `default` del `switch` de `TaskMavlink`, `src/mavlink.cpp:222`:

```cpp
sendStatusText("Mensaje recibido con ID desconocido: " + msg->msgid, MAV_SEVERITY_WARNING);
```

`sendStatusText` recibe un `const char*`, así que ahí no hay concatenación de
cadenas: el `+` es **aritmética de punteros**. El literal ocupa 37 caracteres y el
puntero avanza `msg->msgid` bytes sobre él, de modo que:

- con un `msgid` menor que 37 se envía un trozo del final del literal, sin el
  número que se pretendía mostrar;
- con un `msgid` mayor —el caso habitual, los identificadores de MAVLink llegan a
  centenares— se lee **fuera del literal** y se transmiten a tierra bytes
  arbitrarios de flash hasta topar con un `\0`.

Se dispara con cualquier mensaje que no esté contemplado en el `switch`, que es
justo para lo que existe la rama `default`.

El arreglo es formatear el número en un búfer propio, con el tamaño acotado
(`snprintf` sobre un `char[]` local) antes de llamar a `sendStatusText`, teniendo
en cuenta que `STATUSTEXT` corta el texto a 50 caracteres y que la pila de
`TaskMavlink` son 256 palabras.

Conviene revisarlo junto a *[Compilación debug y release...]*: si la traza de
protocolo acaba imprimiendo el `msgid` por consola, el formateo del número debería
resolverse una sola vez y no en dos sitios.

### El registro de la SD nunca guarda los datos de la batería

**Estado:** definida
**Ámbito:** `src/logger.cpp`

En `src/logger.cpp` el inicializador de `Data` rellena `unixtime`, `uptime` y
`system`, pero **no `energy`**. El fichero ni siquiera incluye `Battery.h` ni
declara `extern Battery battery`. Al ser inicialización agregada, los miembros que
faltan quedan a cero, así que `src/sdwrite.cpp` escribe `millivolts: 0` y
`remaining: 0` en cada muestra, siempre.

Es decir: la telemetría de energía **no está en ninguno de los `.mpk` grabados
hasta ahora**, aunque el campo aparezca en el fichero. La tensión sí sale por
MAVLink en `BATTERY_STATUS`, así que el fallo pasa desapercibido con el GCS
delante; solo se nota al abrir los ficheros.

El arreglo es declarar el `extern`, incluir la cabecera y rellenar `energy` con
`battery.millivolts()` y `battery.remaining()`. `lib/Battery` ya cachea 125 ms, así
que llamarlo a 1 Hz desde `TaskLogger` no añade lecturas de ADC.

Al tocarlo, comprobar el high-water mark de `TaskLogger`: son 96 palabras, de las
más ajustadas del proyecto.


### El hook de desbordamiento de pila se cuelga antes de avisar

**Estado:** definida
**Ámbito:** `src/hooks.cpp`, `CLAUDE.md`

`vApplicationStackOverflowHook()` hace `taskDISABLE_INTERRUPTS()` y a continuación
`while (!Serial) {}`. El CDC USB necesita interrupciones para enumerar: sin un host
conectado —o sea, en vuelo— ese bucle no termina nunca y la placa queda muerta
**sin parpadear**. El `delay(2000)` posterior tiene el mismo problema, porque
depende del tick, que también acaba de quedar sin interrupciones.

O sea que el diagnóstico documentado solo funciona si ya había un PC enchufado, que
es justo el caso en el que menos falta hace.

Además el parpadeo son 2000 ms encendido y 2000 apagado: un periodo de 4 s, 0,25 Hz,
no los 0,5 Hz que dice `CLAUDE.md`.

Por decidir: si el aviso visual debe ir primero y el mensaje por serie después
(solo si el puerto ya estaba listo), o si conviene mover el parpadeo a manipulación
directa del registro del pin y un retardo por bucle de espera, sin depender de nada
que necesite interrupciones. Hay que actualizar `CLAUDE.md` con la frecuencia real
y con lo que realmente se puede esperar de este hook.

Se solapa con *[Añadir un watchdog]*: si hay watchdog, quedarse aquí parpadeando
para siempre deja de ser la respuesta obvia a un desbordamiento.


### `uptime` desborda a los ~49,7 días

**Estado:** propuesta
**Ámbito:** `src/logger.cpp`, `include/Data.h`

`uptime` se calcula como `xTaskGetTickCount() * portTICK_PERIOD_MS` sobre un
`uint32_t`. Con `configTICK_RATE_HZ` a 1000 y ticks de 32 bits, el contador da la
vuelta a los ~49,7 días. En una misión de meses el campo deja de significar nada
justo cuando empieza a ser interesante.

Los `vTaskDelayUntil` del firmware toleran el desbordamiento por diseño; el campo
del log, no.

Por decidir: llevar la cuenta de vueltas y guardar el uptime en 64 bits, o registrar
en su lugar un contador de arranques más el tiempo desde el último, que además
serviría para detectar los reinicios de *[Añadir un watchdog]*.


### El fallo del registro en SD es silencioso

**Estado:** propuesta
**Ámbito:** `lib/SdData`, `src/sdwrite.cpp`, `src/mavlink.cpp`

Si `SD.open` falla en `SdData::begin()`, el objeto queda sin fichero y `write()`
retorna sin hacer nada indefinidamente. Ni `begin()` ni `write()` devuelven nada, y
`TaskSdWrite` no puede distinguir «guardado» de «tirado a la basura».

Resultado: se puede perder el registro completo de la misión sin un solo aviso por
el enlace. `setup()` sí usa `configASSERT(SD.begin(9))`, pero eso solo cubre el
arranque; una tarjeta que falle o se desmonte después pasa desapercibida.

Por decidir: que `begin()`/`write()` devuelvan resultado y `TaskSdWrite` lo
propague; y por dónde se entera tierra —un `STATUSTEXT`, un campo en el heartbeat
de *[Reflejar el estado real del satélite en el heartbeat]*, o ambos—. Cuidado con
no inundar el enlace repitiendo el aviso a 1 Hz.


### `TaskSerialRead` sondea el puerto en lugar de esperar

**Estado:** propuesta
**Ámbito:** `src/serial.cpp`

El bucle de `TaskSerialRead` vacía lo disponible y hace `vTaskDelay(10 ms)`. A
57600 baudios eso son ~57 bytes por ciclo contra un búfer de recepción típico de
64: el margen es mínimo, y a más velocidad se pierden bytes en silencio —lo que
desde tierra se ve como tramas MAVLink corruptas intermitentes, de lo más difícil
de diagnosticar.

Encima la tarea es `PRIORITY_HIGHEST`, así que despierta cien veces por segundo
aunque no haya nada que leer.

Por decidir: si se pasa a una espera bloqueante de verdad (notificación de tarea
desde la recepción, o semáforo) o simplemente se acorta el periodo de sondeo. Lo
primero es lo correcto pero depende de lo que exponga el puerto elegido en
*[Mover el enlace MAVLink a `Serial1`...]*, así que conviene resolverlo después de
esa entrada.


### Que Dependabot vigile también las librerías de PlatformIO

**Estado:** definida
**Ámbito:** `.github/dependabot.yml`

`.github/dependabot.yml` solo declara el ecosistema `github-actions`. Las
dependencias de `lib_deps` en `platformio.ini` —MAVLink, ArduinoJson, RTClib,
Adafruit BusIO, SD, SolarCharger— no las vigila nadie: se actualizan cuando alguien
se acuerda.

Dependabot no tiene ecosistema para PlatformIO, así que hay que decidir la
alternativa: fijar versiones en `lib_deps` y revisarlas a mano de forma periódica,
o un job programado que compruebe si hay versiones nuevas y abra el aviso.

Ligado a esto: `lib_deps` no fija versiones de ninguna librería. Reproducir una
compilación de hace seis meses hoy no es posible, y una actualización rompiente de
cualquiera de las seis entra en el siguiente `pio run` sin avisar.


### Limpieza de restos menores

**Estado:** definida
**Ámbito:** `src/mavlink.cpp`, `src/logger.cpp`, `src/hooks.cpp`,
`test/test_main.cpp`

Cosas pequeñas, sin relación entre sí, que conviene quitar de en medio de una vez:

- `src/mavlink.cpp` declara `extern RTC_DS1307 rtc;`, un global que no existe en
  ningún sitio. No da error de enlace solo porque nadie lo usa.
- `src/logger.cpp` inicializa `Data` con la sintaxis GNU de etiquetas
  (`unixtime: ...`), una extensión que las versiones recientes de GCC rechazan en
  C++. Los inicializadores designados de C++20 (`.unixtime = ...`) son el
  equivalente estándar.
- `test/test_main.cpp` usa `StaticJsonDocument`, deprecado en ArduinoJson 7,
  mientras `src/sdwrite.cpp` ya usa `JsonDocument`.
- La constante `TEST_FILE_SIZE_MB` del test vale `1024UL`, que son bytes, no
  megabytes: el nombre engaña sobre lo que realmente se está probando.
- Falta un espacio en `"Overflow on" + String(pcTaskName)` de `src/hooks.cpp`.

## Hecho

_Vacío por ahora._
