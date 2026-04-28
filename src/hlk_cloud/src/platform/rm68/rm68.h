#ifndef _H_HLK_PRODUCT_RM68_H_
#define _H_HLK_PRODUCT_RM68_H_ 

#include <stddef.h>

// MTD设备相关常量 - RM68 特定配置
#define MTD_DEVICE_PATH "/dev/mtd13"
#define LICENSE_OFFSET 0x21000
#define LICENSE_SIZE 0x2000

// 函数声明
extern int cfmGetLicense_RM68(char *DN_, char *PjK_, char *PdK_, char *PdS_, char *DS_, size_t size_);
extern int cfmSetLicense_RM68(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_);
extern int cfmClearLicense_RM68(void);

#endif
