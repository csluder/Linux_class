/**
 * @file l3harris_listener.c
 * @brief User-space listener for L3Harris Netlink broadcasts.
 * 
 * PRESENTATION NOTES:
 * 1. Protocol: USES NETLINK_USER (31) to match the kernel driver.
 * 2. Multicast: Binds to Group 1 to receive broadcast events.
 * 3. Blocking: The recvmsg() call sleeps efficiently until the kernel speaks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/* Must match the kernel driver defines */
#define NETLINK_L3HARRIS 31
#define L3H_MCAST_GROUP  1

int main() {
    int sock_fd;
    struct sockaddr_nl src_addr, dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct iovec iov;
    struct msghdr msg;

    /* 1. Create a Netlink Socket */
    sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_L3HARRIS);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    /* 2. Set up Source Address (Our app) */
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid(); /* Our unique ID */
    /* Bind to the multicast group mask: (1 << (group_id - 1)) */
    src_addr.nl_groups = (1 << (L3H_MCAST_GROUP - 1));

    if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("Bind failed");
        close(sock_fd);
        return -1;
    }

    /* 3. Prepare the Buffer for incoming messages */
    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(1024));
    memset(nlh, 0, NLMSG_SPACE(1024));

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = (void *)nlh;
    iov.iov_len = NLMSG_SPACE(1024);

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    printf("--- L3Harris Event Monitor ---\n");
    printf("Listening for Kernel events on Group %d...\n", L3H_MCAST_GROUP);

    /* 4. Infinite Loop: Wait for Kernel Broadcasts */
    while (1) {
        ssize_t ret = recvmsg(sock_fd, &msg, 0);
        if (ret > 0) {
            /* Print the data payload (skipping the Netlink header) */
            char *payload = (char *)NLMSG_DATA(nlh);
            
            if (strcmp(payload, "STATE:MOTION") == 0) {
                printf("[NOTIFICATION] %-15s | Motion Triggered! LEDs Flashing.\n", payload);
            } else if (strcmp(payload, "STATE:CLEAR") == 0) {
                printf("[NOTIFICATION] %-15s | Area Secure. LEDs Off.\n", payload);
            } else {
                printf("[EVENT] Received: %s\n", payload);
            }
        }
    }

    close(sock_fd);
    free(nlh);
    return 0;
}
