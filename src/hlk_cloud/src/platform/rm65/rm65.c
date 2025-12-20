#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "rm65.h"
#include "license_manager.h"

// RM65设备特定配置
static const license_config_t rm65_license_config = {
    .mtd_device_path = MTD_DEVICE_PATH,
    .license_offset = LICENSE_OFFSET,
    .license_size = LICENSE_SIZE,
    .magic_header = MAGIC_HEADER
};

// 设置五元组数据
int cfmSetLicense_RM65(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_)
{
    return license_set(&rm65_license_config, DN_, PjK_, PdK_, PdS_, DS_);
}

// 获取五元组数据
int cfmGetLicense_RM65(char *DN_, char *PjK_, char *PdK_, char *PdS_, char *DS_, size_t size_)
{
    return license_get(&rm65_license_config, DN_, PjK_, PdK_, PdS_, DS_, size_);
}

#if 0
int cfmGetLicense_RM65(char *DN_,char *PjK_,char *PdK_,char *PdS_,char *DS_, size_t size_)
{
    int ret = 0;

    if (!DN_ || !PjK_ || !PdK_ || !PdS_ || !DS_) {
        syslog(LOG_DEBUG, "Err input value is NULL\r\n");
        return -1;
    }

    //RM65 获取五元组
    //RM65 五元组存放位置在 校准分区内的便宜位置：0x10000 ~ 0x12000
    //判断是否有存放五元组？
    char *deviceName = "snEWRFHI00A";
    char *projectKey = "zzDQTWD40000000L";
    char *productKey = "zeTDUXO100J";
    char *productSecret = "TnR26oVE7TeOAJxs";
    char *deviceSecret = "wvEOoLYkgyEJjsBk";

    //测试
    // 将获取到的五元组信息复制到全局认证结构体中
    strcpy(DN_, deviceName );
    strcpy(PjK_, projectKey);
    strcpy(PdK_, productKey);
    strcpy(PdS_, productSecret);
    strcpy(DS_, deviceSecret);

#if 1
    printf("DN_:<%s>\r\n",DN_);
    printf("PjK_:<%s>\r\n",PjK_);
    printf("PdK_:<%s>\r\n",PdK_);
    printf("PdS_:<%s>\r\n",PdS_);
    printf("DS_:<%s>\r\n",DS_);
#endif

exit_:
    return ret;
}
#endif