/*
 * Servidor Web RESTful para archivos CSV
 * Curso: IC6600 - Principios de Sistemas Operativos
 * Tecnologico de Costa Rica
 *
 * Descripcion: Servidor HTTP que permite consultar y actualizar
 * archivos CSV mediante operaciones REST (GET, POST, PUT, DELETE)
 * utilizando multiplexacion de E/S con poll()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 4096
#define MAX_LINE_SIZE 1024
#define MAX_INDEX_SIZE 1000
#define PORT 8080
#define CSV_FILE "data.csv"

/* Estructura para el indice de registros CSV */
struct IndexEntry {
    int id;              // llave del registro (primer campo)
    long byte_offset;    // byte de inicio de la linea
    int length;          // longitud del registro
    int deleted;         // 1 si esta borrado, 0 si esta activo
};

/* Variables globales para el indice */
static struct IndexEntry index_table[MAX_INDEX_SIZE];
static int index_count = 0;
static FILE *csv_file = NULL;

/* Prototipos de funciones */
int init_server(int port);
void load_csv_index(void);
void handle_request(int client_fd, char *buffer, int bytes_read);
void parse_http_request(char *buffer, char *method, char *uri, char *body);
void handle_get(int client_fd, char *uri);
void handle_post(int client_fd, char *body);
void handle_put(int client_fd, char *uri, char *body);
void handle_delete(int client_fd, char *uri);
void send_response(int client_fd, int status_code, char *status_text, char *content_type, char *body);
void send_error(int client_fd, int status_code, char *message);
int extract_id_from_uri(char *uri);
int find_index_by_id(int id);
char *read_record(int index_pos);
int find_deleted_slot(int needed_size);
void write_record(int index_pos, char *data);
void add_record(char *data, int id);
void mark_deleted(int index_pos);
void update_record(int index_pos, char *new_data);
int get_next_id(void);
void trim_whitespace(char *str);

int main(int argc, char *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    struct pollfd fds[MAX_CLIENTS + 1];
    int timeout = 5000; // 5 segundos
    char buffer[BUFFER_SIZE];
    int port = PORT;

    // Permitir especificar puerto por linea de comandos
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    // Abrir archivo CSV
    csv_file = fopen(CSV_FILE, "r+");
    if (csv_file == NULL) {
        // Crear archivo si no existe
        csv_file = fopen(CSV_FILE, "w+");
        if (csv_file == NULL) {
            perror("Error opening CSV file");
            exit(EXIT_FAILURE);
        }
        printf("Archivo %s creado\n", CSV_FILE);
    }

    // Cargar indice del archivo CSV
    load_csv_index();
    printf("Indice cargado: %d registros\n", index_count);

    // Crear socket del servidor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Permitir reutilizar el puerto
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Configurar direccion del servidor
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor CSV RESTful escuchando en puerto %d...\n", port);
    printf("Endpoints disponibles:\n");
    printf("  GET    /items      - Obtener todos los registros\n");
    printf("  GET    /items/{id} - Obtener un registro por ID\n");
    printf("  POST   /items      - Crear nuevo registro\n");
    printf("  PUT    /items/{id} - Actualizar registro\n");
    printf("  DELETE /items/{id} - Eliminar registro\n");

    // Inicializar array de pollfd
    for (int i = 0; i <= MAX_CLIENTS; i++) {
        fds[i].fd = -1;
        fds[i].events = POLLIN;
    }

    fds[0].fd = server_fd;

    while (1) {
        int ret = poll(fds, MAX_CLIENTS + 1, timeout);

        if (ret < 0) {
            perror("poll failed");
            break;
        } else if (ret == 0) {
            // Timeout - continuar
            continue;
        }

        // Verificar nuevo cliente
        if (fds[0].revents & POLLIN) {
            socklen_t addrlen = sizeof(address);
            new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);

            if (new_socket >= 0) {
                printf("Nueva conexion aceptada: socket %d desde %s:%d\n",
                       new_socket, inet_ntoa(address.sin_addr), ntohs(address.sin_port));

                // Buscar slot libre
                int slot = -1;
                for (int i = 1; i <= MAX_CLIENTS && slot == -1; i++) {
                    if (fds[i].fd == -1) {
                        slot = i;
                        fds[i].fd = new_socket;
                        fds[i].events = POLLIN;
                    }
                }

                if (slot == -1) {
                    printf("No hay slots disponibles\n");
                    send_error(new_socket, 503, "Service Unavailable");
                    close(new_socket);
                }
            }
        }

        // Verificar datos de clientes existentes
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].fd != -1 && (fds[i].revents & POLLIN)) {
                memset(buffer, 0, BUFFER_SIZE);
                int bytes_read = recv(fds[i].fd, buffer, BUFFER_SIZE - 1, 0);

                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    printf("Recibido de cliente %d: %d bytes\n", fds[i].fd, bytes_read);

                    // Procesar solicitud HTTP
                    handle_request(fds[i].fd, buffer, bytes_read);

                    // Cerrar conexion despues de responder (HTTP/1.0 style)
                    close(fds[i].fd);
                    fds[i].fd = -1;
                } else if (bytes_read == 0) {
                    // Cliente desconectado
                    printf("Cliente %d desconectado\n", fds[i].fd);
                    close(fds[i].fd);
                    fds[i].fd = -1;
                } else {
                    perror("recv failed");
                    close(fds[i].fd);
                    fds[i].fd = -1;
                }
            }
        }
    }

    // Limpiar recursos
    if (csv_file != NULL) {
        fclose(csv_file);
    }
    close(server_fd);
    return 0;
}

/*
 * Carga el indice del archivo CSV en memoria
 * Lee linea por linea y almacena offset, id y longitud
 */
void load_csv_index(void) {
    char line[MAX_LINE_SIZE];
    long offset = 0;
    int line_num = 0;

    index_count = 0;
    rewind(csv_file);

    while (fgets(line, MAX_LINE_SIZE, csv_file) != NULL) {
        int len = strlen(line);

        // Saltar lineas vacias o solo con espacios
        int is_empty = 1;
        for (int i = 0; i < len; i++) {
            if (line[i] != ' ' && line[i] != '\n' && line[i] != '\r') {
                is_empty = 0;
                break;
            }
        }

        if (!is_empty && index_count < MAX_INDEX_SIZE) {
            // Extraer el ID (primer campo antes de la coma)
            char id_str[32];
            int j = 0;
            for (int i = 0; i < len && line[i] != ',' && j < 31; i++) {
                id_str[j++] = line[i];
            }
            id_str[j] = '\0';

            int id = atoi(id_str);

            // Verificar si es un registro borrado (todo espacios excepto newline)
            int is_deleted = 1;
            for (int i = 0; i < len - 1; i++) {
                if (line[i] != ' ') {
                    is_deleted = 0;
                    break;
                }
            }

            index_table[index_count].id = id;
            index_table[index_count].byte_offset = offset;
            index_table[index_count].length = len;
            index_table[index_count].deleted = is_deleted;
            index_count++;
        }

        offset += len;
        line_num++;
    }
}

/*
 * Parsea una solicitud HTTP y extrae metodo, URI y cuerpo
 */
void parse_http_request(char *buffer, char *method, char *uri, char *body) {
    // Inicializar
    method[0] = '\0';
    uri[0] = '\0';
    body[0] = '\0';

    // Parsear primera linea: METHOD URI VERSION
    char *line = strtok(buffer, "\r\n");
    if (line != NULL) {
        sscanf(line, "%s %s", method, uri);
    }

    // Buscar el cuerpo (despues de linea vacia)
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (body_start != NULL) {
        body_start += 4; // Saltar \r\n\r\n
        strcpy(body, body_start);
    }
}

/*
 * Maneja una solicitud HTTP completa
 */
void handle_request(int client_fd, char *buffer, int bytes_read) {
    char method[32];
    char uri[256];
    char body[BUFFER_SIZE];
    char buffer_copy[BUFFER_SIZE];

    (void)bytes_read; // Evitar warning de parametro no usado

    // Hacer copia porque strtok modifica el buffer
    strncpy(buffer_copy, buffer, BUFFER_SIZE - 1);
    buffer_copy[BUFFER_SIZE - 1] = '\0';

    // Parsear solicitud
    parse_http_request(buffer_copy, method, uri, body);

    printf("Metodo: %s, URI: %s\n", method, uri);

    // Buscar cuerpo en buffer original
    char *body_ptr = strstr(buffer, "\r\n\r\n");
    if (body_ptr != NULL) {
        strcpy(body, body_ptr + 4);
    }

    // Enrutar segun metodo HTTP
    if (strcmp(method, "GET") == 0) {
        handle_get(client_fd, uri);
    } else if (strcmp(method, "POST") == 0) {
        handle_post(client_fd, body);
    } else if (strcmp(method, "PUT") == 0) {
        handle_put(client_fd, uri, body);
    } else if (strcmp(method, "DELETE") == 0) {
        handle_delete(client_fd, uri);
    } else {
        send_error(client_fd, 405, "Method Not Allowed");
    }
}

/*
 * Extrae el ID numerico de una URI tipo /items/123
 * Retorna -1 si no hay ID, o el ID si existe
 */
int extract_id_from_uri(char *uri) {
    // Verificar que comience con /items
    if (strncmp(uri, "/items", 6) != 0) {
        return -2; // URI invalida
    }

    // Si es exactamente /items o /items/
    if (strlen(uri) <= 7) {
        return -1; // Sin ID
    }

    // Extraer ID despues de /items/
    char *id_str = uri + 7;
    return atoi(id_str);
}

/*
 * Busca un registro en el indice por su ID
 * Retorna el indice en la tabla o -1 si no existe
 */
int find_index_by_id(int id) {
    for (int i = 0; i < index_count; i++) {
        if (index_table[i].id == id && !index_table[i].deleted) {
            return i;
        }
    }
    return -1;
}

/*
 * Lee un registro del archivo CSV usando fseek
 * Retorna un string con el contenido (sin el newline final)
 */
char *read_record(int index_pos) {
    static char record[MAX_LINE_SIZE];

    if (index_pos < 0 || index_pos >= index_count) {
        return NULL;
    }

    // Posicionar en el byte de inicio
    if (fseek(csv_file, index_table[index_pos].byte_offset, SEEK_SET) != 0) {
        perror("fseek failed");
        return NULL;
    }

    // Leer el registro
    if (fgets(record, index_table[index_pos].length + 1, csv_file) == NULL) {
        return NULL;
    }

    // Remover newline
    int len = strlen(record);
    while (len > 0 && (record[len - 1] == '\n' || record[len - 1] == '\r')) {
        record[--len] = '\0';
    }

    // Remover espacios al final (padding)
    while (len > 0 && record[len - 1] == ' ') {
        record[--len] = '\0';
    }

    return record;
}

/*
 * Encuentra un slot borrado con espacio suficiente
 * Retorna el indice o -1 si no hay espacio
 */
int find_deleted_slot(int needed_size) {
    for (int i = 0; i < index_count; i++) {
        if (index_table[i].deleted && index_table[i].length >= needed_size) {
            return i;
        }
    }
    return -1;
}

/*
 * Escribe un registro en una posicion existente del archivo
 * Rellena con espacios si sobra espacio
 */
void write_record(int index_pos, char *data) {
    int data_len = strlen(data);
    int available_len = index_table[index_pos].length;

    // Posicionar en el byte de inicio
    if (fseek(csv_file, index_table[index_pos].byte_offset, SEEK_SET) != 0) {
        perror("fseek failed");
        return;
    }

    // Escribir los datos
    fwrite(data, 1, data_len, csv_file);

    // Rellenar con espacios (sin contar el newline)
    int padding = available_len - data_len - 1;
    for (int i = 0; i < padding; i++) {
        fputc(' ', csv_file);
    }

    // Escribir newline
    fputc('\n', csv_file);

    fflush(csv_file);
}

/*
 * Agrega un nuevo registro al archivo CSV
 */
void add_record(char *data, int id) {
    char record[MAX_LINE_SIZE];
    int data_len = strlen(data);
    int record_len = data_len + 1; // +1 para newline

    // Formatear registro con ID
    if (id > 0) {
        snprintf(record, MAX_LINE_SIZE, "%d,%s", id, data);
    } else {
        strncpy(record, data, MAX_LINE_SIZE - 1);
    }
    record_len = strlen(record) + 1;

    // Buscar slot borrado disponible
    int slot = find_deleted_slot(record_len);

    if (slot >= 0) {
        // Usar slot existente
        write_record(slot, record);

        // Actualizar indice
        index_table[slot].id = id > 0 ? id : atoi(record);
        index_table[slot].deleted = 0;
    } else {
        // Agregar al final del archivo
        fseek(csv_file, 0, SEEK_END);
        long offset = ftell(csv_file);

        fprintf(csv_file, "%s\n", record);
        fflush(csv_file);

        // Actualizar indice
        if (index_count < MAX_INDEX_SIZE) {
            index_table[index_count].id = id > 0 ? id : atoi(record);
            index_table[index_count].byte_offset = offset;
            index_table[index_count].length = strlen(record) + 1;
            index_table[index_count].deleted = 0;
            index_count++;
        }
    }
}

/*
 * Marca un registro como borrado (borrado perezoso)
 * Rellena la linea con espacios
 */
void mark_deleted(int index_pos) {
    if (index_pos < 0 || index_pos >= index_count) {
        return;
    }

    // Posicionar en el byte de inicio
    if (fseek(csv_file, index_table[index_pos].byte_offset, SEEK_SET) != 0) {
        perror("fseek failed");
        return;
    }

    // Rellenar con espacios (mantener longitud original)
    int len = index_table[index_pos].length;
    for (int i = 0; i < len - 1; i++) {
        fputc(' ', csv_file);
    }
    fputc('\n', csv_file);

    fflush(csv_file);

    // Actualizar indice
    index_table[index_pos].deleted = 1;
}

/*
 * Actualiza un registro existente
 * Si el nuevo dato cabe, lo escribe en el mismo lugar
 * Si no, marca el viejo como borrado y agrega uno nuevo
 */
void update_record(int index_pos, char *new_data) {
    int new_len = strlen(new_data) + 1; // +1 para newline
    int available_len = index_table[index_pos].length;

    if (new_len <= available_len) {
        // Cabe en el espacio actual
        write_record(index_pos, new_data);
    } else {
        // No cabe, buscar nuevo espacio
        int id = index_table[index_pos].id;
        mark_deleted(index_pos);
        add_record(new_data, id);
    }
}

/*
 * Obtiene el siguiente ID disponible
 */
int get_next_id(void) {
    int max_id = 0;
    for (int i = 0; i < index_count; i++) {
        if (!index_table[i].deleted && index_table[i].id > max_id) {
            max_id = index_table[i].id;
        }
    }
    return max_id + 1;
}

/*
 * Elimina espacios al inicio y final de un string
 */
void trim_whitespace(char *str) {
    char *start = str;
    char *end;

    // Espacios al inicio
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    // Todo espacios?
    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    // Espacios al final
    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }
    *(end + 1) = '\0';

    // Mover si es necesario
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/*
 * Envia una respuesta HTTP completa
 */
void send_response(int client_fd, int status_code, char *status_text, char *content_type, char *body) {
    char response[BUFFER_SIZE * 2];
    int body_len = body ? strlen(body) : 0;

    snprintf(response, sizeof(response),
             "HTTP/1.0 %d %s\r\n"
             "Server: csv-restful-server\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             status_code, status_text, content_type, body_len, body ? body : "");

    send(client_fd, response, strlen(response), 0);
}

/*
 * Envia una respuesta de error HTTP
 */
void send_error(int client_fd, int status_code, char *message) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\": \"%s\"}", message);
    send_response(client_fd, status_code, message, "application/json", body);
}

/*
 * Maneja solicitud GET
 * GET /items - retorna todos los registros
 * GET /items/{id} - retorna un registro especifico
 */
void handle_get(int client_fd, char *uri) {
    int id = extract_id_from_uri(uri);

    if (id == -2) {
        send_error(client_fd, 404, "Not Found");
        return;
    }

    if (id == -1) {
        // GET /items - retornar todos los registros
        char body[BUFFER_SIZE * 4];
        char *ptr = body;
        int remaining = sizeof(body);

        ptr += snprintf(ptr, remaining, "[");
        remaining = sizeof(body) - (ptr - body);

        int first = 1;
        for (int i = 0; i < index_count; i++) {
            if (!index_table[i].deleted) {
                char *record = read_record(i);
                if (record != NULL) {
                    if (!first) {
                        ptr += snprintf(ptr, remaining, ",");
                        remaining = sizeof(body) - (ptr - body);
                    }
                    ptr += snprintf(ptr, remaining, "\n  \"%s\"", record);
                    remaining = sizeof(body) - (ptr - body);
                    first = 0;
                }
            }
        }

        snprintf(ptr, remaining, "\n]");

        send_response(client_fd, 200, "OK", "application/json", body);
    } else {
        // GET /items/{id} - retornar un registro
        int index_pos = find_index_by_id(id);

        if (index_pos < 0) {
            send_error(client_fd, 404, "Record Not Found");
            return;
        }

        char *record = read_record(index_pos);
        if (record == NULL) {
            send_error(client_fd, 500, "Internal Server Error");
            return;
        }

        char body[BUFFER_SIZE];
        snprintf(body, sizeof(body), "{\"data\": \"%s\"}", record);
        send_response(client_fd, 200, "OK", "application/json", body);
    }
}

/*
 * Maneja solicitud POST
 * POST /items - crea un nuevo registro
 * El cuerpo debe contener los datos CSV (sin el ID, se asigna automaticamente)
 */
void handle_post(int client_fd, char *body) {
    if (body == NULL || strlen(body) == 0) {
        send_error(client_fd, 400, "Bad Request - Empty body");
        return;
    }

    // Limpiar el cuerpo
    trim_whitespace(body);

    // Generar nuevo ID
    int new_id = get_next_id();

    // Crear registro con el nuevo ID
    char record[MAX_LINE_SIZE];
    snprintf(record, sizeof(record), "%d,%s", new_id, body);

    // Agregar al archivo
    add_record(record, new_id);

    // Responder con el nuevo registro creado
    char response_body[BUFFER_SIZE];
    snprintf(response_body, sizeof(response_body),
             "{\"id\": %d, \"data\": \"%s\", \"message\": \"Record created\"}", new_id, record);

    send_response(client_fd, 201, "Created", "application/json", response_body);
    printf("Registro creado: ID=%d, Data=%s\n", new_id, record);
}

/*
 * Maneja solicitud PUT
 * PUT /items/{id} - actualiza un registro existente
 */
void handle_put(int client_fd, char *uri, char *body) {
    int id = extract_id_from_uri(uri);

    if (id <= 0) {
        send_error(client_fd, 400, "Bad Request - Invalid ID");
        return;
    }

    if (body == NULL || strlen(body) == 0) {
        send_error(client_fd, 400, "Bad Request - Empty body");
        return;
    }

    // Limpiar el cuerpo
    trim_whitespace(body);

    // Buscar el registro
    int index_pos = find_index_by_id(id);

    if (index_pos < 0) {
        send_error(client_fd, 404, "Record Not Found");
        return;
    }

    // Crear nuevo registro con el mismo ID
    char new_record[MAX_LINE_SIZE];
    snprintf(new_record, sizeof(new_record), "%d,%s", id, body);

    // Actualizar
    update_record(index_pos, new_record);

    // Recargar indice para reflejar cambios
    load_csv_index();

    // Responder
    char response_body[BUFFER_SIZE];
    snprintf(response_body, sizeof(response_body),
             "{\"id\": %d, \"data\": \"%s\", \"message\": \"Record updated\"}", id, new_record);

    send_response(client_fd, 200, "OK", "application/json", response_body);
    printf("Registro actualizado: ID=%d\n", id);
}

/*
 * Maneja solicitud DELETE
 * DELETE /items/{id} - elimina un registro (borrado perezoso)
 */
void handle_delete(int client_fd, char *uri) {
    int id = extract_id_from_uri(uri);

    if (id <= 0) {
        send_error(client_fd, 400, "Bad Request - Invalid ID");
        return;
    }

    // Buscar el registro
    int index_pos = find_index_by_id(id);

    if (index_pos < 0) {
        send_error(client_fd, 404, "Record Not Found");
        return;
    }

    // Marcar como borrado
    mark_deleted(index_pos);

    // Responder
    char response_body[BUFFER_SIZE];
    snprintf(response_body, sizeof(response_body),
             "{\"id\": %d, \"message\": \"Record deleted\"}", id);

    send_response(client_fd, 200, "OK", "application/json", response_body);
    printf("Registro eliminado: ID=%d\n", id);
}
