#include "shell.h"
#include "buffer.h"

#include <unistd.h>   /* read, write, close, lseek, ftruncate, fsync */
#include <fcntl.h>    /* open, O_RDWR, O_CREAT, O_TRUNC */
#include <errno.h>    /* errno */
#include <stdio.h>    /* printf, perror, snprintf */
#include <stdlib.h>   /* malloc, realloc, free */
#include <string.h>   /* memmove, memcpy, strlen, strstr */

/* ====================================================================================
 * AYUDANTES INTERNOS
 * ==================================================================================== */

/**
 * write() NO garantiza escribir todos los bytes solicitados: puede retornar menos
 * por una señal, por el disco lleno o por límites del sistema de archivos. Si no
 * se reintenta, el archivo queda truncado silenciosamente y el editor reporta
 * éxito. Este bucle es la diferencia entre "escribí" y "creo que escribí".
 *
 * EINTR merece trato aparte: no es un error real, es "una señal interrumpió la
 * llamada antes de transferir nada". Se reintenta sin contarlo como fallo.
 */
static ssize_t escribir_todo(int fd, const char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t w = write(fd, buf + hechos, n - hechos);
        if (w == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        hechos += (size_t)w;
    }
    return (ssize_t)hechos;
}

/* Mismo razonamiento que escribir_todo(), en el sentido de la lectura.
   read() puede devolver menos bytes de los pedidos aunque falten por leer. */
static ssize_t leer_todo(int fd, char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t r = read(fd, buf + hechos, n - hechos);
        if (r == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;   /* fin de archivo */
        hechos += (size_t)r;
    }
    return (ssize_t)hechos;
}

/**
 * Garantiza espacio para al menos 'necesarias' descriptores.
 *
 * La capacidad se DUPLICA en lugar de crecer por una constante. Con duplicación,
 * agregar n líneas provoca log2(n) llamadas a realloc() y el costo amortizado por
 * línea es O(1). Creciendo de a uno serían n reallocs y O(n) por línea.
 */
static int asegurar_capacidad(Buffer *b, size_t necesarias) {
    if (necesarias <= b->cap) return 0;

    size_t nueva = b->cap ? b->cap : 16;
    while (nueva < necesarias) nueva *= 2;

    Linea *tmp = realloc(b->lineas, nueva * sizeof(Linea));
    if (tmp == NULL) {
        /* realloc devuelve NULL SIN liberar el bloque anterior: asignar
           directamente a b->lineas habría perdido el arreglo original. */
        perror("realloc");
        return -1;
    }
    b->lineas = tmp;
    b->cap    = nueva;
    return 0;
}

/* Copia 'len' bytes a un bloque nuevo terminado en '\0'. */
static char *duplicar(const char *src, size_t len) {
    char *dst = malloc(len + 1);
    if (dst == NULL) { perror("malloc"); return NULL; }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* ====================================================================================
 * CICLO DE VIDA
 * ==================================================================================== */

void buf_init(Buffer *b) {
    memset(b->filename, 0, sizeof(b->filename));
    b->lineas        = NULL;
    b->num           = 0;
    b->cap           = 0;
    b->cargado       = 0;
    b->dirty         = 0;
    b->newline_final = 1;
}

void buf_liberar(Buffer *b) {
    /* Cada línea tiene su propio bloque en el heap: hay que liberarlos uno a uno
       ANTES de liberar el arreglo de descriptores, o se pierden las direcciones. */
    if (b->lineas != NULL) {
        for (size_t i = 0; i < b->num; i++) {
            if (b->lineas[i].texto != NULL) {
                free(b->lineas[i].texto);
            }
        }
        free(b->lineas);
    }
    buf_init(b);
}

int buf_cargar(Buffer *b, const char *ruta) {
    /* Si ya había un documento cargado, se descarta su memoria primero. */
   
    if (ruta == NULL || ruta[0] == '\0') {
        return -1;
    }
    char ruta_segura[sizeof(b->filename)];
    memset(ruta_segura, 0, sizeof(ruta_segura));
    strncpy(ruta_segura, ruta, sizeof(ruta_segura) - 1);

    if (b->cargado) {
        buf_liberar(b);
    } else {
        buf_init(b);
    }

    /* --- 1. Abrir -------------------------------------------------------- */
    /* O_RDWR|O_CREAT y deliberadamente SIN O_TRUNC: truncar borraría el
       contenido justo al abrirlo. El 0644 solo aplica si el archivo se crea. */
    LOG_SYSCALL("open", "\"%s\", O_RDWR|O_CREAT, 0644", ruta_segura);
    int fd = open(ruta_segura, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("open");
        return -1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* --- 2. Tamaño ------------------------------------------------------- */
    /* lseek al final no se usa aquí para preparar una lectura, sino como forma
       de averiguar el tamaño: su valor de retorno ES la posición final, o sea
       el número de bytes del archivo. */
    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", fd);
    off_t tam = lseek(fd, 0, SEEK_END);
    if (tam == (off_t)-1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("lseek");
        close(fd);
        return -1;
    }
    LOG_SYSCALL_RESULT((long)tam);

    LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", fd);
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        perror("lseek");
        close(fd);
        return -1;
    }
    LOG_SYSCALL_RESULT(0);

    /* --- 3. Leer todo de una sola vez ------------------------------------ */
    char *crudo = NULL;
    ssize_t leidos = 0;
    if (tam > 0) {
        crudo = malloc((size_t)tam);
        if (crudo == NULL) {
            perror("malloc");
            close(fd);
            return -1;
        }
        LOG_SYSCALL("read", "%d, buffer, %ld", fd, (long)tam);
        leidos = leer_todo(fd, crudo, (size_t)tam);
        if (leidos == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("read");
            free(crudo);
            close(fd);
            return -1;
        }
        LOG_SYSCALL_RESULT(leidos);
    }

    /* --- 4. Cerrar de inmediato ------------------------------------------ */
    /* El descriptor no se conserva entre comandos: el documento ya vive en RAM.
       Un fd abierto sin uso es un recurso retenido y una fuente de estado
       desincronizado (posición del cursor, cambios externos al archivo). */
    
    LOG_SYSCALL("close", "%d", fd);
    int cres = close(fd);
    LOG_SYSCALL_RESULT(cres);

    /* --- 5. Trocear en descriptores de línea ------------------------------ */

    size_t inicio = 0;
    for (ssize_t i = 0; i < leidos; i++) {
        if (crudo[i] == '\n') {
            if (asegurar_capacidad(b, b->num + 1) == -1) { free(crudo); return -1; }
            size_t len = (size_t)i - inicio;
            char *txt = duplicar(crudo + inicio, len);
            if (txt == NULL) { free(crudo); return -1; }
            b->lineas[b->num].texto = txt;
            b->lineas[b->num].len   = len;
            b->num++;
            inicio = (size_t)i + 1;
        }
    }
    /* Cola sin '\n' final: es una línea válida y hay que registrarla. Se recuerda
       el hecho en newline_final para reproducir el archivo tal cual al guardar y
       no agregarle un salto de línea que el usuario nunca escribió. */
    if (leidos > 0 && inicio < (size_t)leidos) {
        if (asegurar_capacidad(b, b->num + 1) == -1) { free(crudo); return -1; }
        size_t len = (size_t)leidos - inicio;
        char *txt = duplicar(crudo + inicio, len);
        if (txt == NULL) { free(crudo); return -1; }
        b->lineas[b->num].texto = txt;
        b->lineas[b->num].len   = len;
        b->num++;
        b->newline_final = 0;
    } else {
        b->newline_final = 1;
    }

    if (crudo) free(crudo);  /* el texto ya fue copiado a bloques por línea */

    strncpy(b->filename, ruta_segura, sizeof(b->filename) - 1);
    b->filename[sizeof(b->filename) - 1] = '\0';
    b->cargado = 1;
    b->dirty   = 0;
    return 0;
}

/**
 * Serializa el documento a un único bloque contiguo listo para escribir.
 *
 * Por qué no un write() por línea: cada syscall cuesta un cambio a modo kernel.
 * Un archivo de 500 líneas serían 1000 llamadas. Armando el bloque en memoria
 * primero, el guardado completo cuesta UNA escritura.
 *
 * Devuelve el bloque (que el llamador debe liberar) y su tamaño en *out_tam.
 */
static char *serializar(const Buffer *b, size_t *out_tam) {
    size_t total = 0;
    for (size_t i = 0; i < b->num; i++) total += b->lineas[i].len + 1;  /* +1 por '\n' */
    if (b->num > 0 && !b->newline_final) total -= 1;                    /* sin salto final */

    /* malloc(0) puede devolver NULL legítimamente; se pide 1 byte para
       distinguir "documento vacío" de "fallo de asignación". */
    char *out = malloc(total ? total : 1);
    if (out == NULL) { perror("malloc"); return NULL; }

    size_t off = 0;
    for (size_t i = 0; i < b->num; i++) {
        memcpy(out + off, b->lineas[i].texto, b->lineas[i].len);
        off += b->lineas[i].len;
        if (i + 1 < b->num || b->newline_final) out[off++] = '\n';
    }

    *out_tam = total;
    return out;
}

int buf_guardar(Buffer *b, ModoGuardado modo) {
    if (!b->cargado) return -1;

    size_t total = 0;
    char *datos = serializar(b, &total);
    if (datos == NULL) return -1;

    int fd;

    if (modo == GUARDA_EN_SITIO) {
        /* ------------------------------------------------------------------
         * ESTRATEGIA A: escritura en sitio.
         * Se reescribe el archivo original desde el byte 0 y se recorta con
         * ftruncate() al nuevo tamaño. El ftruncate es indispensable: si el
         * documento encogió, sin él quedarían "bytes fantasma" del contenido
         * viejo pegados al final.
         *
         * LIMITACIÓN CONOCIDA: entre el write() y el ftruncate() el archivo en
         * disco no es ni el viejo ni el nuevo. Si el proceso muere ahí, el
         * original ya se perdió. Ver ESTRATEGIA B.
         * ------------------------------------------------------------------ */
        LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_CREAT, 0644", b->filename);
        fd = open(b->filename, O_WRONLY | O_CREAT, 0644);
        if (fd == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("open");
            free(datos);
            return -1;
        }
        LOG_SYSCALL_RESULT(fd);

        LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", fd);
        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("lseek");
            close(fd); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(0);

        LOG_SYSCALL("write", "%d, buffer, %zu", fd, total);
        if (escribir_todo(fd, datos, total) == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("write");
            close(fd); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(total);

        LOG_SYSCALL("ftruncate", "%d, %zu", fd, total);
        if (ftruncate(fd, (off_t)total) == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("ftruncate");
            close(fd); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(0);

        LOG_SYSCALL("close", "%d", fd);
        LOG_SYSCALL_RESULT(close(fd));

    } else {
        /* ------------------------------------------------------------------
         * ESTRATEGIA B: escritura atómica.
         * Se escribe un archivo temporal completo y recién entonces se pone en
         * el lugar del original con rename(). POSIX garantiza que reemplazar un
         * archivo con rename() dentro del MISMO sistema de archivos es atómico:
         * un observador ve el archivo viejo completo o el nuevo completo, jamás
         * un estado intermedio. Por eso el temporal se crea en el mismo
         * directorio (si cruzara de filesystem, rename fallaría con EXDEV).
         *
         * El fsync() antes del rename es lo que separa "a prueba de que el
         * proceso muera" de "a prueba de corte de energía": sin él, el rename
         * puede consolidarse antes de que los datos salgan del page cache.
         * ------------------------------------------------------------------ */
        char tmp[sizeof(b->filename) + 16];
        snprintf(tmp, sizeof(tmp), "%s.tmp", b->filename);

        LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_CREAT|O_TRUNC, 0644", tmp);
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("open");
            free(datos);
            return -1;
        }
        LOG_SYSCALL_RESULT(fd);

        LOG_SYSCALL("write", "%d, buffer, %zu", fd, total);
        if (escribir_todo(fd, datos, total) == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("write");
            close(fd); unlink(tmp); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(total);

        LOG_SYSCALL("fsync", "%d", fd);
        if (fsync(fd) == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("fsync");
            close(fd); unlink(tmp); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(0);

        LOG_SYSCALL("close", "%d", fd);
        LOG_SYSCALL_RESULT(close(fd));

        LOG_SYSCALL("rename", "\"%s\", \"%s\"", tmp, b->filename);
        if (rename(tmp, b->filename) == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            perror("rename");
            unlink(tmp); free(datos); return -1;
        }
        LOG_SYSCALL_RESULT(0);
    }

    free(datos);
    b->dirty = 0;
    return 0;
}

/* ====================================================================================
 * CONSULTA
 * ==================================================================================== */

size_t buf_num_lineas(const Buffer *b) {
    return b->num;
}

const char *buf_linea(const Buffer *b, size_t n, size_t *len) {
    if (n == 0 || n > b->num) return NULL;   /* n es 1-indexado */
    if (len) *len = b->lineas[n - 1].len;
    return b->lineas[n - 1].texto;
}

/* ====================================================================================
 * EDICIÓN  (ninguna de estas funciones ejecuta una sola syscall)
 * ==================================================================================== */

int buf_agregar(Buffer *b, const char *texto) {
    if (asegurar_capacidad(b, b->num + 1) == -1) return -1;

    size_t len = strlen(texto);
    char *txt = duplicar(texto, len);
    if (txt == NULL) return -1;

    b->lineas[b->num].texto = txt;
    b->lineas[b->num].len   = len;
    b->num++;

    /* Si el archivo venía sin salto de línea final, la línea nueva obliga a
       cerrar la anterior: el documento vuelve a terminar en '\n'. */
    b->newline_final = 1;
    b->dirty = 1;
    return 0;
}

int buf_borrar(Buffer *b, size_t n) {
    if (n == 0 || n > b->num) return -1;

    free(b->lineas[n - 1].texto);   /* primero el texto, o se pierde la dirección */

    /* Se desplazan los DESCRIPTORES, no el texto. 16 bytes por línea sobre
       memoria contigua, en vez de mover el contenido completo del documento. */
    memmove(&b->lineas[n - 1],
            &b->lineas[n],
            (b->num - n) * sizeof(Linea));
    b->num--;

    b->dirty = 1;
    return 0;
}

int buf_insertar(Buffer *b, size_t n, const char *texto) {
    if (n == 0 || n > b->num + 1) return -1;   /* n == num+1 => al final */
    if (asegurar_capacidad(b, b->num + 1) == -1) return -1;

    size_t len = strlen(texto);
    char *txt = duplicar(texto, len);
    if (txt == NULL) return -1;

    /* Mismo memmove que en buf_borrar pero en sentido contrario: abre un hueco. */
    memmove(&b->lineas[n],
            &b->lineas[n - 1],
            (b->num - (n - 1)) * sizeof(Linea));

    b->lineas[n - 1].texto = txt;
    b->lineas[n - 1].len   = len;
    b->num++;

    b->newline_final = 1;
    b->dirty = 1;
    return 0;
}

size_t buf_buscar(const Buffer *b, const char *palabra, size_t desde) {
    if (desde == 0) desde = 1;
    for (size_t i = desde; i <= b->num; i++) {
        if (strstr(b->lineas[i - 1].texto, palabra) != NULL) return i;
    }
    return 0;   /* 0 = no encontrado (las líneas se numeran desde 1) */
}
