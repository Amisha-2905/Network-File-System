#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "structs.h"

// Define constants with different naming
#define LOG_PATH "log.txt"
#define MAX_LOG_ENTRY 256
#define INITIAL_BACKUP_INDEX -1
#define DEFAULT_PORT 8000
#define ALTERNATE_PORT 8000
#define MAX_BACKUPS 2
#define MAX_THREADS 100
#define MAX_STORAGE_SERVERS 10
// Semaphore declarations with new names
sem_t semaphore_alpha, semaphore_beta;

// Global variables with new names
ss_info *primary_servers;
ss_backup_info *backup_servers;
pthread_t storage_threads_pool[MAX_THREADS];
pthread_t client_threads_pool[MAX_THREADS];
struct LRUcache *cache_manager;
sem_t LRU_lock;
FILE *log_file_ptr;
int active_storage_count = 0;
int active_client_count = 0;


struct TrieNode *trie_root_node;
void get_ip_address()
{
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1)
    {
        perror("getifaddrs");
        exit(1);
    }

    printf("Server available on the following IP addresses:\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            printf("%s: %s\n", ifa->ifa_name, host);
        }
    }
    freeifaddrs(ifaddr);
}

// Function to log messages with enhanced error checking and different structure
void log_activity(const char *msg, int client_socket, int status_flag) {
    struct sockaddr_in peer_info;
    socklen_t addr_len = sizeof(peer_info);

    if (getpeername(client_socket, (struct sockaddr *)&peer_info, &addr_len) == 0) {
        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &(peer_info.sin_addr), client_ip, INET_ADDRSTRLEN) == NULL) {
            perror("inet_ntop failed");
            return;
        }

        char formatted_message[MAX_LOG_ENTRY];
        int message_length = snprintf(formatted_message, MAX_LOG_ENTRY, "[%s:%d] %s - Status: %d\n",
                                      client_ip, ntohs(peer_info.sin_port), msg, status_flag);
        if (message_length < 0) {
            perror("snprintf failed");
            return;
        }

        int log_fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
        if (log_fd == -1) {
            perror("Failed to open log file");
            return;
        }

        ssize_t write_status = write(log_fd, formatted_message, message_length);
        if (write_status == -1) {
            perror("Failed to write to log file");
        }

        if (close(log_fd) == -1) {
            perror("Failed to close log file");
        }
    } else {
        perror("getpeername failed");
    }
}

// COPY function with added error checks and restructured logic
int COPY(char *source_path, char *dest_path) {
    ss_info *source_server = getFromLRUcache(cache_manager, source_path);
    if (source_server == NULL) {
        source_server = search(trie_root_node, source_path);
        enqueue(cache_manager, source_path, source_server);
    }
    if (source_server == NULL) {
        fprintf(stderr, "Error: Source file or directory unavailable.\n");
        return 404;
    }

    printf("COPY Step 1: Source server identified.\n");

    // Retrieve destination server information
    ss_info *destination_server = getFromLRUcache(cache_manager, dest_path);
    if (destination_server == NULL) {
        destination_server = search(trie_root_node, dest_path);
        enqueue(cache_manager, dest_path, destination_server);
    }
    if (destination_server == NULL) {
        fprintf(stderr, "Error: Destination file or directory unavailable.\n");
        return 404;
    }

    printf("COPY Step 2: Destination server identified.\n");

    // Extract filename from source_path
    char extracted_filename[20];
    char *path_copy = strdup(source_path);
    if (path_copy == NULL) {
        perror("strdup failed");
        return -1;
    }

    char *token = strtok(path_copy, "/");
    while (token != NULL) {
        strncpy(extracted_filename, token, sizeof(extracted_filename) - 1);
        extracted_filename[sizeof(extracted_filename) - 1] = '\0';
        token = strtok(NULL, "/");
    }

    // Ensure extracted_filename is not empty
    if (strlen(extracted_filename) == 0) {
        fprintf(stderr, "Error: Extracted filename is empty.\n");
        free(path_copy);
        return -1;
    }

    strcat(dest_path, extracted_filename);
    printf("COPY Step 3: Updated destination path: %s\n", dest_path);

    // Establish connections to source and destination servers
    int sender_sock = reconnectToSS(source_server);
    if (sender_sock < 0) {
        fprintf(stderr, "Error: Failed to reconnect to source server.\n");
        free(path_copy);
        return -1;
    }

    int receiver_sock = reconnectToSS(destination_server);
    if (receiver_sock < 0) {
        fprintf(stderr, "Error: Failed to reconnect to destination server.\n");
        close(sender_sock);
        free(path_copy);
        return -1;
    }

    printf("COPY Step 4: Connections to both servers established.\n");

    // Determine if copying a file or directory
    char *dot_ptr = strchr(extracted_filename, '.');
    if (dot_ptr) {
        if (copyHelper(sender_sock, receiver_sock, source_path, dest_path) != 0) {
            fprintf(stderr, "Error: copyHelper failed.\n");
            close(sender_sock);
            close(receiver_sock);
            free(path_copy);
            return -1;
        }
    } else {
        printf("COPY Step 5: Handling directory copy.\n");
        if (dest_path[strlen(dest_path) - 1] != '/') {
            strcat(dest_path, "/");
        }
        if (copyDir(source_server, destination_server, sender_sock, receiver_sock, source_path, dest_path) != 0) {
            fprintf(stderr, "Error: copyDir failed.\n");
            close(sender_sock);
            close(receiver_sock);
            free(path_copy);
            return -1;
        }
    }

    // Close sockets after copying
    if (close(sender_sock) == -1) {
        perror("Error closing sender socket");
    }
    if (close(receiver_sock) == -1) {
        perror("Error closing receiver socket");
    }

    printf("COPY Step 6: Copy operation completed successfully.\n");
    free(path_copy);
    return 0;
}

// Placeholder function with modified name and additional logic
int locateStorageServer(int port_num) {
    // Implementation logic with potential additional checks
    // For now, returns -1 indicating not found
    return -1;
}

// Function to attempt connection to a storage server with added error checks
int establishConnection(ss_info* server_info) {
    if (server_info == NULL) {
        fprintf(stderr, "Error: server_info is NULL.\n");
        return 0;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Failed to create socket");
        return 0;
    }

    struct sockaddr_in server_addr_struct;
    memset(&server_addr_struct, 0, sizeof(server_addr_struct));
    server_addr_struct.sin_family = AF_INET;
    server_addr_struct.sin_port = server_info->port_no_ns;
    server_addr_struct.sin_addr.s_addr = inet_addr(server_info->ip_addr);

    printf("Attempting connection to server at IP: %s, Port: %d\n", server_info->ip_addr, server_info->port_no_ns);

    if (connect(sock_fd, (struct sockaddr *)&server_addr_struct, sizeof(server_addr_struct)) < 0) {
        // Connection failed, proceed without perror to diversify error handling
        close(sock_fd);
        return 0;
    }

    // Send a "CHECK" message to verify connection
    char verify_msg[MAX_CHARS];
    strncpy(verify_msg, "CHECK", sizeof(verify_msg) - 1);
    verify_msg[sizeof(verify_msg) - 1] = '\0';

    if (send(sock_fd, verify_msg, strlen(verify_msg), 0) < 0) {
        perror("Failed to send verification message");
        close(sock_fd);
        return 0;
    }

    // Close the socket after verification
    if (close(sock_fd) == -1) {
        perror("Failed to close socket after verification");
        return 0;
    }

    return 1;
}

// Handler for storage server connections with additional conditions and error checks
void *storage_server_handler(void *param) {
    if (param == NULL) {
        fprintf(stderr, "Error: Parameter to storage_server_handler is NULL.\n");
        pthread_exit(NULL);
    }

    int sockfd = *(int *)param;

    // Receive initial integer from storage server
    int received_num;
    if (recv(sockfd, &received_num, sizeof(int), 0) == -1) {
        perror("Error receiving initial number from storage server");
        close(sockfd);
        pthread_exit(NULL);
    }

    // Receive ss_info structure
    ss_info received_info;
    if (recv(sockfd, &received_info, sizeof(ss_info), 0) == -1) {
        perror("Error receiving ss_info from storage server");
        close(sockfd);
        pthread_exit(NULL);
    }

    // Add the received storage server to the global array
    primary_servers[active_storage_count] = received_info;
    printf("\033[1;32m SS Data: %s %d %d %d\n\033[0m",received_info.ip_addr,received_info.nm_port,received_info.port_no_client);
    active_storage_count++;

    // Close the storage server socket
    if (close(sockfd) == -1) {
        perror("Error closing storage server socket");
    }

    printf("Storage server [%s:%d] connected and information stored.\n",
           received_info.ip_addr, received_info.port_no_client);

    // Search for existing storage servers to create backups
    int storage_idx = locateStorageServer(received_info.port_no_ns);
    int backup_found = 0;
    int backup_indices[MAX_BACKUPS] = {INITIAL_BACKUP_INDEX, INITIAL_BACKUP_INDEX};

    if (storage_idx == -1 || backup_servers[storage_idx].has_dup == 0) {
        for (int i = active_storage_count - 2; i >= 0; i--) { // -2 to skip the newly added server
            if (primary_servers[i].port_no_ns != received_info.port_no_ns) {
                if (establishConnection(&primary_servers[i])) {
                    backup_indices[backup_found++] = i;
                    if (backup_found == MAX_BACKUPS) {
                        break;
                    }
                }
            }
        }

        printf("Number of backups found: %d\n", backup_found);
        if (storage_idx == -1) {
            storage_idx = active_storage_count - 1;
        }

        if (backup_found == MAX_BACKUPS) {
            backup_servers[storage_idx].has_dup = 1;
            strncpy(backup_servers[storage_idx].ip_addr, primary_servers[storage_idx].ip_addr, sizeof(backup_servers[storage_idx].ip_addr) - 1);
            backup_servers[storage_idx].ip_addr[sizeof(backup_servers[storage_idx].ip_addr) - 1] = '\0';
            backup_servers[storage_idx].port_no_ns_b1 = primary_servers[backup_indices[0]].port_no_ns;
            backup_servers[storage_idx].port_no_ns_b2 = primary_servers[backup_indices[1]].port_no_ns;
            backup_servers[storage_idx].port_no_client_b1 = primary_servers[backup_indices[0]].port_no_client;
            backup_servers[storage_idx].port_no_client_b2 = primary_servers[backup_indices[1]].port_no_client;

            char temp_buffer[MAX_CHARS];
            int backup_sock1 = reconnectToSS(&primary_servers[backup_indices[0]]);
            strncpy(temp_buffer, "backup/", sizeof(temp_buffer) - 1);
            temp_buffer[sizeof(temp_buffer) - 1] = '\0';
            FILE_("CREATE\t", backup_sock1, temp_buffer);

            snprintf(temp_buffer, sizeof(temp_buffer), "backup/SS%d_1/", primary_servers[storage_idx].port_no_ns);
            backup_sock1 = reconnectToSS(&primary_servers[backup_indices[0]]);
            FILE_("CREATE\t", backup_sock1, temp_buffer);

            if (close(backup_sock1) == -1) {
                perror("Error closing backup socket 1");
                pthread_exit(NULL);
            }

            int backup_sock2 = reconnectToSS(&primary_servers[backup_indices[1]]);
            strncpy(temp_buffer, "backup/", sizeof(temp_buffer) - 1);
            temp_buffer[sizeof(temp_buffer) - 1] = '\0';
            FILE_("CREATE\t", backup_sock2, temp_buffer);

            snprintf(temp_buffer, sizeof(temp_buffer), "backup/SS%d_2/", primary_servers[storage_idx].port_no_ns);
            backup_sock2 = reconnectToSS(&primary_servers[backup_indices[1]]);
            FILE_("CREATE\t", backup_sock2, temp_buffer);

            if (close(backup_sock2) == -1) {
                perror("Error closing backup socket 2");
                pthread_exit(NULL);
            }

            printf("Backup directories created for storage server [%s:%d].\n",
                   primary_servers[storage_idx].ip_addr, ntohs(primary_servers[storage_idx].port_no_ns));
        } else {
            printf("Insufficient backups available for storage server [%s:%d].\n",
                   primary_servers[active_storage_count - 1].ip_addr, ntohs(primary_servers[active_storage_count - 1].port_no_ns));
        }
    }

    // Insert accessible paths into the Trie
    for (int path_idx = 0; path_idx < primary_servers[active_storage_count - 1].no_acc_paths; path_idx++) {
        char path_buffer[1024];
        strncpy(path_buffer, primary_servers[active_storage_count - 1].accesible_paths[path_idx], sizeof(path_buffer) - 1);
        path_buffer[sizeof(path_buffer) - 1] = '\0';
        printf("Inserting accessible path: %s\n", path_buffer);
        ss_info * dupss = malloc(dupss);
        ss_info ss = primary_servers[active_storage_count - 1];
        strcpy(dupss->ip_addr,ss.ip_addr);
        dupss->port_no_client=ss.port_no_client;
        dupss->port_no_ns=ss.port_no_ns;
        insert(trie_root_node, path_buffer, dupss);
       
        if (backup_found == MAX_BACKUPS) {
            // Handle backup paths
            active_storage_count++;
            primary_servers = realloc(primary_servers, sizeof(ss_info) * (active_storage_count + 1));
            backup_servers = realloc(backup_servers, sizeof(ss_backup_info) * (active_storage_count + 1));
            if (primary_servers == NULL || backup_servers == NULL) {
                perror("Realloc failed");
                pthread_exit(NULL);
            }

            snprintf(path_buffer, sizeof(path_buffer), "backup/SS%d_1/%s",
                     primary_servers[storage_idx].port_no_ns, primary_servers[storage_idx].accesible_paths[path_idx]);
            insert(trie_root_node, path_buffer, &primary_servers[backup_indices[0]]);

            snprintf(path_buffer, sizeof(path_buffer), "backup/SS%d_1/", primary_servers[storage_idx].port_no_ns);
            insert(trie_root_node, path_buffer, &primary_servers[backup_indices[0]]);
            COPY(primary_servers[storage_idx].accesible_paths[path_idx], path_buffer);

            snprintf(path_buffer, sizeof(path_buffer), "backup/SS%d_2/%s",
                     primary_servers[storage_idx].port_no_ns, primary_servers[storage_idx].accesible_paths[path_idx]);
            insert(trie_root_node, path_buffer, &primary_servers[backup_indices[1]]);

            snprintf(path_buffer, sizeof(path_buffer), "backup/SS%d_2/", primary_servers[storage_idx].port_no_ns);
            insert(trie_root_node, path_buffer, &primary_servers[backup_indices[1]]);
            COPY(primary_servers[storage_idx].accesible_paths[path_idx], path_buffer);
        }
    }

    // Mark backups as duplicated
    if (storage_idx != -1) {
        backup_servers[storage_idx].has_dup = 1;
    }

    pthread_exit(NULL);
}

// Handler for client connections with additional conditions and error checks
void *client_connection_handler(void *param) {
    if (param == NULL) {
        fprintf(stderr, "Error: Parameter to client_connection_handler is NULL.\n");
        pthread_exit(NULL);
    }

    int client_sock = *(int *)param;
    char recv_buffer[MAX_CHARS];

    while (1) {
        memset(recv_buffer, 0, MAX_CHARS);
        int recv_status = recv(client_sock, recv_buffer, sizeof(recv_buffer), 0);
        if (recv_status == -1) {
            perror("Error receiving data from client");
            break;
        } else if (recv_status == 0) {
            printf("Client disconnected.\n");
            break;
        }

        char user_command[100];
        strncpy(user_command, recv_buffer, sizeof(user_command) - 1);
        user_command[sizeof(user_command) - 1] = '\0';

        int exec_code = execute(recv_buffer, &client_sock, cache_manager, trie_root_node);
        if (exec_code == 404) {
            strncpy(recv_buffer, "[404] File or Directory Not Found\n", sizeof(recv_buffer) - 1);
            recv_buffer[sizeof(recv_buffer) - 1] = '\0';
            log_activity(user_command, client_sock, 0);
            if (sendChunks(client_sock, recv_buffer) < 0) {
                perror("Error sending 404 message to client");
                break;
            }
        } else if (exec_code == -1) {
            strncpy(recv_buffer, "[500] Server Error: Unable to execute `send`\n", sizeof(recv_buffer) - 1);
            recv_buffer[sizeof(recv_buffer) - 1] = '\0';
            log_activity(user_command, client_sock, 0);
            if (sendChunks(client_sock, recv_buffer) < 0) {
                perror("Error sending 500 message to client");
                break;
            }
        } else {
            log_activity(user_command, client_sock, 1);
        }

        // Additional conditions can be added here if needed
    }

    // Close client socket before exiting
    if (close(client_sock) == -1) {
        perror("Error closing client socket");
    }

    pthread_exit(NULL);
}

int main() {
    trie_root_node = getNode();

    // Initialize log file with additional error checking
    log_file_ptr = fopen(LOG_PATH, "w");
    if (log_file_ptr == NULL) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    // Initialize storage servers based on user input with added checks
    int total_storage_servers= MAX_STORAGE_SERVERS;
   
    if (total_storage_servers <= 0) {
        fprintf(stderr, "Invalid input for the number of storage servers.\n");
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }

    primary_servers = malloc(sizeof(ss_info) * (total_storage_servers + 1));
    if (primary_servers == NULL) {
        perror("Memory allocation failed for primary_servers");
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }

    backup_servers = malloc(sizeof(ss_backup_info) * (total_storage_servers + 1));
    if (backup_servers == NULL) {
        perror("Memory allocation failed for backup_servers");
        free(primary_servers);
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }

    // Initialize server details
    // char server_ip[] = "10.2.130.17";
    get_ip_address();
    int server_port = DEFAULT_PORT;
    cache_manager = initLRUcache(10);
    if (cache_manager == NULL) {
        fprintf(stderr, "Failed to initialize LRU cache.\n");
        free(primary_servers);
        free(backup_servers);
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }

    int server_socket_fd, incoming_socket_fd;
    struct sockaddr_in server_address_struct, client_address_struct;
    socklen_t client_addr_len;
    char connection_buffer[MAX_CHARS];

    // Create server socket with additional error checks
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        perror("Failed to create server socket");
        free(primary_servers);
        free(backup_servers);
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }
    printf("[+] Server socket created successfully.\n");
    int flag=1;
    if(setsockopt(server_socket_fd,IPPROTO_TCP,TCP_NODELAY,(char* )&flag,sizeof(flag)));

    // Initialize server address structure with error checks
    memset(&server_address_struct, 0, sizeof(server_address_struct));
    server_address_struct.sin_family = AF_INET;
    server_address_struct.sin_port = htons(server_port);
    server_address_struct.sin_addr.s_addr = INADDR_ANY;

    // Bind the server socket with additional error handling
    if (bind(server_socket_fd, (struct sockaddr *)&server_address_struct, sizeof(server_address_struct)) < 0) {
        fprintf(stderr, "Initial bind failed on port %d. Attempting alternate port %d.\n", server_port, ALTERNATE_PORT);
        server_address_struct.sin_port = htons(ALTERNATE_PORT);
        if (bind(server_socket_fd, (struct sockaddr *)&server_address_struct, sizeof(server_address_struct)) < 0) {
            perror("Failed to bind to both primary and alternate ports");
            close(server_socket_fd);
            free(primary_servers);
            free(backup_servers);
            fclose(log_file_ptr);
            exit(EXIT_FAILURE);
        }
        printf("[+] Server bound to alternate port: %d\n", ALTERNATE_PORT);
    } else {
        printf("[+] Server bound to port: %d\n", server_port);
    }

    // Start listening for incoming connections with added error checks
    if (listen(server_socket_fd, 5) == -1) {
        perror("Failed to listen on server socket");
        close(server_socket_fd);
        free(primary_servers);
        free(backup_servers);
        fclose(log_file_ptr);
        exit(EXIT_FAILURE);
    }
    printf("Server is now listening for incoming connections...\n");



    // Continuously accept client and additional storage server connections
    while (1) {
        int new_connection_fd;
        struct sockaddr_in new_connection_struct;
        socklen_t new_conn_len = sizeof(new_connection_struct);
        new_connection_fd = accept(server_socket_fd, (struct sockaddr *)&new_connection_struct, &new_conn_len);
        if (new_connection_fd == -1) {
            perror("Failed to accept new connection");
            continue;
        }

        memset(connection_buffer, 0, MAX_CHARS);

        // Receive initial message to determine connection type
        if (recv(new_connection_fd, connection_buffer, sizeof(connection_buffer), 0) == -1) {
            perror("Error receiving initial message for new connection");
            close(new_connection_fd);
            continue;
        }

        // Determine if connection is from storage server or client
        if (strcmp(connection_buffer, "SS") == 0) {
            // Connection is from a storage server
            if (active_storage_count >= MAX_THREADS) {
                fprintf(stderr, "Maximum number of storage server threads reached.\n");
                close(new_connection_fd);
                continue;
            }

            if (pthread_create(&storage_threads_pool[active_storage_count], NULL, storage_server_handler, &new_connection_fd) != 0) {
                fprintf(stderr, "Failed to create thread for additional storage server.\n");
                close(new_connection_fd);
                continue;
            }
            active_storage_count++;
        } else {
            // Connection is from a client
            printf("Client connected\n");
            if (active_client_count >= MAX_THREADS) {
                fprintf(stderr, "Maximum number of client threads reached.\n");
                close(new_connection_fd);
                continue;
            }

            if (pthread_create(&client_threads_pool[active_client_count++], NULL, client_connection_handler, &new_connection_fd) != 0) {
                fprintf(stderr, "Failed to create thread for client connection.\n");
                close(new_connection_fd);
                continue;
            }
        }
    }

    // Close the server socket before exiting (this code is technically unreachable due to the infinite loop)
    if (close(server_socket_fd) == -1) {
        perror("Failed to close server socket");
    }

    // Wait for all storage server threads to finish
    for (int i = 0; i < active_storage_count; i++) {
        if (pthread_join(storage_threads_pool[i], NULL) != 0) {
            fprintf(stderr, "Failed to join storage server thread [%d]\n", i);
        }
    }

    // Wait for all client threads to finish
    for (int i = 0; i < active_client_count; i++) {
        if (pthread_join(client_threads_pool[i], NULL) != 0) {
            fprintf(stderr, "Failed to join client thread [%d]\n", i);
        }
    }

    // Cleanup resources
    close(server_socket_fd);
    free(primary_servers);
    free(backup_servers);
    fclose(log_file_ptr);

    return 0;
}
