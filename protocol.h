#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_PATH_LEN 256
#define MAX_PATHS 50
#define MAX_DATA_SIZE 16000
#define IP_LEN 16

typedef enum
{
    MSG_REGISTER,
    MSG_REGISTER_ACK,
    MSG_LOOKUP,
    MSG_LOOKUP_ACK,
    MSG_READ,
    MSG_WRITE,
    MSG_STREAM,
    MSG_CREATE,
    MSG_DELETE,
    MSG_COPY,
    MSG_LIST,
    MSG_INFO,
    MSG_ACK,
    MSG_ERROR
} MsgType;

typedef enum
{
    SUCCESS = 0,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_BUSY_WRITING,
    ERR_SS_UNREACHABLE,
    ERR_INVALID_PATH,
    ERR_PERMISSION_DENIED
} ErrorCode;

typedef struct
{
    char ip[IP_LEN];
    int nm_port;
    int client_port;
    int path_count;
    char paths[MAX_PATHS][MAX_PATH_LEN];
} SS_RegisterPayload;

typedef struct
{
    uint32_t msg_type;
    uint32_t error_code;
    uint32_t data_size;
    char path[MAX_PATH_LEN];
    union
    {
        char text[MAX_DATA_SIZE];
        SS_RegisterPayload ss_payload;
    } payload;
} Packet;

#endif