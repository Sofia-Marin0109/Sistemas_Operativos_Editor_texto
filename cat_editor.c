#include "shell.h"
#include "buffer.h"

#include <unistd.h>   /* write */
#include <stdio.h>    /* printf, fgets, fflush */
#include <stdlib.h>   /* atoi */
#include <string.h>   /* strcmp */

/**
 * ====================================================================================
 * CATEGORÍA: editor        COMANDO DE ENTRADA: editor
 * ====================================================================================
 *
 * JUSTIFICACIÓN DE LA INTEGRACIÓN CON EL SHELL
 * --------------------------------------------
 * Se creó una categoría NUEVA ("editor") en la tabla commands[] de main.c en
 * lugar de ubicarlo bajo "datos". El criterio no es temático sino de contrato de
 * ejecución: todos los comandos existentes del shell son ATÓMICOS -reciben argc/
 * argv, abren, operan, cierran y devuelven el control en una sola invocación.
 * El editor abre una SESIÓN: toma el control de stdin, corre su propio bucle REPL
 * anidado con prompt propio y mantiene estado (el documento en memoria) entre un
 * comando interno y el siguiente. Clasificarlo bajo "datos" haría que 'help datos'
 * mezclara comandos con dos semánticas de ejecución incompatibles.
 *
 * MODELO DE EDICIÓN
 * -----------------
 * Este archivo NO contiene lógica de manipulación de datos ni llamadas al sistema:
 * es exclusivamente el parser y despachador de comandos. Toda la estructura de
 * datos y el acceso a disco están en buffer.c. Esa separación es la que permite
 * que ambos integrantes trabajen en paralelo sin tocar los mismos archivos.
 */

/* Imprime una línea del documento al descriptor 1 (stdout).
 *
 * Se usa write(1, ...) en lugar de printf porque el enunciado exige volcar el
 * contenido por llamada al sistema. Como printf escribe en el buffer de stdio
 * (espacio de usuario) y write va directo al kernel, sin el fflush(stdout) previo
 * los dos flujos salen desordenados: el contenido aparecería ANTES de las trazas.
 */
static void imprimir_linea(const char *texto, size_t len) {
    fflush(stdout);
    write(1, texto, len);
    write(1, "\n", 1);
}

static void mostrar_ayuda(void) {
    printf(COLOR_TITLE "\n--- Editor de Texto (sesión interactiva) ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "         Carga el archivo a memoria (lo crea si no existe).\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "               Imprime todo el documento, o solo la línea n.\n");
    printf("  " COLOR_PROMPT "a [\"<texto>\"]" COLOR_RESET "     Agrega texto al final. Sin comillas entra a modo párrafo.\n");
    printf("  " COLOR_PROMPT "i <n> [\"<texto>\"]" COLOR_RESET " Inserta texto en la línea n desplazando el resto.\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "               Elimina la línea n.\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "         Busca subcadenas e imprime las líneas coincidentes.\n");
    printf("  " COLOR_PROMPT "w" COLOR_RESET "                   Guarda los cambios en disco (en sitio).\n");
    printf("  " COLOR_PROMPT "w!" COLOR_RESET "                  Guarda de forma atómica (temporal + rename).\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "                   Sale; avisa si hay cambios sin guardar.\n");
    printf("  " COLOR_PROMPT "q!" COLOR_RESET "                  Sale descartando los cambios.\n");
    printf(COLOR_INFO "\n Tip: Si usas 'a' o 'i <n>' sin texto, entra a modo multilínea; finaliza con '.' en una línea sola.\n\n" COLOR_RESET);
}

/* Modo interactivo para escribir múltiples líneas hasta que el usuario digite '.' */
static void leer_parrafo_multilinea(Buffer *buf, int insertar_en_pos) {
    printf(COLOR_INFO "(Modo párrafo activo: escribe tu contenido libremente. Termina con un solo punto '.' en una línea)\n" COLOR_RESET);
    char block[2048];
    int lineas_insertadas = 0;

    while (1) {
        printf(COLOR_PROMPT "... " COLOR_RESET);
        fflush(stdout);

        if (fgets(block, sizeof(block), stdin) == NULL) break;
        block[strcspn(block, "\r\n")] = '\0';

        if (strcmp(block, ".") == 0) {
            break;
        }

        if (insertar_en_pos > 0) {
            buf_insertar(buf, (size_t)(insertar_en_pos + lineas_insertadas), block);
        } else {
            buf_agregar(buf, block);
        }
        lineas_insertadas++;
    }
    printf(COLOR_RESULT "Se agregaron %d líneas al buffer.\n" COLOR_RESET, lineas_insertadas);
}

int cmd_editor(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Buffer buf;
    buf_init(&buf);

    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strncpy(hostname, "eafitOS", sizeof(hostname) - 1);
    }

    mostrar_ayuda();

    char line[4096];
    char *eargv[16];

    while (1) {
        if (buf.cargado && buf.filename[0] != '\0') {
            printf(COLOR_PROMPT "%s:%s%s> " COLOR_RESET, hostname, buf.filename, buf.dirty ? "*" : "");
        } else {
            printf(COLOR_PROMPT "%s:editor> " COLOR_RESET, hostname);
        }
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) break;   /* Ctrl+D */

        /* Soporte para argumentos con comillas multilínea */
        int quote_count = 0;
        for (char *c = line; *c; c++) {
            if (*c == '"') quote_count++;
        }
        while ((quote_count % 2) != 0) {
            char extra[1024];
            printf(COLOR_PROMPT "quote> " COLOR_RESET);
            fflush(stdout);
            if (fgets(extra, sizeof(extra), stdin) == NULL) break;
            for (char *c = extra; *c; c++) {
                if (*c == '"') quote_count++;
            }
            if (strlen(line) + strlen(extra) < sizeof(line) - 1) {
                strcat(line, extra);
            } else {
                break;
            }
        }

        int eargc = parse_line(line, eargv);
        if (eargc == 0) continue;

        /* ================================================================
         * q / q!  -- salir de la sesión
         * El chequeo de 'dirty' es la contraparte necesaria de editar en
         * memoria: sin él sería trivial perder el trabajo saliendo por
         * accidente. Es el mismo contrato de vi.
         * ================================================================ */
        if (strcmp(eargv[0], "q") == 0 || strcmp(eargv[0], "q!") == 0) {
            if (strcmp(eargv[0], "q") == 0 && buf.dirty) {
                printf(COLOR_ERROR "Hay cambios sin guardar.\n" COLOR_RESET);
                printf(COLOR_INFO "Usa 'w' para guardarlos o 'q!' para descartarlos.\n" COLOR_RESET);
                continue;
            }
            printf(COLOR_INFO "Saliendo del editor...\n" COLOR_RESET);
            break;
        }

        /* ================================================================
         * o <archivo>  -- cargar a memoria
         * ================================================================ */
        else if (strcmp(eargv[0], "o") == 0) {
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: o <archivo>\n" COLOR_RESET);
                continue;
            }
            if (buf.cargado && buf.dirty) {
                printf(COLOR_ERROR "'%s' tiene cambios sin guardar. Usa 'w' primero.\n" COLOR_RESET,
                       buf.filename);
                continue;
            }
            if (buf_cargar(&buf, eargv[1]) == 0) {
                printf(COLOR_RESULT "'%s' cargado en memoria (%zu líneas).\n" COLOR_RESET,
                       buf.filename, buf_num_lineas(&buf));
            }
        }

        /* ================================================================
         * p [n]  -- imprimir todo o una línea
         * ================================================================ */
        else if (strcmp(eargv[0], "p") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }

            if (eargc >= 2) {
                int n = atoi(eargv[1]);
                if (n <= 0) {
                    printf(COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
                    continue;
                }
                size_t len;
                const char *txt = buf_linea(&buf, (size_t)n, &len);
                if (txt == NULL) {
                    printf(COLOR_ERROR "La línea %d no existe (el documento tiene %zu líneas).\n" COLOR_RESET,
                           n, buf_num_lineas(&buf));
                    continue;
                }
                imprimir_linea(txt, len);
            } else {
                size_t total = buf_num_lineas(&buf);
                if (total == 0) {
                    printf(COLOR_INFO "(documento vacío)\n" COLOR_RESET);
                    continue;
                }
                for (size_t i = 1; i <= total; i++) {
                    size_t len;
                    const char *txt = buf_linea(&buf, i, &len);
                    imprimir_linea(txt, len);
                }
            }
        }

        /* ================================================================
         * a "<texto>"  -- agregar línea al final
         * ================================================================ */
        else if (strcmp(eargv[0], "a") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                leer_parrafo_multilinea(&buf, 0);
            } else {
                if (buf_agregar(&buf, eargv[1]) == 0) {
                    printf(COLOR_RESULT "Línea %zu agregada (en memoria).\n" COLOR_RESET,
                           buf_num_lineas(&buf));
                }
            }
        }

        /* ================================================================
         * i <n> ["<texto>"]  -- inserción arbitraria (Requisito Pareja)
         * ================================================================ */
        else if (strcmp(eargv[0], "i") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: i <n> [\"<texto>\"]\n" COLOR_RESET);
                continue;
            }
            int n = atoi(eargv[1]);
            if (n <= 0 || (size_t)n > buf_num_lineas(&buf) + 1) {
                printf(COLOR_ERROR "Posición %d fuera de rango (1 a %zu).\n" COLOR_RESET,
                       n, buf_num_lineas(&buf) + 1);
                continue;
            }

            if (eargc < 3) {
                leer_parrafo_multilinea(&buf, n);
            } else {
                if (buf_insertar(&buf, (size_t)n, eargv[2]) == 0) {
                    printf(COLOR_RESULT "Línea insertada en posición %d (en memoria).\n" COLOR_RESET, n);
                }
            }
        }

        /* ================================================================
         * d <n>  -- borrar línea
         * ================================================================ */
        else if (strcmp(eargv[0], "d") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
                continue;
            }
            int n = atoi(eargv[1]);
            if (n <= 0) {
                printf(COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
                continue;
            }
            if (buf_borrar(&buf, (size_t)n) == -1) {
                printf(COLOR_ERROR "La línea %d no existe (el documento tiene %zu líneas).\n" COLOR_RESET,
                       n, buf_num_lineas(&buf));
                continue;
            }
            printf(COLOR_RESULT "Línea %d eliminada (en memoria). Quedan %zu líneas.\n" COLOR_RESET,
                   n, buf_num_lineas(&buf));
        }

        /* ================================================================
         * s <palabra>  -- búsqueda simple
         * ================================================================ */
        else if (strcmp(eargv[0], "s") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
                continue;
            }
            const char *query = eargv[1];
            size_t coincidencia = 0;
            size_t actual = 1;
            int encontrados = 0;

            printf(COLOR_TITLE "--- Coincidencias para '%s' ---\n" COLOR_RESET, query);
            while ((coincidencia = buf_buscar(&buf, query, actual)) != 0) {
                size_t len;
                const char *txt = buf_linea(&buf, coincidencia, &len);
                printf(COLOR_PARAM "[Línea %zu]: " COLOR_RESET "%s\n", coincidencia, txt ? txt : "");
                actual = coincidencia + 1;
                encontrados++;
            }
            if (encontrados == 0) {
                printf(COLOR_INFO "No se encontraron ocurrencias.\n" COLOR_RESET);
            } else {
                printf(COLOR_RESULT "Total coincidencias: %d\n" COLOR_RESET, encontrados);
            }
        }

        /* ================================================================
         * w / w!  -- persistir a disco
         * ================================================================ */
        else if (strcmp(eargv[0], "w") == 0 || strcmp(eargv[0], "w!") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            ModoGuardado modo = (strcmp(eargv[0], "w!") == 0) ? GUARDA_ATOMICA : GUARDA_EN_SITIO;
            if (buf_guardar(&buf, modo) == 0) {
                printf(COLOR_RESULT "'%s' guardado (%zu líneas).\n" COLOR_RESET,
                       buf.filename, buf_num_lineas(&buf));
            }
        }

        /* ================================================================
         * clear  -- limpiar pantalla en sesión
         * ================================================================ */


        else if (strcmp(eargv[0], "clear") == 0) {
            printf("\033[H\033[J");
            continue;
        }

        else {
            printf(COLOR_ERROR "Comando '%s' no reconocido dentro del editor.\n" COLOR_RESET, eargv[0]);
            printf(COLOR_INFO "Válidos: o <archivo> | p [n] | a [\"<texto>\"] | i <n> [\"<texto>\"] | d <n> | s <palabra> | w | w! | q | q! | clear\n" COLOR_RESET);
        }
    }

    buf_liberar(&buf);
    return 0;
}
