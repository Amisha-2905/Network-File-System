#include "network.h"
#include <sys/time.h>
#include <errno.h>

// Configures a strict 2-second timeout window on the socket descriptor [cite: 321, 327]
int configure_socket_timeout(int sock_fd)
{
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        LOG_ERROR("Failed to set socket receive timeout configuration option.");
        return -1;
    }
    return 0;
}

void handle_read_stream(const char *nm_ip, int nm_port, const char *path, int is_stream)
{
    Packet lookup_req;
    memset(&lookup_req, 0, sizeof(Packet));
    lookup_req.msg_type = MSG_LOOKUP;
    lookup_req.error_code = 0;
    strncpy(lookup_req.path, path, MAX_PATH_LEN);

    LOG_INFO("Querying Naming Server at %s:%d for path route: %s", nm_ip, nm_port, path);
    int nm_fd = connect_to_server(nm_ip, nm_port);
    if (nm_fd < 0)
        return;

    configure_socket_timeout(nm_fd); // [cite: 327, 329]
    send(nm_fd, &lookup_req, sizeof(Packet), 0);

    // 1. Await Initial ACK from Naming Server [cite: 300, 326, 330]
    Packet initial_ack;
    ssize_t bytes = recv(nm_fd, &initial_ack, sizeof(Packet), 0);
    if (bytes <= 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            LOG_ERROR("Naming Server request timed out! (Initial ACK not received within 2s deadline)");
        }
        else
        {
            LOG_ERROR("Network link dropped while waiting for Initial ACK.");
        }
        close(nm_fd);
        return;
    }
    LOG_INFO("Initial ACK received from Naming Server. Request queued."); // [cite: 335]

    // 2. Await Final Resolution Packet [cite: 300, 330]
    Packet lookup_ack;
    bytes = recv(nm_fd, &lookup_ack, sizeof(Packet), 0);
    close(nm_fd);

    if (bytes <= 0 || lookup_ack.msg_type == MSG_ERROR)
    {
        LOG_ERROR("Path resolution rejected by Naming Server. Error Code: %d", lookup_ack.error_code);
        return;
    }

    char ss_ip[IP_LEN];
    int ss_client_port;
    sscanf(lookup_ack.payload.text, "%[^:]:%d", ss_ip, &ss_client_port);
    LOG_SUCCESS("Path resolved to Storage Server at %s:%d", ss_ip, ss_client_port);

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
        pipe_fp = popen("mpv -", "w");
        if (!pipe_fp)
            LOG_ERROR("Failed to initialize mpv subprocess media pipe extension.");
        else
            LOG_INFO("Media streaming channel live...");
    }

    Packet chunk;
    while (recv(ss_fd, &chunk, sizeof(Packet), 0) > 0)
    {
        if (chunk.msg_type == MSG_ACK)
            break;
        if (chunk.msg_type == MSG_ERROR)
        {
            LOG_ERROR("Data stream encountered runtime error: %d", chunk.error_code);
            break;
        }
        if (is_stream && pipe_fp)
        {
            fwrite(chunk.payload.text, 1, chunk.data_size, pipe_fp);
            fflush(pipe_fp);
        }
        else if (!is_stream)
        {
            printf("%.*s", (int)chunk.data_size, chunk.payload.text);
            fflush(stdout);
        }
    }

    if (pipe_fp)
        pclose(pipe_fp);
    close(ss_fd);
    printf("\n");
    LOG_SUCCESS("Data session terminated successfully.");
}

void handle_nm_mediated_op(const char *nm_ip, int nm_port, MsgType type, const char *path)
{
    Packet req;
    memset(&req, 0, sizeof(Packet));
    req.msg_type = type;
    strncpy(req.path, path, MAX_PATH_LEN);

    LOG_INFO("Forwarding directory request transaction down to Naming Server...");
    int nm_fd = connect_to_server(nm_ip, nm_port);
    if (nm_fd < 0)
        return;

    configure_socket_timeout(nm_fd);
    send(nm_fd, &req, sizeof(Packet), 0);

    // 1. Await Initial ACK [cite: 300, 326, 330]
    Packet initial_ack;
    ssize_t bytes = recv(nm_fd, &initial_ack, sizeof(Packet), 0);
    if (bytes <= 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            LOG_ERROR("Naming Server request timed out! (Initial ACK not received within 2s deadline)");
        }
        else
        {
            LOG_ERROR("Connection link broken during Initial ACK handshake phase.");
        }
        close(nm_fd);
        return;
    }
    LOG_INFO("Initial ACK received from Naming Server. Processing operation."); // [cite: 335]

    // 2. Await Final Structural ACK [cite: 300, 330]
    Packet ack;
    bytes = recv(nm_fd, &ack, sizeof(Packet), 0);
    close(nm_fd);

    if (bytes > 0 && ack.msg_type != MSG_ERROR && ack.error_code == SUCCESS)
    {
        LOG_SUCCESS("Command execution confirmed: %s", ack.payload.text);
    }
    else
    {
        LOG_ERROR("Transaction rejected by cluster. Error Code: %d", ack.error_code);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        LOG_ERROR("Usage: %s <NM_IP> <NM_Port> <COMMAND> <PATH> [WRITE_PAYLOAD]", argv[0]);
        LOG_MSG("Commands: READ, STREAM, CREATE, DELETE, INFO, WRITE");
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
    else if (strcmp(cmd, "INFO") == 0)
    {
        handle_nm_mediated_op(nm_ip, nm_port, MSG_INFO, path);
    }
    else if (strcmp(cmd, "WRITE") == 0)
    {
        if (argc < 6)
        {
            LOG_ERROR("Usage error: WRITE command requires string text payload context data input.");
            exit(EXIT_FAILURE);
        }

        Packet lookup_req;
        memset(&lookup_req, 0, sizeof(Packet));
        lookup_req.msg_type = MSG_LOOKUP;
        lookup_req.error_code = 1; // Explicit write configuration flag [cite: 163]
        strncpy(lookup_req.path, path, MAX_PATH_LEN);

        LOG_INFO("Querying Naming Server at %s:%d for WRITE path route: %s", nm_ip, nm_port, path);
        int nm_fd = connect_to_server(nm_ip, nm_port);
        if (nm_fd < 0)
            exit(EXIT_FAILURE);

        configure_socket_timeout(nm_fd); // [cite: 327, 329]
        send(nm_fd, &lookup_req, sizeof(Packet), 0);

        // 1. Await Initial ACK
        Packet initial_ack;
        if (recv(nm_fd, &initial_ack, sizeof(Packet), 0) <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                LOG_ERROR("Naming Server write lookup request timed out! (Initial ACK not received)");
            }
            else
            {
                LOG_ERROR("Timeout or disconnection encountered on initial WRITE ACK check.");
            }
            close(nm_fd);
            exit(EXIT_FAILURE);
        }
        LOG_INFO("Initial ACK received from Naming Server. Write request queued."); // RESTORED LOG [cite: 335]

        // 2. Await Final Resolution Packet [cite: 300, 330]
        Packet lookup_ack;
        recv(nm_fd, &lookup_ack, sizeof(Packet), 0);
        close(nm_fd);

        if (lookup_ack.msg_type == MSG_ERROR)
        {
            LOG_ERROR("WRITE access denied. Target path resource locked or busy. Code: %d", lookup_ack.error_code);
            exit(EXIT_FAILURE);
        }

        char ss_ip[IP_LEN];
        int ss_port;
        sscanf(lookup_ack.payload.text, "%[^:]:%d", ss_ip, &ss_port);
        LOG_SUCCESS("Write path resolved to Storage Server at %s:%d", ss_ip, ss_port);

        int ss_fd = connect_to_server(ss_ip, ss_port);
        if (ss_fd < 0)
            exit(EXIT_FAILURE);

        Packet write_pkt;
        memset(&write_pkt, 0, sizeof(Packet));
        write_pkt.msg_type = MSG_WRITE;
        strncpy(write_pkt.path, path, MAX_PATH_LEN);
        strncpy(write_pkt.payload.text, argv[5], MAX_DATA_SIZE);
        write_pkt.data_size = strlen(argv[5]);

        send(ss_fd, &write_pkt, sizeof(Packet), 0);
        Packet write_ack;
        recv(ss_fd, &write_ack, sizeof(Packet), 0);
        close(ss_fd);

        LOG_SUCCESS("Server Response: %s", write_ack.payload.text);

        // Notify Naming Server to clear write lock session status [cite: 168]
        int nm_unlock_fd = connect_to_server(nm_ip, nm_port);
        if (nm_unlock_fd >= 0)
        {
            Packet unlock_pkt;
            memset(&unlock_pkt, 0, sizeof(Packet));
            unlock_pkt.msg_type = MSG_ACK;
            strncpy(unlock_pkt.path, path, MAX_PATH_LEN);
            send(nm_unlock_fd, &unlock_pkt, sizeof(Packet), 0);
            close(nm_unlock_fd);
        }
    }
    else
    {
        LOG_ERROR("Unrecognized transaction operation command string entered: %s", cmd);
    }
    return 0;
}