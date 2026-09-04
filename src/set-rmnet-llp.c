#include <linux/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define RMNET_IOCTL_SET_LLP_IP 0x000089F2
#define RMNET_IOCTL_GET_LLP    0x000089F3

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "rmnet0";
    struct ifreq ifr;
    unsigned int mode = 0;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, RMNET_IOCTL_SET_LLP_IP, &ifr)) {
        perror("RMNET_IOCTL_SET_LLP_IP");
        return 1;
    }

    ifr.ifr_data = (void *)&mode;
    if (ioctl(fd, RMNET_IOCTL_GET_LLP, &ifr)) {
        perror("RMNET_IOCTL_GET_LLP");
        return 1;
    }

    printf("%s llp_mode=0x%x\n", name, mode);
    close(fd);
    return mode == 0x2 ? 0 : 1;
}
