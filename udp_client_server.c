#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <netdb.h>

#define BUF_SIZE 1024
#define NUM_CLIENTS 5
#define TIMEOUT 5
#define LOSS_PROBABILITY 30
#define PORT_RETRY_LIMIT 5

void *start_server(void *arg);

// Структура для хранения информации о клиентах
typedef struct {
    const char *server_ip;
    int server_port;
    int client_id;
} client_info_t;

// Генерация содержимого файла для передачи
void generate_file(const char *filename, int client_id, int file_count) {
    FILE *file = fopen(filename, "w");
    if (!file) {
        perror("File opening failed");
        return;
    }

    fprintf(file, "Отправитель: Клиент %d\n", client_id);
    fprintf(file, "Номер файла: %d\n", file_count);

    fclose(file);
}

// Функция для случайной потери пакета
int should_loss_packet() {
    return rand() % 100 < LOSS_PROBABILITY;
}

// Функция для нахождения свободного порта
int find_free_port() {
    struct addrinfo hints, *res, *p;
    int sockfd, port = 12345;
    int retry_count = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    while (retry_count < PORT_RETRY_LIMIT) {
        // Попытка найти свободный порт
        port = rand() % (65535 - 12345) + 12345;

        // Возможно, этот порт окажется закрытым (имитация ошибки)
        if (rand() % 100 < 90) {
            printf("Порт %d закрыт (пропускаем его)\n", port);
            retry_count++;
            continue;
        }

        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", port);

        if (getaddrinfo(NULL, port_str, &hints, &res) != 0) {
            retry_count++;
            continue;
        }

        for (p = res; p != NULL; p = p->ai_next) {
            sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (sockfd == -1) {
                continue;
            }

            // Пробуем привязать сокет
            if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
                freeaddrinfo(res);
                close(sockfd);
                printf("Используется порт: %d\n", port);
                return port;
            } else {
                // Порт занят, выводим сообщение
                printf("Порт %d занят, пробуем следующий...\n", port);
            }

            close(sockfd);
        }

        freeaddrinfo(res);
        retry_count++;
    }

    return -1;
}

// Серверная часть
void *start_server(void *arg) {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
    char buffer[BUF_SIZE];
    int packet_count = 0;
    int port = *(int *)arg;

    // Создаем UDP сокет
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Пробуем привязать сокет к адресу и порту
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed: Порт занят");
        close(sockfd);
        return NULL;
    }    

    addr_len = sizeof(client_addr);

    printf("Сервер запущен на порту %d...\n", port);

    while (1) {
        int n = recvfrom(sockfd, (char *)buffer, BUF_SIZE, MSG_WAITALL, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            perror("Failed to receive packet");
            continue;
        }
        buffer[n] = '\0';
        packet_count++;

        printf("Сервер получил пакет #%d: %s\n", packet_count, buffer);

        // Подтверждаем получение
        if (should_loss_packet()) {
            printf("Сервер потерял подтверждение для пакета #%d\n", packet_count);
            continue;
        }

        sendto(sockfd, "ACK", 3, MSG_CONFIRM, (const struct sockaddr *)&client_addr, addr_len);
        printf("Сервер отправил подтверждение для пакета #%d\n", packet_count);

        if (strcmp(buffer, "END") == 0) {
            break;
        }
    }

    printf("Сервер приостановил свою работу. Закрытие соединения...\n");

    close(sockfd);
    return NULL;
}

// Клиентская часть
void *start_client(void *arg) {
    client_info_t *client_info = (client_info_t *)arg;
    int sockfd;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    char filename[30];
    snprintf(filename, sizeof(filename), "client_%d.txt", client_info->client_id);

    // Генерация файла для передачи
    for (int file_count = 1; file_count <= 3; file_count++) {
        generate_file(filename, client_info->client_id, file_count);

        FILE *file = fopen(filename, "rb");
        if (!file) {
            perror("File opening failed");
            return NULL;
        }

        char buffer[BUF_SIZE];
        size_t n;
        int seq_num = 1;

        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(client_info->server_port);
        if (inet_pton(AF_INET, client_info->server_ip, &server_addr.sin_addr) <= 0) {
            perror("Invalid address");
            exit(1);
        }

        // Цикл отправки данных
        while (1) {
            while ((n = fread(buffer, 1, BUF_SIZE, file)) > 0) {
                buffer[n] = '\0';
                printf("Клиент %d: Отправка пакета #%d\n", client_info->client_id, seq_num);

                if (should_loss_packet()) {
                    printf("Клиент %d: Пакет #%d потерян\n", client_info->client_id, seq_num);
                    continue;
                }

                // Отправляем пакет
                int sent = sendto(sockfd, buffer, n, MSG_CONFIRM, (const struct sockaddr *)&server_addr, addr_len);
                if (sent == -1) {
                    perror("Send failed");
                    fclose(file);
                    return NULL;
                }

                // Ожидаем подтверждения с таймаутом
                char ack[4];
                struct timeval tv;
                tv.tv_sec = TIMEOUT;
                tv.tv_usec = 0;
                setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                int ack_len = recvfrom(sockfd, ack, sizeof(ack), MSG_WAITALL, (struct sockaddr *)&server_addr, &addr_len);
                if (ack_len == -1) {
                    printf("Клиент %d: Сервер не отвечает, завершаю работу...\n", client_info->client_id);
                    break;
                }
                ack[ack_len] = '\0';
                if (strcmp(ack, "ACK") == 0) {
                    printf("Клиент %d: Получено подтверждение для пакета #%d\n", client_info->client_id, seq_num);
                } else {
                    printf("Клиент %d: Подтверждение не получено для пакета #%d\n", client_info->client_id, seq_num);
                }
                seq_num++;
            }

            fclose(file);
            if (file_count == 3) {
                sendto(sockfd, "END", 3, MSG_CONFIRM, (const struct sockaddr *)&server_addr, addr_len);
                break;
            }

            generate_file(filename, client_info->client_id, ++file_count);
            file = fopen(filename, "rb");
        }

        close(sockfd);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));

    if (argc < 3) {
        printf("Использование: %s <server_ip> <file_to_send>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = find_free_port();  // Ищем свободный порт
    if (port == -1) {
        printf("Не удалось найти свободный порт.\n");
        return 1;
    }

    printf("Сервер работает на порту: %d\n", port);

    // Запуск сервера в отдельном потоке
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, start_server, (void *)&port);

    // Ожидание запуска сервера
    sleep(1);

    // Запуск клиентов
    pthread_t clients[NUM_CLIENTS];
    client_info_t client_info[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_info[i].server_ip = server_ip;
        client_info[i].server_port = port;
        client_info[i].client_id = i + 1;
        pthread_create(&clients[i], NULL, start_client, &client_info[i]);
    }

    // Ожидание завершения всех клиентов
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(clients[i], NULL);
    }

    pthread_join(server_thread, NULL);

    return 0;
}
