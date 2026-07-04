#include "network.h"

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        LOG_ERROR("Usage error. Correct command: %s <NM_IP> <NM_Port> <My_NM_Facing_Port> <My_Client_Facing_Port> [Accessible_Path_1] [Accessible_Path_2] ...", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *nm_ip = argv[1];
    int nm_port = atoi(argv[2]);
    int my_nm_port = atoi(argv[3]);
    int my_client_port = atoi(argv[4]);

    char local_ip[IP_LEN];
    get_local_ip(local_ip, sizeof(local_ip));

    LOG_INFO("Initializing local Storage Server node...");
    LOG_INFO("Self-Resolved Interface IP: %s", local_ip);

    // Initialize packet structure cleanly
    Packet reg_packet;
    memset(&reg_packet, 0, sizeof(Packet));
    reg_packet.msg_type = MSG_REGISTER;
    reg_packet.error_code = SUCCESS;

    // Map fields directly to the union overlay allocation area
    strncpy(reg_packet.payload.ss_payload.ip, local_ip, IP_LEN);
    reg_packet.payload.ss_payload.nm_port = my_nm_port;
    reg_packet.payload.ss_payload.client_port = my_client_port;

    int path_index = 0;
    for (int i = 5; i < argc && path_index < MAX_PATHS; i++)
    {
        strncpy(reg_packet.payload.ss_payload.paths[path_index], argv[i], MAX_PATH_LEN);
        path_index++;
    }
    reg_packet.payload.ss_payload.path_count = path_index;
    reg_packet.data_size = sizeof(SS_RegisterPayload);

    LOG_INFO("Attempting connection to Naming Server at %s:%d...", nm_ip, nm_port);
    int sock_fd = connect_to_server(nm_ip, nm_port);
    if (sock_fd < 0)
    {
        LOG_ERROR("Could not reach Naming Server. Verify target status parameters.");
        exit(EXIT_FAILURE);
    }

    LOG_MSG("Connected to NM. Sending registration payload structure now...");
    if (send(sock_fd, &reg_packet, sizeof(Packet), 0) < 0)
    {
        LOG_ERROR("Failed to write packet safely across the socket wire interface.");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    LOG_MSG("Registration sent. Standing by for confirmation packet back...");
    Packet ack_packet;
    ssize_t bytes_recv = recv(sock_fd, &ack_packet, sizeof(Packet), 0);

    if (bytes_recv > 0 && ack_packet.msg_type == MSG_REGISTER_ACK)
    {
        LOG_SUCCESS("Registration confirmed by Naming Server! Response: %s", ack_packet.payload.text);
    }
    else
    {
        LOG_ERROR("Failed to register. Connection dropped or incorrect packet verification context.");
    }

    close(sock_fd);
    LOG_SUCCESS("Handshake closed successfully. Storage Server configuration state ready.");
    return 0;
}