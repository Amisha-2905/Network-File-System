#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/tcp.h>
// Storage Server Constants
#define MAX_CLIENTS 100
#define MAX_CHARS 100
#define MAX_FILE_LENGTH 999999
#define MAX_PATHS 20
#define ALPHABET_SIZE 128
#define CHUNK_SIZE 16
#define PORT_NM 8000     // Naming Server's port for SS to connect
#define ALT_PORT_NM 8050 // Alternative port for Naming Server

#define LOG_FILE "log.txt"
#define MAX_LOG_MSG_SIZE 256
#define CHUNK_BUFFER_SIZE 4096
#define MAX_BUFFER_SIZE 999999
#define MAX_COMMAND_LENGTH 100

// Structure Definitions
typedef struct SS_INFO
{
    char ip_addr[30];
    int port_nm;
    int port_cln;
    int no_paths;
    int sockid;
    char accessible_paths[100][100];
} *SS_INFO;

typedef struct TrieNode
{
    struct TrieNode *children[ALPHABET_SIZE];
    sem_t rw_queue;
    sem_t write_lock;
    bool isEndOfWord;
    bool isDir;
    int readers;
} TrieNode;

typedef struct CLIENT
{
    int type;
    int sockid;
    struct sockaddr_in addr;
} *CLIENT;

typedef struct Response
{
    int responseCode;
    char *responseBuffer;
} *Response;

// Global Variables
TrieNode *trie_root;
sem_t trie_lock;

// Function Prototypes
TrieNode *getNode(void);
void insert(TrieNode *root, const char *key);
TrieNode *GetTrieNode(TrieNode *root, const char *key);
void deleteKey(TrieNode *root, const char *key);
bool isNodeEmpty(TrieNode *root);

int CreateFileDirectory(char *token, char *return_buffer);
int DeleteFileDirectory(char *token, char *return_buffer);
int logMessage(const char *message, int socket, int status);
int sendChunks(int socket_descriptor, char *data_buffer);
int receiveChunks(int socket_descriptor, char *destination_buffer);
int ReadFile(int sockid, char *token, char *return_buffer);
int WriteFile(int sockid, char *token, char *return_buffer);
int GetSizeAndPermissions(char *token, char *return_buffer);
int getFilesInDir(char *token, char *return_buffer);
int PutFile(int sockid, char *token, char *return_buffer);

void *client_handler(void *arg);
void *naming_handler(void *arg);

// Termination Signal
const char *termination_signal = "<STOP>";

// Function Implementations

// Trie Functions
TrieNode *getNode(void)
{
    TrieNode *pNode = (TrieNode *)malloc(sizeof(TrieNode));
    if (pNode)
    {
        pNode->isDir = true;
        pNode->isEndOfWord = false;
        pNode->readers = 0;
        for (int i = 0; i < ALPHABET_SIZE; i++)
            pNode->children[i] = NULL;
    }
    return pNode;
}

void insert(TrieNode *root, const char *key)
{
    sem_wait(&trie_lock);
    int length = strlen(key);
    TrieNode *pCrawl = root;

    for (int level = 0; level < length; level++)
    {
        int index = (unsigned char)key[level];
        if (!pCrawl->children[index])
            pCrawl->children[index] = getNode();
        pCrawl = pCrawl->children[index];
    }

    if (!pCrawl->isEndOfWord)
    {
        if (sem_init(&pCrawl->rw_queue, 0, 1) != 0)
        {
            fprintf(stderr, "Semaphore rw_queue initialization failed\n");
            exit(EXIT_FAILURE);
        }
        if (sem_init(&pCrawl->write_lock, 0, 1) != 0)
        {
            fprintf(stderr, "Semaphore write_lock initialization failed\n");
            exit(EXIT_FAILURE);
        }
    }

    pCrawl->isEndOfWord = true;
    if (strchr(key, '.') == NULL)
        pCrawl->isDir = true;
    else
        pCrawl->isDir = false;

    sem_post(&trie_lock);
}

int get_local_ip(char *ip_buffer, size_t buffer_size) {
    char hostname[256];
    struct addrinfo hints, *res, *p;

    // Get the hostname of the local machine
    if (gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname failed");
        return -1;
    }

    // Set up hints for getaddrinfo
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // AF_INET for IPv4
    hints.ai_socktype = SOCK_STREAM;

    // Get the address info
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        perror("getaddrinfo failed");
        return -1;
    }

    // Loop through the results and pick the first one
    for (p = res; p != NULL; p = p->ai_next) {
        struct sockaddr_in *addr = (struct sockaddr_in *)p->ai_addr;
        if (inet_ntop(AF_INET, &addr->sin_addr, ip_buffer, buffer_size) != NULL) {
            freeaddrinfo(res);
            return 0;  // Success
        }
    }

    fprintf(stderr, "No valid IP address found\n");
    freeaddrinfo(res);
    return -1;
}

TrieNode *GetTrieNode(TrieNode *root, const char *key)
{
    sem_wait(&trie_lock);
    int length = strlen(key);
    TrieNode *pCrawl = root;

    for (int level = 0; level < length; level++)
    {
        int index = (unsigned char)key[level];
        if (!pCrawl->children[index])
        {
            sem_post(&trie_lock);
            insert(root, key); // Insert the entire key if not present
            sem_wait(&trie_lock);
            pCrawl = root;
            for (int i = 0; i < length; i++)
            {
                int idx = (unsigned char)key[i];
                pCrawl = pCrawl->children[idx];
            }
            break;
        }
        pCrawl = pCrawl->children[index];
    }
    sem_post(&trie_lock);
    return pCrawl;
}

/**
 * Recursively deletes a key from the Trie.
 * Returns true if the parent should delete the reference to this node.
 */
bool deleteHelper(TrieNode *current, const char *key, int depth, int key_length) {
    if (current == NULL) {
        return false;
    }

    // If we've reached the end of the key
    if (depth == key_length) {
        if (current->isEndOfWord) {
            current->isEndOfWord = false;

            // If the node has no children, it can be deleted
            return isNodeEmpty(current);
        }
        return false;
    }

    int index = (unsigned char)key[depth];
    if (deleteHelper(current->children[index], key, depth + 1, key_length)) {
        // Free the child node and remove the reference
        free(current->children[index]);
        current->children[index] = NULL;

        // Return true if current node is not end of another word and has no other children
        return !current->isEndOfWord && isNodeEmpty(current);
    }

    return false;
}

/**
 * Deletes a key from the Trie with proper synchronization.
 */
void deleteKey(TrieNode *root, const char *key) {
    if (root == NULL || key == NULL) {
        fprintf(stderr, "deleteKey: Invalid arguments.\n");
        return;
    }

    sem_wait(&trie_lock);

    int key_length = strlen(key);
    bool shouldDeleteRoot = deleteHelper(root, key, 0, key_length);

    if (shouldDeleteRoot) {
        // If root needs to be deleted, which typically shouldn't happen
        // Handle accordingly if your Trie implementation allows
        fprintf(stderr, "deleteKey: Attempting to delete the root node.\n");
    }

    sem_post(&trie_lock);
}

bool isNodeEmpty(TrieNode *node) {
    if (node == NULL) {
        return true;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i] != NULL) {
            return false;
        }
    }
    return true;
}

// Logging Function
int logMessage(const char *message, int socket, int status)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (getpeername(socket, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        char ipAddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipAddr, INET_ADDRSTRLEN);

        char logMsg[MAX_LOG_MSG_SIZE];
        int msgLength = snprintf(logMsg, MAX_LOG_MSG_SIZE, "[%s:%d] %s - Status: %d\n",
                                 ipAddr, ntohs(addr.sin_port), message, status);

        int log_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
        if (log_fd == -1)
        {
            fprintf(stderr, "Error opening log file\n");
            return -1;
        }

        if (write(log_fd, logMsg, msgLength) == -1)
        {
            fprintf(stderr, "Error writing to log file\n");
            close(log_fd);
            return -1;
        }

        close(log_fd);
        return 0;
    }
    else
    {
        fprintf(stderr, "Error getting peer name\n");
        return -1;
    }
}

// Chunked Data Transmission
int sendChunks(int socket_descriptor, char *data_buffer)
{
    send(socket_descriptor,data_buffer,strlen(data_buffer),0);
    return 0;
}

int receiveChunks(int socket_descriptor, char *destination_buffer)
{
   recv(socket_descriptor,BUFSIZ,CHUNK_BUFFER_SIZE,0);
    // Log the received data
    if (logMessage(destination_buffer, socket_descriptor, 1) != 0)
    {
        fprintf(stderr, "receiveChunks: Failed to log received message.\n");
        return -1;
    }

    return 1;
}

// File and Directory Operations

int CreateFileDirectory(char *token, char *return_buffer)
{
    if (strchr(token, '.'))
    {
        int fd;
        if ((fd = open(token, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1)
        {
            fprintf(stderr, "Error creating file: %s\n", token);
            strcpy(return_buffer, "[400] Error creating file\n");
            return -1;
        }
        close(fd);
        strcpy(return_buffer, "[200] File created Successfully!\n");
        insert(trie_root, token);
        return 0;
    }
    if (mkdir(token, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0)
    {
        fprintf(stderr, "Error creating directory: %s\n", token);
        return -1;
    }
    strcpy(return_buffer, "[200] Directory created Successfully!\n");

    insert(trie_root, token);
    return 0;
}

int DeleteFileDirectory(char *token, char *return_buffer)
{
    TrieNode *node = GetTrieNode(trie_root, token);
    sem_wait(&node->rw_queue);
    if (strchr(token, '.'))
    {
        if (remove(token) != 0)
        {
            perror("Error deleting file");
            sem_post(&node->rw_queue);
            return -1;
        }
        strcpy(return_buffer, "Deleted file successfully!\n");
    }
    else
    {
        if (rmdir(token) != 0)
        {
            perror("Error removing directory");
            sem_post(&node->rw_queue);
            return -1;
        }
        strcpy(return_buffer, "Directory removed successfully!\n");
    }
    sem_post(&node->rw_queue);
    deleteKey(trie_root, token);
    return 0;
}

int ReadFile(int socket_fd, char *file_token, char *output_buffer) {
    if (file_token == NULL || output_buffer == NULL) {
        fprintf(stderr, "ReadFile: Invalid arguments.\n");
        return -1;
    }

    int fd = open(file_token, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "ReadFile: Error opening file %s.\n", file_token);
        strncpy(output_buffer, "[400] File unavailable\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }


    char read_buffer[CHUNK_SIZE + 1]; // +1 for null terminator
    int chunk_index = 0;

    while (1) {
        ssize_t bytes_read = read(fd, read_buffer, CHUNK_SIZE);
        if (bytes_read == -1) {
            fprintf(stderr, "ReadFile: Error reading file %s.\n", file_token);
            strncpy(output_buffer, "[500] Error reading file\n", MAX_COMMAND_LENGTH - 1);
            output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
            close(fd);
       
            return -1;
        }

        if (bytes_read == 0) {
            break; // EOF reached
        }

        read_buffer[bytes_read] = '\0';

        if (send(socket_fd, read_buffer, CHUNK_SIZE, 0) == -1) {
            fprintf(stderr, "ReadFile: Error sending data for file %s.\n", file_token);
            strncpy(output_buffer, "[500] Error sending file data\n", MAX_COMMAND_LENGTH - 1);
            output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
            close(fd);
         
            return -1;
        }

        chunk_index++;
    }




    close(fd);

    // Send termination signal
    if (send(socket_fd, termination_signal, CHUNK_SIZE, 0) == -1) {
        perror("ReadFile: Failed to send termination signal");
        return -1;
    }

    return 0;
}

/**
 * Writes data received from a socket into a file.
 * Returns 0 on success, -1 on failure, or specific error codes.
 */
int WriteFile(int socket_fd, char *file_token, char *output_buffer) {
    if (file_token == NULL || output_buffer == NULL) {
        fprintf(stderr, "WriteFile: Invalid arguments.\n");
        return -1;
    }

    char write_buffer[MAX_FILE_LENGTH];
    char sanitized_filename[100];
    memset(sanitized_filename, 0, sizeof(sanitized_filename));
    strncpy(sanitized_filename, file_token, sizeof(sanitized_filename) - 1);
    sanitized_filename[sizeof(sanitized_filename) - 1] = '\0';

    TrieNode *trie_node = GetTrieNode(trie_root, sanitized_filename);
    if (trie_node == NULL) {
        fprintf(stderr, "WriteFile: TrieNode for %s not found.\n", sanitized_filename);
        strncpy(output_buffer, "[404] File not found in trie\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    int semaphore_val;
    sem_getvalue(&trie_node->write_lock, &semaphore_val);
    if (semaphore_val <= 0) {
        fprintf(stderr, "WriteFile: File %s is currently being read.\n", sanitized_filename);
        strncpy(output_buffer, "[301] File is being read. Unable to write at the moment.\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return 301;
    }

    int fd = open(sanitized_filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        fprintf(stderr, "WriteFile: Error opening file %s.\n", sanitized_filename);
        strncpy(output_buffer, "[404] Error opening the file\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    // Receive the content to write
    int received_chunks = receiveChunks(socket_fd, write_buffer);
    if (received_chunks == -1) {
        strncpy(output_buffer, "[500] Error receiving data to write\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        close(fd);
        return -1;
    }

    sem_wait(&trie_node->write_lock);

    ssize_t bytes_written = write(fd, write_buffer, strlen(write_buffer));
    if (bytes_written == -1) {
        fprintf(stderr, "WriteFile: Error writing to file %s.\n", sanitized_filename);
        strncpy(output_buffer, "[500] Error writing to the file\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        close(fd);
        sem_post(&trie_node->write_lock);
        return -1;
    }

    strncpy(output_buffer, "Successfully written to file\n", MAX_COMMAND_LENGTH - 1);
    output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';

    sem_post(&trie_node->write_lock);
    close(fd);
    return 0;
}

/**
 * Retrieves the size and permissions of a file.
 * Returns 0 on success, -1 on failure.
 */
int GetSizeAndPermissions(char *file_token, char *output_buffer) {
    if (file_token == NULL || output_buffer == NULL) {
        fprintf(stderr, "GetSizeAndPermissions: Invalid arguments.\n");
        return -1;
    }

    TrieNode *trie_node = GetTrieNode(trie_root, file_token);
    if (trie_node == NULL) {
        fprintf(stderr, "GetSizeAndPermissions: TrieNode for %s not found.\n", file_token);
        strncpy(output_buffer, "Error: File not found in trie.\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    sem_wait(&trie_node->rw_queue);

    struct stat file_info;
    if (stat(file_token, &file_info) == -1) {
        strncpy(output_buffer, "Error: Unable to retrieve file information.\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        perror("GetSizeAndPermissions: stat failed");
        sem_post(&trie_node->rw_queue);
        return -1;
    }

    sem_post(&trie_node->rw_queue);

    // Format the size and permissions into the output buffer
    int formatted_length = snprintf(output_buffer, MAX_COMMAND_LENGTH * 2,
                                    "File Size: %lld bytes\nFile Permissions: %o\n",
                                    (long long)file_info.st_size,
                                    file_info.st_mode & 0777);

    if (formatted_length < 0) {
        perror("GetSizeAndPermissions: snprintf failed");
        return -1;
    }

    return 0;
}

/**
 * Receives data and writes it to a file.
 * Returns 0 on success, -1 on failure, or specific error codes.
 */
int PutFile(int socket_fd, char *file_token, char *output_buffer) {
    if (file_token == NULL || output_buffer == NULL) {
        fprintf(stderr, "PutFile: Invalid arguments.\n");
        return -1;
    }

    TrieNode *trie_node = GetTrieNode(trie_root, file_token);
    if (trie_node == NULL) {
        fprintf(stderr, "PutFile: TrieNode for %s not found.\n", file_token);
        strncpy(output_buffer, "[404] File not found in trie\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    char sanitized_filename[100];
    memset(sanitized_filename, 0, sizeof(sanitized_filename));
    strncpy(sanitized_filename, file_token, sizeof(sanitized_filename) - 1);
    sanitized_filename[sizeof(sanitized_filename) - 1] = '\0';

    int fd = open(sanitized_filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        perror("PutFile: Error opening file");
        strncpy(output_buffer, "[404] Error opening the file\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    int semaphore_val;
    sem_getvalue(&trie_node->write_lock, &semaphore_val);
    if (semaphore_val <= 0) {
        fprintf(stderr, "PutFile: File %s is currently being read.\n", sanitized_filename);
        strncpy(output_buffer, "[301] File is being read. Unable to write at the moment.\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        close(fd);
        return 301;
    }

    sem_wait(&trie_node->write_lock);

    char read_buffer[CHUNK_SIZE + 1]; // +1 for null terminator
    int chunk_counter = 0;
    while (1) {
        ssize_t bytes_received = recv(socket_fd, read_buffer, CHUNK_SIZE, 0);
        if (bytes_received < 0) {
            perror("PutFile: recv failed");
            strncpy(output_buffer, "[500] Error receiving data\n", MAX_COMMAND_LENGTH - 1);
            output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
            close(fd);
            sem_post(&trie_node->write_lock);
            return -1;
        } else if (bytes_received == 0) {
            printf("PutFile: Connection closed by peer.\n");
            break;
        }

        read_buffer[bytes_received] = '\0';

        // Check for termination signal
        if (strcmp(read_buffer, termination_signal) == 0) {
            printf("PutFile: Termination signal received.\n");
            break;
        } else {
            sem_wait(&trie_node->rw_queue);
            ssize_t bytes_written = write(fd, read_buffer, strlen(read_buffer));
            sem_post(&trie_node->rw_queue);

            if (bytes_written == -1) {
                perror("PutFile: Error writing to file");
                strncpy(output_buffer, "[500] Error writing to the file\n", MAX_COMMAND_LENGTH - 1);
                output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
                close(fd);
                sem_post(&trie_node->write_lock);
                return -1;
            }
        }

        chunk_counter++;
    }

    close(fd);
    sem_post(&trie_node->write_lock);

    strncpy(output_buffer, "PUT: Content written successfully.\n", MAX_COMMAND_LENGTH - 1);
    output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
    return 0;
}

/**
 * Retrieves a list of files in a directory, separated by commas.
 * Returns 0 on success, -1 on failure.
 */
int getFilesInDir(char *directory_path, char *output_buffer) {
    if (directory_path == NULL || output_buffer == NULL) {
        fprintf(stderr, "getFilesInDir: Invalid arguments.\n");
        return -1;
    }

    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        perror("getFilesInDir: Failed to open directory");
        strncpy(output_buffer, "Error opening directory\n", MAX_COMMAND_LENGTH - 1);
        output_buffer[MAX_COMMAND_LENGTH - 1] = '\0';
        return -1;
    }

    struct dirent *entry;
    size_t buffer_index = 0;
    output_buffer[0] = '\0'; // Initialize as empty string

    while ((entry = readdir(directory)) != NULL) {
        // Skip the current and parent directory entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t name_length = strlen(entry->d_name);
        if (buffer_index + name_length + 2 > MAX_COMMAND_LENGTH) { // +1 for comma, +1 for null terminator
            fprintf(stderr, "getFilesInDir: Output buffer is full. Truncating file list.\n");
            break;
        }

        strcat(output_buffer, entry->d_name);
        strcat(output_buffer, ",");
        buffer_index += name_length + 1;
    }

    closedir(directory);

    // Remove the trailing comma if present
    size_t out_length = strlen(output_buffer);
    if (out_length > 0 && output_buffer[out_length - 1] == ',') {
        output_buffer[out_length - 1] = '\0';
    }

    printf("getFilesInDir: Retrieved files - %s\n", output_buffer);
    return 0;
}


// Client Handler
void *client_handler(void *arg)
{
    CLIENT client = (CLIENT)arg;
    printf("[+]Client connected. Socket: %d Port: %d\n", client->sockid, ntohs(client->addr.sin_port));

    char buffer[MAX_CHARS];
    memset(buffer, 0, MAX_CHARS);

    // Receive the command from the client
    int n = recv(client->sockid, buffer, sizeof(buffer), 0);
    if (n == -1)
    {
        perror("recv");
        close(client->sockid);
        free(client);
        pthread_exit(NULL);
    }
    printf("Client: %s\n", buffer);

    char return_buffer[MAX_FILE_LENGTH];
    memset(return_buffer, 0, MAX_FILE_LENGTH);

    char path[MAX_CHARS];
    strcpy(path, ".");

    char *operation = strtok(buffer, " ");
    if (operation == NULL)
    {
        strcpy(return_buffer, "Invalid command format.\n");
        sendChunks(client->sockid, return_buffer);
        close(client->sockid);
        free(client);
        pthread_exit(NULL);
    }

    if (strcmp(operation, "CHECK") == 0)
    {
        // Implement CHECK operation if needed
        strcpy(return_buffer, "CHECK operation not implemented.\n");
        sendChunks(client->sockid, return_buffer);
        close(client->sockid);
        free(client);
        pthread_exit(NULL);
    }

    char *def_path = strtok(NULL, " ");
    if (def_path == NULL)
    {
        strcpy(return_buffer, "Path not defined :(\n");
        sendChunks(client->sockid, return_buffer);
        close(client->sockid);
        free(client);
        pthread_exit(NULL);
    }

    if (def_path[0] == '/')
        strcat(path, def_path);
    else
        strncpy(path, def_path, sizeof(path) - 1);

    if (client->type)
    { // Naming Server Operations
        if (strcmp(operation, "CREATE") == 0)
        {
            int response = CreateFileDirectory(path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] CREATE operation failed.\n");
        }
        else if (strcmp(operation, "DELETE") == 0)
        {
            int response = DeleteFileDirectory(path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] DELETE operation failed.\n");
        }
        else if (strcmp(operation, "GIVEFILES") == 0)
        {
            int response = getFilesInDir(path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] GIVEFILES operation failed.\n");
        }
        else if (strcmp(operation, "GET") == 0)
        {
            int response = ReadFile(client->sockid, path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] GET operation failed.\n");
        }
        else if (strcmp(operation, "PUT") == 0)
        {
            int response = PutFile(client->sockid, path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] PUT operation failed.\n");
        }
        sendChunks(client->sockid, return_buffer);
    }
    else
    { // Client Operations
        if (strcmp(operation, "READ") == 0)
        {
            int response = ReadFile(client->sockid, path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] READ operation failed.\n");
        }
        else if (strcmp(operation, "WRITE") == 0)
        {
            int response = WriteFile(client->sockid, path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] WRITE operation failed.\n");
        }
        else if (strcmp(operation, "INFO") == 0)
        {
            int response = GetSizeAndPermissions(path, return_buffer);
            if (response == -1)
                strcpy(return_buffer, "[500] INFO operation failed.\n");
        }
        sendChunks(client->sockid, return_buffer);
    }

    close(client->sockid);
    free(client);
    pthread_exit(NULL);
}

// Naming Server Handler
void *naming_handler(void *arg)
{
    int nm_sock = *(int *)arg;

    while (1)
    {
        CLIENT NM = (CLIENT)malloc(sizeof(struct CLIENT));
        socklen_t addr_size = sizeof(NM->addr);

        // Accepting Naming Server connections
        NM->sockid = accept(nm_sock, (struct sockaddr *)&NM->addr, &addr_size);
        if (NM->sockid == -1)
        {
            perror("accept in naming_handler");
            free(NM);
            continue;
        }
        NM->type = 1;
        printf("[+] NM connected. Socket: %d\n", NM->sockid);

        pthread_t nm_handler;
        if (pthread_create(&nm_handler, NULL, client_handler, NM) != 0)
        {
            perror("pthread_create for NM handler");
            close(NM->sockid);
            free(NM);
            continue;
        }
        pthread_detach(nm_handler); // Detach thread to reclaim resources on exit
    }

    pthread_exit(NULL);
}

// Main Function
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <Client Port> <Naming Server Reverse Port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port_cln = atoi(argv[1]);
    int PORT_NM_REV = atoi(argv[2]);

    char serv_ip[32]; // Buffer for server IP address
    get_local_ip(serv_ip, sizeof(serv_ip));
    char nm_ip[32]="127.0.0.1";
    

    SS_INFO ss_info = (SS_INFO)malloc(sizeof(struct SS_INFO));
    if (ss_info == NULL)
    {
        perror("malloc for ss_info failed");
        exit(EXIT_FAILURE);
    }

    strncpy(ss_info->ip_addr, serv_ip, sizeof(ss_info->ip_addr) - 1);
    ss_info->ip_addr[sizeof(ss_info->ip_addr) - 1] = '\0';
    ss_info->port_cln = port_cln;
    ss_info->port_nm = PORT_NM_REV;

    trie_root = getNode();
    if (trie_root == NULL)
    {
        perror("Failed to initialize trie_root");
        exit(EXIT_FAILURE);
    }

    if (sem_init(&trie_lock, 0, 1) != 0)
    {
        perror("Semaphore trie_lock initialization failed");
        exit(EXIT_FAILURE);
    }

    int N;
    printf("No. of accessible paths: ");
    if (scanf("%d", &N) != 1 || N > MAX_PATHS)
    {
        fprintf(stderr, "Invalid number of paths.\n");
        exit(EXIT_FAILURE);
    }
    ss_info->no_paths = N;

    char paths[MAX_PATHS][100];
    for (int i = 0; i < N; i++)
    {
        if (scanf("%99s", paths[i]) != 1)
        {
            fprintf(stderr, "Error reading path.\n");
            exit(EXIT_FAILURE);
        }
        insert(trie_root, paths[i]);
        strncpy(ss_info->accessible_paths[i], paths[i], sizeof(ss_info->accessible_paths[i]) - 1);
        ss_info->accessible_paths[i][sizeof(ss_info->accessible_paths[i]) - 1] = '\0';
    }

    int nm_sock, ss_sock,flag=1;
    struct sockaddr_in nm_addr, ss_addr;
    socklen_t addr_size;

    // Initialize Storage Server socket
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0)
    {
        perror("Storage Server socket creation failed");
        exit(EXIT_FAILURE);
    }
    ss_info->sockid = ss_sock;
    if(setsockopt(ss_sock,IPPROTO_TCP,TCP_NODELAY,(char* )&flag,sizeof(flag)));
    // Initialize Naming Server socket
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0)
    {
        perror("Naming Server socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("* | Created Storage Server's socket for Naming Server on port %d.\n", PORT_NM);

    // Configure Naming Server address
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(PORT_NM); // Use htons for network byte order
    if (inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr) <= 0)
    {
        perror("Invalid Naming Server IP address");
        exit(EXIT_FAILURE);
    }

    // Connect to Naming Server
    int conn = connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr));
    if (conn == -1)
    {
        perror("Connect to Naming Server failed, trying alternative port");
        nm_addr.sin_port = htons(ALT_PORT_NM);
        conn = connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr));
        if (conn == -1)
        {
            perror("Alternative connection to Naming Server failed");
            close(nm_sock);
            exit(EXIT_FAILURE);
        }
    }
    printf("Connected to the Naming Server at port %d with status: %d\n", PORT_NM, conn);

    // Register with Naming Server
    char *ss_identifier = "SS";
    if (send(nm_sock, ss_identifier, MAX_CHARS, 0) == -1 ||
        send(nm_sock, &ss_info->no_paths, sizeof(int), 0) == -1 ||
        send(nm_sock, ss_info, sizeof(struct SS_INFO), 0) == -1)
    {
        perror("Failed to send registration data to Naming Server");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Close Naming Server connection after registration
    close(nm_sock);

    // Bind Naming Server Reverse Socket
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0)
    {
        perror("Naming Server Reverse socket creation failed");
        exit(EXIT_FAILURE);
    }
    if(setsockopt(nm_sock,IPPROTO_TCP,TCP_NODELAY,(char* )&flag,sizeof(flag)));

    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(PORT_NM_REV); // Use htons for network byte order
    if (inet_pton(AF_INET, serv_ip, &nm_addr.sin_addr) <= 0)
    {
        perror("Invalid server IP address");
        exit(EXIT_FAILURE);
    }

    if (bind(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0)
    {
        perror("Bind error for Naming Server Reverse socket");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }
    printf("* | Bound the Naming Server Reverse socket to port: %d\n", PORT_NM_REV);

    if (listen(nm_sock, MAX_CLIENTS) < 0)
    {
        perror("Listen error for Naming Server Reverse socket");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Start Naming Server handler thread
    pthread_t naming_thread;
    if (pthread_create(&naming_thread, NULL, naming_handler, &nm_sock) != 0)
    {
        perror("Failed to create Naming Server handler thread");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }
    pthread_detach(naming_thread); // Detach to reclaim resources on exit

    // Bind Client Socket
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port_cln); // Use htons for network byte order
    if (inet_pton(AF_INET, serv_ip, &ss_addr.sin_addr) <= 0)
    {
        perror("Invalid server IP address for client socket");
        exit(EXIT_FAILURE);
    }

    if (bind(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("Bind error for client socket");
        close(ss_sock);
        exit(EXIT_FAILURE);
    }
    printf("* | Bound to the client port number: %d\n", port_cln);

    if (listen(ss_sock, MAX_CLIENTS) < 0)
    {
        perror("Listen error for client socket");
        close(ss_sock);
        exit(EXIT_FAILURE);
    }
    printf("* | Listening for clients...\n");

    // Handle Client Connections
    pthread_t threads[MAX_CLIENTS];
    int no_clients = 0;

    while (1)
    {
        CLIENT client = (CLIENT)malloc(sizeof(struct CLIENT));
        if (client == NULL)
        {
            perror("malloc for client failed");
            continue;
        }
        addr_size = sizeof(client->addr);

        // Accept client connection
        client->sockid = accept(ss_sock, (struct sockaddr *)&client->addr, &addr_size);
        if (client->sockid == -1)
        {
            perror("accept for client failed");
            free(client);
            continue;
        }
        client->type = 0;

        // Create thread for client
        if (pthread_create(&threads[no_clients++], NULL, client_handler, client) != 0)
        {
            perror("pthread_create for client_handler failed");
            close(client->sockid);
            free(client);
            continue;
        }

        // Detach thread to prevent memory leaks
        pthread_detach(threads[no_clients - 1]);

        if (no_clients >= MAX_CLIENTS)
        {
            fprintf(stderr, "Maximum number of clients reached.\n");
            break;
        }
    }

    // Cleanup (This part is theoretically unreachable in an infinite server loop)
    close(ss_sock);
    sem_destroy(&trie_lock);
    free(ss_info);
    return 0;
}
