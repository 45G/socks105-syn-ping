#include <iostream>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include "socks105/socks105.h"

using namespace std;

int main(int argc, char *argv[])
{
	if (argc != 6)
	{
		fprintf(stderr, "usage: %s <ip> <port> <proxy ip> <proxy port> <tfo 0/1>\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	uint32_t ip =inet_addr(argv[1]);
	uint16_t p = htons(atoi(argv[2]));
	
	uint32_t pip = inet_addr(argv[3]);;
	uint16_t pp = htons(atoi(argv[4]));
	
	bool tfo = atoi(argv[5]);
	
	int ret = EXIT_FAILURE;
	
	enum
	{
		RECV_IREP,
		RECV_FREP,
		DONE,
	};
	
	int stage = RECV_IREP;
	int read_offset = 0;
	int msg_offset = 0;
	
	timeval begin;
	timeval end;
	uint64_t time_diff;
	static const int USECS_PER_SEC = 100000;
	
	//TODO: parse args
	
	struct socks105_request req = {
		.auth_info = { 0, NULL },
		.req_type = SOCKS105_REQ_TCP_CONNECT,
		.tfo = 0,
		.server_info = {
			.addr_type = SOCKS105_ADDR_IPV4,
			.addr = {
				.ipv4 = ip,
			},
			.port = ntohs(p),
		},
		.initial_data_size = 0,
		.initial_data = NULL,
	};
	
	char buf[1500];
		
	ssize_t req_size = socks105_request_pack(&req, buf, 1500);
	if (req_size < 0)
	{
		fprintf(stderr, "error packing request: %d\n", (int)req_size);
		return EXIT_FAILURE;
	}
	
	int sock;
	
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
	{
		perror("socket");
		return EXIT_FAILURE;
	}
	
	struct sockaddr_in proxy= {
		.sin_family = AF_INET,
		.sin_port = pp,
		.sin_addr = { .s_addr = pip },
		.sin_zero = { 0 },
	};
	
	gettimeofday(&begin, NULL);
	
	if (tfo)
	{
		int err = sendto(sock, (const void *)buf, req_size, MSG_FASTOPEN, (const struct sockaddr *)&proxy, sizeof(struct sockaddr_in));
		if (err < 0)
		{
			perror("sendto");
			return EXIT_FAILURE;
		}
	}
	else
	{
		int err = connect(sock, (const struct sockaddr *)&proxy, sizeof(struct sockaddr_in));
		if (err < 0)
		{
			perror("conenct");
			return EXIT_FAILURE;
		}
		
		int offset = 0;
		while (offset < req_size)
		{
			int bytes = send(sock, (const void *)(buf + offset), req_size - offset, 0);
			if (bytes == 0)
			{
				fprintf(stderr, "connection closed\n");
				goto close_sock;
			}
			if (bytes < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					continue;
				perror("send");
				goto close_sock;
			}
			
			offset += bytes;
		}
	}
	
	memset(buf, 0, sizeof(buf));
	
	while (stage != DONE)
	{
		ssize_t bytes = recv(sock, buf + read_offset, sizeof(buf) - read_offset, 0);
		if (bytes == 0)
		{
			fprintf(stderr, "connection closed\n");
			goto close_sock;
		}
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			perror("recv");
			goto close_sock;
		}
		
		read_offset += bytes;
		
		if (stage == RECV_IREP)
		{
			socks105_initial_reply *irep;
			ssize_t size = socks105_initial_reply_parse(buf + msg_offset, read_offset - msg_offset, &irep);
			if (size == SOCKS105_ERROR_BUFFER)
				continue;
			if (size < 0)
			{
				fprintf(stderr, "error parsing irep: %d\n", (int)size);
				break;
			}
			msg_offset += size;
			
			if (irep->irep_type != SOCKS105_INITIAL_REPLY_SUCCESS)
				stage = DONE;
			else
				stage = RECV_FREP;
			
			socks105_initial_reply_delete(irep);
			fprintf(stderr, "got irep\n");
			
		}
		else if (stage == RECV_FREP)
		{
			socks105_final_reply *frep;
			ssize_t size = socks105_final_reply_parse(buf + msg_offset, read_offset - msg_offset, &frep);
			if (size == SOCKS105_ERROR_BUFFER)
				continue;
			if (size < 0)
			{
				fprintf(stderr, "error parsing frep: %d\n", (int)size);
				goto close_sock;
			}
			
			if (frep->frep_type == SOCKS105_FINAL_REPLY_SUCCESS)
				ret = EXIT_SUCCESS;
			else
				fprintf(stderr, "frep fail: %d\n", (int)frep->frep_type);
			
			socks105_final_reply_delete(frep);
			fprintf(stderr, "got frep\n");
			break;
		}
	}
	
	gettimeofday(&end, NULL);
	
	time_diff = end.tv_sec * USECS_PER_SEC + end.tv_usec - begin.tv_sec * USECS_PER_SEC - begin.tv_usec;
	
	printf("%.2f ms\n", (float)time_diff / 1000);
		
close_sock:
	close(sock);
	
	return ret;
}
