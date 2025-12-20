#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "rm68.h"
#include "license_manager.h"

// RM68设备特定配置
static const license_config_t rm68_license_config = {
    .mtd_device_path = MTD_DEVICE_PATH,
    .license_offset = LICENSE_OFFSET,
    .license_size = LICENSE_SIZE,
    .magic_header = MAGIC_HEADER
};

// 设置五元组数据
int cfmSetLicense_RM68(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_)
{
    return license_set(&rm68_license_config, DN_, PjK_, PdK_, PdS_, DS_);
}

// 获取五元组数据
int cfmGetLicense_RM68(char *DN_, char *PjK_, char *PdK_, char *PdS_, char *DS_, size_t size_)
{
    return license_get(&rm68_license_config, DN_, PjK_, PdK_, PdS_, DS_, size_);
}
