#include "network.h"
#include <pthread.h>
#include <sys/stat.h>

int my_client_port;
char nm_global_ip[IP_LEN];
int nm_global_port;

// Day 6: Async Task Queue
typedef struct AsyncWriteTask
{
    char path[MAX_PATH_LEN];
    char data[MAX_DATA_SIZE];
    size_t size;
    uint32_t request_id;
    struct AsyncWriteTask *next;
} AsyncWriteTask;

AsyncWriteTask *task_queue_head = NULL;
AsyncWriteTask *task_queue_tail = NULL;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

void *bg_flush_worker(void *arg)
{
    while (1)
    {
        pthread_mutex_lock(&queue_mutex);
        while (task_queue_head == NULL)
        {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }

        AsyncWriteTask *task = task_queue_head;
        task_queue_head = task->next;
        if (task_queue_head == NULL)
            task_queue_tail = NULL;
        pthread_mutex_unlock(&queue_mutex);

        // Flush to persistent disk
        LOG_INFO("Background thread flushing %zu bytes to %s...", task->size, task->path);
        FILE *fp = fopen(task->path, "wb");
        if (fp)
        {
            fwrite(task->data, 1, task->size, fp);
            fclose(fp);
            LOG_SUCCESS("Async flush complete for path: %s", task->path);
        }
        else
        {
            LOG_ERROR("Async flush failed. Permission denied on path: %s", task->path);
        }

        // Notify NM to clear lock and update status table
        int nm_fd = connect_to_server(nm_global_ip, nm_global_port);
        if (nm_fd >= 0)
        {
            Packet complete_pkt;
            memset(&complete_pkt, 0, sizeof(Packet));
            complete_pkt.msg_type = MSG_ASYNC_COMPLETE;
            complete_pkt.request_id = task->request_id;
            strncpy(complete_pkt.path, task->path, MAX_PATH_LEN);
            send(nm_fd, &complete_pkt, sizeof(Packet), 0);
            close(nm_fd);
        }
        free(task);
    }
    return NULL;
}

void *handle_client_requests(void *arg)
{
    int client_fd = *((int *)arg);
    free(arg);

    Packet req;
    if (recv(client_fd, &req, sizeof(Packet), 0) <= 0)
    {
        close(client_fd);
        return NULL;
    }

    Packet reply;
    memset(&reply, 0, sizeof(Packet));

    if (req.msg_type == MSG_READ || req.msg_type == MSG_STREAM)
    {
        LOG_INFO("Processing data retrieval request for file: %s", req.path);
        FILE *fp = fopen(req.path, "rb");
        if (!fp)
        {
            reply.msg_type = MSG_ERROR;
            reply.error_code = ERR_FILE_NOT_FOUND;
            send(client_fd, &reply, sizeof(Packet), 0);
            close(client_fd);
            return NULL;
        }

        Packet block;
        while (!feof(fp))
        {
            memset(&block, 0, sizeof(Packet));
            block.msg_type = req.msg_type;
            block.data_size = fread(block.payload.text, 1, MAX_DATA_SIZE, fp);
            if (block.data_size > 0)
                send(client_fd, &block, sizeof(Packet), 0);
            if (req.msg_type == MSG_STREAM)
                usleep(15000);
        }
        fclose(fp);
        reply.msg_type = MSG_ACK;
        reply.error_code = SUCCESS;
        send(client_fd, &reply, sizeof(Packet), 0);
        LOG_SUCCESS("Data streaming for file [%s] completed successfully.", req.path);
    }
    else if (req.msg_type == MSG_WRITE)
    {
        if (req.sync_flag)
        {
            LOG_INFO("Executing baseline synchronous WRITE operation on file: %s", req.path);
            FILE *fp = fopen(req.path, "wb");
            if (fp)
            {
                size_t written = fwrite(req.payload.text, 1, req.data_size, fp);
                fclose(fp);
                reply.msg_type = MSG_ACK;
                reply.error_code = SUCCESS;
                sprintf(reply.payload.text, "Successfully wrote %zu bytes synchronously to storage disk.", written);
                LOG_SUCCESS("Synchronous write completed for path: %s", req.path);
            }
            else
            {
                reply.msg_type = MSG_ERROR;
                reply.error_code = ERR_PERMISSION_DENIED;
            }
            send(client_fd, &reply, sizeof(Packet), 0);
        }
        else
        {
            LOG_INFO("Queueing ASYNC write task to memory buffer for path: %s", req.path);
            AsyncWriteTask *task = malloc(sizeof(AsyncWriteTask));
            strncpy(task->path, req.path, MAX_PATH_LEN);
            memcpy(task->data, req.payload.text, req.data_size);
            task->size = req.data_size;
            task->request_id = req.request_id;
            task->next = NULL;

            pthread_mutex_lock(&queue_mutex);
            if (task_queue_tail == NULL)
            {
                task_queue_head = task;
                task_queue_tail = task;
            }
            else
            {
                task_queue_tail->next = task;
                task_queue_tail = task;
            }
            pthread_cond_signal(&queue_cond);
            pthread_mutex_unlock(&queue_mutex);

            reply.msg_type = MSG_ACK;
            reply.error_code = SUCCESS;
            sprintf(reply.payload.text, "Async task queued successfully. Tracking ID: %d", req.request_id);
            send(client_fd, &reply, sizeof(Packet), 0);
        }
    }

    close(client_fd);
    return NULL;
}

void *client_listener_loop(void *arg)
{
    int listen_fd = create_listening_socket(my_client_port);
    if (listen_fd < 0)
        return NULL;

    while (1)
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int c_fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
        if (c_fd < 0)
            continue;

        int *w_fd = malloc(sizeof(int));
        *w_fd = c_fd;
        pthread_t t;
        pthread_create(&t, NULL, handle_client_requests, w_fd);
        pthread_detach(t);
    }
    close(listen_fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        LOG_ERROR("Usage: %s <NM_IP> <NM_Port> <My_NM_Port> <My_Client_Port> [Paths...]", argv[0]);
        exit(EXIT_FAILURE);
    }

    strncpy(nm_global_ip, argv[1], IP_LEN);
    nm_global_port = atoi(argv[2]);
    int my_nm_port = atoi(argv[3]);
    my_client_port = atoi(argv[4]);

    char local_ip[IP_LEN];
    get_local_ip(local_ip, sizeof(local_ip));

    // Boot background async flusher
    pthread_t flush_thread;
    pthread_create(&flush_thread, NULL, bg_flush_worker, NULL);
    pthread_detach(flush_thread);

    pthread_t client_thread;
    pthread_create(&client_thread, NULL, client_listener_loop, NULL);
    pthread_detach(client_thread);

    Packet reg;
    memset(&reg, 0, sizeof(Packet));
    reg.msg_type = MSG_REGISTER;
    strncpy(reg.payload.ss_payload.ip, local_ip, IP_LEN);
    reg.payload.ss_payload.nm_port = my_nm_port;
    reg.payload.ss_payload.client_port = my_client_port;

    int p_idx = 0;
    for (int i = 5; i < argc && p_idx < MAX_PATHS; i++)
    {
        strncpy(reg.payload.ss_payload.paths[p_idx++], argv[i], MAX_PATH_LEN);
    }
    reg.payload.ss_payload.path_count = p_idx;

    int nm_fd = connect_to_server(nm_global_ip, nm_global_port);
    if (nm_fd >= 0)
    {
        send(nm_fd, &reg, sizeof(Packet), 0);
        Packet ack;
        recv(nm_fd, &ack, sizeof(Packet), 0);
        close(nm_fd);
        LOG_SUCCESS("Registration confirmation complete: %s", ack.payload.text);
    }

    int nm_listen_fd = create_listening_socket(my_nm_port);
    while (1)
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int nm_task_fd = accept(nm_listen_fd, (struct sockaddr *)&addr, &len);
        if (nm_task_fd < 0)
            continue;

        Packet task;
        recv(nm_task_fd, &task, sizeof(Packet), 0);
        Packet reply;
        memset(&reply, 0, sizeof(Packet));
        reply.msg_type = MSG_ACK;

        if (task.msg_type == MSG_CREATE)
        {
            FILE *fp = fopen(task.path, "w");
            if (fp)
            {
                fclose(fp);
                reply.error_code = SUCCESS;
                strcpy(reply.payload.text, "Empty file created successfully.");
                LOG_SUCCESS("Executed CREATE command for resource: %s", task.path);
            }
            else
                reply.error_code = ERR_PERMISSION_DENIED;
        }
        else if (task.msg_type == MSG_DELETE)
        {
            if (remove(task.path) == 0)
            {
                reply.error_code = SUCCESS;
                strcpy(reply.payload.text, "File removed successfully.");
                LOG_SUCCESS("Executed DELETE command for resource: %s", task.path);
            }
            else
                reply.error_code = ERR_FILE_NOT_FOUND;
        }
        else if (task.msg_type == MSG_INFO)
        {
            struct stat st;
            if (stat(task.path, &st) == 0)
            {
                reply.error_code = SUCCESS;
                sprintf(reply.payload.text, "Size: %ld bytes | Perms: %o | Modified: %ld",
                        (long)st.st_size, st.st_mode & 0777, (long)st.st_mtime);
                LOG_SUCCESS("Retrieved METADATA metrics for: %s", task.path);
            }
            else
            {
                reply.msg_type = MSG_ERROR;
                reply.error_code = ERR_FILE_NOT_FOUND;
            }
        }
        send(nm_task_fd, &reply, sizeof(Packet), 0);
        close(nm_task_fd);
    }
    close(nm_listen_fd);
    return 0;
}