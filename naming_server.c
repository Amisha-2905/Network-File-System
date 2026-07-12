#include "network.h"
#include <pthread.h>

typedef struct
{
    char ip[IP_LEN];
    int nm_port;
    int client_port;
    int path_count;
    char paths[MAX_PATHS][MAX_PATH_LEN];
    int is_online;
} RegisteredSS;

RegisteredSS ss_table[10];
int registered_ss_count = 0;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

void *handle_node_connection(void *arg)
{
    int client_fd = *((int *)arg);
    free(arg);

    Packet inbound;
    if (recv(client_fd, &inbound, sizeof(Packet), 0) <= 0)
    {
        close(client_fd);
        return NULL;
    }

    if (inbound.msg_type == MSG_REGISTER)
    {
        SS_RegisterPayload *payload = &inbound.payload.ss_payload;
        pthread_mutex_lock(&table_mutex);

        // Recover if storage server node re-attaches with existing matching configurations
        int found_idx = -1;
        for (int i = 0; i < registered_ss_count; i++)
        {
            if (ss_table[i].client_port == payload->client_port && strcmp(ss_table[i].ip, payload->ip) == 0)
            {
                found_idx = i;
                break;
            }
        }

        RegisteredSS *target = NULL;
        if (found_idx != -1)
        {
            target = &ss_table[found_idx];
            target->is_online = 1;
            LOG_SUCCESS("Storage Server at port %d recovered back online successfully!", target->client_port);
        }
        else if (registered_ss_count < 10)
        {
            target = &ss_table[registered_ss_count++];
            target->is_online = 1;
            strncpy(target->ip, payload->ip, IP_LEN);
            target->nm_port = payload->nm_port;
            target->client_port = payload->client_port;
            target->path_count = payload->path_count;
            for (int i = 0; i < payload->path_count; i++)
            {
                strncpy(target->paths[i], payload->paths[i], MAX_PATH_LEN);
            }
            LOG_SUCCESS("Successfully registered Storage Server #%d from %s", registered_ss_count, payload->ip);
        }
        pthread_mutex_unlock(&table_mutex);

        Packet ack;
        memset(&ack, 0, sizeof(Packet));
        ack.msg_type = MSG_REGISTER_ACK;
        strcpy(ack.payload.text, "Registration processed.");
        send(client_fd, &ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_LOOKUP)
    {
        pthread_mutex_lock(&table_mutex);
        int matched_ss = -1;
        for (int i = 0; i < registered_ss_count; i++)
        {
            if (!ss_table[i].is_online)
                continue;
            for (int j = 0; j < ss_table[i].path_count; j++)
            {
                if (strcmp(ss_table[i].paths[j], inbound.path) == 0)
                {
                    matched_ss = i;
                    break;
                }
            }
            if (matched_ss != -1)
                break;
        }

        Packet out;
        memset(&out, 0, sizeof(Packet));
        if (matched_ss != -1)
        {
            out.msg_type = MSG_LOOKUP_ACK;
            sprintf(out.payload.text, "%s:%d", ss_table[matched_ss].ip, ss_table[matched_ss].client_port);
            LOG_INFO("Routing path lookup [%s] to Storage Server %d", inbound.path, matched_ss + 1);
        }
        else
        {
            out.msg_type = MSG_ERROR;
            out.error_code = ERR_FILE_NOT_FOUND;
            LOG_ERROR("Requested path resource could not be found: %s", inbound.path);
        }
        pthread_mutex_unlock(&table_mutex);
        send(client_fd, &out, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_CREATE || inbound.msg_type == MSG_DELETE)
    {
        // Direct mediation: Forward command directly over to the designated primary Storage Server
        pthread_mutex_lock(&table_mutex);
        int target_ss = 0; // Baseline: Route to primary node under simplified 3-day conditions

        int ss_sock = connect_to_server(ss_table[target_ss].ip, ss_table[target_ss].nm_port);
        Packet relay_ack;
        memset(&relay_ack, 0, sizeof(Packet));

        if (ss_sock >= 0)
        {
            send(ss_sock, &inbound, sizeof(Packet), 0);
            recv(ss_sock, &relay_ack, sizeof(Packet), 0);
            close(ss_sock);

            if (inbound.msg_type == MSG_CREATE && relay_ack.error_code == SUCCESS)
            {
                // Dynamically register new additions directly inside our global directory structure
                int p_idx = ss_table[target_ss].path_count;
                if (p_idx < MAX_PATHS)
                {
                    strncpy(ss_table[target_ss].paths[p_idx], inbound.path, MAX_PATH_LEN);
                    ss_table[target_ss].path_count++;
                }
            }
        }
        else
        {
            relay_ack.msg_type = MSG_ERROR;
            relay_ack.error_code = ERR_SS_UNREACHABLE;
        }
        pthread_mutex_unlock(&table_mutex);
        send(client_fd, &relay_ack, sizeof(Packet), 0);
    }

    close(client_fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        LOG_ERROR("Usage: %s <NM_Port>", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);

    // Restore dynamic IP resolution and logging
    char local_ip[IP_LEN];
    get_local_ip(local_ip, sizeof(local_ip));

    LOG_INFO("Initializing Threaded Naming Server Core Engine...");
    int s_fd = create_listening_socket(port);
    if (s_fd < 0)
    {
        LOG_ERROR("Critical failure initializing master listening socket. Exiting.");
        exit(EXIT_FAILURE);
    }

    LOG_SUCCESS("Naming Server online [IP: %s, Port: %d] and monitoring requests...", local_ip, port);

    while (1)
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int c_fd = accept(s_fd, (struct sockaddr *)&addr, &len);
        if (c_fd < 0)
            continue;

        int *w_fd = malloc(sizeof(int));
        *w_fd = c_fd;
        pthread_t t;
        pthread_create(&t, NULL, handle_node_connection, w_fd);
        pthread_detach(t);
    }

    close(s_fd);
    return 0;
}