# Editor de Texto con Llamadas al Sistema (Linux)

Editor de texto en **C** que trabaja directamente contra el kernel de Linux mediante
**llamadas al sistema POSIX** (`open`, `read`, `write`, `lseek`, `ftruncate`, `fsync`,
`rename`, `close`), sin usar la capa de `stdio.h` (`fopen`, `fread`, `fwrite`, `fclose`)
para manipular el archivo.

El editor vive dentro de un shell educativo (`eafitOS`) que imprime en pantalla, estilo
`strace` simplificado, cada syscall ejecutada con sus parámetros y su valor de retorno.

> **La decisión central del proyecto:** el editor **no modifica el archivo en disco
> mientras editas**. El documento se carga a memoria al abrir, todas las operaciones
> ocurren sobre una estructura en RAM, y el disco se toca únicamente al guardar. Es el
> modelo de `vi`, `nano` y VS Code. La sección [Decisiones de diseño](#decisiones-de-diseño)
> explica por qué.

---

## Compilar y ejecutar

Requiere Linux con `gcc` y `make`.

```bash
make          # compila y produce el binario ./eafitOS
./eafitOS     # inicia el shell
make run      # atajo: compila y ejecuta
make clean    # borra los .o y el binario
./pruebas.sh  # batería de pruebas automatizadas (18 casos)
```

Dentro del shell, para entrar al editor:

```
Wordsito> editor
editor>
```

---

## Comandos del editor

| Comando       | Qué hace                                                  | Syscalls involucradas                                     |
| ------------- | --------------------------------------------------------- | --------------------------------------------------------- |
| `o <archivo>` | Carga el archivo a memoria (lo crea vacío si no existe)   | `open(2)`, `lseek(2)`, `read(2)`, `close(2)`              |
| `p`           | Imprime el documento completo                             | `write(2)` a FD 1                                          |
| `p <n>`       | Imprime solo la línea `n`                                 | `write(2)` a FD 1                                          |
| `a "<texto>"` | Agrega el texto como nueva línea al final                 | **ninguna** — solo memoria                                 |
| `d <n>`       | Elimina la línea `n`                                      | **ninguna** — solo memoria                                 |
| `w`           | Guarda en disco reescribiendo el archivo original         | `open(2)`, `lseek(2)`, `write(2)`, `ftruncate(2)`, `close(2)` |
| `w!`          | Guarda en disco de forma atómica                          | `open(2)`, `write(2)`, `fsync(2)`, `close(2)`, `rename(2)` |
| `q`           | Sale; **avisa si hay cambios sin guardar**                | ninguna                                                    |
| `q!`          | Sale descartando los cambios                              | ninguna                                                    |

**Importante:** el texto de `a` debe ir **entre comillas dobles** para que el tokenizador
`parse_line()` lo entregue como un solo argumento aunque contenga espacios.

### Sesión de ejemplo (salida real)

```
Wordsito> editor

--- Editor de Texto (sesión interactiva) ---
  o <archivo>   Carga el archivo a memoria (lo crea si no existe).
  p [n]         Imprime todo el documento, o solo la línea n.
  a "<texto>"   Agrega el texto como nueva línea al final.
  d <n>         Elimina la línea n.
  w             Guarda los cambios en disco (en sitio).
  w!            Guarda de forma atómica (temporal + rename).
  q             Sale; avisa si hay cambios sin guardar.
  q!            Sale descartando los cambios.

editor> o notas.txt
[syscall] open("notas.txt", O_RDWR|O_CREAT, 0644) ... = 3
[syscall] lseek(3, 0, SEEK_END) ... = 0
[syscall] lseek(3, 0, SEEK_SET) ... = 0
[syscall] close(3) ... = 0
'notas.txt' cargado en memoria (0 líneas).

editor> a "primera linea"
Línea 1 agregada (en memoria).

editor> a "segunda linea"
Línea 2 agregada (en memoria).

editor> p
primera linea
segunda linea

editor> d 1
Línea 1 eliminada (en memoria). Quedan 1.

editor> w
[syscall] open("notas.txt", O_WRONLY|O_CREAT, 0644) ... = 3
[syscall] lseek(3, 0, SEEK_SET) ... = 0
[syscall] write(3, buffer, 14) ... = 14
[syscall] ftruncate(3, 14) ... = 0
[syscall] close(3) ... = 0
'notas.txt' guardado (1 líneas).

editor> q
Saliendo del editor...
```

**Lo que hay que notar en esa traza:** entre `a` y `d` no aparece **ni una sola syscall**.
Tres comandos de edición, cero accesos a disco. Todo el costo de E/S se concentra en `o`
(una lectura) y `w` (una escritura). Ese vacío en la traza es la evidencia visible del
modelo de diseño.

---

## Decisiones de diseño

Esta sección documenta las decisiones arquitectónicas del proyecto, con las alternativas
que se descartaron y el razonamiento detrás de cada una.

### 1. Por qué el documento vive en memoria y no se edita en disco

**La alternativa descartada.** La implementación original mantenía el descriptor de archivo
abierto durante toda la sesión y cada comando modificaba el archivo real de inmediato: `d`
leía el archivo completo, hacía `memmove()` sobre los bytes, reescribía desde el offset 0 y
recortaba con `ftruncate()`.

**Los tres problemas de ese modelo.**

*No es atómico.* Borrar una línea son tres syscalls encadenadas: `lseek(0)` → `write(nuevo)`
→ `ftruncate(nuevo_tamaño)`. Entre el `write` y el `ftruncate` el archivo en disco contiene
el contenido nuevo **más la cola vieja pegada al final**. Si el proceso muere en ese instante
—Ctrl+C, un `kill`, un corte de energía, el disco lleno— el usuario se queda con un archivo
que no es ni el viejo ni el nuevo, y sin ninguna copia a la cual volver, porque el original
ya fue sobrescrito.

*No hay reversibilidad.* Cada comando es definitivo. No existe "cancelar", porque no hay
ningún estado anterior guardado en ningún lado.

*El costo de E/S es desproporcionado.* Cada `d` lee el archivo completo y lo reescribe
completo. Borrar 10 líneas de un archivo de 1 MB mueve ~20 MB entre el proceso y el kernel.
Con el documento en memoria, esas 10 ediciones son 10 `memmove()` en RAM y **una** escritura
al final.

**La decisión.** Separar el *modelo del documento* de su *persistencia*. `o` carga, los
comandos de edición operan sobre la estructura en RAM, y `w` persiste. El precio es que
aparece un comando que el enunciado no pedía (`w`) y que `q` debe verificar cambios
pendientes; a cambio, el archivo del usuario nunca queda en un estado intermedio.

### 2. Por qué un arreglo de descriptores y no una lista enlazada

La opción intuitiva para un editor es una lista doblemente enlazada de líneas —es lo que usa
GNU nano. Se descartó tras analizar el patrón de acceso real de los comandos.

**El argumento.** Todos los comandos direccionan por **número absoluto de línea**: `p 3`,
`d 5`, `i 12`. Ninguno tiene concepto de cursor o posición actual. La ventaja de una lista
enlazada es desenlazar en O(1), pero **solo cuando ya se tiene el puntero al nodo**; aquí hay
que llegar recorriendo desde `head`, que es O(n) con *pointer chasing* —cada salto es a una
dirección arbitraria del heap y por tanto un fallo de caché potencial. Se paga el costo sin
cobrar el beneficio.

**La estructura elegida.**

```c
typedef struct {
    char  *texto;   /* la línea, SIN el '\n' */
    size_t len;
} Linea;            /* 16 bytes: un puntero y un size_t */

typedef struct {
    Linea *lineas;  /* arreglo dinámico de DESCRIPTORES */
    size_t num;     /* líneas en uso */
    size_t cap;     /* líneas con espacio reservado */
    ...
} Buffer;
```

```
lineas[]  ->  [ptr,len][ptr,len][ptr,len][ptr,len]   contiguo, 16 B por entrada
                  |         |        |        |
                  v         v        v        v
heap:          "hola"   "mundo"  "tercera"  "final"  disperso, y NUNCA se mueve
```

**Por qué su desventaja aparente no lo es.** Borrar o insertar exige un `memmove()`, que es
O(n). Pero el arreglo guarda **descriptores, no texto**: borrar la línea 5 de un archivo de
10.000 líneas desplaza ~160 KB de descriptores contiguos —a velocidad de ancho de banda de
memoria, con prefetch perfecto y vectorización— en lugar de mover megabytes de contenido.
El texto de cada línea vive aparte en el heap y no se mueve nunca.

**La lección general:** la complejidad asintótica no es lo único que importa. Dos algoritmos
O(n) pueden diferir en un orden de magnitud según su localidad de memoria.

| Operación | Lista enlazada           | Arreglo de descriptores          |
| --------- | ------------------------ | -------------------------------- |
| `p n`     | O(n) con pointer chasing | **O(1)**, una indexación         |
| `d n`     | O(n) + O(1) desenlazar   | O(1) + `memmove` de 16 B/línea   |
| `a`       | O(1) con puntero `tail`  | O(1) amortizado                  |

**Lo que se pierde.** El acceso aleatorio barato tiene como contraparte que insertar al
principio de un archivo enorme desplaza todos los descriptores. Para archivos de texto de
tamaño razonable el intercambio es claramente favorable. Estructuras que optimizan ambos
casos —*gap buffer* (Emacs), *piece table* (VS Code), *rope*— existen, pero su complejidad
no se justifica en este alcance.

### 3. Por qué dos estrategias de guardado

`w` y `w!` producen **exactamente el mismo archivo**. Se diferencian en la ruta por la que
los bytes llegan al disco, y por tanto en qué ocurre si el proceso se interrumpe a la mitad.

| | `w` (en sitio) | `w!` (atómico) |
| --- | --- | --- |
| Escribe sobre | el archivo original | un temporal `archivo.tmp` |
| Syscalls | `open`, `lseek`, `write`, `ftruncate`, `close` | `open`, `write`, `fsync`, `close`, `rename` |
| Si se interrumpe | el original queda **corrupto** | el original queda **intacto** |
| Inodo resultante | el mismo | uno nuevo |
| Conserva permisos/dueño/hardlinks | sí | **no** (habría que replicarlos con `fchmod`/`fchown`) |

**Cómo funciona el modo atómico.** En Unix, un archivo no "tiene" un nombre: los datos y
metadatos viven en un **inodo**, y un directorio es una tabla que asocia nombres a números de
inodo. `rename()` **no copia un solo byte de contenido**: solo reescribe esa tabla para que
el nombre `notas.txt` apunte al inodo nuevo, y elimina la entrada del temporal. El inodo
viejo se queda sin enlaces y el sistema de archivos libera sus bloques.

```
ANTES                                  DESPUÉS
notas.txt      ──► inodo 4401          notas.txt  ──► inodo 4402  (contenido nuevo)
notas.txt.tmp  ──► inodo 4402          (inodo 4401: bloques liberados)
```

Esa operación es atómica por dos razones que se refuerzan: ambas modificaciones caen en el
mismo bloque de directorio, y los sistemas de archivos con journal (ext4, XFS, btrfs) la
registran como **una sola transacción**. Tras un corte de energía, la recuperación aplica la
transacción completa o la descarta entera; nunca a medias. Por eso `rename()` tarda lo mismo
en un archivo de 1 KB que en uno de 100 MB.

**Por qué el temporal va en el mismo directorio.** Los números de inodo son únicos *por
sistema de archivos*. Si el temporal estuviera en `/tmp` y el original en `$HOME`, podrían
ser filesystems distintos y no habría inodo que intercambiar: `rename` fallaría con `EXDEV`.

**Por qué el `fsync()` antes del `rename()`.** Cuando `write()` retorna, los datos suelen
estar en el *page cache* del kernel, no en el disco físico. Sin `fsync`, un corte de energía
podría dejar el `rename` ya consolidado pero los datos del temporal aún en RAM: el nombre
correcto apuntando a un inodo vacío. `fsync` fuerza la bajada y no retorna hasta que el disco
confirma, estableciendo el orden correcto.

**Por qué se implementaron las dos y no solo la mejor.** El enunciado nombra `ftruncate()`
como syscall del proyecto, y esa llamada **solo aparece en la ruta en sitio**: en el modo
atómico el temporal se crea vacío con `O_TRUNC` y no hay nada que recortar. Mantener ambas
rutas cumple el requisito y, más importante, permite demostrar empíricamente la diferencia:

```bash
ls -i notas.txt   # anota el número de inodo
# ... editar y guardar con w   -> el inodo se MANTIENE
# ... editar y guardar con w!  -> el inodo CAMBIA
```

Ese número cambiando es la prueba observable de que son dos mecanismos distintos.

### 4. Por qué el editor es una categoría nueva del shell

El enunciado exige decidir y justificar si el editor entra en una categoría existente del
shell o en una nueva. Se creó la categoría **`editor`** en la tabla `commands[]` de `main.c`.

**El criterio no es temático sino de contrato de ejecución.** Todos los comandos existentes
del shell son **atómicos**: reciben `argc`/`argv`, abren, operan, cierran y devuelven el
control en una sola invocación. El editor abre una **sesión**: toma el control de `stdin`,
corre su propio bucle REPL anidado con prompt propio, y mantiene estado (el documento en
memoria) entre un comando interno y el siguiente.

Clasificarlo bajo `datos` —que agrupa `d_create`, `d_read`, `d_copy`— haría que `help datos`
mezclara comandos con dos semánticas de ejecución incompatibles. Un usuario que ejecuta
`d_read archivo` recupera el prompt; uno que ejecuta `editor` queda dentro de otro programa.
Esa diferencia es más relevante para el usuario que el hecho de que ambos manipulen archivos.

### 5. Por qué el descriptor se cierra de inmediato

`buf_cargar()` cierra el `fd` apenas termina de leer, en lugar de mantenerlo abierto durante
la sesión como hacía la versión anterior.

Mantenerlo abierto obligaba a que **cada comando empezara con un `lseek()`** para reposicionar
el cursor, porque ningún comando sabía dónde lo había dejado el anterior. Eso es estado
compartido implícito: cualquier comando nuevo que olvidara el `lseek` corrompería al
siguiente. Además, un descriptor abierto sin usar es un recurso retenido y una fuente de
desincronización si el archivo cambia por fuera.

Con el documento en RAM, el descriptor no aporta nada entre comandos. Cerrarlo hace que cada
operación sea independiente y elimina toda una clase de bugs.

---

## Detalles de implementación que importan

Cinco puntos de `buffer.c` que no son obvios y que son fuente frecuente de errores.

### `write()` no escribe todo lo que le pides

```c
static ssize_t escribir_todo(int fd, const char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t w = write(fd, buf + hechos, n - hechos);
        if (w == -1) {
            if (errno == EINTR) continue;   /* señal, no error: reintentar */
            return -1;
        }
        hechos += (size_t)w;
    }
    return (ssize_t)hechos;
}
```

POSIX permite que `write()` transfiera **menos bytes de los solicitados** —por una señal, por
disco lleno o por límites del sistema de archivos. Sin este bucle, escribir 800 de 1000 bytes
dejaría el archivo truncado y el editor reportaría éxito. `EINTR` se trata aparte porque no
es un fallo: significa que una señal interrumpió la llamada **antes de transferir nada**.
`leer_todo()` es el espejo, con `if (r == 0) break;` porque en `read` un 0 significa fin de
archivo, no error.

### La capacidad se duplica, no crece de a uno

```c
size_t nueva = b->cap ? b->cap : 16;
while (nueva < necesarias) nueva *= 2;

Linea *tmp = realloc(b->lineas, nueva * sizeof(Linea));
if (tmp == NULL) { perror("realloc"); return -1; }
b->lineas = tmp;
```

Creciendo de a una posición, agregar *n* líneas costaría *n* reallocs con posible copia
completa: O(n²). Duplicando, son log₂(n) reallocs y el costo se reparte: **O(1) amortizado**
por línea.

La variable `tmp` no es cosmética. Si `realloc` falla devuelve `NULL` **sin liberar el bloque
anterior**; escribir `b->lineas = realloc(b->lineas, ...)` habría machacado el único puntero
al arreglo viejo, perdiendo el documento completo y filtrando la memoria.

### `memmove`, nunca `memcpy`

```c
memmove(&b->lineas[n - 1], &b->lineas[n], (b->num - n) * sizeof(Linea));
```

`memcpy` asume que origen y destino **no se solapan** y puede copiar en cualquier orden. Aquí
sí se solapan: se mueve `lineas[2..3]` sobre `lineas[1..2]`. Usar `memcpy` es comportamiento
indefinido, y lo peligroso es que suele *parecer* correcto hasta que cambia el nivel de
optimización del compilador.

### El orden de los `free()`

```c
free(b->lineas[n - 1].texto);   /* PRIMERO el texto de la línea */
memmove(...);                   /* después se desplazan los descriptores */
```

Una vez desplazados los descriptores, la dirección del texto borrado ya no está en ninguna
parte: sería una fuga irrecuperable. Lo mismo aplica en `buf_liberar()`, que libera cada
`texto` antes que el arreglo `lineas`.

### El `\n` final que el usuario no escribió

Un archivo puede terminar o no en salto de línea, y **eso es contenido del usuario**. El
buffer registra el hecho al cargar:

```c
if (leidos > 0 && inicio < (size_t)leidos) {
    /* quedaron bytes tras el último '\n': son una línea válida */
    b->newline_final = 0;
}
```

Sin este flag, abrir un archivo sin salto final y guardarlo sin editar nada le agregaría un
byte. Un editor que modifica lo que no se le pidió modificar está roto, aunque el cambio
parezca inofensivo.

---

## Estructura del proyecto

```
Editor_de_Texto/
├── Makefile          # objetivos: all, run, clean
├── pruebas.sh        # batería de 18 pruebas automatizadas
├── shell.h           # macros de color, LOG_SYSCALL, struct Command, prototipos
├── buffer.h          # EL CONTRATO: tipos del buffer y API pública
├── buffer.c          # LA ESTRUCTURA DE DATOS + todas las syscalls del editor
├── cat_editor.c      # EL EDITOR: parser y despachador (cero syscalls de archivo)
├── main.c            # REPL principal, tabla de comandos, parse_line(), help
├── cat_datos.c       # d_create, d_read, d_info, d_copy, d_append
├── cat_memoria.c     # m_sbrk, m_mmap, m_info
├── cat_monitoreo.c   # p_fork, p_exec, p_kill, p_monitor
├── cat_listado.c     # l_dir
└── cat_util.c        # saludar, despedir, hora, fecha
```

**La separación `buffer.c` / `cat_editor.c` es deliberada.** `cat_editor.c` no contiene ni
una llamada al sistema sobre archivos: es exclusivamente parser y despachador. Toda la
manipulación de datos y el acceso a disco están en `buffer.c`. Esto permite que el módulo del
buffer se pruebe de forma independiente y que dos personas trabajen en paralelo sin tocar los
mismos archivos: `buffer.h` es la frontera contractual entre ambos.

---

## Pruebas

```bash
./pruebas.sh              # 18 casos funcionales y de borde
./pruebas.sh --valgrind   # añade verificación de fugas de memoria
```

Casos cubiertos, agrupados por lo que validan:

**Comandos base** — `a` agrega al final; `d` borra la primera, una del medio y la última
línea; `p n` imprime la línea correcta.

**Casos de borde** — archivo cuya última línea no termina en `\n` (guardar no le agrega uno);
archivo inexistente (`o` lo crea); archivo vacío (`p` no rompe); borrar todas las líneas
(`ftruncate` deja 0 bytes, sin bytes fantasma); **el disco permanece intacto si no se ejecuta
`w`**; `q` advierte con cambios pendientes; `w!` produce el mismo resultado que `w` y no deja
el `.tmp` huérfano.

**Manejo de errores** — línea fuera de rango; comandos sin archivo abierto; argumentos
faltantes, no numéricos o negativos; comando desconocido.

La compilación usa `-Wall -Wextra` y no produce warnings.

---

## Limitaciones conocidas

Documentadas explícitamente porque conocer los límites de una decisión es parte de haberla
tomado bien.

**El `!` tiene dos significados distintos.** En `q!` significa "forzar, ignorando la
advertencia"; en `w!` significa "usar la otra estrategia de escritura". Es una inconsistencia
de nomenclatura detectada después de implementar ambos comandos. La corrección propuesta es
renombrar `w!` a `wa` ("write atómico"), dejando el `!` con un único significado en todo el
editor.

**El modo atómico no preserva metadatos.** Como `rename()` deja un inodo nuevo en el lugar del
viejo, se pierden permisos, dueño, grupo, hard links y atributos extendidos del original. La
solución sería hacer `stat()` sobre el original y aplicar `fchmod()`/`fchown()` sobre el
temporal antes del `rename`. No está implementado.

**El documento completo vive en RAM.** Un archivo más grande que la memoria disponible no se
puede abrir. Los editores que manejan archivos arbitrariamente grandes cargan por demanda
(*mmap* o paginación manual), lo cual está fuera del alcance de este proyecto.

**Sin undo.** El arreglo de descriptores no guarda historial. Añadirlo requeriría snapshots
del arreglo o migrar a un *piece table*, donde los buffers de texto son inmutables y deshacer
cuesta solo restaurar la lista de piezas anterior.

**Sin control de concurrencia.** Si otro proceso modifica el archivo mientras está cargado en
memoria, `w` sobrescribe esos cambios sin avisar. Detectarlo requeriría comparar el `mtime`
del inodo antes de guardar, o bloqueo con `fcntl(F_SETLK)`.
