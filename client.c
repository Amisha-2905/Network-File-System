#include "network.h"

void handle_read_stream(const char *nm_ip, int nm_port, const char *path, int is_stream)
{
    // 1. Ask the Naming Server where the file lives
    Packet lookup_req;
    memset(&lookup_req, 0, sizeof(Packet));
    lookup_req.msg_type = MSG_LOOKUP;
    strncpy(lookup_req.path, path, MAX_PATH_LEN);

    LOG_INFO("Querying Naming Server at %s:%d for path route: %s", nm_ip, nm_port, path);
    int nm_fd = connect_to_server(nm_ip, nm_port);
    if (nm_fd < 0)
        return;

    send(nm_fd, &lookup_req, sizeof(Packet), 0);

    Packet lookup_ack;
    ssize_t bytes = recv(nm_fd, &lookup_ack, sizeof(Packet), 0);
    close(nm_fd); // Disconnect immediately from NM

    if (bytes <= 0 || lookup_ack.msg_type == MSG_ERROR)
    {
        LOG_ERROR("Path resolution failed. Error Code: %d", lookup_ack.error_code);
        return;
    }

    // Extract resolved SS address coordinates
    char ss_ip[IP_LEN];
    int ss_client_port;
    sscanf(lookup_ack.payload.text, "%[^:]:%d", ss_ip, &ss_client_port);
    LOG_SUCCESS("Path resolved to Storage Server at %s:%d", ss_ip, ss_client_port);

    // 2. Open a direct TCP channel with the target Storage Server node
    int ss_fd = connect_to_server(ss_ip, ss_client_port);
    if (ss_fd < 0)
        return;

    Packet data_req;
    memset(&data_req, 0, sizeof(Packet));
    data_req.msg_type = is_stream ? MSG_STREAM : MSG_READ;
    strncpy(data_req.path, path, MAX_PATH_LEN);
    send(ss_fd, &data_req, sizeof(Packet), 0);

    FILE *pipe_fp = NULL;
    if (is_stream)
    {
        // Open a media pipeline to mpv player taking binary data straight via stdin
        pipe_fp = popen("mpv -", "w");
        if (!pipe_fp)
        {
            LOG_ERROR("Failed to initialize mpv subprocess pipe extension.");
        }
        else
        {
            LOG_INFO("Media pipeline initialized. Streaming live audio bytes...");
        }
    }

    Packet chunk;
    while (recv(ss_fd, &chunk, sizeof(Packet), 0) > 0)
    {
        if (chunk.msg_type == MSG_ACK && chunk.error_code == SUCCESS)
        {
            // STOP condition met or transmission completed gracefully
            break;
        }
        if (chunk.msg_type == MSG_ERROR)
        {
            LOG_ERROR("Storage Server experienced read disruption. Code: %d", chunk.error_code);
            break;
        }

        if (is_stream && pipe_fp)
        {
            fwrite(chunk.payload.text, 1, chunk.data_size, pipe_fp);
            fflush(pipe_fp);
        }
        else if (!is_stream)
        {
            // Standard file text read: Output contents directly to standard console
            printf("%.*s", (int)chunk.data_size, chunk.payload.text);
            fflush(stdout);
        }
    }

    if (pipe_fp)
        pclose(pipe_fp);
    close(ss_fd);
    printf("\n");
    LOG_SUCCESS("Data transfer session terminated successfully.");
}

void handle_nm_mediated_op(const char *nm_ip, int nm_port, MsgType type, const char *path)
{
    Packet req;
    memset(&req, 0, sizeof(Packet));
    req.msg_type = type;
    strncpy(req.path, path, MAX_PATH_LEN);

    LOG_INFO("Sending directory operation request to Naming Server...");
    int nm_fd = connect_to_server(nm_ip, nm_port);
    if (nm_fd < 0)
        return;

    send(nm_fd, &req, sizeof(Packet), 0);

    Packet ack;
    recv(nm_fd, &ack, sizeof(Packet), 0);
    close(nm_fd);

    if (ack.msg_type == MSG_ACK && ack.error_code == SUCCESS)
    {
        LOG_SUCCESS("Operation executed successfully: %s", ack.payload.text);
    }
    else
    {
        LOG_ERROR("Operation failed. Server returned error code: %d", ack.error_code);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        LOG_ERROR("Usage: %s <NM_IP> <NM_Port> <COMMAND> <PATH>", argv[0]);
        LOG_MSG("Commands: READ, STREAM, CREATE, DELETE");
        exit(EXIT_FAILURE);
    }

    char *nm_ip = argv[1];
    int nm_port = atoi(argv[2]);
    char *cmd = argv[3];
    char *path = argv[4];

    if (strcmp(cmd, "READ") == 0)
    {
        handle_read_stream(nm_ip, nm_port, path, 0);
    }
    else if (strcmp(cmd, "STREAM") == 0)
    {
        handle_read_stream(nm_ip, nm_port, path, 1);
    }
    else if (strcmp(cmd, "CREATE") == 0)
    {
        handle_nm_mediated_op(nm_ip, nm_port, MSG_CREATE, path);
    }
    else if (strcmp(cmd, "DELETE") == 0)
    {
        handle_nm_mediated_op(nm_ip, nm_port, MSG_DELETE, path);
    }
    else
    {
        LOG_ERROR("Unrecognized runtime action command string entered: %s", cmd);
    }

    return 0;
}