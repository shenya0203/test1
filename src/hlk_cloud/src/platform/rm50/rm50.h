#ifndef _H_HLK_PRODUCT_RM50_H_
#define _H_HLK_PRODUCT_RM50_H_ 

#include <stddef.h>

// MTD设备相关常量 - RM50 特定配置
#define MTD_DEVICE_PATH "/dev/mtd3"
#define LICENSE_OFFSET 0x2000
#define LICENSE_SIZE 0x2000

// 函数声明
extern int cfmGetLicense_RM50(char *DN_, char *PjK_, char *PdK_, char *PdS_, char *DS_, size_t size_);
extern int cfmSetLicense_RM50(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_);
extern int cfmClearLicense_RM50(void);

#endif
