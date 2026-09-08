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

## Por modificar

_Vacío por ahora._

## Hecho

_Vacío por ahora._
