/******************************************************************************
  Filename   : alarm_rule.c
  Description: 云端下发告警规则配置的实现 - 仅保存到本地文件
               严格仿照 hlk_mqtt_handle_set 中的 SyncFlow 处理范式:
                 二次 Parse InputData -> 校验 -> 落盘 -> 回 TOPIC_GET_REPLY
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <curl/curl.h>

#include "app_api.h"
#include "alarm_rule.h"
#include "cJSON.h"
#include "hlk_log.h"
#include "hi_mqtt.h"

/*****************************************************************************
 * 内部函数: 向云端回复处理结果
 * 与 hlk_syncflow_response 风格一致
 *   status=0 -> Data="OK"
 *   status!=0 -> Data="Fail"
 *****************************************************************************/
static int hlk_alarm_rule_response(int status, const char *id)
{
    cJSON *root = NULL;
    char *json_str = NULL;

    if (id == NULL) {
        HLK_LOG_ERR("[AlarmRule] response aborted: id is NULL\n");
        return -1;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        HLK_LOG_ERR("[AlarmRule] cJSON_CreateObject error\n");
        return -1;
    }

    cJSON_AddStringToObject(root, "ID", id);
    cJSON_AddNumberToObject(root, "Status", status);
    cJSON_AddStringToObject(root, "Data", status == 0 ? "OK" : "Fail");

    json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        HLK_LOG_ERR("[AlarmRule] cJSON_PrintUnformatted error\n");
        cJSON_Delete(root);
        return -1;
    }

    hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_GET_REPLY].topic, QOS0,
                     json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return 0;
}

/*****************************************************************************
 * 内部函数: 将 cJSON 规则对象原子写入 /etc/config/device/alarm_rules.json
 * 流程:
 *   1) mkdir -p /etc/config/device
 *   2) 序列化 cJSON -> 字符串
 *   3) 写 .tmp 临时文件
 *   4) rename .tmp -> 正式文件 (原子替换)
 *   5) 失败任何一步均回滚并返回对应状态码
 * 始终保留一份 .bak 上次成功文件，方便人工回滚
 *****************************************************************************/
static int alarm_rule_write_file(cJSON *rule_root)
{
    int    ret = ALARM_RULE_RSP_OTHER;
    char  *json_str = NULL;
    FILE  *fp = NULL;
    size_t written = 0;
    struct stat st_info;

    printf("[AlarmRule] write file\n");
    /* 1) 目录准备 */
    if (zig_file_stat(ALARM_RULE_FILE_DIR, &st_info) != 0) {
        printf("[AlarmRule] %s not exist, creating...\n", ALARM_RULE_FILE_DIR);
        if (mkdir(ALARM_RULE_FILE_DIR, 0755) != 0) {
            HLK_LOG_ERR("[AlarmRule] mkdir %s failed: %s\n",
                        ALARM_RULE_FILE_DIR, strerror(errno));
            printf("[AlarmRule] mkdir %s failed: %s\n",
                   ALARM_RULE_FILE_DIR, strerror(errno));
            return ALARM_RULE_RSP_MKDIR_FAIL;
        }
    } else if (!S_ISDIR(st_info.st_mode)) {
        HLK_LOG_ERR("[AlarmRule] %s exists but not a dir\n", ALARM_RULE_FILE_DIR);
        printf("[AlarmRule] %s exists but not a dir\n", ALARM_RULE_FILE_DIR);
        return ALARM_RULE_RSP_MKDIR_FAIL;
    }
    printf("[AlarmRule] mkdir %s success\n", ALARM_RULE_FILE_DIR);

    /* 2) 序列化: 使用格式化输出版本，方便人工 diff 调试 */
    json_str = cJSON_Print(rule_root);
    if (json_str == NULL) {
        printf("[AlarmRule] cJSON_Print error\n");
        HLK_LOG_ERR("[AlarmRule] cJSON_Print error\n");
        return ALARM_RULE_RSP_WRITE_FAIL;
    }
    printf("[AlarmRule] json_str:\n%s\n", json_str);

    /* 3) 写临时文件 */
    fp = fopen(ALARM_RULE_FILE_TMP, "w");
    if (fp == NULL) {
        HLK_LOG_ERR("[AlarmRule] fopen %s failed: %s\n",
                    ALARM_RULE_FILE_TMP, strerror(errno));
        free(json_str);
        printf("[AlarmRule] fopen %s failed: %s\n",
               ALARM_RULE_FILE_TMP, strerror(errno));
        return ALARM_RULE_RSP_WRITE_FAIL;
    }
    printf("[AlarmRule] fp: %p\n", fp);

    written = fwrite(json_str, 1, strlen(json_str), fp);
    if (written != strlen(json_str)) {
        HLK_LOG_ERR("[AlarmRule] fwrite short: %zu/%zu\n",
                    written, strlen(json_str));
        fclose(fp);
        free(json_str);
        unlink(ALARM_RULE_FILE_TMP);
        printf("[AlarmRule] fwrite short: %zu/%zu\n",
               written, strlen(json_str));
        return ALARM_RULE_RSP_WRITE_FAIL;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(json_str);
    printf("[AlarmRule] write tmp file %s success\n", ALARM_RULE_FILE_TMP);

    /* 4) 先备份原文件 (若有) 再原子替换 */
    if (zig_file_stat(ALARM_RULE_FILE_PATH, &st_info) == 0) {
        unlink(ALARM_RULE_FILE_BAK);
        if (rename(ALARM_RULE_FILE_PATH, ALARM_RULE_FILE_BAK) != 0) {
            HLK_LOG_WARN("[AlarmRule] backup old file failed: %s\n", strerror(errno));
            /* 备份失败不致命，继续推进 */
        }
    }
    printf("[AlarmRule] backup old file %s success\n", ALARM_RULE_FILE_BAK);

    if (rename(ALARM_RULE_FILE_TMP, ALARM_RULE_FILE_PATH) != 0) {
        HLK_LOG_ERR("[AlarmRule] rename %s -> %s failed: %s\n",
                    ALARM_RULE_FILE_TMP, ALARM_RULE_FILE_PATH, strerror(errno));
        /* 临时文件残留，下次写入会覆盖 */
        return ALARM_RULE_RSP_RENAME_FAIL;
    }
    printf("[AlarmRule] rename %s -> %s success\n",
           ALARM_RULE_FILE_TMP, ALARM_RULE_FILE_PATH);

    HLK_LOG_INFO("[AlarmRule] save success -> %s\n", ALARM_RULE_FILE_PATH);
    return ALARM_RULE_RSP_OK;
}

/*****************************************************************************
 * 对外接口: hlk_mqtt_handle_alarm_rule_set
 * 由 hi_mqtt.c::hlk_mqtt_handle_set 中 Name=="AlarmRuleSet" 分支调用
 * root 已是外层 cJSON* (含 Name/Id/InputData 等)
 *****************************************************************************/
int hlk_mqtt_handle_alarm_rule_set(cJSON *root, const char *id)
{
    cJSON *inputData = NULL;
    cJSON *inner = NULL;
    cJSON *rs = NULL;
    int    status = ALARM_RULE_RSP_OTHER;

    if (root == NULL) {
        HLK_LOG_ERR("[AlarmRule] root is NULL\n");
        return -1;
    }

    /* 取外层 InputData 字符串 */
    inputData = cJSON_GetObjectItem(root, "InputData");
    if (!cJSON_IsString(inputData) || inputData->valuestring == NULL) {
        HLK_LOG_ERR("[AlarmRule] InputData not string\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        return -1;
    }

    /* 二次 Parse InputData -> 内层紧凑协议 JSON */
    inner = cJSON_Parse(inputData->valuestring);
    if (inner == NULL) {
        HLK_LOG_ERR("[AlarmRule] parse InputData fail\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        return -1;
    }

    /* 校验: 顶层 rs 字段必须存在且为数组; v 字段可选 */
    rs = cJSON_GetObjectItem(inner, "rs");
    if (!cJSON_IsArray(rs)) {
        HLK_LOG_ERR("[AlarmRule] rs missing or not array\n");
        cJSON_Delete(inner);
        hlk_alarm_rule_response(ALARM_RULE_RSP_FORMAT_BAD, id);
        return -1;
    }

    /* 附加本地元数据: 保存时间戳 (带下划线前缀, 与云端下发字段不冲突) */
    if (cJSON_GetObjectItem(inner, "_saved_at") == NULL) {
        cJSON_AddNumberToObject(inner, "_saved_at", zig_get_timestamp());
    } else {
        cJSON_ReplaceItemInObject(inner, "_saved_at",
                                  cJSON_CreateNumber(zig_get_timestamp()));
    }

    /* 落盘 */
    status = alarm_rule_write_file(inner);
    cJSON_Delete(inner);

    /* 回复云端 */
    hlk_alarm_rule_response(status, id);

    if (status != ALARM_RULE_RSP_OK) {
        HLK_LOG_ERR("[AlarmRule] handle fail status=%d\n", status);
        return -1;
    }
    //通知edge进程 配置更新
    HLK_LOG_INFO("[AlarmRule] restart edge process");
    system("/etc/init.d/edge restart");
    
    return 0;
}

/* 内部结构: libcurl 内存写入回调 */
struct AlarmRuleMemoryStruct {
    char  *memory;
    size_t size;
};

static size_t alarm_rule_curl_write_cb(void *ptr, size_t size, size_t nmemb, void *stream)
{
    size_t realsize = size * nmemb;
    struct AlarmRuleMemoryStruct *mem = (struct AlarmRuleMemoryStruct *)stream;

    char *ptr_realloc = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr_realloc == NULL) {
        HLK_LOG_ERR("[AlarmRule] realloc memory failed\n");
        return 0;
    }

    mem->memory = ptr_realloc;
    memcpy(&(mem->memory[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

/*****************************************************************************
 * 内部函数: 通过 HTTP POST 获取规则正文字符串 (SHA1 签名)
 * 签名方式与 query_request_address 一致
 * 返回动态分配的字符串 (需 caller free)，失败返回 NULL
 *****************************************************************************/
static char* alarm_rule_http_get(const char *url_in)
{
    CURL *curl = NULL;
    CURLcode res;
    struct AlarmRuleMemoryStruct chunk = {0};
    char url[512] = {0};
    struct curl_slist *headers = NULL;

    /* SHA1 签名相关 */
    char time_str[16] = {0};
    char nonce[8] = {0};
    char signature[70] = {0};
    char post_body[128] = {0};

    if (url_in == NULL || strlen(url_in) == 0) {
        return NULL;
    }

    snprintf(url, sizeof(url), "%s", url_in);
    replace_https_with_http(url);

    /* 1. 生成毫秒级时间戳 */
    time_t time_now = get_system_timestamp();
    if (time_now == 0) {
        HLK_LOG_ERR("[AlarmRule] system time invalid, try sync\n");
        if (sync_system_time() != 0) {
            HLK_LOG_ERR("[AlarmRule] sync_system_time failed\n");
            return NULL;
        }
        time_now = get_system_timestamp();
        if (time_now == 0) {
            HLK_LOG_ERR("[AlarmRule] system time still invalid\n");
            return NULL;
        }
    }
    snprintf(time_str, sizeof(time_str), "%lld000", (long long)time_now);

    /* 2. 生成 nonce (%.5ld5 + rand() % 9999) */
    srand((unsigned int)time_now);
    snprintf(nonce, sizeof(nonce), "%.5ld5", rand() % 9999);

    /* 3. SHA1 签名 */
    hlk_get_signature(time_str, ALARM_RULE_TOKEN, nonce, signature);

    /* 4. 构造 POST body (仅传 deviceCode) */
    snprintf(post_body, sizeof(post_body), "Sn=%s", mqtt_user_cert.deviceName);

    HLK_LOG_INFO("[AlarmRule] POST url=%s time=%s nonce=%s signature=%s body=%s\n",
                 url, time_str, nonce, signature, post_body);

    /* 5. 初始化 curl */
    curl = curl_easy_init();
    if (!curl) {
        HLK_LOG_ERR("[AlarmRule] curl_easy_init failed\n");
        return NULL;
    }

    /* 6. 构造 HTTP 头 */
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    {
        char accessid_header[64];
        char nonce_header[32];
        char signature_header[128];
        char timestamp_header[64];
        char content_length_header[32];
        snprintf(accessid_header, sizeof(accessid_header), "AccessId: %s", ALARM_RULE_ACCESSID);
        snprintf(nonce_header, sizeof(nonce_header), "Nonce: %s", nonce);
        snprintf(signature_header, sizeof(signature_header), "Signature: %s", signature);
        snprintf(timestamp_header, sizeof(timestamp_header), "Timestamp: %s", time_str);
        snprintf(content_length_header, sizeof(content_length_header), "Content-Length: %d", (int)strlen(post_body));
        headers = curl_slist_append(headers, accessid_header);
        headers = curl_slist_append(headers, nonce_header);
        headers = curl_slist_append(headers, signature_header);
        headers = curl_slist_append(headers, timestamp_header);
        headers = curl_slist_append(headers, content_length_header);
    }

    /* 7. 设置 curl 选项 */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(post_body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, alarm_rule_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);          /* 15秒超时 */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);   /* 5秒连接超时 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);    /* 忽略SSL校验 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    /* 8. 执行请求 */
    res = curl_easy_perform(curl);

    /* 9. 打印 HTTP 响应状态码 */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    HLK_LOG_INFO("[AlarmRule] http_post response code=%ld url=%s\n", http_code, url);

    /* 10. 释放资源 */
    if (headers) {
        curl_slist_free_all(headers);
        headers = NULL;
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        HLK_LOG_ERR("[AlarmRule] http post failed: %s (url: %s)\n", curl_easy_strerror(res), url);
        if (chunk.memory) free(chunk.memory);
        return NULL;
    }

    return chunk.memory;
}

/*****************************************************************************
 * 内部函数: 保存 ReportUrl 到本地文件
 *****************************************************************************/
static int alarm_rule_save_report_url(const char *report_url)
{
    FILE *fp = fopen(ALARM_REPORT_URL_FILE_PATH, "w");
    if (!fp) {
        HLK_LOG_ERR("[AlarmRule] save report url open failed: %s\n", strerror(errno));
        return -1;
    }
    if (report_url && strlen(report_url) > 0) {
        fputs(report_url, fp);
    }
    fclose(fp);
    HLK_LOG_INFO("[AlarmRule] report url saved -> %s\n", ALARM_REPORT_URL_FILE_PATH);
    return 0;
}

/*****************************************************************************
 * 内部函数: alarm_rule_sync_urls
 * 公共核心逻辑 - 给定 RuleUrl 和 ReportUrl，完成:
 *   1) 立即回复 MQTT 确认
 *   2) 两个 URL 都为空 -> 清理本地告警配置
 *   3) ReportUrl 不为空 -> 保存上报地址
 *   4) RuleUrl 不为空   -> HTTP GET 下载规则正文 -> 校验 rs 数组 -> 原子落盘
 * 返回值: 0-成功 -1-失败
 *****************************************************************************/
static int alarm_rule_sync_urls(const char *rule_url, const char *report_url, const char *id)
{
    char  *http_body = NULL;
    cJSON *downloaded_rules = NULL;
    cJSON *rs = NULL;
    int    status = ALARM_RULE_RSP_OK;

    /* 1. 立即回复 MQTT 确认，告知云端已成功收到指令 */
    hlk_alarm_rule_response(ALARM_RULE_RSP_OK, id);

    /* 2. 若 RuleUrl 与 ReportUrl 均为空，视为云端未启用告警，清理本地规则与上报地址 */
    if ((!rule_url || strlen(rule_url) == 0) && (!report_url || strlen(report_url) == 0)) {
        HLK_LOG_INFO("[AlarmRule] both URLs empty, disable alarm rules\n");
        unlink(ALARM_RULE_FILE_PATH);
        unlink(ALARM_REPORT_URL_FILE_PATH);
        return 0;
    }

    /* 3. 保存 ReportUrl */
    if (report_url) {
        alarm_rule_save_report_url(report_url);
    }

    /* 4. 若 RuleUrl 不为空，通过 HTTP GET 下载告警规则正文 */
    if (rule_url && strlen(rule_url) > 0) {
        http_body = alarm_rule_http_get(rule_url);
        if (http_body == NULL) {
            HLK_LOG_ERR("[AlarmRule] download alarm rules from RuleUrl failed\n");
            return -1;
        }

        /* 调试用：打印实际下载到的内容（最多 1024 字节） */
        HLK_LOG_INFO("[AlarmRule] http_get body_len=%zu, first 1024 bytes:\n%.1024s\n",
                     strlen(http_body), http_body);

        printf("[AlarmRule] downloaded rules:\n");
        /* 5. 解析下载到的规则 JSON 并校验 rs 数组 */
        downloaded_rules = cJSON_Parse(http_body);
        free(http_body);
        printf("[AlarmRule] downloaded rules parsed\n");

        if (downloaded_rules == NULL) {
            HLK_LOG_ERR("[AlarmRule] downloaded rules content is not valid JSON\n");
            return -1;
        }
        printf("[AlarmRule] downloaded rules checked\n");

        /* 检查响应状态码 */
        cJSON *code_obj = cJSON_GetObjectItem(downloaded_rules, "Code");
        if (cJSON_IsNumber(code_obj) && code_obj->valueint != 1) {
            HLK_LOG_ERR("[AlarmRule] API returned error code %d\n", code_obj->valueint);
            cJSON_Delete(downloaded_rules);
            return -1;
        }
        printf("[AlarmRule] downloaded rules code check passed\n");

        /* 云端返回格式可能有 Data 封装层: {"Code":1, "Data": {"rs": [...]}} */
        cJSON *rules_obj = downloaded_rules;
        cJSON *data_obj = cJSON_GetObjectItem(downloaded_rules, "Data");
        if (data_obj && cJSON_IsObject(data_obj)) {
            rules_obj = data_obj;
        }
        printf("[AlarmRule] downloaded rules Data object check passed\n");

        rs = cJSON_GetObjectItem(rules_obj, "rs");
        if (!cJSON_IsArray(rs)) {
            HLK_LOG_ERR("[AlarmRule] downloaded rules missing 'rs' array\n");
            char *debug_str = cJSON_Print(downloaded_rules);
            if (debug_str) {
                HLK_LOG_ERR("[AlarmRule] full response: %s\n", debug_str);
                free(debug_str);
            }
            cJSON_Delete(downloaded_rules);
            return -1;
        }
        printf("[AlarmRule] downloaded rules rs array check passed\n");

        /* 6. 附加保存时间戳并原子落盘 */
        if (cJSON_GetObjectItem(downloaded_rules, "_saved_at") == NULL) {
            printf("[AlarmRule] adding _saved_at timestamp\n");
            cJSON_AddNumberToObject(downloaded_rules, "_saved_at", zig_get_timestamp());
            printf("[AlarmRule] _saved_at timestamp added\n");
        } else {
            printf("[AlarmRule] replacing _saved_at timestamp\n");
            cJSON_ReplaceItemInObject(downloaded_rules, "_saved_at",
                                      cJSON_CreateNumber(zig_get_timestamp()));
        }
        printf("[AlarmRule] downloaded rules saved\n");

        status = alarm_rule_write_file(downloaded_rules);
        printf("[AlarmRule] downloaded rules write_file status=%d\n", status);
        cJSON_Delete(downloaded_rules);
        printf("[AlarmRule] downloaded rules deleted\n");

        if (status == ALARM_RULE_RSP_OK) {
            HLK_LOG_INFO("[AlarmRule] AlarmPush download and save success!\n");
        }
        printf("[AlarmRule] downloaded rules final status=%d\n", status);
    }

    return (status == ALARM_RULE_RSP_OK) ? 0 : -1;
}

/*****************************************************************************
 * 对外接口: hlk_mqtt_handle_alarm_push
 * 由 hi_mqtt.c::hlk_mqtt_handle_set 中 Name=="AlarmPush" 分支调用
 * 协议格式(三层嵌套):
 *   InputData = {"AlarmType":99,"AlarmInfo":"{\"RuleUrl\":\"...\",\"ReportUrl\":\"...\",\"Type\":\"SyncAlarmRule\"}","AlarmTime":"..."}
 *   AlarmInfo 是二次转义的 JSON 字符串，需二次 cJSON_Parse
 *****************************************************************************/
int hlk_mqtt_handle_alarm_push(cJSON *root, const char *id)
{
    cJSON *inputData = NULL;
    cJSON *in = NULL;
    cJSON *alarmTypeObj = NULL;
    cJSON *alarmInfoObj = NULL;
    cJSON *alarmTimeObj = NULL;
    cJSON *inInfo = NULL;
    cJSON *ruleUrlObj = NULL;
    cJSON *reportUrlObj = NULL;
    char  *rule_url = NULL;
    char  *report_url = NULL;

    if (root == NULL) {
        HLK_LOG_ERR("[AlarmRule] root is NULL\n");
        return -1;
    }

    /* 1. 取外层 InputData 字符串 */
    inputData = cJSON_GetObjectItem(root, "InputData");
    if (!cJSON_IsString(inputData) || inputData->valuestring == NULL) {
        HLK_LOG_ERR("[AlarmRule] InputData not string\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        return -1;
    }

    /* 2. 解析 InputData JSON: {"AlarmType":99,"AlarmInfo":"{...}","AlarmTime":"..."} */
    in = cJSON_Parse(inputData->valuestring);
    if (in == NULL) {
        HLK_LOG_ERR("[AlarmRule] parse InputData json fail\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        return -1;
    }

    /* 3. 取 AlarmType / AlarmInfo / AlarmTime (仅做日志，不参与逻辑) */
    alarmTypeObj = cJSON_GetObjectItem(in, "AlarmType");
    alarmTimeObj = cJSON_GetObjectItem(in, "AlarmTime");
    HLK_LOG_INFO("[AlarmRule] AlarmPush AlarmType=%d AlarmTime=%s\n",
                 cJSON_IsNumber(alarmTypeObj) ? alarmTypeObj->valueint : -1,
                 (cJSON_IsString(alarmTimeObj) && alarmTimeObj->valuestring) ? alarmTimeObj->valuestring : "null");

    /* 4. 取 AlarmInfo 字符串并二次 cJSON_Parse 得到 {"RuleUrl":...,"ReportUrl":...,"Type":...} */
    alarmInfoObj = cJSON_GetObjectItem(in, "AlarmInfo");
    if (!cJSON_IsString(alarmInfoObj) || alarmInfoObj->valuestring == NULL) {
        HLK_LOG_ERR("[AlarmRule] AlarmInfo not string\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        cJSON_Delete(in);
        return -1;
    }

    /* 调试用：打印 AlarmInfo 原始字符串 */
    HLK_LOG_INFO("[AlarmRule] AlarmInfo raw: %s\n", alarmInfoObj->valuestring);

    inInfo = cJSON_Parse(alarmInfoObj->valuestring);
    if (inInfo == NULL) {
        HLK_LOG_ERR("[AlarmRule] parse AlarmInfo json fail\n");
        hlk_alarm_rule_response(ALARM_RULE_RSP_INPUT_BAD, id);
        cJSON_Delete(in);
        return -1;
    }

    /* 5. 取 RuleUrl / ReportUrl (Type 字段不做判断，收到 AlarmPush 即视为告警规则同步) */
    ruleUrlObj   = cJSON_GetObjectItem(inInfo, "RuleUrl");
    reportUrlObj = cJSON_GetObjectItem(inInfo, "ReportUrl");

    if (cJSON_IsString(ruleUrlObj) && ruleUrlObj->valuestring) {
        rule_url = ruleUrlObj->valuestring;
    }
    if (cJSON_IsString(reportUrlObj) && reportUrlObj->valuestring) {
        report_url = reportUrlObj->valuestring;
    }

    HLK_LOG_INFO("[AlarmRule] AlarmPush RuleUrl=%s, ReportUrl=%s\n",
                 rule_url ? rule_url : "null", report_url ? report_url : "null");

    /* 6. 调用公共核心逻辑 */
    alarm_rule_sync_urls(rule_url, report_url, id);

    cJSON_Delete(inInfo);
    cJSON_Delete(in);
    return 0;
}
