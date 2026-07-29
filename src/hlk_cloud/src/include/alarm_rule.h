/******************************************************************************
  Filename   : alarm_rule.h
  Description: 云端下发告警规则配置 - 接收与本地持久化
               仅完成 MQTT 下发到设备端的告警规则保存，
               不做评估、不做上报、不做联动触发。
               配套协议: 设备端告警规则配置协议(紧凑版)
 *****************************************************************************/

#ifndef __ALARM_RULE_H
#define __ALARM_RULE_H

#include "cJSON.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

/* 告警规则配置文件存放路径 */
#define ALARM_RULE_FILE_DIR        "/etc/config/device"
#define ALARM_RULE_FILE_PATH       ALARM_RULE_FILE_DIR "/alarm_rules.json"
#define ALARM_RULE_FILE_TMP        ALARM_RULE_FILE_DIR "/alarm_rules.json.tmp"
#define ALARM_RULE_FILE_BAK        ALARM_RULE_FILE_DIR "/alarm_rules.json.bak"
#define ALARM_REPORT_URL_FILE_PATH ALARM_RULE_FILE_DIR "/alarm_report_url.txt"

/* 下发指令 Name 字段值 */
#define ALARM_RULE_SET_NAME        "AlarmRuleSet"
#define SYNC_ALARM_RULE_NAME       "SyncAlarmRule"

/* 回复云端状态码 */
#define ALARM_RULE_RSP_OK              0   /* 成功 */
#define ALARM_RULE_RSP_MKDIR_FAIL      1   /* 创建目录失败 */
#define ALARM_RULE_RSP_WRITE_FAIL      2   /* 写入临时文件失败 */
#define ALARM_RULE_RSP_RENAME_FAIL     3   /* rename 替换失败 */
#define ALARM_RULE_RSP_INPUT_BAD       4   /* InputData 不是合法 JSON */
#define ALARM_RULE_RSP_FORMAT_BAD      5   /* rs 字段缺失或不是数组 */
#define ALARM_RULE_RSP_OTHER           6   /* 其它内部错误 */

/*****************************************************************************
 * 函数名    : hlk_mqtt_handle_alarm_rule_set
 * 功能描述  : 在 hlk_mqtt_handle_set 中被调用，处理 Name=AlarmRuleSet 下发
 * 输入参数  : root - 已解析的外层 cJSON 根对象(含 Name/Id/InputData 等字段)
 *             id  - 外层 Id 字段字符串(用于回复), 可为 NULL
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : 1) 从 root 中取 InputData 字符串做二次 cJSON_Parse
 *             2) 校验顶层 v 与 rs[]
 *             3) 原子落盘到 /etc/config/device/alarm_rules.json
 *             4) 通过 TOPIC_GET_REPLY 回复云端处理结果
 *****************************************************************************/
int hlk_mqtt_handle_alarm_rule_set(cJSON *root, const char *id);

/*****************************************************************************
 * 函数名    : hlk_mqtt_handle_sync_alarm_rule
 * 功能描述  : 在 hlk_mqtt_handle_set 中被调用，处理 Name=SyncAlarmRule 下发
 * 输入参数  : root - 已解析的外层 cJSON 根对象(含 Name/Id/InputData 等字段)
 *             id  - 外层 Id 字段字符串(用于回复), 可为 NULL
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : 1) 从 root 中取 InputData 做 cJSON_Parse，提取 RuleUrl/ReportUrl
 *             2) 通过 TOPIC_GET_REPLY 立即回复云端收到指令
 *             3) 若 RuleUrl 不为空，HTTP GET 下载规则正文并保存到 alarm_rules.json
 *             4) 若 ReportUrl 不为空，保存上报地址到 alarm_report_url.txt
 *             5) 若 RuleUrl/ReportUrl 均为空，则清理本地告警配置
 *****************************************************************************/
int hlk_mqtt_handle_sync_alarm_rule(cJSON *root, const char *id);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __ALARM_RULE_H */
