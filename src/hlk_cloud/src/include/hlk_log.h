/**
 * hlk_log.h — syslog 调试日志宏封装
 *
 * 用法概要：
 * 1. 进程启动早期调用一次：hlk_log_open("ident", LOG_DAEMON);
 * 2. 可选：hlk_log_set_upto(LOG_INFO);  仅记录 INFO 及以上
 * 3. 用 HLK_LOG_ERR HLK_LOG_WARN HLK_LOG_INFO HLK_LOG_DEBUG 替代 printf
 * 4. 退出前可选：hlk_log_close();
 *
 * 某 .c 内若需自定义标签，可在 #include 前定义：
 *   #define HLK_LOG_TAG "data_collector"
 */

#ifndef HLK_LOG_H
#define HLK_LOG_H

#include <syslog.h>

#ifndef HLK_LOG_TAG
#define HLK_LOG_TAG "hlk_cloud"
#endif

#ifndef HLK_LOG_PREFIX
#define HLK_LOG_PREFIX "[" HLK_LOG_TAG "] "
#endif

/* 定义 HLK_LOG_STRIP_DEBUG 可在编译期完全去掉 DEBUG 级别（零开销） */
#if defined(HLK_LOG_STRIP_DEBUG)
#define HLK_LOG_DEBUG(fmt, ...) ((void)0)
#else
#define HLK_LOG_DEBUG(fmt, ...) syslog(LOG_DEBUG, HLK_LOG_PREFIX fmt, ##__VA_ARGS__)
#endif

#define HLK_LOG_INFO(fmt, ...)  do { syslog(LOG_INFO,  HLK_LOG_PREFIX fmt, ##__VA_ARGS__); printf(HLK_LOG_PREFIX fmt, ##__VA_ARGS__); } while (0)
#define HLK_LOG_WARN(fmt, ...)  do { syslog(LOG_WARNING, HLK_LOG_PREFIX fmt, ##__VA_ARGS__); printf(HLK_LOG_PREFIX fmt, ##__VA_ARGS__); } while (0)
#define HLK_LOG_ERR(fmt, ...)   do { syslog(LOG_ERR,   HLK_LOG_PREFIX fmt, ##__VA_ARGS__); printf(HLK_LOG_PREFIX fmt, ##__VA_ARGS__); } while (0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册 syslog（应尽早调用，且全进程通常只需一次）。
 * @param ident  出现在日志中的程序名，可为 NULL（则使用 "hlk_cloud"）
 * @param facility 如 LOG_DAEMON、LOG_USER，与系统日志配置一致即可
 */
static inline void hlk_log_open(const char *ident, int facility)
{
    openlog(ident ? ident : "hlk_cloud", LOG_PID | LOG_CONS, facility);
}

static inline void hlk_log_close(void)
{
    closelog();
}

/**
 * 运行期过滤：只记录不大于 @p upto_pri 的消息（含该级别）。
 * 例：hlk_log_set_upto(LOG_INFO) 表示丢弃 LOG_DEBUG。
 * 需在支持 setlogmask 的 libc 上使用。
 */
static inline void hlk_log_set_upto(int upto_pri)
{
    setlogmask(LOG_UPTO(upto_pri));
}

#ifdef __cplusplus
}
#endif

#endif /* HLK_LOG_H */
