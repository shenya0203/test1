/******************************************************************************

                  Copyright (C),  

 ******************************************************************************
  Filename   : hi_link.c
  Version    : 
  Author     : 
  Creation   : 
  Description: MQTT链接管理模块，实现设备与云平台的MQTT通信功能
******************************************************************************/

/*****************************************************************************
 *                                INCLUDE                                    *
 *****************************************************************************/
/* 系统核心头文件 */
#include <stdint.h>

#ifdef HLK_PRODUCT_WR10
#include "hi_uspace.h"

#include "hi_timer.h"

#include "hi_board.h"
#include "hi_ioreactor.h"
#include "hi_notifier.h"
#include "hi_oam_notify.h"
#include "hi_omci_api.h"
#include "hi_pon_gpon_api.h"
#include "hi_ipc_def.h"
#include "hi_env.h"
#include "hi_sys_env.h"

#include "os/hi_os_thread.h"
#endif
#define _GNU_SOURCE

#ifdef HLK_PRODUCT_7628
#include <sys/prctl.h>  /* 为7628平台的prctl函数调用 */
#endif

#include <pthread.h>

/* MQTT客户端相关头文件 */
#include "client/MQTTLinux.h"

/* 应用程序接口头文件 */
#include "app_api.h"
#include "hi_link.h"
#include "hi_mqtt.h"

#ifdef SUPPORT_OPENWRT
#include "openwrt/openwrt.h"
#else
#include "linux/linux.h"
#endif

/*****************************************************************************
 *                                LOCAL_DEFINE                               *
 *****************************************************************************/
/* 十六进制转字符串宏定义 */
#define HEX2STR(h) ((h) < 0x0A ? (h) + '0' : ((h)-0x0A) + 'A')
/* 字符串转十六进制宏定义 */
#define STR2HEX(h) ((h) < 'A' ? (h) - '0' : ((h) < 'a' ? ((h) - 'A') + 0x0A : ((h) - 'a') + 0x0A))

/*****************************************************************************
 *                                LOCAL_TYPEDEF                              *
 *****************************************************************************/




/*****************************************************************************
 *                                LOCAL_VARIABLE                             *
 *****************************************************************************/
/* 用户线程ID全局变量 */
#ifdef HLK_PRODUCT_WR10
HI_PTHREAD_T g_user_pthread_id = NULL;
#else
pthread_t g_user_pthread_id = NULL;
#endif

/* 线程间共享数据结构 */
SHARED_DATA_S sharedData;

/* MQTT连接配置选项结构体 */
struct Options{
  	char* type;          /* 连接类型 */
	char* host;          /* MQTT服务器主机地址 */
  	int port;            /* MQTT服务器端口号 */
	char* proxy_host;    /* 代理服务器主机地址 */
  	int proxy_port;      /* 代理服务器端口号 */
	int verbose;         /* 详细输出标志 */
	int test_no;         /* 测试编号 */
	int MQTTVersion;     /* MQTT协议版本 */
	int iterations;      /* 迭代次数 */
} options = {
  	"hlk",               /* 默认连接类型为hlk */
	"yun.hlktech.com",   /* 默认MQTT服务器地址 */
    1883,                /* 默认MQTT端口号 */
	"yun.hlktech.com",   /* 默认代理服务器地址 */
	1883,                /* 默认代理服务器端口 */
	0,                   /* 默认关闭详细输出 */
	0,                   /* 默认测试编号 */
	4,                   /* 默认MQTT版本4 */
	1,                   /* 默认迭代1次 */
};

/*****************************************************************************
 *                                LOCAL_FUNCTION                             *
 *****************************************************************************/

 /******************************************************************************
 Function    : usage
 Description : 打印帮助信息
 Input  Parm : none 
 Output Parm : 
 Return      : 
******************************************************************************/
void usage(void)
{
	printf("help!!\r\n");
}

/******************************************************************************
 Function    : getopts
 Description : 解析命令行参数，设置MQTT连接配置
 Input  Parm : argc - 参数个数
 Input  Parm : argv - 参数数组
 Output Parm : N/A
 Return      : 无
******************************************************************************/
void getopts(int argc, char** argv)
{
	int count = 1;

	/* 遍历所有命令行参数 */
	while (count < argc)
	{
		/* 解析连接类型参数 */
		if (strcmp(argv[count], "--type") == 0)
		{
			if (++count < argc)
      		{
				options.type = argv[count];
        		printf("\nSetting type to %s\n", options.type);
      		}
			else
				usage();
		} 
		/* 解析测试编号参数 */
    	else if (strcmp(argv[count], "--test_no") == 0)
		{
			if (++count < argc)
				options.test_no = atoi(argv[count]);
			else
				usage();
		}
		/* 解析主机地址参数 */
		else if (strcmp(argv[count], "--host") == 0)
		{
			if (++count < argc)
			{
				options.host = argv[count];
				printf("\nSetting host to %s\n", options.host);
			}
			else
				usage();
		}
		/* 解析端口号参数 */
		else if (strcmp(argv[count], "--port") == 0)
		{
			if (++count < argc)
			{
				options.port = atoi(argv[count]);
				printf("\nSetting port to %d\n", options.port);
			}
			else
				usage();
			}
		/* 解析代理主机地址参数 */
		else if (strcmp(argv[count], "--proxy_host") == 0)
		{
			if (++count < argc)
			{
				options.proxy_host = argv[count];
				printf("\nSetting proxy_host to %s\n", options.proxy_host);
			}
				else
					usage();
			}
		/* 解析代理端口号参数 */
		else if (strcmp(argv[count], "--proxy_port") == 0)
		{
		if (++count < argc)
		{
			options.proxy_port = atoi(argv[count]);
			printf("\nSetting proxy_port to %d\n", options.proxy_port);
		}
		else
			usage();
		}
		/* 解析MQTT版本参数 */
		else if (strcmp(argv[count], "--MQTTversion") == 0)
		{
			if (++count < argc)
			{
				options.MQTTVersion = atoi(argv[count]);
				printf("setting MQTT version to %d\n", options.MQTTVersion);
			}
			else
				usage();
		}
		/* 解析迭代次数参数 */
		else if (strcmp(argv[count], "--iterations") == 0)
		{
			if (++count < argc)
				options.iterations = atoi(argv[count]);
			else
				usage();
		}
		/* 解析详细输出标志参数 */
		else if (strcmp(argv[count], "--verbose") == 0)
		{
			options.verbose = 1;
			printf("\nSetting verbose on\n");
		}
		count++;
	}
}

//设置通道值（支持多种数据类型）
int hi_link_set_channel_value(const char *channel, const hi_link_value_t *value)
{
    if (!channel || !value) {
        PRF("hi_link_set_channel_value: Invalid parameters\r\n");
        return -1;
    }

    switch (value->type) {
        case HI_LINK_TYPE_STRING:
            PRF("hi_link_set_channel_value channel: %s, string value: %s\r\n", 
                channel, value->data.str_val ? value->data.str_val : "NULL");
            break;
        case HI_LINK_TYPE_INT:
            PRF("hi_link_set_channel_value channel: %s, int value: %d\r\n", 
                channel, value->data.int_val);
            break;
        case HI_LINK_TYPE_FLOAT:
            PRF("hi_link_set_channel_value channel: %s, float value: %.2f\r\n", 
                channel, value->data.float_val);
            break;
        case HI_LINK_TYPE_BOOL:
            PRF("hi_link_set_channel_value channel: %s, bool value: %s\r\n", 
                channel, value->data.bool_val ? "true" : "false");
            break;
        default:
            PRF("hi_link_set_channel_value: Unknown value type %d\r\n", value->type);
            return -1;
    }

    #if defined(SUPPORT_OPENWRT)
    return hi_link_set_channel_value_openwrt(channel, value);
    #else
    return hi_link_set_channel_value_linux(channel, value);
    #endif
}

//获取通道值（支持多种数据类型）
int hi_link_get_channel_value(const char *channel, hi_link_value_t *value)
{
    if (!channel || !value) {
        PRF("hi_link_get_channel_value: Invalid parameters\r\n");
        return -1;
    }

    PRF("hi_link_get_channel_value channel: %s\r\n", channel);

    #if defined(SUPPORT_OPENWRT)
    int ret = hi_link_get_channel_value_openwrt(channel, value);
    #else
    int ret = hi_link_get_channel_value_linux(channel, value);
    #endif

    if (ret == 0) {
        switch (value->type) {
            case HI_LINK_TYPE_STRING:
                PRF("Got string value: %s\r\n", value->data.str_val ? value->data.str_val : "NULL");
                break;
            case HI_LINK_TYPE_INT:
                PRF("Got int value: %d\r\n", value->data.int_val);
                break;
            case HI_LINK_TYPE_FLOAT:
                PRF("Got float value: %.2f\r\n", value->data.float_val);
                break;
            case HI_LINK_TYPE_BOOL:
                PRF("Got bool value: %s\r\n", value->data.bool_val ? "true" : "false");
                break;
        }
    }

    return ret;
}

/* 便捷接口实现 */

//设置字符串类型通道值
int hi_link_set_channel_string(const char *channel, const char *str_value)
{
    hi_link_value_t value;
    value.type = HI_LINK_TYPE_STRING;
    value.data.str_val = (char *)str_value;
    return hi_link_set_channel_value(channel, &value);
}

//设置整数类型通道值
int hi_link_set_channel_int(const char *channel, int int_value)
{
    hi_link_value_t value;
    value.type = HI_LINK_TYPE_INT;
    value.data.int_val = int_value;
    return hi_link_set_channel_value(channel, &value);
}

//设置浮点数类型通道值
int hi_link_set_channel_float(const char *channel, float float_value)
{
    hi_link_value_t value;
    value.type = HI_LINK_TYPE_FLOAT;
    value.data.float_val = float_value;
    return hi_link_set_channel_value(channel, &value);
}

//设置布尔类型通道值
int hi_link_set_channel_bool(const char *channel, bool bool_value)
{
    hi_link_value_t value;
    value.type = HI_LINK_TYPE_BOOL;
    value.data.bool_val = bool_value;
    return hi_link_set_channel_value(channel, &value);
}

//获取字符串类型通道值
int hi_link_get_channel_string(const char *channel, char *str_value, size_t buf_size)
{
    hi_link_value_t value;
    int ret = hi_link_get_channel_value(channel, &value);
    
    if (ret == 0 && value.type == HI_LINK_TYPE_STRING) {
        if (value.data.str_val && str_value && buf_size > 0) {
            strncpy(str_value, value.data.str_val, buf_size - 1);
            str_value[buf_size - 1] = '\0';
        }
        hi_link_free_channel_value(&value);
    } else {
        ret = -1;
    }
    
    return ret;
}

//获取整数类型通道值
int hi_link_get_channel_int(const char *channel, int *int_value)
{
    hi_link_value_t value;
    int ret = hi_link_get_channel_value(channel, &value);
    
    if (ret == 0 && value.type == HI_LINK_TYPE_INT && int_value) {
        *int_value = value.data.int_val;
    } else {
        ret = -1;
    }
    
    return ret;
}

//获取浮点数类型通道值
int hi_link_get_channel_float(const char *channel, float *float_value)
{
    hi_link_value_t value;
    int ret = hi_link_get_channel_value(channel, &value);
    
    if (ret == 0 && value.type == HI_LINK_TYPE_FLOAT && float_value) {
        *float_value = value.data.float_val;
    } else {
        ret = -1;
    }
    
    return ret;
}

//获取布尔类型通道值
int hi_link_get_channel_bool(const char *channel, bool *bool_value)
{
    hi_link_value_t value;
    int ret = hi_link_get_channel_value(channel, &value);
    
    if (ret == 0 && value.type == HI_LINK_TYPE_BOOL && bool_value) {
        *bool_value = value.data.bool_val;
    } else {
        ret = -1;
    }
    
    return ret;
}

//释放通道值结构体中的内存
void hi_link_free_channel_value(hi_link_value_t *value)
{
    if (value && value->type == HI_LINK_TYPE_STRING && value->data.str_val) {
        free(value->data.str_val);
        value->data.str_val = NULL;
    }
}

 /*****************************************************************************
 *                                APP_FUNCTION                                *
 *****************************************************************************/

 /******************************************************************************
 Function    : mqtt_main
 Description : MQTT主函数，负责建立MQTT连接并处理通信
 Input  Parm : options - MQTT连接配置选项
 Output Parm : N/A
 Return      : 成功返回0，失败返回错误码
******************************************************************************/
int mqtt_main(struct Options options)
{
    PRF("mqtt_main\r\n");
    #if 1
	/* MQTT相关变量定义 */
	Network n;                    /* 网络连接对象 */
	MQTTClient c;                /* MQTT客户端对象 */
	int rc = 0;                  /* 返回码 */

	/* 缓冲区定义 */
	unsigned char buf[10240];     /* 发送缓冲区 */
	unsigned char readbuf[10240]; /* 接收缓冲区 */

	/* MQTT连接重试标签 */
	conn:
	sharedData.connect_status = 1;            // 连接状态设为2
	/* 初始化网络连接对象 */
	memset(&n, 0, sizeof(Network));
	NetworkInit(&n);
	
	PRF("options.host %s  options.port %d\r\n",options.host, options.port);

	/* 尝试连接到MQTT服务器 */
	rc = NetworkConnect(&n, options.host, options.port);
	if(rc != SUCCESS){
		/* 连接失败，断开网络连接并重试 */
		NetworkDisconnect(&n);
		PRF("NetworkConnect rc : %d\n",rc);
		app_msleep(10000);  /* 等待10秒后重试 */
		goto conn;
	}
	
	/* 初始化MQTT客户端 */
	MQTTClientInit(&c, &n, 30000, buf, sizeof(buf), readbuf, sizeof(readbuf));

	/* 配置MQTT连接参数 */
	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	data.willFlag = 1;                              /* 启用遗嘱消息 */
	data.MQTTVersion = options.MQTTVersion;         /* 设置MQTT版本 */

	/* 设置客户端连接凭证 */
	data.clientID.cstring = mqtt_connect_cret.id;           /* 客户端ID */
	data.username.cstring = mqtt_connect_cret.user_name;    /* 用户名 */
	data.password.cstring = mqtt_connect_cret.password;     /* 密码 */
	
	/* 设置保活间隔时间 */
  	if(mqtt_user_cert.keepalive == 0)
  		data.keepAliveInterval = 10;    /* 默认保活间隔10秒 */
	else
		data.keepAliveInterval = mqtt_user_cert.keepalive;  /* 使用配置的保活间隔 */
	
	/* 设置清理会话标志 */
	if(mqtt_user_cert.cleanSession == 0)
  		data.cleansession = 0;          /* 不清理会话 */
	else
  		data.cleansession = 1;          /* 清理会话 */

	/* 配置遗嘱消息 */
	data.will.message.cstring = "will message";    /* 遗嘱消息内容 */
	data.will.qos = 1;                             /* 遗嘱消息QoS级别 */
	data.will.retained = 0;                        /* 不保留遗嘱消息 */
	data.will.topicName.cstring = "will topic";   /* 遗嘱消息主题 */

	/* 尝试连接到MQTT代理 */
	rc = MQTTConnect(&c, &data);
	if (rc != SUCCESS){
		/* MQTT连接失败，重试 */
		PRF("\r\nMQTT Connect failed !\r\n");
		NetworkDisconnect(&n);
		app_msleep(1000);
		goto conn;
	}
    PRF("MQTT Connecting\r\n");

	/* 保存MQTT客户端和网络连接对象到全局IoT结构体 */
	hlk_iot.client = &c;
	hlk_iot.network = &n;
	app_msleep(1000);

	/* 调用IoT功能函数处理业务逻辑 */
	rc = hlk_iot.func(&sharedData);
	if(rc != SUCCESS){
		goto conn;  /* 业务处理失败，重新连接 */
	}
#endif
	return 0;
}

/******************************************************************************
 Function    : hlk_user_main
 Description : 用户主线程，负责MQTT连接管理和多线程协调
 Input  Parm : arg - 线程参数
 Output Parm : N/A
 Return      : 无
******************************************************************************/
#ifdef HLK_PRODUCT_WR10
static hi_void *hlk_user_main(hi_void *arg)
#else
static void *hlk_user_main(void *arg)
#endif
{
    /* 设置线程名称 */
	#ifdef HLK_PRODUCT_WR10
    hi_os_prctl_name("hlk_user_thread");
    
    hi_os_prctl_name("hlk_user_thread");
	#elif defined(HLK_PRODUCT_7628)
	//prctl(PR_SET_NAME, "hlk_user_thread");
	//MT7628 不支持 pthread_setname_np
	#else
    pthread_setname_np(pthread_self(), "hlk_user_thread");
	#endif
    /* 设置线程为分离状态，线程结束时自动回收资源 */
    pthread_detach(pthread_self());
    
	/* 等待系统初始化完成 */
	printf("hlk_user_main\r\n");
	app_msleep(U2C_INTERVAL);
	printf("hlk_user_main 2\r\n");
	
    int rc = 0;
	/* 测试函数指针数组，索引0为空，索引1为mqtt_main函数 */
	int (*tests[])() = {NULL, mqtt_main};
	int i;
	pthread_t thread[2] = {0};  /* 线程数组 */

	/* 根据连接类型设置IoT连接模式 */
	if(strncmp(options.type,"hlk", strlen("hlk")) == 0){
		hlk_iot.connnect_type = MQTT_CONNECT_HLK;      /* HLK连接类型 */
	}else{
		hlk_iot.connnect_type = MQTT_CONNECT_OTHER;    /* 其他连接类型 */
	}

	/* 初始化MQTT主模块 */
	if(hlk_mqtt_main() != 0){
		return NULL;
	}

	/* 初始化线程间共享数据结构 */
	sharedData.response_status = -1;     /* 响应状态初始化为-1 */
	sharedData.s_recv_flag = 0;          /* 服务器接收标志初始化为0 */
	sharedData.c_recv_flag = 0;          /* 客户端接收标志初始化为0 */
	sharedData.reponse_time_limmit = 5;  /* 设置响应超时时间为5秒 */
	sharedData.ptopic_packet = NULL;     /* 主题数据包指针初始化为空 */
	sharedData.connect_status = 0;       /* 连接状态初始化为0 */
	pthread_mutex_init(&sharedData.mutex, NULL);  /* 初始化互斥锁 */

	/* 创建socket处理线程 */
	if((rc = pthread_create(&thread[0], NULL, socket_main, (void *)&sharedData)) != 0) 
		PRF(" pthread_create failed !\n");
	else
		PRF(" pthread_create success !\n");

	/* 执行测试循环 */
	for (i = 0; i < options.iterations; ++i){
		if (options.test_no == 0){ 
			/* 运行所有测试 */
			for (options.test_no = 1; options.test_no < ARRAY_SIZE(tests); ++options.test_no)
				rc += tests[options.test_no](options); /* 累计失败次数，0表示测试成功 */
		}
		else
			rc = tests[options.test_no](options); /* 只运行选定的测试 */
	}

	/* 销毁互斥锁，释放资源 */
	pthread_mutex_destroy(&sharedData.mutex);
	return (void *)(intptr_t)rc;
}

/******************************************************************************
 Function    : hi_link_init
 Description : link模块初始化函数，创建通知器和用户线程
 Input Parm  : 无
 Output Parm : N/A
 Return      : HI_RET_SUCC/HI_RET_FAIL
******************************************************************************/
#ifdef HLK_PRODUCT_WR10
hi_int32 hi_link_init(void)
#else
int32_t hi_link_init(void)
#endif
{
    /* 打印应用启动信息 */
    printf("\r\n-----------------------------------------------------------------------------\r\n");
    printf("\r\n----------------------------------APP_Start----------------------------------\r\n");
    printf("\r\n-----------------------------------------------------------------------------\r\n");
    
    /* 创建U2C通知器，用于用户空间到内核空间的通信 */
	#ifdef HLK_PRODUCT_WR10
    HI_U2C_CHECK(hi_notifier_create(HI_U2C_NOTIFIER, sizeof(hi_u2c_msg_e)), "u2c notifier fail\n");
	#endif
    
    /* 创建用户主线程 */
	#ifdef HLK_PRODUCT_WR10
    HI_U2C_CHECK(hi_os_pthread_create(&g_user_pthread_id, NULL, (hi_os_threadfun_t)hlk_user_main, NULL),
                 "hlk_u2c_main create fail\n");
	#else
	
	pthread_create(&g_user_pthread_id, NULL, hlk_user_main, NULL);
	#endif
    
    /* 再次创建U2C通知器（可能是冗余代码） */
	#ifdef HLK_PRODUCT_WR10
    HI_U2C_CHECK(hi_notifier_create(HI_U2C_NOTIFIER, sizeof(hi_u2c_msg_e)), "u2c notifier fail\n");
	#endif
    
	#ifdef HLK_PRODUCT_WR10
    return HI_RET_SUCC;
	#else
	return 0;
	#endif
}

/******************************************************************************
 Function    : hi_link_exit
 Description : link模块注销函数，清理资源并销毁线程
 Input Parm  : 无
 Output Parm : N/A
 Return      : 无
******************************************************************************/
#ifdef HLK_PRODUCT_WR10
hi_void hi_link_exit(void)
#else
void hi_link_exit(void)
#endif
{
    printf("%s,%d\r\n",__func__,__LINE__);
    
    /* 如果用户线程存在，则取消并清理线程 */
    if (g_user_pthread_id != NULL)
    {
		#ifdef HLK_PRODUCT_WR10
        hi_os_pthread_join(g_user_pthread_id, NULL);
		#else
		pthread_join(g_user_pthread_id, NULL);
		#endif
        g_user_pthread_id = NULL;                 /* 清空线程ID */
    }

	#ifdef HLK_PRODUCT_WR10
    /* 销毁U2C通知器，释放通信资源 */
    HI_U2C_CHECK(hi_notifier_destroy(HI_U2C_NOTIFIER), "u2c notifier fail");
	#endif
	
    return;
}

#ifdef HLK_PRODUCT_WR10
HI_VERSION_GET(link);
#endif
