# SIM 流量次月处理方案分析

> 文档整理自代码审查对话，涉及文件：`src/hlk_cloud/src/source/hi_mqtt.c`  
> 日期：2026-06-17

---

## 一、背景与业务模型

### 产品套餐

- 8 年合约，每月 100M
- 月超停机，次月解锁（软件侧也需解锁）
- 当前仅使用月套餐（`FlowType == 0`），年套餐暂未上线

### 云端协议约定

- 跨月时**不要求**补报上月流量，上月由云端结算
- 跨月边界最多导致约 15~30 分钟（上报周期）的流量归到新月份，可接受
- 设备时间由 OpenWrt NTP 同步

---

## 二、实现方案解读

### 核心状态

| 状态来源 | 含义 |
|---------|------|
| `sharedData.current_month_flow` | 云端心跳下发的当月流量快照（KB） |
| `sharedData.current_year_flow` | 云端心跳下发的当年流量快照（KB） |
| `sharedData.current_month / current_flow_year` | 快照所属月/年 |
| `g_sim_traffic_state.total_rx/tx_bytes` | 设备本地自上次确认以来的增量（字节） |

上报公式：

```
total_bytes = current_month_flow + (total_rx_bytes + total_tx_bytes) / 1024
```

云端通过 `FLOW_UPDATE` 主题的 `T`（UTC 毫秒时间戳）判断流量归档到哪一月。

### 流程概览

```
每分钟 sim_traffic_process 采样
    ↓
sim_traffic_next_month_proc() 比较云端快照月/年 vs 设备本地时间
    ↓
┌─ 周期上报发现跨月 → hlk_mqtt_ping(1)，不上报 FLOW_UPDATE
│
└─ ping reply 收到套餐数据
       ↓
   若跨月：清零月流量、更新本地月/年、isnextmonth=1、解除停机
       ↓
   FLOW_UPDATE 上报（T=当前时间, F=0+本地增量）
       ↓
   flow/post_reply 确认
       ↓
   若 isnextmonth==1 → 再 ping 一次拉取新套餐快照
   否则 → clear_state + 判断是否再次停机
```

### 1. 跨月检测：`sim_traffic_next_month_proc`

```c
int sim_traffic_next_month_proc(void)
{
    int current_month = zig_get_month();
    int current_year = zig_get_year();

    if (sharedData.current_month != current_month) {
        return 1;  // 次月
    } else if (sharedData.current_flow_year != current_year) {
        return 2;  // 次年
    }
    return 0;
}
```

- 云端快照月/年与设备本地时间（`localtime`，CST-8）不一致 → 判定跨期
- 次月返回 `1`，次年返回 `2`（已修复）

### 2. 周期上报：`sim_traffic_report`

跨月时只触发 `hlk_mqtt_ping(1)`，不上报流量，等云端 ping reply 同步后再继续。

### 3. 心跳回复：`hlk_mqtt_handle_ping_reply`

关键顺序（约 1895-1924 行）：

1. 先从云端赋值 `current_month_flow`、`current_year_flow`、`current_flow_year`、`current_month`
2. 再调用 `sim_traffic_next_month_proc()` 判断
3. 若跨月/跨年：
   - 清零 `current_month_flow`
   - 若 `is_next == 2`，清零 `current_year_flow`
   - 更新本地月/年为设备当前时间
   - 设置 `isnextmonth = 1`
   - `sim_traffic_clear_block()` 解除停机
   - 调用 `sim_traffic_process(is_next)` 立即触发上报

### 4. 流量确认：`hlk_mqtt_handle_flow_update_confirm`

- `isnextmonth == 1` 时：清零标志后再 `hlk_mqtt_ping(1)` 拉取云端更新后的套餐快照
- 否则：正常 `sim_traffic_clear_state()` + 根据流量判断是否再次停机

---

## 三、问题分析与结论

### 已排除的问题

#### `current_month = -1` 首次启动误触发

**结论：不成立。**

ping reply 处理时，先赋值云端快照（约 1895-1899 行），再调用 `sim_traffic_next_month_proc()`。比较的是云端月/年 vs 设备本地月/年，`-1` 初始值不参与判断。

此外 `flowtype == -1` 时 `sim_traffic_process` 直接 return，首次 ping 回复前不会走周期上报。

#### `return 2` 死代码

**结论：已修复。** 次年分支可正常清零 `current_year_flow`。

### 可接受的设计

| 项目 | 说明 |
|------|------|
| 跨月不补报上月流量 | 符合云端协议，最多 30 分钟误差 |
| 12月→1月走次月而非次年 | 对月套餐正确，按次月解锁处理 |
| 次月强制解锁 | 符合「月超停机、次月解锁」业务 |

### 仍需关注的风险

#### 中风险：NTP 未就绪的启动竞态

MQTT 连接/首次 ping 可能早于 NTP 同步。若云端快照 6 月、设备本地仍 5 月，会误触发次月流程（提前解锁、按新月份上报）。

**建议确认：** hlk_cloud 是否等 NTP 同步后再连 MQTT；或增加「时间未同步则跳过跨月判断」。

#### 中风险：停机状态跨月只依赖 ping reply

`isblocked == 1` 时 `sim_traffic_process` 直接 return，跨月检测仅在 ping reply 中执行。

停机时每 600 秒 `hlk_mqtt_ping(1)` 带 IMEI，需确认云端 reply **是否一定携带** `FlowType/CurrentMonthFlow/...` 等套餐字段，否则跨月解锁不会发生。

#### 低风险

- **12月→1月不清 `current_year_flow`**：对 flowtype=0 无实质影响
- **断线重连 `current_month = 0`**：下次 ping reply 会重新赋值，正常路径无问题
- **空 `is_next == 2` 分支**（约 1920-1922 行）：建议删除或补逻辑
- **JSON 字段名**：ping reply 用 `CurrentMonth`，flow confirm 用 `CurrentFlowMonth`，需与云端确认一致
- **日志字段印反**（约 931-932 行）：仅影响排障

### 未来待做：年套餐（flowtype==1）

当前未上线，但现有逻辑存在问题：每月换月都会触发次月分支并无条件解锁，年套餐用户可能被每月错误解锁。上线前需区分：

- `flowtype==0`：按月检测跨月
- `flowtype==1`：按年检测跨年

---

## 四、待澄清事项（可选）

1. hlk_cloud 与 NTP 的启动顺序是否保证「NTP 同步后才首次 ping」？
2. 停机状态 ping reply 是否仍返回完整套餐字段？
3. 8 年合约在协议里 `FlowType` 实际传值是否为固定 `0`？
4. 跨月时是否需要在次月分支主动 `sim_traffic_clear_state()`（优化项，非 bug）？

---

## 五、总结

在「月套餐 100M/月、月超停机、次月解锁」的产品约束下，当前次月实现**逻辑自洽**：

- 核心链路：跨月 → ping 同步 → 判定次月 → 本地解锁 → 上报新月份 → confirm → 再 ping 拉新套餐
- `-1` 首次启动误触发：**不成立**
- 最需关注：**NTP 启动竞态**、**停机跨月时 ping reply 字段完整性**
- 年套餐逻辑：**未来再做**

---

## 六、相关代码位置

| 函数/逻辑 | 文件 | 约行号 |
|----------|------|--------|
| `sim_traffic_next_month_proc` | hi_mqtt.c | 926-947 |
| `sim_traffic_report` | hi_mqtt.c | 949-1014 |
| `sim_traffic_process` | hi_mqtt.c | 1087-1152 |
| `hlk_mqtt_handle_ping_reply` | hi_mqtt.c | 1789-1987 |
| `hlk_mqtt_handle_flow_update_confirm` | hi_mqtt.c | 2001-2071 |
| `zig_get_month` / `zig_get_year` | main.zig | 115-126 |
| `SHARED_DATA_S` 定义 | hi_mqtt.h | 144-161 |
