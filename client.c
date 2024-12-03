#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "structs.h"

#define TIMEFRAME 1.0f
#define BUFFER_SIZE 100

char buffer[CHUNK_SIZE];

// Function to receive chunks of data from the server
int receive_chunks(int sockid) {
    int num_chunks = 0;
    ssize_t n_bytes;

    while (1) {
        memset(buffer, 0, CHUNK_SIZE);
        n_bytes = recv(sockid, buffer, CHUNK_SIZE, 0);
        if (n_bytes < 0) {
            perror("[-] Receive error");
            return -1;
        }

        if (strncmp(buffer, "<STOP>", 6) == 0) {
            break;
        } else {
            printf("%s", buffer);
            fflush(stdout);
        }
        num_chunks++;
    }

    return num_chunks;
}

int main() {
    char ip_address[30];
    printf("Enter Naming server ip address: ");
    scanf("%s",ip_address);
    int port_number = 0;
    int sock_fd = -1;
    struct sockaddr_in server_addr;
    char send_buffer[BUFFER_SIZE];
    char receive_buffer[BUFFER_SIZE];
    ssize_t bytes_sent = 0;
    ssize_t bytes_received = 0;

    // Prompt user for port number
    printf("Enter port number: ");
    if (scanf("%d", &port_number) != 1) {
        fprintf(stderr, "Invalid input for port number.\n");
        exit(EXIT_FAILURE);
    }

    // Create socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[-] Socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("[+] TCP client NM socket created successfully.\n");

    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        perror("[-] Invalid IP address");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Connection to server failed");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }
    printf("Connected to NM server.\n");

    // Clear input buffers
    memset(send_buffer, 0, BUFFER_SIZE);
    memset(receive_buffer, 0, BUFFER_SIZE);

    // Flush remaining input
    int c;
    while ((c = getchar()) != '\n' && c != EOF);

    // Get user input to send to server
    printf("Enter message to send: ");
    if (fgets(send_buffer, BUFFER_SIZE, stdin) == NULL) {
        fprintf(stderr, "Error reading input.\n");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // Send initial message to server
    bytes_sent = send(sock_fd, send_buffer, strlen(send_buffer), 0);
    if (bytes_sent < 0) {
        perror("[-] Failed to send message");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        memset(send_buffer, 0, BUFFER_SIZE);

        // Prompt user for next command
        printf("Enter command: ");
        if (fgets(send_buffer, BUFFER_SIZE, stdin) == NULL) {
            fprintf(stderr, "Error reading command.\n");
            break;
        }

        // Remove newline character from input
        size_t len = strlen(send_buffer);
        if (len > 0 && send_buffer[len - 1] == '\n') {
            send_buffer[len - 1] = '\0';
        }

        // Send command to server
        bytes_sent = send(sock_fd, send_buffer, strlen(send_buffer), 0);
        if (bytes_sent < 0) {
            perror("[-] Failed to send command");
            break;
        }

        // Receive next_step indicator from server
        int next_step = -1;
        int retry_count = 0;
        while ((bytes_received = recv(sock_fd, &next_step, sizeof(int), 0)) < 0) {
            usleep(10000); // Sleep for 10ms
            retry_count++;
            if (retry_count >= 20) {
                printf("[400] Server doesn't seem to be responding. Please try again later.\n");
                break;
            }
        }

        if (bytes_received < 0) {
            break;
        }

        // Receive and display chunks from server
        if (receive_chunks(sock_fd) < 0) {
            break;
        }

        printf("%s\n", send_buffer);

        if (next_step) {
            continue;
        }

        // Allocate memory for ss_info structure
        ss_info *ss_struct = (ss_info *)malloc(sizeof(ss_info));
        if (ss_struct == NULL) {
            fprintf(stderr, "Memory allocation failed.\n");
            break;
        }
        memset(ss_struct, 0, sizeof(ss_info));

        // Receive ss_info from server
        bytes_received = recv(sock_fd, ss_struct, sizeof(ss_info), 0);
        if (bytes_received < 0) {
            perror("[-] Failed to receive ss_info");
            free(ss_struct);
            break;
        }

        // Extract SS server details
        char ss_ip[30];
        strncpy(ss_ip, ss_struct->ip_addr, sizeof(ss_ip) - 1);
        ss_ip[sizeof(ss_ip) - 1] = '\0';
        int ss_port = ntohs(ss_struct->port_no_client);
        int ss_sock = -1;
        struct sockaddr_in ss_addr;

        // Create socket for SS server
        ss_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ss_sock < 0) {
            perror("[-] SS Socket creation failed");
            free(ss_struct);
            exit(EXIT_FAILURE);
        }
        printf("[+] TCP client SS socket created successfully.\n");

        // Initialize SS server address structure
        memset(&ss_addr, 0, sizeof(ss_addr));
        ss_addr.sin_family = AF_INET;
        ss_addr.sin_port = htons(ss_port);
        if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
            perror("[-] Invalid SS IP address");
            close(ss_sock);
            free(ss_struct);
            exit(EXIT_FAILURE);
        }

        // Connect to SS server
        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
            perror("[-] Connection to SS server failed");
            int flag = 0;
            if (send(sock_fd, &flag, sizeof(int), 0) < 0) {
                perror("[-] Failed to send flag to NM server");
            }
            close(ss_sock);
            free(ss_struct);
            continue;
        }
        printf("Connected to SS server.\n");

        // Notify NM server of successful connection
        int flag = 1;
        if (send(sock_fd, &flag, sizeof(int), 0) < 0) {
            perror("[-] Failed to send flag to NM server");
            close(ss_sock);
            free(ss_struct);
            break;
        }

        // Send command to SS server
        bytes_sent = send(ss_sock, send_buffer, strlen(send_buffer), 0);
        if (bytes_sent < 0) {
            perror("[-] Failed to send command to SS server");
            close(ss_sock);
            free(ss_struct);
            continue;
        }

        // Receive and display chunks from SS server
        if (receive_chunks(ss_sock) < 0) {
            close(ss_sock);
            free(ss_struct);
            break;
        }

        // Close SS server connection and free allocated memory
        close(ss_sock);
        printf("Disconnected from SS server.\n");
        free(ss_struct);
    }

    // Close main socket before exiting
    close(sock_fd);
    return 0;
}
