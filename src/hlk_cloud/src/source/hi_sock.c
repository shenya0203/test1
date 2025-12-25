//#include <uci.h>
#include "hi_sock.h"
#include "hi_mqtt.h"
#include "hi_link.h"

//#include "app_api.h"
//#include "hlk_mqtt.h"

/******************************************************************************
 Function    : create_local_socket
 Description : 创建本地套接字通信
 Input  Parm : none
 Output Parm : 
 Return      : 0 成功 -1 失败
******************************************************************************/
int create_local_socket()
{
    int _socket;

    // 创建Unix域套接字，使用流式协议(SOCK_STREAM)进行可靠的双向连接
    // AF_UNIX表示使用Unix域套接字地址族，适用于本地进程间通信
    _socket = socket(AF_UNIX,SOCK_STREAM,0);
    if(_socket == -1)
    {
        PRF("Error creating local socket\n");
        return -1;
    }

    return _socket;
}

/******************************************************************************
 Function    : bind_listen_socket
 Description : 绑定listen
 Input  Parm : socket_fd - 套接字文件描述符
 Output Parm : none
 Return      : 0 成功 非0 失败
******************************************************************************/
int bind_listen_socket(int socket_fd) {
    struct sockaddr_un addr;
    
    // 初始化地址结构体，清空所有字段
    memset(&addr, 0, sizeof(addr));  // 清空结构体
    
    // 设置地址族为Unix域套接字
    addr.sun_family = AF_UNIX;
    
    // 设置套接字文件路径，用于进程间通信的标识
    // 注意：路径长度不能超过sizeof(addr.sun_path)-1，预留结束符位置
    strncpy(addr.sun_path, "/tmp/my_socket", sizeof(addr.sun_path)-1);

    // 删除可能存在的旧套接字文件，避免地址已被使用的错误
    // unlink()会删除文件系统中的文件，如果文件不存在则忽略错误
    unlink(addr.sun_path);

    // 将套接字绑定到指定的地址
    // bind()将套接字文件描述符与地址结构关联
    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        PRF("bind error: %s (errno=%d)\n", strerror(errno), errno);  // 打印具体错误信息和错误码
        return -1;
    }
    
    // 将套接字设置为监听模式，准备接受连接请求
    // 第二个参数5表示等待连接队列的最大长度
    if (listen(socket_fd, 5) == -1) {
        PRF("listen error: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/******************************************************************************
 Function    : socket_main
 Description : socket主函数，处理客户端连接和数据通信
 Input  Parm : arg - 传入的参数指针(SHARED_DATA_S类型)
 Output Parm : none
 Return      : NULL
******************************************************************************/
void *socket_main(void* arg)
{
    int ret;
    int local_socket;     // 本地套接字文件描述符
    int accept_st;        // 接受连接后的套接字文件描述符

    // 将传入参数转换为共享数据结构指针
    SHARED_DATA_S* psharedData = (SHARED_DATA_S*)arg;
    
    // 注意：这里有一个bug，应该在创建套接字后再设置套接字选项
    // 当前代码在套接字创建前就尝试设置选项，会导致错误
    struct timeval reuse = {0};
    setsockopt(local_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 创建本地Unix域套接字
    local_socket = create_local_socket();
    if(local_socket == -1){
        close(local_socket);  // 创建失败时关闭套接字
        return NULL;
    }
    PRF("local_socket:%d\r\n",local_socket);

    // 绑定套接字到本地地址并开始监听
    ret = bind_listen_socket(local_socket);
    if(ret == -1){
        PRF("bind listen_socket errr\r\n");
        close(local_socket);  // 绑定失败时关闭套接字
        return NULL;
    }   
    PRF("bind ret:%d\r\n",ret);

    // 标签：用于重新接受新的客户端连接
_accept:
    // 等待并接受客户端连接请求
    // accept()会阻塞直到有客户端连接，返回新的套接字用于与该客户端通信
    accept_st = accept(local_socket,NULL,NULL);
    if(accept_st == -1){
        PRF("Errror acceping connection\n");
        close(local_socket);
        return NULL;
    }
    PRF("accept sucess :%d\r\n",accept_st);
    
    // 主循环：处理客户端发送的数据
    while(1)
    {
        // 初始化接收数据包结构体
        TOPIC_PACKET_S recv_packet_s = {0};
        
        // 从客户端接收数据
        // recv()会阻塞等待数据到达，接收TOPIC_PACKET_S结构体大小的数据
        int bytes_received = recv(accept_st,(char *)&recv_packet_s,sizeof(TOPIC_PACKET_S),0);
        
        if(bytes_received == -1)
        {
            // 接收数据出错
            PRF("Error receiving data");
        }
        else if(bytes_received > 0)
        {
            // 成功接收到数据，打印接收到的信息
            PRF("buffer message :%d %s %d,bytes_received:%d\n", 
                recv_packet_s.topic_type,           // 主题类型
                recv_packet_s._topic,               // 主题内容
                recv_packet_s.packet_topic_len,     // 数据包长度
                bytes_received);                    // 实际接收字节数
            
            // 注释掉的数据分析和响应处理
            //analyse_and_response(accept_st,&recv_packet_s,psharedData);
        }
        else
        {
            // bytes_received == 0 表示客户端已断开连接
            //MESSAGE_PRINT("bytes_received:%d,client disconnected !!!\n",bytes_received); 
            
            // 关闭当前客户端连接的套接字
            if (accept_st){
                PRF("close\r\n");
                close(accept_st);
            }  
            
            // 跳转到重新等待新客户端连接
            goto _accept;
        }
    }

    // 程序结束时关闭主监听套接字
    // 注意：由于上面是无限循环，正常情况下不会执行到这里
    if(local_socket)
        close(local_socket);

    //pthread_exit(NULL);  // 线程退出
    
    return NULL;
}
