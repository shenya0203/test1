#ifndef _H_HLK_PRODUCT_RM65_H_
#define _H_HLK_PRODUCT_RM65_H_ 

#include <stddef.h>

// MTD设备相关常量
#define MTD_DEVICE_PATH "/dev/mtd3"
#define LICENSE_OFFSET 0xa0000
#define LICENSE_SIZE 0x2000

// 函数声明
extern int cfmGetLicense_RM65(char *DN_, char *PjK_, char *PdK_, char *PdS_, char *DS_, size_t size_);
extern int cfmSetLicense_RM65(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_);

#endif