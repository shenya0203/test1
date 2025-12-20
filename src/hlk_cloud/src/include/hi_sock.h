
/**
 * @file hi_sock.h
 * @brief
 * @author 
 * @version 0.1
 * @date 2025-04-16
 *
 * @copyright (c) 
 */

#ifndef __HI_SOCK_H_
#define __HI_SOCK_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


#ifdef __cplusplus
extern "C"
{
#endif

/*****************************************************************************
*                                DEFINE                                      *
*****************************************************************************/
#define SOCKET_PATH "/tmp/mqtt/socket/"
#define SOCKET_FILE "local_socket" // 本地套接字路径

/*****************************************************************************
 *                                TYPEDEF                                    *
 *****************************************************************************/
typedef enum{
    SUBSCRIBLE_T = 0,
    UNSUBSCRIBE_T,
    PUBLISH_T,
    UNPUBLISH_T
}TOPIC_HANDLE_E;

typedef struct{
    int topic_type;
    char _topic[128];
    char _message[5120];
    int packet_topic_len;
}TOPIC_PACKET_S;

/*****************************************************************************
*                                FUNCTION                                    *
*****************************************************************************/
extern void *socket_main();
//extern int tcp_client();

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __HI_SOCK_H_ */

/******************************************************************************
          End of the File (EOF). Do not put anything after this part!
******************************************************************************/
