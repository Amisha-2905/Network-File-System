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

// Central directory table
RegisteredSS ss_table[10];
int registered_ss_count = 0;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread function to handle individual node handshakes asynchronously
void *handle_node_connection(void *arg)
{
    int client_fd = *((int *)arg);
    free(arg);

    Packet inbound_packet;
    ssize_t bytes_received = recv(client_fd, &inbound_packet, sizeof(Packet), 0);

    if (bytes_received <= 0)
    {
        LOG_ERROR("Connection dropped or zero data read from connecting node.");
        close(client_fd);
        return NULL;
    }

    if (inbound_packet.msg_type == MSG_REGISTER)
    {
        // Read directly from our new compiler-safe union layout structure
        SS_RegisterPayload *payload = &inbound_packet.payload.ss_payload;

        pthread_mutex_lock(&table_mutex);
        if (registered_ss_count < 10)
        {
            RegisteredSS *new_ss = &ss_table[registered_ss_count];
            strncpy(new_ss->ip, payload->ip, IP_LEN);
            new_ss->nm_port = payload->nm_port;
            new_ss->client_port = payload->client_port;
            new_ss->path_count = payload->path_count;
            new_ss->is_online = 1;

            for (int i = 0; i < payload->path_count; i++)
            {
                strncpy(new_ss->paths[i], payload->paths[i], MAX_PATH_LEN);
            }
            registered_ss_count++;

            LOG_SUCCESS("Successfully registered Storage Server #%d from %s", registered_ss_count, payload->ip);
            LOG_INFO("Active Paths stored in table for this node:");
            for (int i = 0; i < payload->path_count; i++)
            {
                LOG_MSG("  -> %s", payload->paths[i]);
            }
        }
        else
        {
            LOG_ERROR("Directory table capacity overflow. Cannot register more servers.");
        }
        pthread_mutex_unlock(&table_mutex);

        Packet ack_packet;
        memset(&ack_packet, 0, sizeof(Packet));
        ack_packet.msg_type = MSG_REGISTER_ACK;
        ack_packet.error_code = SUCCESS;
        strcpy(ack_packet.payload.text, "Registration verified and saved by Naming Server directory table.");

        send(client_fd, &ack_packet, sizeof(Packet), 0);
    }
    else
    {
        LOG_ERROR("Unexpected instruction code received: %d", inbound_packet.msg_type);
    }

    close(client_fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        LOG_ERROR("Usage error. Correct command: %s <NM_Port>", argv[0]);
        exit(EXIT_FAILURE);
    }

    int nm_port = atoi(argv[1]);
    char local_ip[IP_LEN];
    get_local_ip(local_ip, sizeof(local_ip));

    LOG_INFO("Initializing Threaded Naming Server Core Engine...");
    int server_fd = create_listening_socket(nm_port);
    if (server_fd < 0)
    {
        LOG_ERROR("Critical failure initializing master listening socket. Exiting.");
        exit(EXIT_FAILURE);
    }

    LOG_SUCCESS("Naming Server is online [IP: %s, Port: %d] and waiting for multiple concurrent connections...", local_ip, nm_port);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd < 0)
        {
            LOG_ERROR("Failed to accept incoming node connection.");
            continue;
        }

        LOG_INFO("New connection detected from network address: %s", inet_ntoa(client_addr.sin_addr));

        // Allocate memory for the file descriptor to hand over to the worker thread safely
        int *worker_fd = malloc(sizeof(int));
        *worker_fd = client_fd;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_node_connection, worker_fd) != 0)
        {
            LOG_ERROR("Failed to spawn parallel worker thread for incoming request.");
            close(client_fd);
            free(worker_fd);
        }
        else
        {
            // Detach thread to allow auto-cleanup of memory resources upon execution exit
            pthread_detach(thread_id);
        }
    }

    close(server_fd);
    return 0;
}