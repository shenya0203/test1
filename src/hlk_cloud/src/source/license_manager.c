#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <stdint.h>
#include <errno.h>
#include <mtd/mtd-user.h>
#include "license_manager.h"

// Base64编码表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// XOR加密密钥 (可以根据设备信息动态生成)
static const char xor_key[] = "HiLinkRM65Device2025";

// CRC32计算
static uint32_t crc32_table[256];
static int crc32_table_computed = 0;

static void make_crc32_table(void)
{
    uint32_t c;
    int n, k;
    
    for (n = 0; n < 256; n++) {
        c = (uint32_t) n;
        for (k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320L ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[n] = c;
    }
    crc32_table_computed = 1;
}

uint32_t calculate_crc32(uint32_t crc, const unsigned char *buf, size_t len)
{
    if (!crc32_table_computed)
        make_crc32_table();
    crc = crc ^ 0xffffffffL;
    while (len-- > 0) {
        crc = crc32_table[(crc ^ (*buf++)) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffL;
}

// Base64编码
int base64_encode_data(const unsigned char *data, size_t input_length, char *encoded_data)
{
    size_t output_length = 4 * ((input_length + 2) / 3);
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[output_length - 1 - i] = '=';
        
    encoded_data[output_length] = '\0';
    return output_length;
}

// Base64解码
int base64_decode_data(const char *data, unsigned char *decoded_data, size_t *output_length)
{
    size_t input_length = strlen(data);
    if (input_length % 4 != 0) return -1;
    
    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : strchr(base64_chars, data[i++]) - base64_chars;
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : strchr(base64_chars, data[i++]) - base64_chars;
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : strchr(base64_chars, data[i++]) - base64_chars;
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : strchr(base64_chars, data[i++]) - base64_chars;
        
        uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
        
        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    
    return 0;
}

// XOR加密/解密
void xor_encrypt_decrypt_data(const char *input, char *output, size_t length, const char *key)
{
    size_t key_len = strlen(key);
    for (size_t i = 0; i < length; i++) {
        output[i] = input[i] ^ key[i % key_len];
    }
}

// 写入MTD设备
int write_mtd_device(const void *data, size_t size, uint32_t offset, const char *device_path)
{
    int fd = open(device_path, O_RDWR);  // 改为O_RDWR，因为擦除需要读写权限
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", device_path, strerror(errno));
        return -1;
    }
    
    // 获取MTD设备信息
    mtd_info_t mtd_info;
    if (ioctl(fd, MEMGETINFO, &mtd_info) != 0) {
        syslog(LOG_ERR, "Failed to get MTD info: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    // 计算需要擦除的起始地址（对齐到擦除块边界）
    uint32_t erase_start = (offset / mtd_info.erasesize) * mtd_info.erasesize;
    // 计算需要擦除的结束地址
    uint32_t erase_end = ((offset + size + mtd_info.erasesize - 1) / mtd_info.erasesize) * mtd_info.erasesize;
    uint32_t erase_length = erase_end - erase_start;
    
    syslog(LOG_INFO, "MTD erase: start=0x%x, length=0x%x, erasesize=0x%x", 
           erase_start, erase_length, mtd_info.erasesize);
    
    // 如果偏移不在擦除块边界，需要先读取原有数据
    char *backup_buffer = NULL;
    if (offset != erase_start || size != erase_length) {
        backup_buffer = malloc(erase_length);
        if (!backup_buffer) {
            syslog(LOG_ERR, "Failed to allocate backup buffer");
            close(fd);
            return -1;
        }
        
        // 读取原有数据
        if (lseek(fd, erase_start, SEEK_SET) != (off_t)erase_start) {
            syslog(LOG_ERR, "Failed to seek for backup read: %s", strerror(errno));
            free(backup_buffer);
            close(fd);
            return -1;
        }
        
        if (read(fd, backup_buffer, erase_length) != (ssize_t)erase_length) {
            syslog(LOG_ERR, "Failed to backup data: %s", strerror(errno));
            free(backup_buffer);
            close(fd);
            return -1;
        }
        
        // 将新数据合并到备份缓冲区
        memcpy(backup_buffer + (offset - erase_start), data, size);
    }
    
    // 执行擦除操作
    erase_info_t erase_info;
    erase_info.start = erase_start;
    erase_info.length = erase_length;
    
    if (ioctl(fd, MEMERASE, &erase_info) != 0) {
        syslog(LOG_ERR, "Failed to erase MTD: %s", strerror(errno));
        if (backup_buffer) free(backup_buffer);
        close(fd);
        return -1;
    }
    
    // 写入数据
    if (lseek(fd, erase_start, SEEK_SET) != (off_t)erase_start) {
        syslog(LOG_ERR, "Failed to seek for write: %s", strerror(errno));
        if (backup_buffer) free(backup_buffer);
        close(fd);
        return -1;
    }
    
    const void *write_data = backup_buffer ? backup_buffer : data;
    size_t write_size = backup_buffer ? erase_length : size;
    
    ssize_t written = write(fd, write_data, write_size);
    if (written != (ssize_t)write_size) {
        syslog(LOG_ERR, "Failed to write data: %s", strerror(errno));
        if (backup_buffer) free(backup_buffer);
        close(fd);
        return -1;
    }
    
    if (backup_buffer) free(backup_buffer);
    close(fd);
    
    syslog(LOG_INFO, "MTD write completed successfully");
    return 0;
}

// 从MTD设备读取
int read_mtd_device(void *data, size_t size, uint32_t offset, const char *device_path)
{
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", device_path, strerror(errno));
        return -1;
    }
    
    if (lseek(fd, offset, SEEK_SET) != (off_t)offset) {
        syslog(LOG_ERR, "Failed to seek to offset 0x%x: %s", offset, strerror(errno));
        close(fd);
        return -1;
    }
    
    ssize_t bytes_read = read(fd, data, size);
    if (bytes_read != (ssize_t)size) {
        syslog(LOG_ERR, "Failed to read data: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    close(fd);
    return 0;
}

// 设置五元组数据
int license_set(const license_config_t *config, const char *DN_, const char *PjK_, 
                const char *PdK_, const char *PdS_, const char *DS_)
{
    if (!config || !DN_ || !PjK_ || !PdK_ || !PdS_ || !DS_) {
        syslog(LOG_ERR, "Invalid input parameters");
        return -1;
    }
    
    // 构建JSON格式的五元组数据
    char json_data[1024];
    int json_len = snprintf(json_data, sizeof(json_data),
        "{\"DN\":\"%s\",\"PjK\":\"%s\",\"PdK\":\"%s\",\"PdS\":\"%s\",\"DS\":\"%s\"}",
        DN_, PjK_, PdK_, PdS_, DS_);
    
    if (json_len >= sizeof(json_data)) {
        syslog(LOG_ERR, "License data too large");
        return -1;
    }
    
    // XOR加密
    char encrypted_data[1024];
    xor_encrypt_decrypt_data(json_data, encrypted_data, json_len, xor_key);
    
    // Base64编码
    char encoded_data[2048];
    int encoded_len = base64_encode_data((unsigned char*)encrypted_data, json_len, encoded_data);
    
    // 构建存储格式：[魔术字|编码长度|编码数据|CRC32]
    char *storage_buffer = malloc(config->license_size);
    if (!storage_buffer) {
        syslog(LOG_ERR, "Failed to allocate memory");
        return -1;
    }
    memset(storage_buffer, 0, config->license_size);
    
    uint32_t magic = config->magic_header;

    uint32_t data_len = encoded_len;
    uint32_t crc_value = calculate_crc32(0, (unsigned char*)encoded_data, encoded_len);
    
    // 写入魔术字
    memcpy(storage_buffer, &magic, sizeof(magic));
    // 写入数据长度
    memcpy(storage_buffer + 4, &data_len, sizeof(data_len));
    // 写入编码数据
    memcpy(storage_buffer + 8, encoded_data, encoded_len);
    // 写入CRC校验
    memcpy(storage_buffer + 8 + encoded_len, &crc_value, sizeof(crc_value));
    
    // 写入MTD设备
    int ret = write_mtd_device(storage_buffer, config->license_size, config->license_offset, config->mtd_device_path);
    free(storage_buffer);
    
    if (ret != 0) {
        syslog(LOG_ERR, "Failed to write license data to MTD");
        return -1;
    }
    
    syslog(LOG_INFO, "License data written successfully");
    return 0;
}

// 清除许可证槽位（出厂测试等）：写满 license_size 为 0，使 license_get 校验失败
int license_clear(const license_config_t *config)
{
    if (!config || config->license_size == 0) {
        syslog(LOG_ERR, "license_clear: invalid config");
        return -1;
    }

    char *storage_buffer = malloc(config->license_size);
    if (!storage_buffer) {
        syslog(LOG_ERR, "license_clear: allocation failed");
        return -1;
    }
    memset(storage_buffer, 0, config->license_size);

    int ret = write_mtd_device(storage_buffer, config->license_size,
                               config->license_offset, config->mtd_device_path);
    free(storage_buffer);

    if (ret != 0) {
        syslog(LOG_ERR, "license_clear: MTD write failed");
        return -1;
    }

    syslog(LOG_INFO, "License slot cleared successfully");
    return 0;
}

// 获取五元组数据
int license_get(const license_config_t *config, char *DN_, char *PjK_, 
                char *PdK_, char *PdS_, char *DS_, size_t size_)
{
    syslog(LOG_INFO, "license_get: config=%p, DN_=%p, PjK_=%p, PdK_=%p, PdS_=%p, DS_=%p, size_=%zu", 
           config, DN_, PjK_, PdK_, PdS_, DS_, size_);
    if (!config || !DN_ || !PjK_ || !PdK_ || !PdS_ || !DS_) {
        syslog(LOG_ERR, "Invalid input parameters");
        return -1;
    }

    // 从MTD设备读取数据
    char *storage_buffer = malloc(config->license_size);
    if (!storage_buffer) {
        syslog(LOG_ERR, "Failed to allocate memory");
        return -1;
    }
    
    if (read_mtd_device(storage_buffer, config->license_size, config->license_offset, config->mtd_device_path) != 0) {
        syslog(LOG_ERR, "Failed to read license data from MTD");
        free(storage_buffer);
        return -1;
    }
    
    // 验证魔术字
    uint32_t magic;
    memcpy(&magic, storage_buffer, sizeof(magic));
    if (magic != config->magic_header) {
        syslog(LOG_ERR, "Invalid magic header: 0x%x", magic);
        free(storage_buffer);
        return -1;
    }
    
    // 读取数据长度
    uint32_t data_len;
    memcpy(&data_len, storage_buffer + 4, sizeof(data_len));
    if (data_len > config->license_size - 12) {  // 12 = 4(magic) + 4(len) + 4(crc)
        syslog(LOG_ERR, "Invalid data length: %u", data_len);
        free(storage_buffer);
        return -1;
    }
    
    // 读取编码数据
    char encoded_data[2048];
    memcpy(encoded_data, storage_buffer + 8, data_len);
    encoded_data[data_len] = '\0';
    
    // 读取并验证CRC
    uint32_t stored_crc, calculated_crc;
    memcpy(&stored_crc, storage_buffer + 8 + data_len, sizeof(stored_crc));
    calculated_crc = calculate_crc32(0, (unsigned char*)encoded_data, data_len);
    if (stored_crc != calculated_crc) {
        syslog(LOG_ERR, "CRC verification failed");
        free(storage_buffer);
        return -1;
    }
    
    free(storage_buffer);
    
    // Base64解码
    unsigned char decoded_data[1024];
    size_t decoded_len;
    if (base64_decode_data(encoded_data, decoded_data, &decoded_len) != 0) {
        syslog(LOG_ERR, "Base64 decode failed");
        return -1;
    }
    
    // XOR解密
    char json_data[1024];
    xor_encrypt_decrypt_data((char*)decoded_data, json_data, decoded_len, xor_key);
    json_data[decoded_len] = '\0';
    
    // 解析JSON数据 (简单的字符串解析)
    char *dn_start = strstr(json_data, "\"DN\":\"");
    char *pjk_start = strstr(json_data, "\"PjK\":\"");
    char *pdk_start = strstr(json_data, "\"PdK\":\"");
    char *pds_start = strstr(json_data, "\"PdS\":\"");
    char *ds_start = strstr(json_data, "\"DS\":\"");
    
    if (!dn_start || !pjk_start || !pdk_start || !pds_start || !ds_start) {
        syslog(LOG_ERR, "Failed to parse JSON data");
        return -1;
    }
    
    // 提取各字段值
    char *end_ptr;
    
    // 提取DN
    dn_start += 6;  // 跳过 "DN":"
    end_ptr = strchr(dn_start, '"');
    if (end_ptr && (end_ptr - dn_start) < size_) {
        strncpy(DN_, dn_start, end_ptr - dn_start);
        DN_[end_ptr - dn_start] = '\0';
    } else {
        return -1;
    }
    
    // 提取PjK
    pjk_start += 7;  // 跳过 "PjK":"
    end_ptr = strchr(pjk_start, '"');
    if (end_ptr && (end_ptr - pjk_start) < size_) {
        strncpy(PjK_, pjk_start, end_ptr - pjk_start);
        PjK_[end_ptr - pjk_start] = '\0';
    } else {
        return -1;
    }
    
    // 提取PdK
    pdk_start += 7;  // 跳过 "PdK":"
    end_ptr = strchr(pdk_start, '"');
    if (end_ptr && (end_ptr - pdk_start) < size_) {
        strncpy(PdK_, pdk_start, end_ptr - pdk_start);
        PdK_[end_ptr - pdk_start] = '\0';
    } else {
        return -1;
    }
    
    // 提取PdS
    pds_start += 7;  // 跳过 "PdS":"
    end_ptr = strchr(pds_start, '"');
    if (end_ptr && (end_ptr - pds_start) < size_) {
        strncpy(PdS_, pds_start, end_ptr - pds_start);
        PdS_[end_ptr - pds_start] = '\0';
    } else {
        return -1;
    }
    
    // 提取DS
    ds_start += 6;  // 跳过 "DS":"
    end_ptr = strchr(ds_start, '"');
    if (end_ptr && (end_ptr - ds_start) < size_) {
        strncpy(DS_, ds_start, end_ptr - ds_start);
        DS_[end_ptr - ds_start] = '\0';
    } else {
        return -1;
    }
    
    syslog(LOG_INFO, "License data read successfully");
    return 0;
} 