-- Inputs are supplied by CeTableUpdater.cpp; never execute CT/INI contents.
-- Runs on the existing CE bridge's main-thread evaluate_lua handler.
local function trim(s) return (s:gsub('^%s+', ''):gsub('%s+$', '')) end
local function hex(s) return (s:gsub('.', function(c) return string.format('%02X', c:byte()) end)) end
local function run()
    local rules, order, values = {}, {}, {}
    rulesText = rulesText:gsub('^\239\187\191', '')
    for line in (rulesText .. '\n'):gmatch('(.-)\n') do
        line = line:gsub('\r$', '')
        if trim(line) ~= '' and not line:match('^%s*#') then
            local name, key, module = line:match('^([^\t]+)\t([^\t]+)\t([^\t]+)$')
            assert(name, '名称映射格式错误，必须是：名称<TAB>键<TAB>模块')
            key, module = trim(key):upper(), trim(module):lower()
            assert(not rules[name], '名称映射重复：' .. name)
            rules[name] = {key = key, module = module, found = 0}
            order[#order + 1] = name
        end
    end
    assert(#order > 0, '名称映射为空')
    local section = ''
    offsetsText = offsetsText:gsub('^\239\187\191', '')
    for line in (offsetsText .. '\n'):gmatch('(.-)\n') do
        line = trim(line)
        local header = line:match('^%[([^%]]+)%]$')
        if header then section = trim(header):upper()
        elseif section == 'CCEE' and not line:match('^[;#]') then
            local key, value = line:match('^([^=]+)=(.*)$')
            if key then
                key = trim(key):upper()
                assert(values[key] == nil, 'CCEE 键重复：' .. key)
                values[key] = trim((value:gsub('[;#].*$', '')))
            end
        end
    end
    -- Validate all configured keys before touching any record.
    for _, name in ipairs(order) do
        local rule = rules[name]
        local raw = values[rule.key]
        assert(raw, 'CCEE 缺少偏移：' .. rule.key)
        assert(raw:match('^%d+$') or raw:match('^0[xX]%x+$'), '偏移格式错误：' .. rule.key)
        local value = tonumber(raw)
        assert(value and value > 0 and value <= 0xFFFFFFFF and value % 1 == 0,
            '偏移超出有效 DWORD 范围：' .. rule.key)
        rule.offset = string.format('%X', value)
    end
    local al = getAddressList()
    assert(al and al.Count > 0, 'CE 当前地址表为空，请先加载 CT')
    local plan, lines = {}, {}
    local unchanged, skipped, missing = 0, 0, 0
    for i = 0, al.Count - 1 do
        local mr = al[i]
        local name = mr.Description
        local rule = rules[name]
        if rule then
            rule.found = rule.found + 1
            local before = mr.Address
            local module, old = before:match('^%s*(.-)%s*%+%s*(%x+)%s*$')
            if module then module = trim(module):gsub('^"(.*)"$', '%1'):lower() end
            local reason
            if mr.Active then reason = '已激活/冻结'
            elseif mr.Type == vtAutoAssembler or mr.OffsetCount == 0 then reason = '不是普通指针记录'
            elseif module ~= rule.module then reason = '模块或地址表达式不匹配' end
            if reason then
                skipped = skipped + 1
                lines[#lines + 1] = string.format('跳过 ID=%s %s：%s', mr.ID, name, reason)
            elseif tonumber(old, 16) == tonumber(rule.offset, 16) then
                unchanged = unchanged + 1
            else
                local after = before:match('^(.-%+)') .. rule.offset
                local offsets = {}
                for j = 0, mr.OffsetCount - 1 do offsets[j + 1] = mr.OffsetText[j] end
                plan[#plan + 1] = {id = mr.ID, name = name, before = before, after = after, offsets = offsets}
                lines[#lines + 1] = string.format('ID=%s %s [%s]：%s -> %s', mr.ID, name, rule.key, before, after)
            end
        end
    end
    for _, name in ipairs(order) do
        if rules[name].found == 0 then
            missing = missing + 1
            lines[#lines + 1] = '未匹配：' .. name
        end
    end
    if not dryRun and #plan > 0 then
        -- Keep one reversible snapshot in CE. Never activate records or write Value.
        DCM_UpdateD_Backup = plan
        local applied = {}
        local ok, err = pcall(function()
            for _, change in ipairs(plan) do
                local mr = al.getMemoryRecordByID(change.id)
                assert(mr and mr.Description == change.name and not mr.Active and mr.Address == change.before,
                    '记录在更新前发生变化：' .. change.name)
                applied[#applied + 1] = change
                mr.Address = change.after
                -- CE's Address setter clears OffsetCount; restore the complete chain
                -- immediately, before reading offsets or returning to the UI loop.
                mr.OffsetCount = #change.offsets
                assert(mr.OffsetCount == #change.offsets, '恢复指针层数失败：' .. change.name)
                for j = 0, #change.offsets - 1 do mr.OffsetText[j] = change.offsets[j + 1] end
                assert(mr.Address == change.after and mr.OffsetCount == #change.offsets,
                    '地址回读失败：' .. change.name)
                for j = 0, mr.OffsetCount - 1 do
                    assert(mr.OffsetText[j] == change.offsets[j + 1], '指针偏移发生变化：' .. change.name)
                end
            end
        end)
        if not ok then
            local rollbackErrors = {}
            for i = #applied, 1, -1 do
                local change = applied[i]
                local restored, why = pcall(function()
                    local mr = al.getMemoryRecordByID(change.id)
                    assert(mr and not mr.Active, '记录缺失或已激活')
                    mr.Address = change.before
                    mr.OffsetCount = #change.offsets
                    assert(mr.OffsetCount == #change.offsets, '回滚指针层数失败')
                    for j = 0, #change.offsets - 1 do mr.OffsetText[j] = change.offsets[j + 1] end
                    assert(mr.Address == change.before, '回滚回读失败')
                end)
                if not restored then rollbackErrors[#rollbackErrors + 1] = change.name .. ': ' .. tostring(why) end
            end
            error(tostring(err) .. '\n回滚异常数量=' .. #rollbackErrors .. '\n' .. table.concat(rollbackErrors, '\n'))
        end
    end
    table.insert(lines, 1, string.format('%s：%d；无需修改：%d；跳过：%d；未匹配名称：%d；CE PID=%s',
        dryRun and '预览待更新' or '已更新', #plan, unchanged, skipped, missing, getOpenedProcessID()))
    if not dryRun and #plan > 0 then
        lines[#lines + 1] = '更新只作用于当前 CE 表；需要持久化时请在 CE 保存 CT。旧地址保存在 DCM_UpdateD_Backup。'
    end
    return table.concat(lines, '\r\n')
end
local ok, result = pcall(run)
return 'DCM_UPDATE_D_' .. (ok and 'OK:' or 'ERROR:') .. hex(tostring(result))
