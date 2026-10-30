/* Loopback TCP pair, one line each way, then close in the order asked; prints each step. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct sockaddr_in address;
	socklen_t length;
	struct pollfd p;
	char buffer[16];
	int listener, client, server, nonblocking, order;

	order = argc > 1 ? atoi(argv[1]) : 0;
	nonblocking = argc > 2 ? atoi(argv[2]) : 1;
	listener = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(listener, (struct sockaddr *)&address, sizeof(address));
	listen(listener, 1);
	length = sizeof(address);
	getsockname(listener, (struct sockaddr *)&address, &length);
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (nonblocking)
		fcntl(client, F_SETFL, O_NONBLOCK);
	if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0)
		printf("connect errno=%d\n", errno);
	server = accept(listener, NULL, NULL);
	p.fd = client; p.events = POLLOUT; p.revents = 0;
	poll(&p, 1, 2000);
	printf("accepted %d revents=%x\n", server, p.revents);
	fflush(stdout);
	(void)write(client, "ping\n", 5);
	p.fd = server; p.events = POLLIN; poll(&p, 1, 2000);
	printf("server read %zd\n", read(server, buffer, sizeof(buffer)));
	(void)write(server, "ping\n", 5);
	p.fd = client; p.events = POLLIN; poll(&p, 1, 2000);
	printf("client read %zd\n", read(client, buffer, sizeof(buffer)));
	fflush(stdout);
	if (order == 0) {
		printf("close server\n"); fflush(stdout); close(server);
		printf("close client\n"); fflush(stdout); close(client);
	} else {
		printf("close client\n"); fflush(stdout); close(client);
		printf("close server\n"); fflush(stdout); close(server);
	}
	printf("close listener\n"); fflush(stdout); close(listener);
	printf("done\n");
	return 0;
}
