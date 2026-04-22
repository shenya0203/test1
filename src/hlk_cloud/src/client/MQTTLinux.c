/*******************************************************************************
 * Copyright (c) 2014, 2023 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Allan Stockdill-Mander - initial API and implementation and/or initial documentation
 *    Ian Craggs - return codes from linux_read
 *******************************************************************************/
#include <errno.h>
#include <sys/select.h>  // 添加select相关函数
#include <fcntl.h>       // 添加fcntl相关函数
#include "MQTTLinux.h"
#include "hi_link.h"
#include "hlk_log.h"
#include "app_api.h"     // 包含Zig实现的函数声明
void TimerInit(Timer* timer)
{
	timer->end_time = (struct timeval){0, 0};
}

char TimerIsExpired(Timer* timer)
{
	zig_timeval now, res;
	zig_gettimeofday(&now, NULL);
	timersub(&timer->end_time, &now, &res);
	//printf("TimerIsExpired: res.tv_sec=%ld, res.tv_usec=%ld\n", res.tv_sec, res.tv_usec);
	return res.tv_sec < 0 || (res.tv_sec == 0 && res.tv_usec <= 0);
}


void TimerCountdownMS(Timer* timer, unsigned int timeout)
{
	zig_timeval now;
	zig_gettimeofday(&now, NULL);
	struct timeval interval = {timeout / 1000, (timeout % 1000) * 1000};
	timeradd(&now, &interval, &timer->end_time);
}


void TimerCountdown(Timer* timer, unsigned int timeout)
{
	zig_timeval now;
	zig_gettimeofday(&now, NULL);
	struct timeval interval = {timeout, 0};
	timeradd(&now, &interval, &timer->end_time);
}


int TimerLeftMS(Timer* timer)
{
	zig_timeval now, res;
	zig_gettimeofday(&now, NULL);
	timersub(&timer->end_time, &now, &res);
	//printf("left %d ms\n", (res.tv_sec < 0) ? 0 : res.tv_sec * 1000 + res.tv_usec / 1000);
	return (res.tv_sec < 0) ? 0 : res.tv_sec * 1000 + res.tv_usec / 1000;
}


int linux_read(Network* n, unsigned char* buffer, int len, int timeout_ms)
{
	// 参数验证
	if (timeout_ms < 0)
	{
		HLK_LOG_ERR("linux_read: Invalid timeout_ms=%d, using default 1000ms\n", timeout_ms);
		timeout_ms = 1000;
	}

	// 获取当前socket标志
	int flags = fcntl(n->my_socket, F_GETFL, 0);
	if (flags == -1)
	{
		HLK_LOG_ERR("linux_read: fcntl F_GETFL failed with error: %s (errno=%d)\n", strerror(errno), errno);
		return -1;
	}

	// 设置socket为非阻塞模式
	if (fcntl(n->my_socket, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		HLK_LOG_ERR("linux_read: fcntl F_SETFL failed with error: %s (errno=%d)\n", strerror(errno), errno);
		return -1;
	}

	int bytes = 0;
	zig_timeval start_time, current_time;
	zig_gettimeofday(&start_time, NULL);

	while (bytes < len)
	{
		// 计算剩余超时时间
		zig_gettimeofday(&current_time, NULL);
		long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
						 (current_time.tv_usec - start_time.tv_usec) / 1000;
		long remaining_ms = timeout_ms - elapsed_ms;

		if (remaining_ms <= 0)
		{
			break;
		}

		// 准备select参数
		zig_fd_set read_fds;
		zig_timeval timeout;

		timeout.tv_sec = remaining_ms / 1000;
		timeout.tv_usec = (remaining_ms % 1000) * 1000;

		zig_FD_ZERO(&read_fds);
		zig_FD_SET(n->my_socket, &read_fds);

		// 使用zig实现的select等待socket可读
		int select_result = zig_select(n->my_socket + 1, &read_fds, NULL, NULL, &timeout);

		if (select_result == -1)
		{
			int saved_errno = errno;
			HLK_LOG_ERR("linux_read: select failed with error: %s (errno=%d)\n", strerror(saved_errno), saved_errno);
			bytes = -1;
			break;
		}
		else if (select_result == 0)
		{
			break;
		}

		// 检查socket是否可读
		if (zig_FD_ISSET(n->my_socket, &read_fds))
		{
			int rc = recv(n->my_socket, &buffer[bytes], (size_t)(len - bytes), MSG_DONTWAIT);
			int saved_errno = errno;

			if (rc == -1)
			{
				if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
				{
					// 这种情况在select返回可读后不应该发生，但为了安全起见继续循环
					HLK_LOG_ERR("linux_read: recv would block after select indicated ready\n");
					continue;
				}
				else
				{
					HLK_LOG_ERR("linux_read: recv failed with error: %s (errno=%d)\n", strerror(saved_errno), saved_errno);
					bytes = -1;
					break;
				}
			}
			else if (rc == 0)
			{
				//PRF("linux_read: Connection closed by peer\n");
				break;
			}
			else
			{
				bytes += rc;
			}
		}
	}

	// 恢复socket为原来的阻塞模式
	if (fcntl(n->my_socket, F_SETFL, flags) == -1)
	{
		HLK_LOG_ERR("linux_read: fcntl restore flags failed with error: %s (errno=%d)\n", strerror(errno), errno);
	}

	return bytes;
}


int linux_write(Network* n, unsigned char* buffer, int len, int timeout_ms)
{
	struct timeval tv;

	tv.tv_sec = 0;  /* 30 Secs Timeout */
	tv.tv_usec = timeout_ms * 1000;  // Not init'ing this can cause strange errors

	setsockopt(n->my_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv,sizeof(struct timeval));
	int	rc = write(n->my_socket, buffer, len);
	return rc;
}


void NetworkInit(Network* n)
{
	signal(SIGPIPE, SIG_IGN);
	n->my_socket = 0;
	n->mqttread = linux_read;
	n->mqttwrite = linux_write;
}


int NetworkConnect(Network* n, char* addr, int port)
{
	struct sockaddr_in sAddr;
	int retVal = -1;

    struct hostent *hostinfo = gethostbyname(addr);
	HLK_LOG_INFO("NetworkConnect\n");
	if(hostinfo == NULL)
	{
		HLK_LOG_ERR("hostinfo == NULL\n");
		return retVal;
	}
    sAddr.sin_family = AF_INET;
	sAddr.sin_port = htons(port);
    sAddr.sin_addr = *((struct in_addr *)hostinfo->h_addr);
	uint32_t address = sAddr.sin_addr.s_addr;
	
	struct in_addr tmp_addr;
	tmp_addr.s_addr = address;  // 将 uint32_t 赋值给 s_addr
	char *ip = inet_ntoa(tmp_addr);
    // char *ip = inet_ntoa(address);
    HLK_LOG_INFO("Server ip Address : %s\r\n", ip);

	if ((n->my_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0){
		HLK_LOG_ERR("my_socket--->errr\r\n");
		goto exit;
	}
		

	struct timeval opt_on;
	opt_on.tv_sec = 5;
	opt_on.tv_usec = 0;

	struct timeval opt_send;
	opt_send.tv_sec = 1;
	opt_send.tv_usec = 0;
	int keepAlive = 1;    // 开启keepalive属性. 缺省值: 0(关闭)
	setsockopt(n->my_socket, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
	setsockopt(n->my_socket, SOL_SOCKET, SO_RCVTIMEO, &opt_on, sizeof(opt_on));
	setsockopt(n->my_socket, SOL_SOCKET, SO_SNDTIMEO, &opt_send, sizeof(opt_send));
	// int recvbuf = 2048,len = 4;
	int flag = 1;
    setsockopt(n->my_socket, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(int));

    // setsockopt( n->my_socket, SOL_SOCKET, SO_RCVBUF, &recvbuf, len );
	// printf("-----------------%s %d  %d %d\n",__func__,__LINE__,recvbuf,len);
	// recvbuf = 200;
	// getsockopt( n->my_socket, SOL_SOCKET, SO_RCVBUF, &recvbuf, &len );
	HLK_LOG_INFO("start_Connect--------->\r\n");
	if ((retVal = connect(n->my_socket, (const struct sockaddr *)&sAddr, sizeof(sAddr))) < 0)
	{
		HLK_LOG_ERR("Connect err\n");
		close(n->my_socket);
	    goto exit;
	}

	//LOG;

exit:
	return retVal;
	#if 0
	int type = SOCK_STREAM;
	struct sockaddr_in address;
	int rc = -1;
	sa_family_t family = AF_INET;
	struct addrinfo *result = NULL;
	struct addrinfo hints = {0, AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL};

	if ((rc = getaddrinfo(addr, NULL, &hints, &result)) == 0)
	{
		struct addrinfo* res = result;

		/* prefer ip4 addresses */
		while (res)
		{
			if (res->ai_family == AF_INET)
			{
				result = res;
				break;
			}
			res = res->ai_next;
		}

		if (result->ai_family == AF_INET)
		{
			address.sin_port = htons(port);
			address.sin_family = family = AF_INET;
			address.sin_addr = ((struct sockaddr_in*)(result->ai_addr))->sin_addr;
			printf("address.sin_addr.s_addr %s\n",inet_ntoa(address.sin_addr));
		}
		else
			rc = -1;

		freeaddrinfo(result);
	}

	if (rc == 0)
	{
		n->my_socket = socket(family, type, 0);
		if (n->my_socket != -1)
			rc = connect(n->my_socket, (struct sockaddr*)&address, sizeof(address));
		else
			rc = -1;
	}

	return rc;
	#endif
}

void NetworkDisconnect(Network* n)
{
	if(n->my_socket != -1)
	{
		close(n->my_socket);
	}
	n->my_socket = -1;
}

