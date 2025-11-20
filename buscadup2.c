#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ----------------------------- Constantes ----------------------------- */
/* Tamaños y constantes para el checksum y la tabla hash */
#define TABLE_SIZE      2048          /* Cantidad de buckets de la tabla de dispersión */
#define BLOCK_SIZE      (128 * 1024)  /* 128 KB por bloque */
#define MAX_BLOCKS      8             /* 8 bloques -> 1 MB como máximo */
#define CHECKSUM_PARTS  8             /* Checksum de 8 enteros */

/* ----------------------- Estructuras de datos ------------------------- */
/* Estructura que representa el checksum de un archivo */
typedef struct {
    unsigned int part[CHECKSUM_PARTS];       /* Cada posición se calcula con un bloque de 128 KB */
} Checksum;

/* Registro almacenado en cada entrada de la tabla hash */
typedef struct FileRecord {
    char *path;                              /* Ruta completa del archivo */
    off_t size;                              /* Tamaño del archivo (para descartar rápido) */
    Checksum sum;                            /* Checksum calculado */
    struct FileRecord *next;                 /* Siguiente en la lista enlazada (colisiones) */
} FileRecord;

/* Tabla de dispersión global */
static FileRecord *hash_table[TABLE_SIZE];

/* ------------------------- Prototipos globales ------------------------ */
/* Asignación de memoria */
static void *xmalloc(size_t size);
static char *xstrdup(const char *s);

/* Manejo de archivos */
static FILE *open_file_read(const char *path);
static void  close_file(FILE *f);
static int   compute_checksum(const char *path, Checksum *out, off_t *size_out);
static int   files_equal(const char *path1, const char *path2, off_t size);

/* Tabla de dispersión */
static void init_table(void);
static void free_table(void);
static int  checksum_equal(const Checksum *a, const Checksum *b);
static unsigned int checksum_hash(const Checksum *sum);
static void insert_or_handle_duplicate(const char *path,
                                       const Checksum *sum,
                                       off_t size,
                                       int delete_mode);
static void process_file(const char *path, int delete_mode);

/* Manejo de directorios */
static void traverse_directory(const char *dirpath, int delete_mode);

/* ---------------------- Asignación de memoria ------------------------- */
/* En vez de usar malloc directamente en todo lado, centralizamos el manejo
 * de errores de memoria en estas funciones auxiliares. */
static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "[ERROR] No hay memoria suficiente.\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)xmalloc(len);
    memcpy(copy, s, len);
    return copy;
}

/* ------------------------ Manejo de archivos -------------------------- */
/* Abre un archivo en modo binario solo lectura y devuelve el FILE* */
static FILE *open_file_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir %s: %s\n",
                path, strerror(errno));
    }
    return f;
}

/* Cierra un archivo si el puntero no es NULL */
static void close_file(FILE *f) {
    if (f) {
        fclose(f);
    }
}

/*
 * Calcula el checksum de un archivo.
 * - Usa stat() para obtener el tamaño real del archivo (struct stat).
 * - Lee el archivo en bloques de 128 KB.
 * - Usa a lo sumo 8 bloques (1 MB) para llenar las 8 partes del checksum.
 * Retorna 0 en éxito, -1 en error.
 */
static int compute_checksum(const char *path, Checksum *out, off_t *size_out) {
    struct stat st;

    /* Uso de stat(): obtención de metadatos del archivo (tamaño, tipo, etc.) */
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo obtener stat de %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Solo procesamos archivos regulares */
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }

    *size_out = st.st_size;

    /* Apertura del archivo para lectura binaria */
    FILE *f = open_file_read(path);
    if (!f) {
        return -1;
    }

    /* Inicializar todas las partes del checksum en 0 */
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        out->part[i] = 0u;
    }

    /* Buffer de lectura para fread() */
    unsigned char *buffer = (unsigned char *)xmalloc(BLOCK_SIZE);

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
            break; /* Se llegó al EOF antes de completar el bloque */
        }
    }

    free(buffer);
    close_file(f);
    return 0;
}

/*
 * Compara dos archivos byte a byte para confirmar si son iguales.
 * size es el tamaño esperado (ya verificado por stat en ambos archivos).
 * Retorna 1 si son iguales, 0 si son distintos, -1 si hay error de lectura.
 */
static int files_equal(const char *path1, const char *path2, off_t size) {
    FILE *f1 = open_file_read(path1);
    if (!f1) {
        return -1;
    }

    FILE *f2 = open_file_read(path2);
    if (!f2) {
        close_file(f1);
        return -1;
    }

    /* Buffer de comparación (lectura secuencial con fread) */
    const size_t CMP_BUFFER = 64 * 1024;
    unsigned char *buf1 = (unsigned char *)xmalloc(CMP_BUFFER);
    unsigned char *buf2 = (unsigned char *)xmalloc(CMP_BUFFER);

    off_t remaining = size;
    while (remaining > 0) {
        size_t to_read =
            (remaining > (off_t)CMP_BUFFER) ? CMP_BUFFER : (size_t)remaining;

        size_t n1 = fread(buf1, 1, to_read, f1);
        size_t n2 = fread(buf2, 1, to_read, f2);

        if (n1 != to_read || n2 != to_read) {
            fprintf(stderr,
                    "[ADVERTENCIA] Error de lectura al comparar %s y %s.\n",
                    path1, path2);
            free(buf1);
            free(buf2);
            close_file(f1);
            close_file(f2);
            return -1;
        }

        if (memcmp(buf1, buf2, to_read) != 0) {
            free(buf1);
            free(buf2);
            close_file(f1);
            close_file(f2);
            return 0; /* Son diferentes */
        }

        remaining -= (off_t)to_read;
    }

    free(buf1);
    free(buf2);
    close_file(f1);
    close_file(f2);
    return 1; /* Son iguales */
}

/* ------------------- Funciones de tabla de dispersión ----------------- */
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

/* Compara dos checksums */
static int checksum_equal(const Checksum *a, const Checksum *b) {
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        if (a->part[i] != b->part[i]) {
            return 0;
        }
    }
    return 1;
}

/* Función de dispersión que mapea el checksum a un índice [0, TABLE_SIZE) */
static unsigned int checksum_hash(const Checksum *sum) {
    unsigned long h = 0;
    for (int i = 0; i < CHECKSUM_PARTS; ++i) {
        h = h * 1315423911u + (unsigned long)sum->part[i];
    }
    return (unsigned int)(h % TABLE_SIZE);
}

/*
 * Inserta un archivo en la tabla de dispersión o maneja el caso de duplicado.
 * - Si encuentra un archivo existente con mismo checksum y mismo tamaño, se
 *   compara byte a byte.
 * - Si realmente son duplicados:
 *     - Siempre se guarda el primero que llegó a la tabla.
 *     - Si delete_mode != 0, se borra el archivo "nuevo" (path).
 */
static void insert_or_handle_duplicate(const char *path,
                                       const Checksum *sum,
                                       off_t size,
                                       int delete_mode) {
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
                        printf("  Acción   : %s fue BORRADO.\n", path);
                    } else {
                        printf("  Acción   : No se pudo borrar %s: %s\n",
                               path, strerror(errno));
                    }
                } else {
                    printf("  Acción   : Solo se reporta, no se borra.\n");
                }
                return; /* No insertamos el duplicado en la tabla */
            } else if (eq == 0) {
                /* Checksum igual pero contenido diferente: colisión del hash. */
            } else {
                /* Error al comparar, por seguridad seguimos y tratamos como distinto. */
            }
        }
        current = current->next;
    }

    /* No se encontraron duplicados, insertar nuevo registro al inicio de la lista */
    FileRecord *node = (FileRecord *)xmalloc(sizeof(FileRecord));
    node->path = xstrdup(path);
    node->size = size;
    node->sum  = *sum;
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

/* ---------------------- Manejo de directorios ------------------------- */
/*
 * Recorre recursivamente un directorio.
 * - Usa opendir/readdir/closedir (como en el tutorial de directorios).
 * - Para cada subdirectorio, se llama a sí misma.
 * - Para cada archivo regular, llama a process_file().
 */
static void traverse_directory(const char *dirpath, int delete_mode) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "[ADVERTENCIA] No se pudo abrir directorio %s: %s\n",
                dirpath, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        /* Omitir las entradas especiales "." y ".." */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        /* Construir la ruta completa: dirpath + "/" + name */
        size_t len_dir  = strlen(dirpath);
        size_t len_name = strlen(name);
        int needs_slash = (len_dir > 0 && dirpath[len_dir - 1] != '/');
        size_t len_full = len_dir + (needs_slash ? 1 : 0) + len_name + 1;

        char *fullpath = (char *)xmalloc(len_full);
        strcpy(fullpath, dirpath);
        if (needs_slash) {
            strcat(fullpath, "/");
        }
        strcat(fullpath, name);

        /* Determinar si es directorio o archivo mediante lstat() */
        struct stat st;
        if (lstat(fullpath, &st) != 0) {
            fprintf(stderr, "[ADVERTENCIA] No se pudo hacer lstat de %s: %s\n",
                    fullpath, strerror(errno));
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
            /* Otros tipos (sockets, pipes, enlaces simbólicos, etc.) se ignoran */
        }

        free(fullpath);
    }

    closedir(dir);
}

/* ------------------------------ main() -------------------------------- */
/* Función principal: procesa argumentos y lanza el recorrido del directorio. */
int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr,
                "Uso: %s <directorio_inicial> [-d]\n"
                "  <directorio_inicial> : directorio donde se inicia la búsqueda.\n"
                "  -d                   : (opcional) si se indica, se borran los duplicados.\n"
                "                         Si no se indica, solo se listan en pantalla.\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *start_dir = argv[1];
    int delete_mode = 0;

    if (argc == 3) {
        if (strcmp(argv[2], "-d") == 0 || strcmp(argv[2], "--delete") == 0) {
            delete_mode = 1;
        } else {
            fprintf(stderr, "Parámetro desconocido: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    init_table();

    printf("Buscando archivos duplicados a partir de: %s\n", start_dir);
    printf("Modo: %s\n",
           delete_mode ? "listar y BORRAR duplicados"
                       : "solo listar duplicados");

    traverse_directory(start_dir, delete_mode);

    free_table();

    return EXIT_SUCCESS;
}
