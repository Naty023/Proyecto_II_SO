#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Tamaños y constantes para el checksum y la tabla hash */
#define TABLE_SIZE      2048                 /* Cantidad de buckets de la tabla de dispersion */
#define BLOCK_SIZE      (128 * 1024)         /* 128 KB por bloque */
#define MAX_BLOCKS      8                    /* 8 bloques -> 1 MB como máximo */
#define CHECKSUM_PARTS  8                    /* Checksum de 8 enteros */

/* Estructura que representa el checksum de un archivo */
typedef struct {
    unsigned int part[CHECKSUM_PARTS];       /* Cada posicion se calcula con un bloque de 128 KB */
} Checksum;

/* Registro almacenado en cada entrada de la tabla hash */
typedef struct FileRecord {
    char *path;                              /* Ruta completa del archivo */
    off_t size;                              /* Tamaño del archivo (para descartar rapido) */
    Checksum sum;                            /* Checksum calculado */
    struct FileRecord *next;                 /* Siguiente en la lista enlazada (colisiones) */
} FileRecord;

/* Tabla de dispersion global */
static FileRecord *hash_table[TABLE_SIZE];

/* Prototipos */
static void init_table(void);
static void free_table(void);
static int compute_checksum(const char *path, Checksum *out, off_t *size_out);
static int checksum_equal(const Checksum *a, const Checksum *b);
static unsigned int checksum_hash(const Checksum *sum);
static int files_equal(const char *path1, const char *path2, off_t size);
static void process_file(const char *path, int delete_mode);
static void traverse_directory(const char *dirpath, int delete_mode);

/* Inicializa la tabla hash poniendo todos los buckets en NULL */
static void init_table(void) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        hash_table[i] = NULL;
    }
}

/* Libera toda la memoria asociada a la tabla hash */
static void free_table(void) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        FileRecord *node = hash_table[i];
        while (node) {
            FileRecord *next = node->next;
            free(node->path);
            free(node);
            node = next;
        }
        hash_table[i] = NULL;
    }
}

/*
 * Calcula el checksum de un archivo.
 * - Lee el archivo en bloques de 128 KB.
 * - Usa a lo sumo 8 bloques (1 MB) para llenar las 8 partes del checksum.
 * - size_out devuelve el tamano real del archivo (via stat()).
 * Retorna 0 en exito, -1 en error.
 */
static int compute_checksum(const char *path, Checksum *out, off_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo obtener stat de %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        return -1; /* Solo se procesan archivos regulares */
    }

    *size_out = st.st_size;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Inicializar todas las partes del checksum en 0 */
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        out->part[i] = 0u;
    }

    unsigned char *buffer = (unsigned char *)malloc(BLOCK_SIZE);
    if (!buffer) {
        fprintf(stderr, "[ERROR] No hay memoria para buffer de lectura.\n");
        fclose(f);
        return -1;
    }

    /* Leer hasta 8 bloques de 128 KB (1 MB) */
    for (int block = 0; block < MAX_BLOCKS; ++block) {
        size_t nread = fread(buffer, 1, BLOCK_SIZE, f);
        if (nread == 0) {
            break; /* EOF alcanzado */
        }

        /* Algoritmo simple: hash polinomial por bloque */
        unsigned int acc = out->part[block];
        for (size_t i = 0; i < nread; ++i) {
            acc = acc * 31u + (unsigned int)buffer[i];
        }
        out->part[block] = acc;

        if (nread < BLOCK_SIZE) {
            break; /* Se llego al EOF antes de completar el bloque */
        }
    }

    free(buffer);
    fclose(f);
    return 0;
}

/* Compara dos checksums */
static int checksum_equal(const Checksum *a, const Checksum *b) {
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        if (a->part[i] != b->part[i]) {
            return 0;
        }
    }
    return 1;
}

/* Funcion de dispersion que mapea el checksum a un indice [0, TABLE_SIZE) */
static unsigned int checksum_hash(const Checksum *sum) {
    unsigned long h = 0;
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        h = h * 1315423911u + (unsigned long)sum->part[i];
    }
    return (unsigned int)(h % TABLE_SIZE);
}

/*
 * Compara dos archivos byte a byte para confirmar si son iguales.
 * size es el tamano esperado (ya verificado por stat en ambos archivos).
 * Retorna 1 si son iguales, 0 si son distintos, -1 si hay error de lectura.
 */
static int files_equal(const char *path1, const char *path2, off_t size) {
    FILE *f1 = fopen(path1, "rb");
    if (!f1) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir %s para comparar: %s\n", path1, strerror(errno));
        return -1;
    }
    FILE *f2 = fopen(path2, "rb");
    if (!f2) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir %s para comparar: %s\n", path2, strerror(errno));
        fclose(f1);
        return -1;
    }

    const size_t CMP_BUFFER = 64 * 1024; /* Buffer de comparacion de 64 KB */
    unsigned char *buf1 = (unsigned char *)malloc(CMP_BUFFER);
    unsigned char *buf2 = (unsigned char *)malloc(CMP_BUFFER);
    if (!buf1 || !buf2) {
        fprintf(stderr, "[ERROR] No hay memoria para buffers de comparacion.\n");
        fclose(f1);
        fclose(f2);
        free(buf1);
        free(buf2);
        return -1;
    }

    off_t remaining = size;
    while (remaining > 0) {
        size_t to_read = (remaining > (off_t)CMP_BUFFER) ? CMP_BUFFER : (size_t)remaining;
        size_t n1 = fread(buf1, 1, to_read, f1);
        size_t n2 = fread(buf2, 1, to_read, f2);
        if (n1 != to_read || n2 != to_read) {
            fprintf(stderr, "[ADVERTENCIA] Error de lectura al comparar %s y %s.\n", path1, path2);
            free(buf1);
            free(buf2);
            fclose(f1);
            fclose(f2);
            return -1;
        }
        if (memcmp(buf1, buf2, to_read) != 0) {
            free(buf1);
            free(buf2);
            fclose(f1);
            fclose(f2);
            return 0; /* Son diferentes */
        }
        remaining -= (off_t)to_read;
    }

    free(buf1);
    free(buf2);
    fclose(f1);
    fclose(f2);
    return 1; /* Son iguales */
}

/*
 * Inserta un archivo en la tabla de dispersion o maneja el caso de duplicado.
 * - Si encuentra un archivo existente con mismo checksum y mismo tamano, se
 *   compara byte a byte.
 * - Si realmente son duplicados:
 *     - Siempre se guarda el primero que llego a la tabla.
 *     - Si delete_mode != 0, se borra el archivo "nuevo" (path).
 */
static void insert_or_handle_duplicate(const char *path, const Checksum *sum, off_t size, int delete_mode) {
    unsigned int index = checksum_hash(sum);
    FileRecord *current = hash_table[index];

    /* Buscar posibles duplicados en la lista enlazada de este bucket */
    while (current) {
        if (current->size == size && checksum_equal(&current->sum, sum)) {
            int eq = files_equal(current->path, path, size);
            if (eq == 1) {
                /* Confirmado archivo duplicado */
                printf("\n[Duplicado encontrado]\n");
                printf("  Original : %s\n", current->path);
                printf("  Duplicado: %s\n", path);

                if (delete_mode) {
                    if (unlink(path) == 0) {
                        printf("  Accion   : %s fue BORRADO.\n", path);
                    } else {
                        printf("  Accion   : No se pudo borrar %s: %s\n", path, strerror(errno));
                    }
                } else {
                    printf("  Accion   : Solo se reporta, no se borra.\n");
                }
                return; /* No insertamos el duplicado en la tabla */
            } else if (eq == 0) {
                /* Checksum igual pero contenido diferente, colision del hash. Continua buscando. */
            } else {
                /* Error al comparar, por seguridad seguimos y tratamos como distinto. */
            }
        }
        current = current->next;
    }

    /* No se encontraron duplicados, insertar nuevo registro al inicio de la lista */
    FileRecord *node = (FileRecord *)malloc(sizeof(FileRecord));
    if (!node) {
        fprintf(stderr, "[ERROR] No hay memoria para nuevo registro.\n");
        return;
    }

    node->path = strdup(path);
    if (!node->path) {
        fprintf(stderr, "[ERROR] No hay memoria para copiar el path.\n");
        free(node);
        return;
    }

    node->size = size;
    node->sum = *sum;
    node->next = hash_table[index];
    hash_table[index] = node;
}

/* Procesa un archivo individual: calcula checksum e inserta en la tabla */
static void process_file(const char *path, int delete_mode) {
    Checksum sum;
    off_t size;

    if (compute_checksum(path, &sum, &size) != 0) {
        return; /* No se pudo procesar este archivo */
    }

    insert_or_handle_duplicate(path, &sum, size, delete_mode);
}

/*
 * Recorre recursivamente un directorio.
 * - Para cada subdirectorio, se llama a si misma.
 * - Para cada archivo regular, llama a process_file().
 */
static void traverse_directory(const char *dirpath, int delete_mode) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir directorio %s: %s\n", dirpath, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        /* Ignorar . y .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        /* Construir la ruta completa: dirpath + "/" + name */
        size_t len_dir = strlen(dirpath);
        size_t len_name = strlen(name);
        int needs_slash = (len_dir > 0 && dirpath[len_dir - 1] != '/');
        size_t len_full = len_dir + (needs_slash ? 1 : 0) + len_name + 1;

        char *fullpath = (char *)malloc(len_full);
        if (!fullpath) {
            fprintf(stderr, "[ERROR] No hay memoria para construir path completo.\n");
            continue;
        }

        strcpy(fullpath, dirpath);
        if (needs_slash) {
            strcat(fullpath, "/");
        }
        strcat(fullpath, name);

        /* Determinar si es directorio o archivo */
        struct stat st;
        if (lstat(fullpath, &st) != 0) {
            fprintf(stderr, "[ADVERTENCIA] No se pudo hacer lstat de %s: %s\n", fullpath, strerror(errno));
            free(fullpath);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Directorio: llamada recursiva */
            traverse_directory(fullpath, delete_mode);
        } else if (S_ISREG(st.st_mode)) {
            /* Archivo regular: procesarlo */
            process_file(fullpath, delete_mode);
        } else {
            /* Otros tipos (sockets, pipes, enlaces simbolicos, etc.) se ignoran */
        }

        free(fullpath);
    }

    closedir(dir);
}

/* Funcion principal: procesa argumentos y lanza el recorrido del directorio. */
int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s <directorio_inicial> [-d]\n", argv[0]);
        fprintf(stderr, "  <directorio_inicial> : directorio donde se inicia la busqueda.\n");
        fprintf(stderr, "  -d                   : (opcional) si se indica, se borran los duplicados.\n");
        fprintf(stderr, "                         Si no se indica, solo se listan en pantalla.\n");
        return EXIT_FAILURE;
    }

    const char *start_dir = argv[1];
    int delete_mode = 0;

    if (argc == 3) {
        if (strcmp(argv[2], "-d") == 0 || strcmp(argv[2], "--delete") == 0) {
            delete_mode = 1;
        } else {
            fprintf(stderr, "Parametro desconocido: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    init_table();

    printf("Buscando archivos duplicados a partir de: %s\n", start_dir);
    printf("Modo: %s\n", delete_mode ? "listar y BORRAR duplicados" : "solo listar duplicados");

    traverse_directory(start_dir, delete_mode);

    free_table();

    return EXIT_SUCCESS;
}
