// SPDX-License-Identifier: MIT
#include <errno.h>
#include <linux/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define RMNET_IOCTL_SET_LLP_ETHERNET 0x000089F1
#define RMNET_IOCTL_SET_LLP_IP       0x000089F2
#define RMNET_IOCTL_GET_LLP          0x000089F3
#define RMNET_IOCTL_EXTENDED         0x000089FD

#define RMNET_IOCTL_GET_EPID         0x0003
#define RMNET_IOCTL_GET_EP_PAIR      0x0010

#define RMNET_MODE_LLP_ETH           0x01
#define RMNET_MODE_LLP_IP            0x02

struct rmnet_ioctl_extended {
	unsigned int extended_ioctl;
	union {
		unsigned int data;
		struct {
			unsigned int consumer_pipe_num;
			unsigned int producer_pipe_num;
		} ep_pair;
	} u;
};

static void usage(FILE *stream, const char *name)
{
	fprintf(stream,
		"Usage: %s COMMAND INTERFACE\n"
		"\n"
		"Commands:\n"
		"  get-endpoints  Print endpoint ID and BAM consumer/producer pipes\n"
		"  get-llp        Print the current link-layer protocol\n"
		"  set-raw-ip     Select raw-IP link-layer protocol\n"
		"  set-ethernet   Select Ethernet link-layer protocol\n",
		name);
}

static int prepare_ifreq(struct ifreq *ifr, const char *interface)
{
	if (strlen(interface) >= sizeof(ifr->ifr_name)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memset(ifr, 0, sizeof(*ifr));
	strcpy(ifr->ifr_name, interface);
	return 0;
}

static int get_extended(int fd, const char *interface, unsigned int command,
			struct rmnet_ioctl_extended *extended)
{
	struct ifreq ifr;

	if (prepare_ifreq(&ifr, interface))
		return -1;
	memset(extended, 0, sizeof(*extended));
	extended->extended_ioctl = command;
	ifr.ifr_data = (void *)extended;
	return ioctl(fd, RMNET_IOCTL_EXTENDED, &ifr);
}

static int get_endpoints(int fd, const char *interface)
{
	struct rmnet_ioctl_extended extended;
	unsigned int endpoint;

	if (get_extended(fd, interface, RMNET_IOCTL_GET_EPID, &extended))
		return -1;
	endpoint = extended.u.data;
	if (get_extended(fd, interface, RMNET_IOCTL_GET_EP_PAIR, &extended))
		return -1;

	printf("interface=%s endpoint=%u consumer=%u producer=%u\n",
		interface, endpoint, extended.u.ep_pair.consumer_pipe_num,
		extended.u.ep_pair.producer_pipe_num);
	return 0;
}

static int get_llp(int fd, const char *interface)
{
	struct ifreq ifr;
	unsigned int mode = 0;
	const char *description = "unknown";

	if (prepare_ifreq(&ifr, interface))
		return -1;
	ifr.ifr_data = (void *)&mode;
	if (ioctl(fd, RMNET_IOCTL_GET_LLP, &ifr))
		return -1;
	if (mode & RMNET_MODE_LLP_IP)
		description = "raw-ip";
	else if (mode & RMNET_MODE_LLP_ETH)
		description = "ethernet";
	printf("interface=%s llp=0x%x (%s)\n", interface, mode, description);
	return 0;
}

static int set_llp(int fd, const char *interface, unsigned long request)
{
	struct ifreq ifr;

	if (prepare_ifreq(&ifr, interface))
		return -1;
	if (ioctl(fd, request, &ifr))
		return -1;
	return get_llp(fd, interface);
}

int main(int argc, char **argv)
{
	int fd;
	int result;

	if (argc != 3) {
		usage(stderr, argv[0]);
		return 2;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	if (!strcmp(argv[1], "get-endpoints"))
		result = get_endpoints(fd, argv[2]);
	else if (!strcmp(argv[1], "get-llp"))
		result = get_llp(fd, argv[2]);
	else if (!strcmp(argv[1], "set-raw-ip"))
		result = set_llp(fd, argv[2], RMNET_IOCTL_SET_LLP_IP);
	else if (!strcmp(argv[1], "set-ethernet"))
		result = set_llp(fd, argv[2], RMNET_IOCTL_SET_LLP_ETHERNET);
	else {
		usage(stderr, argv[0]);
		close(fd);
		return 2;
	}

	if (result)
		perror(argv[1]);
	close(fd);
	return result ? 1 : 0;
}
