#ifndef _H_LICENSE_MANAGER_H_
#define _H_LICENSE_MANAGER_H_

#include <stddef.h>
#include <stdint.h>
#define MAGIC_HEADER 0x484C4B30  // "HLK0"

// 五元组许可证配置结构体
typedef struct {
    const char *mtd_device_path;    // MTD设备路径
    uint32_t license_offset;        // 许可证存储偏移量
    uint32_t license_size;          // 许可证存储大小
    uint32_t magic_header;          // 魔术字
} license_config_t;

// 通用函数声明
int license_set(const license_config_t *config, const char *DN_, const char *PjK_, 
                const char *PdK_, const char *PdS_, const char *DS_);
int license_clear(const license_config_t *config);
int license_get(const license_config_t *config, char *DN_, char *PjK_, 
                char *PdK_, char *PdS_, char *DS_, size_t size_);

// 内部工具函数
int base64_encode_data(const unsigned char *data, size_t input_length, char *encoded_data);
int base64_decode_data(const char *data, unsigned char *decoded_data, size_t *output_length);
void xor_encrypt_decrypt_data(const char *input, char *output, size_t length, const char *key);
uint32_t calculate_crc32(uint32_t crc, const unsigned char *buf, size_t len);
int write_mtd_device(const void *data, size_t size, uint32_t offset, const char *device_path);
int read_mtd_device(void *data, size_t size, uint32_t offset, const char *device_path);

#endif 