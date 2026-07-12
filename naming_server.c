#include "network.h"
#include "trie.h"
#include "lru_cache.h"
#include <pthread.h>
#include <sys/time.h>

typedef struct
{
    char ip[IP_LEN];
    int nm_port;
    int client_port;
    int path_count;
    char paths[MAX_PATHS][MAX_PATH_LEN];
    int is_online;
    int is_being_written[MAX_PATHS];
} RegisteredSS;

RegisteredSS ss_table[10];
int registered_ss_count = 0;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

TrieNode *trie_root;
LRUCache *route_cache;

void send_initial_ack(int client_fd)
{
    Packet init_ack;
    memset(&init_ack, 0, sizeof(Packet));
    init_ack.msg_type = MSG_ACK;
    init_ack.error_code = SUCCESS;
    send(client_fd, &init_ack, sizeof(Packet), 0);
}

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

    // Spec 3.2: Issue immediate initial acknowledgment for incoming client requests
    if (inbound.msg_type != MSG_REGISTER && inbound.msg_type != MSG_ACK)
    {
        send_initial_ack(client_fd);
    }

    if (inbound.msg_type == MSG_REGISTER)
    {
        SS_RegisterPayload *payload = &inbound.payload.ss_payload;
        pthread_mutex_lock(&table_mutex);

        int found_idx = -1;
        for (int i = 0; i < registered_ss_count; i++)
        {
            if (ss_table[i].client_port == payload->client_port && strcmp(ss_table[i].ip, payload->ip) == 0)
            {
                found_idx = i;
                break;
            }
        }

        int target_ss_id = -1;
        if (found_idx != -1)
        {
            ss_table[found_idx].is_online = 1;
            target_ss_id = found_idx;
            ss_table[found_idx].path_count = payload->path_count;
            for (int i = 0; i < payload->path_count; i++)
            {
                strncpy(ss_table[found_idx].paths[i], payload->paths[i], MAX_PATH_LEN);
                trie_insert(trie_root, payload->paths[i], target_ss_id);
            }
            LOG_SUCCESS("Storage Server at port %d recovered back online and Trie paths re-seeded successfully!", ss_table[found_idx].client_port);
        }
        else if (registered_ss_count < 10)
        {
            target_ss_id = registered_ss_count;
            RegisteredSS *target = &ss_table[registered_ss_count++];
            target->is_online = 1;
            strncpy(target->ip, payload->ip, IP_LEN);
            target->nm_port = payload->nm_port;
            target->client_port = payload->client_port;
            target->path_count = payload->path_count;

            for (int i = 0; i < payload->path_count; i++)
            {
                strncpy(target->paths[i], payload->paths[i], MAX_PATH_LEN);
                target->is_being_written[i] = 0;
                trie_insert(trie_root, payload->paths[i], target_ss_id);
            }
            LOG_SUCCESS("Registered Storage Server #%d from %s", registered_ss_count, payload->ip);
        }
        pthread_mutex_unlock(&table_mutex);

        Packet ack;
        memset(&ack, 0, sizeof(Packet));
        ack.msg_type = MSG_REGISTER_ACK;
        strcpy(ack.payload.text, "Registration optimization tracking live.");
        send(client_fd, &ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_LOOKUP)
    {
        pthread_mutex_lock(&table_mutex);
        struct timeval start_time, end_time;

        int matched_ss = cache_get(route_cache, inbound.path);
        if (matched_ss == -1)
        {
            matched_ss = trie_search(trie_root, inbound.path);
            if (matched_ss != -1)
            {
                cache_put(route_cache, inbound.path, matched_ss);
            }
        }

        Packet out;
        memset(&out, 0, sizeof(Packet));

        if (matched_ss != -1 && ss_table[matched_ss].is_online)
        {
            int target_path_idx = -1;
            for (int j = 0; j < ss_table[matched_ss].path_count; j++)
            {
                if (strcmp(ss_table[matched_ss].paths[j], inbound.path) == 0)
                {
                    target_path_idx = j;
                    break;
                }
            }

            if (target_path_idx == -1)
            {
                out.msg_type = MSG_ERROR;
                out.error_code = ERR_INVALID_PATH; // Explicit Day 5 error mapping
                LOG_ERROR("Trie matched but directory struct out of sync for: %s", inbound.path);
            }
            else
            {
                if (inbound.error_code == 1)
                { // WRITE requested
                    if (ss_table[matched_ss].is_being_written[target_path_idx])
                    {
                        out.msg_type = MSG_ERROR;
                        out.error_code = ERR_FILE_BUSY_WRITING; // Explicit Exclusivity Error Code [cite: 302, 304]
                        LOG_ERROR("Rejected WRITE lookup. Path locked: %s", inbound.path);
                    }
                    else
                    {
                        ss_table[matched_ss].is_being_written[target_path_idx] = 1;
                        out.msg_type = MSG_LOOKUP_ACK;
                        sprintf(out.payload.text, "%s:%d", ss_table[matched_ss].ip, ss_table[matched_ss].client_port);
                    }
                }
                else
                { // READ requested
                    if (ss_table[matched_ss].is_being_written[target_path_idx])
                    {
                        out.msg_type = MSG_ERROR;
                        out.error_code = ERR_FILE_BUSY_WRITING; // Explicit Exclusivity Error Code [cite: 302, 304]
                        LOG_ERROR("Rejected READ lookup. Path currently busy: %s", inbound.path);
                    }
                    else
                    {
                        out.msg_type = MSG_LOOKUP_ACK;
                        sprintf(out.payload.text, "%s:%d", ss_table[matched_ss].ip, ss_table[matched_ss].client_port);
                    }
                }
            }
        }
        else
        {
            out.msg_type = MSG_ERROR;
            out.error_code = ERR_FILE_NOT_FOUND; // Explicit missing error mapping
            LOG_ERROR("Path resolution lookup returned missing: %s", inbound.path);
        }
        pthread_mutex_unlock(&table_mutex);
        send(client_fd, &out, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_CREATE || inbound.msg_type == MSG_DELETE || inbound.msg_type == MSG_INFO)
    {
        pthread_mutex_lock(&table_mutex);
        Packet relay_ack;
        memset(&relay_ack, 0, sizeof(Packet));

        if (registered_ss_count == 0)
        {
            relay_ack.msg_type = MSG_ERROR;
            relay_ack.error_code = ERR_SS_UNREACHABLE; // Explicit unreachable code mapping
            pthread_mutex_unlock(&table_mutex);
            send(client_fd, &relay_ack, sizeof(Packet), 0);
            close(client_fd);
            return NULL;
        }

        int target_ss = 0;
        if (inbound.msg_type == MSG_DELETE || inbound.msg_type == MSG_INFO)
        {
            int lookup_ss = trie_search(trie_root, inbound.path);
            if (lookup_ss != -1)
                target_ss = lookup_ss;
            else
            {
                relay_ack.msg_type = MSG_ERROR;
                relay_ack.error_code = ERR_FILE_NOT_FOUND;
                pthread_mutex_unlock(&table_mutex);
                send(client_fd, &relay_ack, sizeof(Packet), 0);
                close(client_fd);
                return NULL;
            }
        }

        int ss_sock = connect_to_server(ss_table[target_ss].ip, ss_table[target_ss].nm_port);
        if (ss_sock >= 0)
        {
            send(ss_sock, &inbound, sizeof(Packet), 0);
            recv(ss_sock, &relay_ack, sizeof(Packet), 0);
            close(ss_sock);

            if (relay_ack.error_code == SUCCESS)
            {
                if (inbound.msg_type == MSG_CREATE)
                {
                    int p_idx = ss_table[target_ss].path_count;
                    if (p_idx < MAX_PATHS)
                    {
                        strncpy(ss_table[target_ss].paths[p_idx], inbound.path, MAX_PATH_LEN);
                        ss_table[target_ss].is_being_written[p_idx] = 0;
                        ss_table[target_ss].path_count++;

                        trie_insert(trie_root, inbound.path, target_ss);
                        cache_invalidate(route_cache, inbound.path);
                    }
                }
                else if (inbound.msg_type == MSG_DELETE)
                {
                    for (int j = 0; j < ss_table[target_ss].path_count; j++)
                    {
                        if (strcmp(ss_table[target_ss].paths[j], inbound.path) == 0)
                        {
                            for (int k = j; k < ss_table[target_ss].path_count - 1; k++)
                            {
                                strcpy(ss_table[target_ss].paths[k], ss_table[target_ss].paths[k + 1]);
                                ss_table[target_ss].is_being_written[k] = ss_table[target_ss].is_being_written[k + 1];
                            }
                            ss_table[target_ss].path_count--;
                            break;
                        }
                    }
                    trie_delete_path(trie_root, inbound.path);
                    cache_invalidate(route_cache, inbound.path);
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
    else if (inbound.msg_type == MSG_ACK)
    {
        pthread_mutex_lock(&table_mutex);
        int matched_ss = trie_search(trie_root, inbound.path);
        if (matched_ss != -1)
        {
            for (int j = 0; j < ss_table[matched_ss].path_count; j++)
            {
                if (strcmp(ss_table[matched_ss].paths[j], inbound.path) == 0)
                {
                    ss_table[matched_ss].is_being_written[j] = 0;
                    LOG_SUCCESS("Cleared exclusive WRITE lock on path: %s", inbound.path);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&table_mutex);
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
    char local_ip[IP_LEN];
    get_local_ip(local_ip, sizeof(local_ip));

    trie_root = create_trie_node("ROOT");
    route_cache = create_cache();

    LOG_INFO("Initializing Async Non-Blocking Naming Server Core...");
    int s_fd = create_listening_socket(port);
    if (s_fd < 0)
        exit(EXIT_FAILURE);

    LOG_SUCCESS("Naming Server online [IP: %s, Port: %d] (Day 5 Non-Blocking ACKs Active)...", local_ip, port);
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