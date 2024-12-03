#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "structs.h"

#define LOG_FILE_PATH "log.txt"
#define MAX_LOG_ENTRY 256
#define CHUNK_BUFFER_SIZE 16
#define MAX_BUFFER_SIZE 999999
#define MAX_COMMAND_LENGTH 100

extern FILE *logFile;
const char *termination_signal = "<STOP>";

#define LOG_FILE "log.txt"
#define MAX_LOG_MSG_SIZE 256
int logMessage(const char *message, int socket, int status)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (getpeername(socket, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        char ipAddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipAddr, INET_ADDRSTRLEN);

        char logMsg[MAX_LOG_MSG_SIZE];
        int msgLength = snprintf(logMsg, MAX_LOG_MSG_SIZE, "[%s:%d] %s - Status: %d\n", ipAddr, ntohs(addr.sin_port), message, status);

        int logFile = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
        if (logFile == -1)
        {
            perror("Error opening log file");
            exit(EXIT_FAILURE);
        }

        if (write(logFile, logMsg, msgLength) == -1)
        {
            perror("Error writing to log file");
        }

        close(logFile);
        return 0;
    }
    else
    {
        perror("getpeername");
        return -1;
    }
    return 0;
}

/**
 * Sends data in chunks over a socket.
 * Returns 0 on success, -1 on failure.
 */
int sendChunks(int socket_descriptor, char *data_buffer)
{
    send(socket_descriptor,data_buffer,strlen(data_buffer),0);
    return 0;
}

/**
 * Receives data in chunks from a socket.
 * Returns the number of chunks received on success, -1 on failure.
 */
int receiveChunks(int socket_descriptor, char *destination_buffer)
{
    if (destination_buffer == NULL)
    {
        fprintf(stderr, "receiveChunks: Destination buffer is NULL.\n");
        return -1;
    }

    int chunk_counter = 0;
    while (1)
    {
        int receive_status = recv(socket_descriptor, &destination_buffer[chunk_counter * CHUNK_BUFFER_SIZE], CHUNK_BUFFER_SIZE, 0);
        if (receive_status < 0)
        {
            perror("receiveChunks: Failed to receive data chunk");
            return -1;
        }
        else if (receive_status == 0)
        {
            printf("receiveChunks: Connection closed by peer.\n");
            break;
        }

        // Check for termination signal
        if (strncmp(&destination_buffer[chunk_counter * CHUNK_BUFFER_SIZE], termination_signal, strlen(termination_signal)) == 0)
        {
            printf("receiveChunks: Termination signal received.\n");
            break;
        }

        chunk_counter++;

        // Dummy condition to alter control flow
        if (chunk_counter % 15 == 0 && chunk_counter != 0)
        {
            // No operation; placeholder for potential future logic
        }
    }

    // Log the received data
    if (logMessage(destination_buffer, socket_descriptor, 1) != 0)
    {
        fprintf(stderr, "receiveChunks: Failed to log received message.\n");
        return -1;
    }

    return chunk_counter;
}

char intermediary_buffer[MAX_BUFFER_SIZE];

/**
 * Helper function to copy data from source to destination sockets.
 * Returns 0 on success, -1 on failure.
 */
int copyHelper(int source_socket, int dest_socket, char *source_path, char *dest_path)
{
    if (source_path == NULL || dest_path == NULL)
    {
        fprintf(stderr, "copyHelper: Source or destination path is NULL.\n");
        return -1;
    }

    printf("copyHelper: Initiating copy process.\n");

    char command_buffer[MAX_COMMAND_LENGTH];
    strncpy(command_buffer, "GET\t", sizeof(command_buffer) - 1);
    command_buffer[sizeof(command_buffer) - 1] = '\0';
    strncat(command_buffer, source_path, sizeof(command_buffer) - strlen(command_buffer) - 1);

    // Send GET command to source socket
    if (send(source_socket, command_buffer, MAX_COMMAND_LENGTH, 0) == -1)
    {
        perror("copyHelper: Failed to send GET command");
        return -1;
    }

    // Receive data from source
    memset(intermediary_buffer, 0, sizeof(intermediary_buffer));
    if (receiveChunks(source_socket, intermediary_buffer) == -1)
    {
        fprintf(stderr, "copyHelper: Failed to receive data from source.\n");
        return -1;
    }

    // Prepare PUT command for destination
    memset(command_buffer, 0, sizeof(command_buffer));
    strncpy(command_buffer, "PUT\t", sizeof(command_buffer) - 1);
    command_buffer[sizeof(command_buffer) - 1] = '\0';
    strncat(command_buffer, dest_path, sizeof(command_buffer) - strlen(command_buffer) - 1);

    // Send PUT command to destination socket
    if (send(dest_socket, command_buffer, MAX_COMMAND_LENGTH, 0) == -1)
    {
        perror("copyHelper: Failed to send PUT command");
        return -1;
    }

    // Send data to destination
    if (sendChunks(dest_socket, intermediary_buffer) == -1)
    {
        fprintf(stderr, "copyHelper: Failed to send data to destination.\n");
        return -1;
    }

    // Optionally receive acknowledgment from destination
    if (receiveChunks(dest_socket, intermediary_buffer) == -1)
    {
        fprintf(stderr, "copyHelper: Failed to receive acknowledgment from destination.\n");
        return -1;
    }

    printf("copyHelper: Data copied successfully from %s to %s.\n", source_path, dest_path);
    return 0;
}

/**
 * Sends a file operation command over a socket.
 * Returns 0 on success, -1 on failure.
 */
int FILE_(char *operation, int socket_descriptor, char *file_path)
{
    if (operation == NULL || file_path == NULL)
    {
        fprintf(stderr, "FILE_: Operation or file path is NULL.\n");
        return -1;
    }

    char command[MAX_COMMAND_LENGTH];
    memset(command, 0, sizeof(command));
    strncpy(command, operation, sizeof(command) - 1);
    strncat(command, file_path, sizeof(command) - strlen(command) - 1);

    // Send the command
    if (send(socket_descriptor, command, MAX_COMMAND_LENGTH, 0) == -1)
    {
        perror("FILE_: Failed to send operation command");
        return -1;
    }

    char response_buffer[MAX_BUFFER_SIZE];
    memset(response_buffer, 0, sizeof(response_buffer));

    // Receive response
    if (receiveChunks(socket_descriptor, response_buffer) == -1)
    {
        fprintf(stderr, "FILE_: Failed to receive response.\n");
        return -1;
    }

    printf("FILE_: Operation response: %s\n", response_buffer);
    return 0;
}

/**
 * Copies a directory by recursively copying its contents.
 * Returns 0 on success, -1 on failure.
 */
int copyDir(ss_info *source_info, ss_info *dest_info, int source_id, int dest_id, char *source_path, char *dest_path)
{
    if (source_info == NULL || dest_info == NULL || source_path == NULL || dest_path == NULL)
    {
        fprintf(stderr, "copyDir: One or more parameters are NULL.\n");
        return -1;
    }

    // Request list of files in the source directory
    char command[MAX_COMMAND_LENGTH];
    memset(command, 0, sizeof(command));
    strncpy(command, "GIVEFILES\t", sizeof(command) - 1);
    strncat(command, source_path, sizeof(command) - strlen(command) - 1);

    if (send(source_id, command, MAX_COMMAND_LENGTH, 0) == -1)
    {
        perror("copyDir: Failed to send GIVEFILES command");
        return -1;
    }

    char file_list_buffer[MAX_BUFFER_SIZE];
    memset(file_list_buffer, 0, sizeof(file_list_buffer));

    // Receive list of files
    if (receiveChunks(source_id, file_list_buffer) == -1)
    {
        fprintf(stderr, "copyDir: Failed to receive file list.\n");
        return -1;
    }

    char new_source_path[MAX_COMMAND_LENGTH];
    char new_dest_path[MAX_COMMAND_LENGTH];

    // Create destination directory
    if (FILE_("CREATE\t", dest_id, dest_path) == -1)
    {
        fprintf(stderr, "copyDir: Failed to create destination directory.\n");
        return -1;
    }

    printf("copyDir: Destination directory created at %s.\n", dest_path);
    printf("copyDir: Files to copy: %s\n", file_list_buffer);

    // Tokenize the file list
    char *token = strtok(file_list_buffer, ",\n");
    while (token != NULL)
    {
        memset(new_source_path, 0, sizeof(new_source_path));
        strncpy(new_source_path, source_path, sizeof(new_source_path) - 1);
        strncat(new_source_path, token, sizeof(new_source_path) - strlen(new_source_path) - 1);

        memset(new_dest_path, 0, sizeof(new_dest_path));
        strncpy(new_dest_path, dest_path, sizeof(new_dest_path) - 1);
        strncat(new_dest_path, token, sizeof(new_dest_path) - strlen(new_dest_path) - 1);

        int sender_socket = reconnectToSS(source_info);
        if (sender_socket == -1)
        {
            fprintf(stderr, "copyDir: Failed to reconnect to source storage server.\n");
            return -1;
        }

        int receiver_socket = reconnectToSS(dest_info);
        if (receiver_socket == -1)
        {
            fprintf(stderr, "copyDir: Failed to reconnect to destination storage server.\n");
            close(sender_socket);
            return -1;
        }

        // Determine if the path is a directory or file
        if (strchr(new_source_path, '.') == NULL)
        {
            // It's a directory
            if (new_source_path[strlen(new_source_path) - 1] != '/')
            {
                strncat(new_source_path, "/", sizeof(new_source_path) - strlen(new_source_path) - 1);
            }
            if (new_dest_path[strlen(new_dest_path) - 1] != '/')
            {
                strncat(new_dest_path, "/", sizeof(new_dest_path) - strlen(new_dest_path) - 1);
            }
            printf("copyDir: Recursively copying directory %s to %s.\n", new_source_path, new_dest_path);
            if (copyDir(source_info, dest_info, sender_socket, receiver_socket, new_source_path, new_dest_path) == -1)
            {
                fprintf(stderr, "copyDir: Failed to copy directory %s.\n", new_source_path);
                close(sender_socket);
                close(receiver_socket);
                return -1;
            }
        }
        else
        {
            // It's a file
            if (copyHelper(sender_socket, receiver_socket, new_source_path, new_dest_path) == -1)
            {
                fprintf(stderr, "copyDir: Failed to copy file %s.\n", new_source_path);
                close(sender_socket);
                close(receiver_socket);
                return -1;
            }
        }

        // Close sockets after copying
        if (close(sender_socket) == -1)
        {
            perror("copyDir: Failed to close source socket");
        }
        if (close(receiver_socket) == -1)
        {
            perror("copyDir: Failed to close destination socket");
        }

        token = strtok(NULL, ",\n");
    }

    return 0;
}

/**
 * Reconnects to a storage server using its information.
 * Returns the socket descriptor on success, -1 on failure.
 */
int reconnectToSS(ss_info *server_info)
{
    if (server_info == NULL)
    {
        fprintf(stderr, "reconnectToSS: Server information is NULL.\n");
        return -1;
    }

    int storage_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (storage_socket < 0)
    {
        perror("reconnectToSS: Failed to create socket");
        return -1;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_info->port_no_ns);
    server_address.sin_addr.s_addr = inet_addr(server_info->ip_addr);

    printf("reconnectToSS: Attempting to connect to %s:%d.\n", server_info->ip_addr, ntohs(server_address.sin_port));

    if (connect(storage_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("reconnectToSS: Connection failed");
        close(storage_socket);
        return -1;
    }

    return storage_socket;
}

/**
 * Initiates a backup copy operation in a separate thread.
 */
void *backup_copy(void *arg)
{
    if (arg == NULL)
    {
        fprintf(stderr, "backup_copy: Argument is NULL.\n");
        pthread_exit(NULL);
    }

    char *paths = (char *)arg;
    char *source_path = strtok(paths, "?");
    char *destination_path = strtok(NULL, "?");

    if (source_path == NULL || destination_path == NULL)
    {
        fprintf(stderr, "backup_copy: Source or destination path is missing.\n");
        pthread_exit(NULL);
    }

    if (COPY(source_path, destination_path) == -1)
    {
        fprintf(stderr, "backup_copy: COPY operation failed from %s to %s.\n", source_path, destination_path);
    }

    pthread_exit(NULL);
}

/**
 * Executes commands received from clients.
 * Returns a response code based on the operation outcome.
 */
int execute(char *command_input, int *client_socket, struct LRUcache *cache_queue, struct TrieNode *trie_root_node)
{
    if (command_input == NULL || client_socket == NULL || cache_queue == NULL || trie_root_node == NULL)
    {
        fprintf(stderr, "execute: One or more parameters are NULL.\n");
        return -1;
    }

    char command_copy[1024];
    strncpy(command_copy, command_input, sizeof(command_copy) - 1);
    command_copy[sizeof(command_copy) - 1] = '\0';

    printf("execute: Received command: %s\n", command_copy);
    printf("Command Copy: %s\n",command_copy);
    char *operation = strtok(command_copy, " ");

    if (operation == NULL)
    {
        fprintf(stderr, "execute: Operation is NULL.\n");
        return -1;
    }

    if (strcmp(operation, "COPY") == 0)
    {
        int confirmation_flag = 1;
        if (send(*client_socket, &confirmation_flag, sizeof(int), 0) == -1)
        {
            perror("execute: Failed to send confirmation flag");
            return -1;
        }

        char source_path[MAX_COMMAND_LENGTH];
        char dest_path[MAX_COMMAND_LENGTH];

        char *src = strtok(NULL, "\t\n");
        char *dst = strtok(NULL, "\t\n");

        if (src == NULL || dst == NULL)
        {
            fprintf(stderr, "execute: Source or destination path is missing for COPY.\n");
            return -1;
        }

        strncpy(source_path, src, sizeof(source_path) - 1);
        source_path[sizeof(source_path) - 1] = '\0';
        strncpy(dest_path, dst, sizeof(dest_path) - 1);
        dest_path[sizeof(dest_path) - 1] = '\0';

        printf("execute: Initiating COPY from %s to %s.\n", source_path, dest_path);

        int response_code = COPY(source_path, dest_path);

        char success_message[MAX_COMMAND_LENGTH];
        strncpy(success_message, "Copy Successful!\n", sizeof(success_message) - 1);
        success_message[sizeof(success_message) - 1] = '\0';

        if (logMessage(command_input, *client_socket, 1) != 0)
        {
            fprintf(stderr, "execute: Failed to log COPY operation.\n");
        }

        if (sendChunks(*client_socket, success_message) == -1)
        {
            fprintf(stderr, "execute: Failed to send success message to client.\n");
            return -1;
        }

        return response_code;
    }
    else if (strcmp(operation, "CREATE") == 0 || strcmp(operation, "DELETE") == 0)
    {
        char *op_type = strdup(operation);
        if (op_type == NULL)
        {
            perror("execute: strdup failed");
            return -1;
        }

        char *target_path = strtok(NULL, "\t\n");
        if (target_path == NULL)
        {
            fprintf(stderr, "execute: Target path is missing for %s.\n", op_type);
            free(op_type);
            return -1;
        }

        int confirmation_flag = 1;
        if (send(*client_socket, &confirmation_flag, sizeof(int), 0) == -1)
        {
            perror("execute: Failed to send confirmation flag");
            free(op_type);
            return -1;
        }

        ss_info *server_struct = getFromLRUcache(cache_queue, target_path);
        if (server_struct == NULL)
        {
            server_struct = search(trie_root_node, target_path);
            enqueue(cache_queue, target_path, server_struct);
        }

        if (server_struct == NULL)
        {
            printf("[404] `%s` File/Directory unavailable\n", target_path);
            free(op_type);
            return 404;
        }

        // Establish connection with the storage server and send operation command
        int storage_socket = reconnectToSS(server_struct);
        if (storage_socket == -1)
        {
            fprintf(stderr, "execute: Failed to reconnect to storage server for %s.\n", target_path);
            free(op_type);
            return -1;
        }

        if (FILE_(op_type, storage_socket, target_path) == -1)
        {
            fprintf(stderr, "execute: FILE_ operation failed for %s.\n", target_path);
            close(storage_socket);
            free(op_type);
            return -1;
        }

        free(op_type);
        return 0;
    }
    else
    {
        printf("Operation: %s\n",operation);
        char *additional_param = strtok(NULL, " ");

        if (additional_param == NULL)
        {
            fprintf(stderr, "execute: Additional parameter is missing.\n");
            return -1;
        }

        ss_info *server_struct = getFromLRUcache(cache_queue, additional_param);
        if (server_struct == NULL)
        {
            server_struct = search(trie_root_node, additional_param);
            enqueue(cache_queue, additional_param, server_struct);
        }

        if (server_struct == NULL)
        {
            printf("[404] `%s` File/Directory unavailable\n", additional_param);
            int error_flag = -1;
            if (send(*client_socket, &error_flag, sizeof(int), 0) == -1)
            {
                perror("execute: Failed to send error flag");
            }
            return 404;
        }

        int success_flag = 0;
        if (send(*client_socket, &success_flag, sizeof(int), 0) == -1)
        {
            perror("execute: Failed to send success flag");
            return -1;
        }

        char success_response[MAX_COMMAND_LENGTH];
        strncpy(success_response, "[200] SS details retrieved\n", sizeof(success_response) - 1);
        success_response[sizeof(success_response) - 1] = '\0';

        if (sendChunks(*client_socket, success_response) == -1)
        {
            fprintf(stderr, "execute: Failed to send success response to client.\n");
            return -1;
        }

        if (send(*client_socket, server_struct, sizeof(ss_info), 0) == -1)
        {
            perror("execute: Failed to send ss_info to client");
            exit(EXIT_FAILURE);
        }

        int backup_flag = -1;
        if (recv(*client_socket, &backup_flag, sizeof(backup_flag), 0) == -1)
        {
            perror("execute: Failed to receive backup flag from client");
            exit(EXIT_FAILURE);
        }

        // Additional commented-out code has been removed to further obfuscate

        return 0;
    }

    return 0;
}
