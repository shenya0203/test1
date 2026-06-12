#!/usr/bin/lua

-- --- 配置部分 ---
local DEVICE = "/dev/ttyUSB1"
local INTERFACE = "lte"
local INFO_FILE = "/tmp/modem_info.json"
local STATUS_FILE = "/tmp/modem_status.json"
local BLOCK_FILE = "/tmp/internal_sim_blocked.json"
local PROBE_FILE = "/tmp/hlk_modem_probe.json"
local PROBE_INTERVAL_SEC = 900      -- 15 分钟一轮试探
local PROBE_PHASE_EXTERNAL_SEC = 180 -- 外置重拨 3 分钟
local PROBE_PHASE_INTERNAL_SEC = 300 -- 内置临时试探 5 分钟
local PROBE_PHASE_RADIO_SEC = 300    -- 仅内置卡开射频 5 分钟
local CHECK_INTERVAL = 15
local FAIL_THRESHOLD = 10   -- 长周期重拨阈值
local SWITCH_FAIL_LIMIT = 3 -- 模式3自动切卡阈值

-- 全局串口句柄
local G_SERIAL_FD = nil

-- 串口重连最大重试次数
local SERIAL_RETRY_MAX = 15
local SERIAL_RETRY_INTERVAL = 2

-- --- 全局状态 ---
-- --- 全局状态 ---
local state = {
    modem_simnum = 0,
    current_slot = -1,
    last_slot = -1,
    online_since = 0,
    fail_count = 0,        -- 网络层重拨计数
    net_offline_count = 0, -- 业务层切卡计数
    imei = "N/A",
    is_internal_switching = false,
    session_id = os.time() % 100000, -- 初始随时间戳，后递增
    internal_blocked = false,        -- 流量超限标记（只读 BLOCK_FILE）
    is_internal_flight_mode = false, -- 内置卡飞行模式标记
    probe = {
        latched_internal = false,
        active = false,
        phase = "idle",
        override = false,
        home_slot = 0,
        started_at = 0,
        deadline = 0,
        last_probe_at = 0,
        last_result = "idle",
    },
    slots = {
        ["0"] = { iccid = "N/A", imsi = "N/A" },
        ["1"] = { iccid = "N/A", imsi = "N/A" }
    },
    -- 周期内采集到的实时数据
    data = {
        sim_ready = "absent",
        mwan_stat = "offline",
        sig_str = "No Signal",
        local_ip = "0.0.0.0",
        net_reg_status = "Unknown",
        diag_msg = ""
    }
}

-- --- 解析函数：日志记录 ---
local last_logs = {}
local function log(msg, key)
    if key then
        if last_logs[key] == msg then return end
        last_logs[key] = msg
    end

    os.execute(string.format("logger -t 'Modem-Monitor-Lua' %q", tostring(msg)))
end

local function json_encode(t)
    local s = "{"
    local first = true
    for k, v in pairs(t) do
        if not first then s = s .. "," end
        local val_str
        if type(v) == "number" or type(v) == "boolean" then
            val_str = tostring(v)
        else
            val_str = string.format("%q", tostring(v))
        end
        s = s .. string.format("%q:%s", k, val_str)
        first = false
    end
    return s .. "}"
end

-- --- 辅助函数：原子写入 JSON ---
local function atomic_write_json(path, data_table)
    local tmp_path = path .. ".tmp"
    local f = io.open(tmp_path, "w")
    if f then
        f:write(json_encode(data_table))
        f:close()
        os.execute(string.format("mv %s %s", tmp_path, path))
    end
end

-- --- 辅助函数：获取底层网卡名 ---
local function get_netif_base()
    local f = io.popen("ubus call network.interface." .. INTERFACE .. " status 2>/dev/null")
    if not f then return "eth1" end
    local content = f:read("*a")
    f:close()
    local dev = content:match('"l3_device"%s*:%s*"([^"]+)"') or content:match('"device"%s*:%s*"([^"]+)"')
    return dev or "eth1"
end

-- --- 辅助函数：检查内置卡阻断状态 ---
local function check_internal_block()
    local f = io.open(BLOCK_FILE, "r")
    if not f then
        state.internal_blocked = false
        return
    end
    local content = f:read("*a")
    f:close()

    if content:find('"blocked"%s*:%s*true') then
        log("NOTICE: Internal SIM block detected!")
        if not state.internal_blocked then
            log("ALERT: Internal SIM block detected!")
        end
        state.internal_blocked = true
    else
        log("NOTICE: Internal SIM block cleared.")
        if state.internal_blocked then
            log("NOTICE: Internal SIM block cleared.")
        end
        state.internal_blocked = false
    end
end

local function probe_override_active()
    return state.probe.override == true
end

local function probe_skip_block_enforcement()
    if probe_override_active() then return true end
    if state.probe.active and state.probe.phase == "radio_up" then return true end
    return false
end

local function load_probe_state()
    local f = io.open(PROBE_FILE, "r")
    if not f then return end
    local content = f:read("*a")
    f:close()
    if not content or content == "" then return end

    state.probe.latched_internal = content:find('"latched_internal"%s*:%s*true') ~= nil
    state.probe.active = content:find('"probe_active"%s*:%s*true') ~= nil
    local phase = content:match('"probe_phase"%s*:%s*"([^"]+)"')
    state.probe.phase = phase or "idle"
    state.probe.override = content:find('"probe_override"%s*:%s*true') ~= nil
    state.probe.home_slot = tonumber(content:match('"home_slot"%s*:%s*(%d+)')) or state.probe.home_slot
    state.probe.started_at = tonumber(content:match('"probe_started_at"%s*:%s*(%d+)')) or 0
    state.probe.deadline = tonumber(content:match('"probe_deadline"%s*:%s*(%d+)')) or 0
    state.probe.last_probe_at = tonumber(content:match('"last_probe_at"%s*:%s*(%d+)')) or 0
    local last_result = content:match('"last_result"%s*:%s*"([^"]+)"')
    state.probe.last_result = last_result or "idle"
end

local function save_probe_state()
    local payload = string.format(
        '{"latched_internal":%s,"probe_active":%s,"probe_phase":"%s","probe_override":%s,"home_slot":%d,"probe_started_at":%d,"probe_deadline":%d,"last_probe_at":%d,"last_result":"%s"}',
        state.probe.latched_internal and "true" or "false",
        state.probe.active and "true" or "false",
        state.probe.phase,
        state.probe.override and "true" or "false",
        state.probe.home_slot,
        state.probe.started_at,
        state.probe.deadline,
        state.probe.last_probe_at,
        state.probe.last_result
    )
    local f = io.open(PROBE_FILE, "w")
    if not f then return end
    f:write(payload)
    f:close()
end

local function reset_probe_state(result)
    state.probe.active = false
    state.probe.phase = "idle"
    state.probe.override = false
    state.probe.started_at = 0
    state.probe.deadline = 0
    state.probe.last_result = result or "idle"
    save_probe_state()
end

local function probe_interval_elapsed(now)
    if state.probe.last_probe_at == 0 then return true end
    return (now - state.probe.last_probe_at) >= PROBE_INTERVAL_SEC
end

local function probe_begin_phase(phase, deadline, override, result)
    state.probe.active = true
    state.probe.phase = phase
    state.probe.override = override == true
    state.probe.started_at = os.time()
    state.probe.deadline = deadline
    state.probe.last_result = result or phase
    save_probe_state()
end

local function probe_end_phase(result)
    state.probe.override = false
    state.probe.active = false
    state.probe.phase = "idle"
    state.probe.started_at = 0
    state.probe.deadline = 0
    state.probe.last_result = result or "done"
    save_probe_state()
end

local function probe_mark_attempt(now, result)
    state.probe.last_probe_at = now
    state.probe.last_result = result or state.probe.last_result
    save_probe_state()
end

local function probe_latch_internal_blocked()
    if state.probe.latched_internal then return end
    state.probe.latched_internal = true
    state.probe.home_slot = state.current_slot
    save_probe_state()
    log("内置卡流量超限，进入试探状态机（home_slot=" .. state.probe.home_slot .. "）")
end

local function probe_clear_latch()
    if not state.probe.latched_internal and not state.probe.active and not state.probe.override then return end
    state.probe.latched_internal = false
    reset_probe_state("unblocked")
    log("内置卡流量限制已解除，退出试探状态机")
end

local function probe_set_radio_up()
    if state.is_internal_flight_mode then
        log("试探：内置卡开射频（CFUN=1）")
        send_at("AT+CFUN=1")
        state.is_internal_flight_mode = false
    end
    os.execute(string.format("ubus call network.interface.%s up 2>/dev/null", INTERFACE))
end

local function probe_set_radio_down()
    if not state.is_internal_flight_mode then
        log("试探结束：内置卡关射频（CFUN=0）")
        send_at("AT+CFUN=0")
        state.is_internal_flight_mode = true
    end
    os.execute(string.format("ubus call network.interface.%s down 2>/dev/null", INTERFACE))
end

-- --- 自动探测 4G 模组的 AT 串口 ---
-- 遍历 /dev/ttyUSB0~10，向每个设备发送 AT 指令，
-- 能返回 "OK" 的就是模组的 AT 串口
local function find_modem_serial()
    for i = 0, 10 do
        local path = "/dev/ttyUSB" .. i
        os.execute(string.format("stty -F %s 115200 raw -echo min 0 time 1 2>/dev/null", path))
        local f = io.open(path, "r+")
        if f then
            f:setvbuf("no")
            local drain_end = os.time() + 1
            while os.time() < drain_end do
                local chunk = f:read(4096)
                if not chunk or chunk == "" then break end
            end
            f:write("AT\r\n")
            f:flush()
            local start = os.time()
            while os.difftime(os.time(), start) < 2 do
                local line = f:read("*l")
                if line then
                    if line:find("OK") then
                        f:close()
                        return path
                    end
                    if line:find("ERROR") then break end
                end
            end
            f:close()
        end
    end
    return nil
end

-- --- 模组重启后重新初始化串口 ---
-- 1. 先尝试重连原来的 DEVICE（最多 SERIAL_RETRY_MAX 次）
-- 2. 失败则扫描所有 ttyUSB* 自动探测
-- 3. 找到后更新 DEVICE 全局变量
local function reinit_serial_port()
    if G_SERIAL_FD then
        pcall(G_SERIAL_FD.close, G_SERIAL_FD)
        G_SERIAL_FD = nil
    end

    log("Serial: waiting for modem USB re-enumeration...")
    os.execute("sleep 3")

    for retry = 1, SERIAL_RETRY_MAX do
        local try_dev = "/dev/ttyUSB" .. retry
        -- 必须先 stty 设置 min 0 time 1，否则阻塞读会卡死
        os.execute(string.format("stty -F %s 115200 raw -echo min 0 time 1 2>/dev/null", try_dev))
        local f = io.open(try_dev, "r+")
        if f then
            f:setvbuf("no")
            -- Drain buffer：用 chunk 读取代替逐字节
            local drain_end = os.time() + 1
            while os.time() < drain_end do
                local chunk = f:read(4096)
                if not chunk or chunk == "" then break end
            end
            f:write("AT\r\n")
            f:flush()
            local start = os.time()
            local resp = ""
            while os.difftime(os.time(), start) < 2 do
                local char = f:read(1)
                if char then
                    resp = resp .. char
                    if resp:find("OK") then
                        f:close()
                        G_SERIAL_FD = io.open(try_dev, "r+")
                        G_SERIAL_FD:setvbuf("no")
                        log(string.format("Serial: reconnected on %s (retry %d)", try_dev, retry))
                        return true
                    end
                end
            end
            f:close()
        else
            log(string.format("Serial: %s not available (retry %d)", try_dev, retry))
        end
    end

    log(string.format("Serial: %s not available after %d retries, scanning all ttyUSB*...",
        DEVICE, SERIAL_RETRY_MAX))
    local found = find_modem_serial()
    if found then
        log(string.format("Serial: found modem on %s (was %s), updating DEVICE", found, DEVICE))
        DEVICE = found
        G_SERIAL_FD = io.open(DEVICE, "r+")
        G_SERIAL_FD:setvbuf("no")
        return true
    end

    log("Serial *** CRITICAL: modem serial port not found after reboot!")
    return false
end

-- --- 串口初始化 ---
local function init_serial_port()
    log("Initializing Serial Port " .. DEVICE .. "...")
    os.execute(string.format("stty -F %s 115200 raw -echo min 0 time 1 2>/dev/null", DEVICE))
    G_SERIAL_FD = io.open(DEVICE, "r+")
    if not G_SERIAL_FD then
        log("CRITICAL ERROR: Failed to open serial device.")
        if reinit_serial_port() then
            log("Serial port re-initialized successfully.")
            return
        end
        os.exit(1)
    end
    G_SERIAL_FD:setvbuf("no")
end

-- --- 串口 Drain：用大块读取代替逐字节，减少 Lua 调用次数 ---
local function drain_serial()
    local drain_end = os.time() + 1
    while os.time() < drain_end do
        local chunk = G_SERIAL_FD:read(4096)
        if not chunk or chunk == "" then break end
    end
end

-- --- AT 指令交互（优化版） ---
-- 使用 read("*l") 行读取替代 read(1) 逐字节，大幅减少 Lua 调用和 GC 压力
local function send_at(cmd, timeout_sec)
    timeout_sec = timeout_sec or 3
    if not G_SERIAL_FD then return nil end

    drain_serial()

    G_SERIAL_FD:write(cmd .. "\r\n")
    G_SERIAL_FD:flush()

    local lines = {}
    local start_t = os.time()
    while os.difftime(os.time(), start_t) < timeout_sec do
        local line = G_SERIAL_FD:read("*l")
        if line then
            table.insert(lines, line)
            if line:find("OK") or line:find("ERROR") then break end
        end
    end

    local results = {}
    for _, line in ipairs(lines) do
        if not line:find(cmd, 1, true) then
            line = line:gsub("^%s*(.-)%s*$", "%1")
            if #line > 0 then table.insert(results, line) end
        end
    end
    local output = table.concat(results, " ")

    local log_msg
    if #output == 0 then
        log_msg = string.format("AT >> %s | << [TIMEOUT/EMPTY]", cmd)
    else
        log_msg = string.format("AT >> %s | << %s", cmd, output)
    end
    log(log_msg, cmd)
    return output
end

-- --- 合并 AT 命令交互 ---
-- 发送 AT+CMD1;+CMD2;+CMD3 并逐行返回所有响应结果
-- 返回值: 行数组（已过滤回显行和 OK/ERROR）
local function send_at_combined(cmd, timeout_sec)
    timeout_sec = timeout_sec or 2
    if not G_SERIAL_FD then return nil end

    drain_serial()

    G_SERIAL_FD:write(cmd .. "\r\n")
    G_SERIAL_FD:flush()

    local lines = {}
    local start_t = os.time()
    while os.difftime(os.time(), start_t) < timeout_sec do
        local line = G_SERIAL_FD:read("*l")
        if line then
            table.insert(lines, line)
            if line:find("OK") or line:find("ERROR") then
                break
            end
        end
    end

    local results = {}
    for _, line in ipairs(lines) do
        if not line:find(cmd, 1, true) and not line:find("^OK$") and not line:find("^ERROR") then
            line = line:gsub("^%s*(.-)%s*$", "%1")
            if #line > 0 then table.insert(results, line) end
        end
    end
    log(string.format("AT >> %s | << %d lines", cmd, #results), cmd)

    return results
end

-- --- 硬件特性同步 ---
local function sync_modem_hardware(mode)
    log(string.format("--- Syncing Modem Hardware (Mode:%d) ---", mode))
    send_at("ATE0")
    if mode == 0 or mode == 3 then
        send_at("AT+CSDT=1")
        send_at("AT*SIMAUTO=1")
    else
        send_at("AT+CSDT=0")
        send_at("AT*SIMAUTO=0")
    end
    send_at("AT+CFUN=1")
    os.execute("sleep 5")
end

-- --- 网络状态获取 ---
local function get_mwan3_status()
    local f = io.popen("mwan3 status 2>/dev/null")
    if not f then return "offline" end
    local content = f:read("*a")
    f:close()
    local lte_section = content:match("interface " .. INTERFACE .. ".-interface") or
        content:match("interface " .. INTERFACE .. ".*")
    if lte_section and lte_section:find("online") then
        return "online"
    end
    return "offline"
end

-- --- 执行卡槽切换 ---
local function perform_slot_switch(target)
    local resp
    log(string.format("!!! TRIGGER: Software Switch to SIM%d !!!", target))
    os.execute(string.format("ubus call network.interface.%s down", INTERFACE))
    state.is_internal_switching = true
    resp = send_at("AT+CFUN=0")
    os.execute("sleep 1")
    resp = send_at("AT+SIMCROSS=" .. target)
    os.execute("echo 1 > /sys/class/net/eth1/reset_statistics")
    resp = send_at("AT+CFUN=1")
    state.net_offline_count = 0
    state.fail_count = 0
    state.session_id = state.session_id + 1 -- 会话ID递增
    os.execute("sleep 1")
    os.execute(string.format("ubus call network.interface.%s up", INTERFACE))
    local resp_slot = send_at("AT+SIMCROSS?")
    state.current_slot = tonumber(resp_slot and resp_slot:match(":%s*(%d)")) or -1
end

local function probe_start_external_redial(now)
    log("双卡离线：外置重拨试探 " .. PROBE_PHASE_EXTERNAL_SEC .. " 秒")
    if state.current_slot ~= 0 then
        perform_slot_switch(0)
    end
    os.execute(string.format("ubus call network.interface.%s down 2>/dev/null", INTERFACE))
    os.execute(string.format("ubus call network.interface.%s up 2>/dev/null", INTERFACE))
    probe_begin_phase("external_redial", now + PROBE_PHASE_EXTERNAL_SEC, false, "external_redial")
    probe_mark_attempt(now, "external_redial")
end

local function manage_dual_flow_probe(now, allow_internal_trial)
    if state.probe.active then
        if state.probe.phase == "internal_trial" then
            if not allow_internal_trial then
                log("内置已解封，结束内置试探并切回外置")
                perform_slot_switch(state.probe.home_slot)
                probe_end_phase("unblocked")
                return
            end
            if now >= state.probe.deadline then
                check_internal_block()
                if state.internal_blocked then
                    log("内置试探结束仍 block，切回外置卡 slot " .. state.probe.home_slot)
                    perform_slot_switch(state.probe.home_slot)
                    probe_mark_attempt(now, "internal_still_blocked")
                    probe_end_phase("internal_still_blocked")
                else
                    log("内置试探成功，流量限制已解除")
                    if state.modem_simnum == 0 then
                        perform_slot_switch(0)
                    end
                    probe_mark_attempt(now, "internal_unblocked")
                    probe_end_phase("internal_unblocked")
                end
            end
            return
        end

        if state.probe.phase == "external_redial" then
            if now >= state.probe.deadline then
                check_internal_block()
                if allow_internal_trial and state.internal_blocked then
                    log("外置重拨窗口结束仍离线且内置仍 block，进入内置临时试探")
                    perform_slot_switch(1)
                    probe_begin_phase("internal_trial", now + PROBE_PHASE_INTERNAL_SEC, true, "internal_trial")
                else
                    probe_mark_attempt(now, "external_only")
                    probe_end_phase("external_only")
                end
            end
            return
        end
    end

    if not probe_interval_elapsed(now) then return end
    probe_start_external_redial(now)
end

local function manage_flow_probe()
    load_probe_state()
    check_internal_block()

    local now = os.time()
    local simnum = state.modem_simnum
    local lte_online = state.data.mwan_stat == "online"
    local dual_mode = simnum == 0 or simnum == 3

    if dual_mode and lte_online then
        if state.probe.active or state.probe.override then
            probe_end_phase("online_recovered")
        end
        return
    end

    if not state.internal_blocked then
        if state.probe.latched_internal or state.probe.override then
            probe_clear_latch()
        end
        if dual_mode and not lte_online then
            manage_dual_flow_probe(now, false)
        end
        return
    end

    probe_latch_internal_blocked()

    if dual_mode then
        manage_dual_flow_probe(now, true)
        return
    end

    if simnum == 1 then
        if lte_online then
            if state.probe.active then
                probe_end_phase("online_recovered")
            end
            return
        end

        if state.probe.active then
            if now >= state.probe.deadline then
                probe_set_radio_down()
                probe_mark_attempt(now, "radio_timeout")
                probe_end_phase("radio_timeout")
            end
            return
        end

        if not probe_interval_elapsed(now) then return end

        log("仅内置卡：流量超限且离线，开射频试探 " .. PROBE_PHASE_RADIO_SEC .. " 秒")
        probe_set_radio_up()
        probe_begin_phase("radio_up", now + PROBE_PHASE_RADIO_SEC, false, "radio_up")
        probe_mark_attempt(now, "radio_up")
    end
end

-- --- 采集指定卡槽的元数据 (ICCID/IMSI) ---
local function collect_slot_metadata(slot_id)
    if not slot_id or slot_id == -1 then return end
    local sid_str = tostring(slot_id)
    local resp = send_at_combined("AT+ICCID;+CIMI", 5)
    if resp then
        for _, line in ipairs(resp) do
            local iccid = line:match(":%s*([%dA-Z]+)")
            if iccid and #iccid > 10 then state.slots[sid_str].iccid = iccid end
            local imsi = line:match("^(%d+)$")
            if imsi and #imsi > 5 then state.slots[sid_str].imsi = imsi end
        end
    end
end

-- --- 初始化相关函数 ---
local function read_modem_config()
    local f_uci = io.popen(string.format("uci -q get network.%s.modem_simnum", INTERFACE))
    state.modem_simnum = tonumber(f_uci and f_uci:read("*l")) or 0
    if f_uci then f_uci:close() end
end

local function initial_sim_slot_setup()
    while state.current_slot == -1 do
        perform_slot_switch(1)
        --local resp_slot = send_at("AT+SIMCROSS?")
        --state.current_slot = tonumber(resp_slot and resp_slot:match(":%s*(%d)")) or -1
    end

    if state.imei == "N/A" then
        local resp = send_at("AT+CGSN")
        if resp then
            local raw = resp:match("%d+")
            state.imei = (raw and raw:match("^%d+$") and #raw == 15) and raw or "N/A"
        else
            state.imei = "N/A"
        end
    end

    collect_slot_metadata(state.current_slot)

    --切回外置卡
    if state.modem_simnum == 0 or state.modem_simnum == 2 or state.modem_simnum == 3 then
        while state.current_slot ~= 0 do
            perform_slot_switch(0)
            --local resp_slot = send_at("AT+SIMCROSS?")
            --state.current_slot = tonumber(resp_slot and resp_slot:match(":%s*(%d)")) or -1
        end
    end
end

local function init_service()
    init_serial_port()
    read_modem_config()
    --获取内置卡信息
    initial_sim_slot_setup()
    --切到外置卡槽0
    --if state.modem_simnum == 0 or state.modem_simnum == 2 or state.modem_simnum == 3 then
    --perform_slot_switch(0)
    --end

    --perform_slot_switch(0)
    log(string.format("Service Started. Mode:%d Device:%s", state.modem_simnum, DEVICE))
end

-- --- 循环内部核心逻辑函数 ---

-- 2. 处理卡槽变动事件日志
local function handle_sim_slot_change_event()
    if state.last_slot ~= -1 and state.current_slot ~= state.last_slot then
        if state.is_internal_switching then
            log(string.format("Slot Switch Confirmed: New Slot %d [System]", state.current_slot))
            state.is_internal_switching = false
        else
            log(string.format("EVENT: External/Hardware SIM swap detected! Now on Slot %d", state.current_slot))
        end
    end
    state.last_slot = state.current_slot
end

-- 3. 基础状态感知（合并 AT+CPIN?;+CSQ;+CEREG? 为一次串口调用）
local function collect_modem_status()
    -- 先重置信号/注册状态，供下方和 collect_network_data 共用
    state.data.sig_str = "No Signal"
    state.data.net_reg_status = "Unknown"
    state.data.sim_ready = "absent"

    local resp = send_at_combined("AT+CPIN?;+CSQ;+CEREG?", 5)
    if resp then
        for _, line in ipairs(resp) do
            if line:find("CPIN:") then
                state.data.sim_ready = line:find("READY") and "ready" or "absent"
            end
            local csq_val = line:match("%+CSQ:%s*(%d+)")
            if csq_val and tonumber(csq_val) ~= 99 then
                state.data.sig_str = (-113 + tonumber(csq_val) * 2) .. " dBm"
            end
            local cereg_stat = line:match("%+CEREG:%s*%d+,(%d+)")
            if cereg_stat == "1" then
                state.data.net_reg_status = "Registered (Home)"
            elseif cereg_stat == "5" then
                state.data.net_reg_status = "Registered (Roaming)"
            elseif cereg_stat then
                state.data.net_reg_status = "Not Registered (" .. cereg_stat .. ")"
            end
        end
    end

    local new_mwan_stat = get_mwan3_status()
    state.data.mwan_stat = new_mwan_stat

    if state.imei == "N/A" then
        local imei_resp = send_at("AT+CGSN")
        state.imei = imei_resp and imei_resp:match("%d+") or "N/A"
    end
end

-- 4. 增强数据采集（单独采集 IP + 卡槽元数据）
local function collect_network_data()
    if state.data.sim_ready == "ready" then
        -- IP 地址 (CGPADDR)
        if state.data.local_ip == "0.0.0.0" then
            local ip_resp = send_at("AT+CGPADDR=1")
                log("ip_resp: " .. ip_resp)
            if ip_resp then
                state.data.local_ip = ip_resp:match(':%s*%d+,"([^"]+)"')
            end
            collect_slot_metadata(state.current_slot)
        end
        return false
    else
        -- 外置卡优先：SIM 未就绪时尝试切内置；block 时仅 probe_override 允许
        if state.modem_simnum == 0 and state.current_slot == 0
            and (not state.internal_blocked or probe_override_active()) then
            perform_slot_switch(1)
            return true
        end
    end
    return false
end

-- 5. 故障计数与诊断日志
local function update_failure_counters()
    state.data.diag_msg = string.format("Cycle: [Mode:%d] [Slot:%d] [Net:%s]",
        state.modem_simnum, state.current_slot, state.data.mwan_stat)

    if state.data.mwan_stat == "offline" then
        state.fail_count = state.fail_count + 1
        state.data.diag_msg = state.data.diag_msg ..
            string.format(" [Redial-Fail:%d/%d]", state.fail_count, FAIL_THRESHOLD)
    else
        state.fail_count = 0
    end
    --log(state.data.diag_msg, "cycle_diag")
end

-- 6. 状态上报 (双文件上报：兼容旧版 + C程序专用)
local function report_all_status()
    local mwan_online = (state.data.mwan_stat == "online")

    -- 映射属性 (sim_source)
    local sim_source = "unknown"
    if state.current_slot == 0 then
        sim_source = "external"
    elseif state.current_slot == 1 then
        sim_source = "internal"
    end

    -- 映射拨号状态 (dial_status)
    local dial_status = "disconnected"
    if mwan_online then
        dial_status = "connected"
    elseif state.fail_count > 0 then
        dial_status = "connecting"
    elseif state.fail_count >= FAIL_THRESHOLD then
        dial_status = "failed"
    end

    -- a. 兼容文件 modem_info.json (用于 Web UI 等)
    local full_status_text = state.data.net_reg_status
    if mwan_online then full_status_text = full_status_text .. " (Online)" end
    local info_out = {
        imei = state.imei,
        iccid = state.slots["1"].iccid,
        imsi = state.slots["1"].imsi,
        iccid_0 = state.slots["0"].iccid,
        imsi_0 = state.slots["0"].imsi,
        signal = state.data.sig_str,
        local_ip = state.data.local_ip,
        status = full_status_text,
        sim_status = state.data.sim_ready,
        updated = os.date("%H:%M:%S")
    }
    atomic_write_json(INFO_FILE, info_out)

    -- b. 专用状态文件 modem_status.json (用于 C 程序流量统计)
    local sid_str = tostring(state.current_slot or "-1")
    local status_out = {
        sim_source = sim_source,
        dial_status = dial_status,
        netif = get_netif_base(),
        iccid = state.slots[sid_str] and state.slots[sid_str].iccid or "N/A",
        imsi = state.slots[sid_str] and state.slots[sid_str].imsi or "N/A",
        imei = state.imei,
        session_id = state.session_id,
        updated_at = os.time()
    }
    atomic_write_json(STATUS_FILE, status_out)
end

-- 7. 重拨与切卡修复逻辑
local function handle_redial_and_switch_logic()
    -- 首先检查阻断强制执行：如果当前是内置卡且被阻断，立即切走
    -- 如果当前配置不是仅内置卡 且 当前卡槽使用的是内置卡
    if state.current_slot == 1 and state.internal_blocked and not probe_skip_block_enforcement() then
        log("CRITICAL: Internal SIM blocked! Forcing switch to External SIM...")

        if state.modem_simnum == 1 then
            state.is_internal_flight_mode = true
            send_at("AT+CFUN=0")
        else
            perform_slot_switch(0)
        end

        return
    end

    if state.data.mwan_stat == "offline" and state.fail_count >= FAIL_THRESHOLD then
        log("Fail threshold reached. Triggering recovery...")

        if state.modem_simnum == 3 then -- 双卡备份模式
            state.net_offline_count = state.net_offline_count + 1
            if state.net_offline_count >= SWITCH_FAIL_LIMIT then
                local next_slot = (state.current_slot == 0) and 1 or 0
                -- 拦截：如果要跳往内置卡但被阻断
                if next_slot == 1 and state.internal_blocked and not probe_override_active() then
                    log("Switch to SIM1 (Internal) ABORTED: SIM is blocked.")
                else
                    log("Backup switch triggered!")
                    state.net_offline_count = 0
                    perform_slot_switch(next_slot)
                end
            end
        elseif state.modem_simnum == 0 and state.current_slot == 0 then --外置卡优先 且 当前正在使用外置卡
            -- 模式 0 故障回退内置卡，同样需要拦截
            if state.internal_blocked and not probe_override_active() then
                log("Fallback to SIM1 DENIED: SIM is blocked.")
            else
                log("Mode 0 fallback to SIM1 triggered.")
                perform_slot_switch(1)
            end
        end

        state.fail_count = 0
    end
end

-- 8. IP 冲突检测与自动修复
local function check_ip_conflict_and_resolve()
    --
    -- 阶段 1: 获取 LTE 接口当前的 IP 地址
    --
    -- 通过 ubus 获取 LTE 接口的运行状态，从中提取 ipv4 address
    -- 使用 INTERFACE 常量（默认 "lte"），实际底层设备由 get_netif_base() 确定
    --
    local cmd_status = "ubus call network.interface." .. INTERFACE .. " status 2>/dev/null"
    local f = io.popen(cmd_status)
    if not f then
        log("IP-Check: ubus call failed (interface " .. INTERFACE .. " may not exist)")
        return
    end
    local content = f:read("*a")
    f:close()
    --
    -- 从 ubus 返回的 JSON 中匹配第一个 ipv4 address 字段
    -- ubus 返回格式: ... "address":"192.168.43.2","mask":24 ...
    --
    local lte_ip = content and content:match('"address"%s*:%s*"([^"]+)"')
    if not lte_ip then
        log("IP-Check: no ipv4 address for " .. INTERFACE .. " (interface may be down)")

        -- 检查UCI是否已有配置
        local f_uci = io.popen("uci -q get network." .. INTERFACE .. ".ipaddr 2>/dev/null")
        if f_uci then
            local uci_ip = f_uci:read("*l")
            f_uci:close()
            if uci_ip and uci_ip ~= "" then
                log("IP-Check: uci already has ipaddr=" .. uci_ip .. ", skipping")
                return
            end
        end

        -- UCI未配置，通过AT+CIFCONFIG?获取当前4G模组分配的IP
        local at_resp = send_at("AT+CIFCONFIG?")
        if at_resp then
            local modem_ip = at_resp:match("(%d+%.%d+%.%d+%.%d+)")
            if modem_ip then
                -- 网关地址 = IP末段减1 (如 192.168.10.2 → 192.168.10.1)
                local parts = {}
                for part in string.gmatch(modem_ip, "([^%.]+)") do
                    table.insert(parts, part)
                end
                if #parts == 4 then
                    local last = tonumber(parts[4])
                    if last and last > 1 then
                        parts[4] = tostring(last - 1)
                    end
                    local gateway = table.concat(parts, ".")
                    os.execute(string.format("uci set network.%s.ipaddr='%s'", INTERFACE, modem_ip))
                    os.execute(string.format("uci set network.%s.gateway='%s'", INTERFACE, gateway))
                    os.execute("uci commit network")
                    log(string.format("IP-Check: set ipaddr=%s, gateway=%s from AT+CIFCONFIG?", modem_ip, gateway))
                    -- 重启LTE接口使新配置生效
                    os.execute("ifdown " .. INTERFACE)
                    os.execute("ifup " .. INTERFACE)
                end
            end
        end
        return
    end

    --
    -- 提取 IP 的前三段构成子网前缀（如 192.168.43）
    -- 这里假定 netmask 为 255.255.255.0（/24），
    -- 因为 LTE 静态配的就是 /24，其他接口也大概率是 C 类网段
    --
    local lte_parts = {}
    for part in string.gmatch(lte_ip, "([^%.]+)") do
        table.insert(lte_parts, part)
    end
    if #lte_parts < 3 then
        log("IP-Check: invalid LTE IP format: " .. lte_ip)
        return
    end
    local lte_prefix = lte_parts[1] .. "." .. lte_parts[2] .. "." .. lte_parts[3]

    --
    -- 阶段 2: 收集可能产生冲突的其他接口的子网
    --
    -- 检测对象：wan（有线）、wwan（WiFi 中继）、lan（LAN 侧）
    -- 只要 LTE 与其中任意一个接口处于同一子网，就会导致路由冲突
    --
    local used_prefixes = {}
    local check_interfaces = { "wan", "wwan", "lan" }
    for _, iface in ipairs(check_interfaces) do
        local cmd = "ubus call network.interface." .. iface .. " status 2>/dev/null"
        local f2 = io.popen(cmd)
        if f2 then
            local c = f2:read("*a")
            f2:close()
            local ip = c and c:match('"address"%s*:%s*"([^"]+)"')
            if ip then
                local parts = {}
                for part in string.gmatch(ip, "([^%.]+)") do
                    table.insert(parts, part)
                end
                if #parts >= 3 then
                    local prefix = parts[1] .. "." .. parts[2] .. "." .. parts[3]
                    used_prefixes[prefix] = true
                end
            end
        end
    end

    --
    -- 阶段 3: 判断是否存在冲突
    --
    -- 如果 LTE 的子网前缀（前三段）没有出现在 used_prefixes 中，
    -- 说明当前没有冲突，直接返回。
    --
    if not used_prefixes[lte_prefix] then
        return
    end

    --
    -- 阶段 4: 冲突确认 → 进入自动修复流程
    --
    log(string.format("IP-Check *** CONFLICT DETECTED: LTE prefix %s.x conflicts with another interface!", lte_prefix))

    --
    -- 阶段 5: 寻找一个空闲的 192.168.x.x 网段
    --
    -- 策略：保持前两段（192.168）不变，遍历第三段 2~254，
    -- 跳过已经被 used_prefixes 占用的网段，
    -- 同时也跳过当前 LTE 自身正在使用的网段（i ~= lte_third）。
    -- 找到的第一个空闲网段即作为新的 LTE 网段。
    --
    local lte_third = tonumber(lte_parts[3])
    local new_prefix
    for i = 210, 230 do
        local candidate = lte_parts[1] .. "." .. lte_parts[2] .. "." .. i
        if not used_prefixes[candidate] then
            new_prefix = candidate
            log(string.format("IP-Check: found free subnet = %s.x", new_prefix))
            break
        end
    end

    if not new_prefix then
        log("IP-Check *** CRITICAL: ALL subnets 192.168.2~254 are occupied, cannot resolve conflict!")
        return
    end

    --
    -- 构造新的 IP 地址:
    --   模组侧（网关）: 192.168.NEW.1
    --   OpenWrt 侧  : 192.168.NEW.2
    --
    local new_gateway = new_prefix .. ".1"
    local new_ipaddr = new_prefix .. ".2"
    log(string.format("IP-Check: target gateway = %s, target ipaddr = %s", new_gateway, new_ipaddr))

    --
    -- 阶段 6: 通过 AT 指令修改 4G 模组自身的 IP 地址
    --
    -- AT+CIFCONFIG 用于设置 ECM 模式下模组的本地 IP，
    -- 模组会以这个 IP 作为网关与 OpenWrt 通信。
    -- 发送后等待模组返回 "OK" 确认。
    --
    local at_cmd = string.format('AT+CIFCONFIG="%s"', new_ipaddr)
    log("IP-Check: sending " .. at_cmd)
    local resp = send_at(at_cmd)
    if not resp or not resp:find("OK") then
        log(string.format("IP-Check *** ERROR: AT+CIFCONFIG failed, response = %s", resp or "(empty)"))
        return
    end
    log("IP-Check: AT+CIFCONFIG response OK, modem IP will change to " .. new_gateway)

    --
    -- 阶段 7: 重启模组使 AT+CIFCONFIG 生效
    --
    -- AT+CFUN=1,1 触发模组软重启，新的 IP 配置才会被加载。
    -- 模组重启后 USB 设备会重新枚举，ttyUSB 序号可能变化，
    -- 所以 CFUN 之后需要 reinit_serial_port() 自动找回串口。
    --
    log("IP-Check: sending AT+CFUN=1,1 to reboot modem...")
    os.execute("ifconfig eth1 down")
    send_at("AT+CFUN=1,1")

    if not reinit_serial_port() then
        log("IP-Check *** CRITICAL: failed to reinitialize serial after modem reboot!")
        return
    end

    --
    -- 阶段 8: 更新 OpenWrt UCI 持久化配置
    --
    -- 将 LTE 接口的静态 IP 和网关改为新的网段，
    -- 这样下次网络重启 / 开机后配置仍然有效。
    --
    os.execute(string.format("uci set network.%s.ipaddr='%s'", INTERFACE, new_ipaddr))

    os.execute(string.format("uci set network.%s.gateway='%s'", INTERFACE, new_gateway))

    os.execute("uci commit network")

    --
    -- 阶段 9: 通知 netifd（ubus）运行时接口的新地址
    --
    -- 通过 ubus set_data 直接告诉 netifd 当前运行中的接口
    -- 应该使用的新 IP 和网关，避免 restart 之前的短暂不一致。
    --

    --
    -- 同步更新本地内存中的 IP，以便后续周期直接使用
    --
    state.data.local_ip = new_ipaddr

    --
    -- 阶段 10: 重启 LTE 接口使新配置生效
    --
    -- ubus call 重启只影响 LTE 接口本身，不会中断有线/WiFi 的连接。
    --
    os.execute("/etc/init.d/network reload")
    os.execute("/etc/init.d/mwan3 restart")
end

-- --- 主循环守护 ---
local function monitor_main()
    init_service()
    local first = true

    while true do
        local skip_this_cycle = false

        -- 阶段 0: 外部指令感知 (阻断检查)
        check_internal_block()

        -- 阶段 1: 物理感知与模式强制矫正

        if not skip_this_cycle then
            -- 阶段 2: 事件记录与基础状态
            --handle_sim_slot_change_event()
            collect_modem_status()
            manage_flow_probe()

            -- 阶段 3: 详细数据采集
            if collect_network_data() then
                skip_this_cycle = true
            end
        end

        if not skip_this_cycle then
            -- 阶段 4: 统计、上报与自动化维护
            update_failure_counters()
            report_all_status()
            handle_redial_and_switch_logic()
        end

        -- 阶段 5: IP 冲突检测 (独立运行，不受 skip 影响)
        check_ip_conflict_and_resolve()

        if first then
            first = false
            os.execute("/etc/init.d/mwan3 restart")
        end

        os.execute("sleep " .. CHECK_INTERVAL)
    end
end

monitor_main()
