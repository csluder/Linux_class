#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define NETLINK_USER 31
#define MY_GROUP 1

int main() {
    int sock_fd;
    struct sockaddr_nl src_addr;
    struct nlmsghdr *nlh;
    struct iovec iov;
    struct msghdr msg;

    sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_USER);
    if (sock_fd < 0) return -1;

    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    /* Correct Group Mask for Group 1: (1 << (1 - 1)) = 1 */
    src_addr.nl_groups = (1 << (MY_GROUP - 1));

    if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(1024));
    memset(nlh, 0, NLMSG_SPACE(1024));

    iov.iov_base = (void *)nlh;
    iov.iov_len = NLMSG_SPACE(1024);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    printf("Waiting for kernel message on Group %d...\n", MY_GROUP);

    while (1) {
        ssize_t ret = recvmsg(sock_fd, &msg, 0);
        if (ret > 0) {
            printf("Received: %s\n", (char *)NLMSG_DATA(nlh));
        }
    }

    return 0;
}

