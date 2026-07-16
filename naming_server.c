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

// Day 6: Async Task Ledger
int async_tasks[1024] = {0};   // TASK_NONE / TASK_PENDING / TASK_COMPLETED / TASK_FAILED
int task_owner_ss[1024] = {0}; // which ss_id each task was issued against
int next_task_id = 1;

// Day 6 (minimal): heartbeat-based failure detection, formalized further on Day 8
#define HEARTBEAT_INTERVAL_SEC 3
#define HEARTBEAT_MISS_LIMIT 2
int missed_heartbeats[10] = {0};

typedef struct
{
    int src_ss;
    int dest_ss;
    char path[MAX_PATH_LEN];
} ReplicaTask;

void *async_replicate_worker(void *arg)
{
    ReplicaTask *task = (ReplicaTask *)arg;

    pthread_mutex_lock(&table_mutex);
    char dest_ip[IP_LEN];
    int dest_nm_port = ss_table[task->dest_ss].nm_port;
    strncpy(dest_ip, ss_table[task->dest_ss].ip, IP_LEN);

    Packet copy_req;
    memset(&copy_req, 0, sizeof(Packet));
    copy_req.msg_type = MSG_COPY;
    strncpy(copy_req.path, task->path, MAX_PATH_LEN);
    snprintf(copy_req.payload.text, MAX_DATA_SIZE, "%s:%d:%s",
             ss_table[task->src_ss].ip, ss_table[task->src_ss].client_port, task->path);
    pthread_mutex_unlock(&table_mutex);

    int dest_fd = connect_to_server(dest_ip, dest_nm_port);
    if (dest_fd >= 0)
    {
        send(dest_fd, &copy_req, sizeof(Packet), 0);
        // Fire and forget - we do not wait for the ACK per the spec!
        close(dest_fd);
        LOG_INFO("Async replication triggered to SS #%d for path: %s", task->dest_ss, task->path);
    }

    free(task);
    return NULL;
}

void trigger_replication(int primary_ss, const char *path)
{
    if (registered_ss_count <= 2)
        return; // Spec: Replicate only when SS > 2

    int replicas[2] = {
        (primary_ss + 1) % registered_ss_count,
        (primary_ss + 2) % registered_ss_count};

    for (int i = 0; i < 2; i++)
    {
        if (ss_table[replicas[i]].is_online)
        {
            ReplicaTask *rt = malloc(sizeof(ReplicaTask));
            rt->src_ss = primary_ss;
            rt->dest_ss = replicas[i];
            strncpy(rt->path, path, MAX_PATH_LEN);

            pthread_t rep_thread;
            pthread_create(&rep_thread, NULL, async_replicate_worker, rt);
            pthread_detach(rep_thread);
        }
    }
}

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

    if (inbound.msg_type != MSG_REGISTER && inbound.msg_type != MSG_ACK && inbound.msg_type != MSG_ASYNC_COMPLETE)
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
            // SPEC 3.6 BONUS [50 Marks]: Node Recovery Reconciliation
            // We strictly mark the node online and reset heartbeats.
            // We explicitly DO NOT process payload->paths to guarantee no duplicate
            // or conflicting path entries are added during the recovery window.
            ss_table[found_idx].is_online = 1;
            missed_heartbeats[found_idx] = 0;

            LOG_SUCCESS("Storage Server #%d (Port %d) recovered back online! Reconciled with original Trie paths.", found_idx, ss_table[found_idx].client_port);
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
        int matched_ss = cache_get(route_cache, inbound.path);
        if (matched_ss != -1)
        {
            LOG_INFO("Cache HIT! Route for '%s' resolved in O(1) time.", inbound.path);
        }
        else
        {
            LOG_INFO("Cache MISS! Traversing Trie for '%s'.", inbound.path);
            matched_ss = trie_search(trie_root, inbound.path);
            if (matched_ss != -1)
                cache_put(route_cache, inbound.path, matched_ss);
        }

        Packet out;
        memset(&out, 0, sizeof(Packet));

        if (matched_ss != -1)
        {
            // --- DAY 8: READ FAILOVER LOGIC ---
            if (!ss_table[matched_ss].is_online)
            {
                if (inbound.error_code == 1) // WRITE requested
                {
                    out.msg_type = MSG_ERROR;
                    out.error_code = ERR_SS_UNREACHABLE;
                    matched_ss = -1; // Block writes to dead primary
                }
                else // READ requested
                {
                    if (registered_ss_count > 2)
                    {
                        int r1 = (matched_ss + 1) % registered_ss_count;
                        int r2 = (matched_ss + 2) % registered_ss_count;

                        if (ss_table[r1].is_online)
                        {
                            matched_ss = r1;
                            LOG_INFO("Primary offline. Read failover to Replica SS #%d", r1);
                        }
                        else if (ss_table[r2].is_online)
                        {
                            matched_ss = r2;
                            LOG_INFO("Primary offline. Read failover to Replica SS #%d", r2);
                        }
                        else
                        {
                            out.msg_type = MSG_ERROR;
                            out.error_code = ERR_SS_UNREACHABLE;
                            matched_ss = -1;
                        }
                    }
                    else
                    {
                        out.msg_type = MSG_ERROR;
                        out.error_code = ERR_SS_UNREACHABLE;
                        matched_ss = -1;
                    }
                }
            }

            // Normal processing if a valid, online SS was found (Primary or Replica)
            if (matched_ss != -1)
            {
                // Find the actual path index to check its specific lock
                int target_path_idx = 0;
                for (int j = 0; j < ss_table[matched_ss].path_count; j++)
                {
                    if (strcmp(ss_table[matched_ss].paths[j], inbound.path) == 0)
                    {
                        target_path_idx = j;
                        break;
                    }
                }

                // SECURITY FIX: Block BOTH Read and Write if the file is currently locked!
                if (ss_table[matched_ss].is_being_written[target_path_idx])
                {
                    out.msg_type = MSG_ERROR;
                    out.error_code = ERR_FILE_BUSY_WRITING;
                }
                else
                {
                    if (inbound.error_code == 1)
                    { // If it is a WRITE request, lock the file now
                        ss_table[matched_ss].is_being_written[target_path_idx] = 1;
                        int task_id = next_task_id++;
                        async_tasks[task_id] = TASK_PENDING;
                        task_owner_ss[task_id] = matched_ss;
                        out.request_id = task_id;
                    }
                    out.msg_type = MSG_LOOKUP_ACK;
                    sprintf(out.payload.text, "%s:%d", ss_table[matched_ss].ip, ss_table[matched_ss].client_port);
                }
            }
        }
        else
        {
            out.msg_type = MSG_ERROR;
            out.error_code = ERR_FILE_NOT_FOUND;
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
            relay_ack.error_code = ERR_SS_UNREACHABLE;
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

        char target_ip[IP_LEN];
        int target_port = ss_table[target_ss].nm_port;
        strncpy(target_ip, ss_table[target_ss].ip, IP_LEN);
        pthread_mutex_unlock(&table_mutex);

        int ss_sock = connect_to_server(target_ip, target_port);
        if (ss_sock >= 0)
        {
            send(ss_sock, &inbound, sizeof(Packet), 0);
            recv(ss_sock, &relay_ack, sizeof(Packet), 0);
            close(ss_sock);

            if (relay_ack.error_code == SUCCESS && (inbound.msg_type == MSG_CREATE || inbound.msg_type == MSG_DELETE))
            {
                pthread_mutex_lock(&table_mutex);
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
                pthread_mutex_unlock(&table_mutex);
            }
        }
        else
        {
            relay_ack.msg_type = MSG_ERROR;
            relay_ack.error_code = ERR_SS_UNREACHABLE;
        }
        send(client_fd, &relay_ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_STATUS)
    {
        Packet status_ack;
        memset(&status_ack, 0, sizeof(Packet));
        status_ack.msg_type = MSG_ACK;

        pthread_mutex_lock(&table_mutex);
        int state = (inbound.request_id < 1024) ? async_tasks[inbound.request_id] : TASK_NONE;
        pthread_mutex_unlock(&table_mutex);

        if (state == TASK_PENDING)
            strcpy(status_ack.payload.text, "PENDING (In Memory Buffer Queue)");
        else if (state == TASK_COMPLETED)
            strcpy(status_ack.payload.text, "COMPLETED (Successfully Flushed to Disk)");
        else if (state == TASK_FAILED)
            strcpy(status_ack.payload.text, "FAILED (Storage Server became unreachable before flush completed)");
        else
            strcpy(status_ack.payload.text, "UNKNOWN (Invalid ID)");

        send(client_fd, &status_ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_LIST)
    {
        pthread_mutex_lock(&table_mutex);
        Packet list_ack;
        memset(&list_ack, 0, sizeof(Packet));
        list_ack.msg_type = MSG_ACK;
        list_ack.error_code = SUCCESS;

        int offset = 0;
        for (int i = 0; i < registered_ss_count; i++)
        {
            if (ss_table[i].is_online)
            {
                offset += snprintf(list_ack.payload.text + offset, MAX_DATA_SIZE - offset, "--- SS #%d (%s:%d) ---\n", i, ss_table[i].ip, ss_table[i].client_port);
                for (int j = 0; j < ss_table[i].path_count; j++)
                {
                    offset += snprintf(list_ack.payload.text + offset, MAX_DATA_SIZE - offset, "  - %s\n", ss_table[i].paths[j]);
                }
            }
        }
        if (offset == 0)
            strcpy(list_ack.payload.text, "No active paths found in cluster.");
        pthread_mutex_unlock(&table_mutex);

        send(client_fd, &list_ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_COPY)
    {
        pthread_mutex_lock(&table_mutex);
        char source_path[MAX_PATH_LEN];
        char dest_path[MAX_PATH_LEN];
        strncpy(source_path, inbound.path, MAX_PATH_LEN);
        strncpy(dest_path, inbound.payload.text, MAX_PATH_LEN);

        int src_ss_idx = cache_get(route_cache, source_path);
        if (src_ss_idx == -1)
            src_ss_idx = trie_search(trie_root, source_path);

        if (src_ss_idx == -1 || !ss_table[src_ss_idx].is_online)
        {
            pthread_mutex_unlock(&table_mutex);
            Packet err;
            memset(&err, 0, sizeof(Packet));
            err.msg_type = MSG_ERROR;
            err.error_code = ERR_FILE_NOT_FOUND;
            send(client_fd, &err, sizeof(Packet), 0);
            return NULL; // Handled
        }

        // Round-robin selection for Destination SS to distribute data
        int dest_ss_idx = -1;
        for (int i = 1; i <= registered_ss_count; i++)
        {
            int candidate = (src_ss_idx + i) % registered_ss_count;
            if (ss_table[candidate].is_online)
            {
                dest_ss_idx = candidate;
                break;
            }
        }
        if (dest_ss_idx == -1)
            dest_ss_idx = src_ss_idx; // Fallback to same SS if only 1 exists

        char dest_ip[IP_LEN];
        int dest_nm_port = ss_table[dest_ss_idx].nm_port;
        strncpy(dest_ip, ss_table[dest_ss_idx].ip, IP_LEN);

        // Prep the payload for the Destination SS
        Packet copy_req;
        memset(&copy_req, 0, sizeof(Packet));
        copy_req.msg_type = MSG_COPY;
        strncpy(copy_req.path, dest_path, MAX_PATH_LEN);
        snprintf(copy_req.payload.text, MAX_DATA_SIZE, "%s:%d:%s", ss_table[src_ss_idx].ip, ss_table[src_ss_idx].client_port, source_path);

        pthread_mutex_unlock(&table_mutex); // Unlock before network I/O!

        // Forward to Destination SS
        int dest_fd = connect_to_server(dest_ip, dest_nm_port);
        Packet dest_ack;
        memset(&dest_ack, 0, sizeof(Packet));

        if (dest_fd >= 0)
        {
            send(dest_fd, &copy_req, sizeof(Packet), 0);
            recv(dest_fd, &dest_ack, sizeof(Packet), 0);
            close(dest_fd);

            if (dest_ack.error_code == SUCCESS)
            {
                // Re-acquire lock to update the Trie/Table securely
                pthread_mutex_lock(&table_mutex);
                if (ss_table[dest_ss_idx].path_count < MAX_PATHS)
                {
                    strncpy(ss_table[dest_ss_idx].paths[ss_table[dest_ss_idx].path_count], dest_path, MAX_PATH_LEN);
                    ss_table[dest_ss_idx].is_being_written[ss_table[dest_ss_idx].path_count] = 0;
                    ss_table[dest_ss_idx].path_count++;
                    trie_insert(trie_root, dest_path, dest_ss_idx);
                    cache_invalidate(route_cache, dest_path);
                }
                pthread_mutex_unlock(&table_mutex);
            }
        }
        else
        {
            dest_ack.msg_type = MSG_ERROR;
            dest_ack.error_code = ERR_SS_UNREACHABLE;
        }

        send(client_fd, &dest_ack, sizeof(Packet), 0);
    }
    else if (inbound.msg_type == MSG_ACK || inbound.msg_type == MSG_ASYNC_COMPLETE)
    {
        pthread_mutex_lock(&table_mutex);
        if (inbound.msg_type == MSG_ASYNC_COMPLETE && inbound.request_id < 1024)
        {
            async_tasks[inbound.request_id] = TASK_COMPLETED;
            LOG_SUCCESS("Async task %d officially marked COMPLETED.", inbound.request_id);
        }

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

            // DAY 8: Trigger Replicas to pull the new data asynchronously
            trigger_replication(matched_ss, inbound.path);
        }
        pthread_mutex_unlock(&table_mutex);

        // Force a two-way handshake so the SS doesn't tear down the socket too early
        Packet ack;
        memset(&ack, 0, sizeof(Packet));
        ack.msg_type = MSG_ACK;
        send(client_fd, &ack, sizeof(Packet), 0);
    }

    close(client_fd);
    return NULL;
}

// Day 6 (minimal version): periodically probes every registered SS on its NM-facing
// port. After HEARTBEAT_MISS_LIMIT consecutive missed probes, the SS is marked offline
// and any of its PENDING async write tasks are marked FAILED so a client polling
// STATUS gets an explicit answer instead of hanging forever. Also releases the
// exclusive write lock those tasks were holding on their paths.
void *heartbeat_worker(void *arg)
{
    (void)arg;
    while (1)
    {
        sleep(HEARTBEAT_INTERVAL_SEC);

        pthread_mutex_lock(&table_mutex);
        int count = registered_ss_count;
        char ips[10][IP_LEN];
        int ports[10];
        int online[10];
        for (int i = 0; i < count; i++)
        {
            strncpy(ips[i], ss_table[i].ip, IP_LEN);
            ports[i] = ss_table[i].nm_port;
            online[i] = ss_table[i].is_online;
        }
        pthread_mutex_unlock(&table_mutex);

        for (int i = 0; i < count; i++)
        {
            if (!online[i])
                continue; // already known dead; recovery is handled separately (Day 9)

            int hb_fd = connect_to_server(ips[i], ports[i]);
            int alive = 0;
            if (hb_fd >= 0)
            {
                set_recv_timeout(hb_fd, 1);
                Packet hb;
                memset(&hb, 0, sizeof(Packet));
                hb.msg_type = MSG_HEARTBEAT;
                send(hb_fd, &hb, sizeof(Packet), 0);

                Packet hb_ack;
                if (recv(hb_fd, &hb_ack, sizeof(Packet), 0) > 0 && hb_ack.msg_type == MSG_ACK)
                    alive = 1;
                close(hb_fd);
            }

            pthread_mutex_lock(&table_mutex);
            if (alive)
            {
                missed_heartbeats[i] = 0;
            }
            else
            {
                missed_heartbeats[i]++;
                LOG_ERROR("Heartbeat missed for Storage Server #%d (%d/%d)", i, missed_heartbeats[i], HEARTBEAT_MISS_LIMIT);

                if (missed_heartbeats[i] >= HEARTBEAT_MISS_LIMIT && ss_table[i].is_online)
                {
                    ss_table[i].is_online = 0;
                    LOG_ERROR("Storage Server #%d declared DEAD after missed heartbeats.", i);

                    // Fail any pending async write tasks that were in-flight on this SS
                    // and release the exclusive write locks they were holding.
                    for (int t = 0; t < 1024; t++)
                    {
                        if (async_tasks[t] == TASK_PENDING && task_owner_ss[t] == i)
                        {
                            async_tasks[t] = TASK_FAILED;
                        }
                    }
                    for (int p = 0; p < ss_table[i].path_count; p++)
                    {
                        ss_table[i].is_being_written[p] = 0;
                    }
                }
            }
            pthread_mutex_unlock(&table_mutex);
        }
    }
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

    LOG_SUCCESS("Naming Server online [IP: %s, Port: %d]...", local_ip, port);

    pthread_t hb_thread;
    pthread_create(&hb_thread, NULL, heartbeat_worker, NULL);
    pthread_detach(hb_thread);

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