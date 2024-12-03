#include <stdio.h>
#include <semaphore.h>
#include <stdbool.h>
#define ALPHABET_SIZE (128)
// #define CHAR_TO_INDEX(c) ( \
//     ((c >= 'a' && c <= 'z') ? (c - 'a') : \
//     ((c >= 'A' && c <= 'Z') ? (c - 'A' + 26) : \
//     ((c >= '0' && c <= '9') ? (c - '0' + 52) : \
//     (c == '/' ? 62 : -1)))))

typedef struct ss_info
{
    char ip_addr[30];
    int port_no_ns;
    int port_no_client;
    int no_acc_paths;
    int socket_id;
    char accesible_paths[100][100];
    int total_paths;
    int nm_port;
} ss_info;

typedef struct ss_backup_info
{
    int has_dup;
    char ip_addr[30];
    int port_no_ns_b1;
    int port_no_ns_b2;
    int port_no_client_b1;
    int port_no_client_b2;
    int socket_id;
    int total_paths;
    char accesible_paths[100][100];
} ss_backup_info;

#define MAX_CHARS 100
#define CHUNK_SIZE 16
#define MAX_FILE_LENGTH 999999

struct LRUNode
{
    char key[100];
    ss_info *value;
    struct LRUNode *next;
    struct LRUNode *prev;
};

struct LRUcache
{
    struct LRUNode *front, *rear;
    int size;
    int capacity;
    
    // sem_t LRU_lock;
    
};
struct TrieNode
{
    struct TrieNode *children[ALPHABET_SIZE];
    ss_info *ss_ptr;
    bool isEndOfWord;
    
    // sem_t rw_queue;
};

struct TrieNode *getNode();
void *ss_handler(void *param);
void *client_handler(void *param);
ss_info *search(struct TrieNode *root, char *key);
void insert(struct TrieNode *root, const char *key, ss_info *ptr);
struct LRUcache *initLRUcache(int capacity);
void enqueue(struct LRUcache *queue, char *key, ss_info *value);
void dequeue(struct LRUcache *queue, char *key);
ss_info *getFromLRUcache(struct LRUcache *queue, char *key);
int FILE_(char *operation, int sockid, char *path);
int reconnectToSS(ss_info *ss_struct);
int COPY(char *path1, char *path2);
int execute(char *command, int *client_socket, struct LRUcache *lruQueue, struct TrieNode *root);
int copyHelper(int sender_sock, int receiver_sock, char *path1, char *path2);
int copyDir(ss_info *ss_struct_1, ss_info *ss_struct_2, int sender_id, int receiver_id, char *path1, char *path2);

int sendChunks(int sockid, char *buffer);
int receiveChunks(int sockid, char *recieve_buffer);
int logMessage(const char *message, int socket, int status);
