-- The rack window. Draws the grid from the coordinator's view and turns mouse input into
-- gestures. It never schedules notes.
local wire = require("furrow.wire")
local host = require("furrow.host")
local Coordinator = require("furrow.coordinator")
local fields = require("furrow.fields")

local ui = {}

local CELL = 22
local GAP = 3
local NAME_WIDTH = 150
local LENGTH_CHANGES = { -16, -1, 1, 16 }
local PAGE_BUTTON_WIDTH = 84
local BAR_HEIGHT = 84
local GRID_OFFSET = 2 * CELL + NAME_WIDTH + 5 * GAP
local DRAIN_SECONDS = 3
local UNDO_COMMAND = 40029
local REDO_COMMAND = 40030

local COLOR = {
  off = 0x3A3F47FF,
  off_alt = 0x2E3239FF,
  on = 0xE8A33DFF,
  on_alt = 0xD08A2AFF,
  playhead = 0xFFFFFFFF,
  missing = 0xE0605AFF,
  muted = 0x8A8F98FF,
  bar_off = 0x555B64FF,
  warn = 0xE8C35AFF,
  drop = 0x2A3A4AFF,
}

local function pick_rack(proj)
  local racks = host.find_racks(proj)
  local selected = reaper.GetSelectedTrack(proj, 0)
  local function guid(track)
    return track and reaper.GetTrackGUID(track)
  end
  local mine = guid(selected)
  local parent = selected and guid(reaper.GetParentTrack(selected))
  for _, rack in ipairs(racks) do
    local folder = guid(rack.rack)
    if mine and (mine == guid(rack.engine) or mine == folder or (folder and parent == folder)) then
      return rack
    end
  end
  return racks[1]
end

local STATUS_TEXT = {
  connecting = "Connecting to SeqCore",
  waiting = "Waiting for SeqCore's first snapshot",
  stalled = "SeqCore is not processing. It may be bypassed, offline, or in a background project tab. Edits are paused.",
  load_error = "SeqCore could not read this rack's saved state. It keeps the state unchanged and plays nothing.",
  offline = "Not connected",
}

function ui.run(ImGui)
  math.randomseed(math.floor(reaper.time_precise() * 1000) % 2147483647)
  local ctx = ImGui.CreateContext("Furrow")
  local proj = reaper.EnumProjects(-1)
  local coordinator = nil
  local paint = nil
  local shaping = nil
  local swing_edit = nil
  local length_shown = nil
  local length_typed = nil
  local swing_shown = nil
  local edit_slot = nil
  local edit_field = 1
  local open = true
  local closing_at = nil
  -- Undo and redo wait here until pending gestures have their undo points.
  local history = nil

  local function connect()
    local rack = pick_rack(proj)
    coordinator = rack and Coordinator.new(proj, rack) or nil
  end
  connect()

  local function new_rack()
    reaper.Undo_BeginBlock2(proj)
    local rack, err = host.create_rack()
    reaper.Undo_EndBlock2(proj, "Furrow: new rack", -1)
    if rack then
      coordinator = Coordinator.new(proj, rack)
    else
      reaper.ShowMessageBox(err, "Furrow", 0)
    end
  end

  local function step_cell(c, lane, step)
    local value = c:step_value(lane.slot, step, wire.F_ON)
    ImGui.InvisibleButton(ctx, "##s" .. step, CELL, CELL, ImGui.ButtonFlags_MouseButtonLeft | ImGui.ButtonFlags_MouseButtonRight)
    local x1, y1 = ImGui.GetItemRectMin(ctx)
    local alt = (step // 4) % 2 == 1
    local color = value == 1 and (alt and COLOR.on_alt or COLOR.on) or (alt and COLOR.off_alt or COLOR.off)
    local draw = ImGui.GetWindowDrawList(ctx)
    ImGui.DrawList_AddRectFilled(draw, x1, y1, x1 + CELL, y1 + CELL, color, 3)
    local playing = c.session:h(wire.H_PLAYING) == 1
    if playing and c.session:playhead(lane.slot) == step then
      ImGui.DrawList_AddRect(draw, x1, y1, x1 + CELL, y1 + CELL, COLOR.playhead, 3, 0, 2)
    end
    if not c:processing() or not c:page_ready() then
      return
    end
    local hovered = ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_AllowWhenBlockedByActiveItem)
    if hovered and not paint then
      local button = nil
      if ImGui.IsMouseClicked(ctx, ImGui.MouseButton_Left) then
        button = ImGui.MouseButton_Left
      elseif ImGui.IsMouseClicked(ctx, ImGui.MouseButton_Right) then
        button = ImGui.MouseButton_Right
      end
      if button then
        local target = button == ImGui.MouseButton_Left and 1 - value or 0
        edit_slot = lane.slot
        paint = {
          button = button,
          value = target,
          done = {},
          gesture = c:begin(target == 1 and "paint steps" or "erase steps"),
        }
      end
    end
    local key = lane.slot * wire.MAX_STEPS + step
    if paint and hovered and ImGui.IsMouseDown(ctx, paint.button) and not paint.done[key] then
      paint.done[key] = true
      if value ~= paint.value then
        c:add(paint.gesture, { { wire.OP_SET_STEP, 0, lane.slot, step, wire.F_ON, paint.value } },
          { { lane.slot, step, wire.F_ON, paint.value } })
      end
    end
  end

  local function toggle(c, lane, field, label, on)
    if on then
      ImGui.PushStyleColor(ctx, ImGui.Col_Button, field == wire.L_MUTE and 0xB0503AFF or 0x3A8F5AFF)
    end
    local clicked = ImGui.Button(ctx, label, CELL, CELL)
    if on then
      ImGui.PopStyleColor(ctx, 1)
    end
    if clicked and c:processing() then
      c:run((on and "un" or "") .. (field == wire.L_MUTE and "mute lane" or "solo lane"),
        { { wire.OP_SET_LANE, lane.slot, field, on and 0 or 1 } })
    end
  end

  local function lane_row(c, lane)
    ImGui.PushID(ctx, "lane" .. lane.slot)
    toggle(c, lane, wire.L_MUTE, "M", lane.mute)
    ImGui.SameLine(ctx, 0, GAP)
    toggle(c, lane, wire.L_SOLO, "S", lane.solo)
    ImGui.SameLine(ctx, 0, GAP)
    local track = c:lane_track(lane)
    local name = track and host.track_name(track) or ("Lane " .. (lane.slot + 1) .. " (missing track)")
    if not track then
      ImGui.PushStyleColor(ctx, ImGui.Col_Text, COLOR.missing)
    elseif lane.mute then
      ImGui.PushStyleColor(ctx, ImGui.Col_Text, COLOR.muted)
    end
    if ImGui.Button(ctx, name .. "##name", NAME_WIDTH, CELL) and c:processing() then
      edit_slot = lane.slot
      c:audition(lane.slot)
    end
    if not track or lane.mute then
      ImGui.PopStyleColor(ctx, 1)
    end
    if ImGui.BeginPopupContextItem(ctx, "lane menu") then
      local ready = c:processing() and not c.tool
      for _, every in ipairs({ 2, 3, 4, 8 }) do
        if ImGui.MenuItem(ctx, "Fill every " .. every, nil, false, ready) then
          c:lane_tool(lane.slot, "fill", every)
        end
      end
      if ImGui.MenuItem(ctx, "Rotate left", nil, false, ready) then
        c:lane_tool(lane.slot, "rotate", -1)
      end
      if ImGui.MenuItem(ctx, "Rotate right", nil, false, ready) then
        c:lane_tool(lane.slot, "rotate", 1)
      end
      if ImGui.MenuItem(ctx, "Invert steps", nil, false, ready) then
        c:lane_tool(lane.slot, "invert")
      end
      if ImGui.MenuItem(ctx, "Clear steps", nil, false, ready) then
        c:lane_tool(lane.slot, "clear")
      end
      ImGui.Separator(ctx)
      if ImGui.MenuItem(ctx, "Delete lane", nil, false, c:processing()) then
        c:remove_lane(lane)
      end
      ImGui.EndPopup(ctx)
    end
    local first = c.page * wire.PAGE_STEPS
    local last = math.min(lane.length, first + wire.PAGE_STEPS) - 1
    if first > last then
      ImGui.SameLine(ctx, 0, GAP * 3)
      ImGui.TextDisabled(ctx, "this lane has " .. lane.length .. " steps")
    end
    for step = first, last do
      ImGui.SameLine(ctx, 0, step == first and GAP * 3 or GAP)
      ImGui.PushID(ctx, step)
      step_cell(c, lane, step)
      ImGui.PopID(ctx)
    end
    ImGui.PopID(ctx)
  end

  local function bar(c, lane, def, step)
    local value = c:step_value(lane.slot, step, def.field)
    ImGui.InvisibleButton(ctx, "##bar", CELL, BAR_HEIGHT, ImGui.ButtonFlags_MouseButtonLeft)
    local x1, y1 = ImGui.GetItemRectMin(ctx)
    local draw = ImGui.GetWindowDrawList(ctx)
    ImGui.DrawList_AddRectFilled(draw, x1, y1, x1 + CELL, y1 + BAR_HEIGHT, COLOR.off, 3)
    local tip = y1 + (1 - fields.fraction(def, value)) * BAR_HEIGHT
    local top, bottom = tip, y1 + BAR_HEIGHT
    if def.centered then
      local middle = y1 + BAR_HEIGHT / 2
      top, bottom = math.min(tip, middle), math.max(tip, middle)
    end
    local on = c:step_value(lane.slot, step, wire.F_ON) == 1
    ImGui.DrawList_AddRectFilled(draw, x1 + 2, top, x1 + CELL - 2, bottom, on and COLOR.on or COLOR.bar_off, 2)
    if not c:processing() or not c:page_ready()
        or not ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_AllowWhenBlockedByActiveItem) then
      return
    end
    local _, my = ImGui.GetMousePos(ctx)
    local target = fields.value_at(def, 1 - (my - y1) / BAR_HEIGHT)
    if ImGui.IsMouseClicked(ctx, ImGui.MouseButton_Left) and not shaping and not paint then
      shaping = { gesture = c:begin("edit " .. def.name:lower()) }
    end
    if shaping and ImGui.IsMouseDown(ctx, ImGui.MouseButton_Left) then
      c:set_field(shaping.gesture, lane.slot, step, def.field, target)
    end
    ImGui.SetTooltip(ctx, def.text(shaping and target or value))
  end

  local function editor(c)
    local lanes = c:lanes()
    local lane = lanes[1]
    for _, candidate in ipairs(lanes) do
      if candidate.slot == edit_slot then
        lane = candidate
      end
    end
    if not lane then
      return
    end
    edit_slot = lane.slot
    local track = c:lane_track(lane)
    ImGui.Separator(ctx)
    ImGui.Text(ctx, "Step editor  " .. (track and host.track_name(track) or ("Lane " .. (lane.slot + 1))))
    for index, def in ipairs(fields.list) do
      ImGui.SameLine(ctx, 0, GAP * 3)
      local active = index == edit_field
      if active then
        ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.on)
      end
      if ImGui.SmallButton(ctx, def.name) then
        edit_field = index
      end
      if active then
        ImGui.PopStyleColor(ctx, 1)
      end
    end
    ImGui.SameLine(ctx, 0, GAP * 6)
    local now = reaper.time_precise()
    local shown = lane.length
    if length_shown and length_shown.slot == lane.slot and now < length_shown.until_time then
      shown = length_shown.value
    end
    ImGui.Text(ctx, "Length " .. shown)
    local function set_length(target)
      target = math.max(1, math.min(wire.MAX_STEPS, target))
      length_typed = nil
      if target ~= shown then
        length_shown = { slot = lane.slot, value = target, until_time = now + 0.5 }
        c:set_length(lane.slot, target)
        shown = target
      end
    end
    for _, change in ipairs(LENGTH_CHANGES) do
      ImGui.SameLine(ctx, 0, GAP)
      if ImGui.SmallButton(ctx, string.format("%+d##length%d", change, change)) and c:processing() then
        set_length(shown + change)
      end
    end
    -- A typed length waits for the Set button, so a half-typed number is never sent.
    if length_typed and length_typed.slot ~= lane.slot then
      length_typed = nil
    end
    ImGui.SameLine(ctx, 0, GAP * 4)
    ImGui.SetNextItemWidth(ctx, 90)
    local edited, typed = ImGui.InputInt(ctx, "##lengthtyped", length_typed and length_typed.value or shown, 0, 0)
    if edited then
      length_typed = { slot = lane.slot, value = typed }
    end
    ImGui.SameLine(ctx, 0, GAP)
    if ImGui.SmallButton(ctx, "Set##lengthset") and length_typed and c:processing() then
      set_length(length_typed.value)
    end
    local def = fields.list[edit_field]
    ImGui.Dummy(ctx, GRID_OFFSET - GAP * 3, BAR_HEIGHT)
    local first = c.page * wire.PAGE_STEPS
    for step = first, math.min(lane.length, first + wire.PAGE_STEPS) - 1 do
      ImGui.SameLine(ctx, 0, step == first and GAP * 3 or GAP)
      ImGui.PushID(ctx, "bar" .. step)
      bar(c, lane, def, step)
      ImGui.PopID(ctx)
    end
  end

  -- Global feel controls. A swing drag is one gesture and one undo point.
  local function feel(c)
    local snap = c.session and c.session.snapshot
    if not snap then
      return
    end
    local now = reaper.time_precise()
    local percent = snap.rack.swing // 100
    if swing_shown and now < swing_shown.until_time then
      percent = swing_shown.value
    end
    ImGui.SetNextItemWidth(ctx, 180)
    local changed, value = ImGui.SliderInt(ctx, "Swing", percent, 0, 100, "%d%%")
    if changed and c:processing() then
      swing_edit = swing_edit or { gesture = c:begin("set swing") }
      swing_shown = { value = value, until_time = now + 0.5 }
      c:add(swing_edit.gesture, { { wire.OP_SET_RACK, wire.R_SWING, value * 100 } })
    end
    ImGui.SameLine(ctx, 0, GAP * 4)
    local cut, cut_value = ImGui.Checkbox(ctx, "Cut swung notes", snap.rack.truncate == 1)
    if cut and c:processing() then
      c:run("set swing cut", { { wire.OP_SET_RACK, wire.R_TRUNCATE, cut_value and 1 or 0 } })
    end
    ImGui.SameLine(ctx, 0, GAP * 4)
    if ImGui.Button(ctx, "Reroll chances") and c:processing() then
      c:run("reroll chances", { { wire.OP_SET_RACK, wire.R_SEED, math.random(0, wire.SEED_MAX) } })
    end
    ImGui.SameLine(ctx, 0, GAP * 2)
    ImGui.TextDisabled(ctx, "seed " .. snap.rack.seed)
    ImGui.Separator(ctx)
  end

  -- One button for each 16 steps of the longest lane. Every lane shows the chosen page.
  local function pager(c)
    local longest = wire.PAGE_STEPS
    for _, lane in ipairs(c:lanes()) do
      longest = math.max(longest, lane.length)
    end
    local pages = (longest + wire.PAGE_STEPS - 1) // wire.PAGE_STEPS
    if c.page >= pages then
      c:set_page(pages - 1)
    end
    if pages < 2 then
      return
    end
    ImGui.Text(ctx, "Steps")
    local per_row = math.max(1, math.floor(ImGui.GetContentRegionAvail(ctx) / PAGE_BUTTON_WIDTH))
    for page = 0, pages - 1 do
      if page % per_row ~= 0 then
        ImGui.SameLine(ctx, 0, GAP * 2)
      end
      local active = page == c.page
      if active then
        ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.on)
      end
      local from = page * wire.PAGE_STEPS + 1
      if ImGui.SmallButton(ctx, string.format("%d-%d##page%d", from, from + wire.PAGE_STEPS - 1, page)) then
        c:set_page(page)
      end
      if active then
        ImGui.PopStyleColor(ctx, 1)
      end
    end
  end

  local function drop_zone(c)
    local w = ImGui.GetContentRegionAvail(ctx)
    ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.drop)
    ImGui.Button(ctx, "Drop audio files here to add lanes", w, 48)
    ImGui.PopStyleColor(ctx, 1)
    if ImGui.BeginDragDropTarget(ctx) then
      local ok, count = ImGui.AcceptDragDropPayloadFiles(ctx)
      if ok and c:processing() then
        for i = 0, count - 1 do
          local got, file = ImGui.GetDragDropPayloadFile(ctx, i)
          if got then
            c:add_sample(file)
          end
        end
      end
      ImGui.EndDragDropTarget(ctx)
    end
  end

  local function top_bar(c)
    local playing = c.session and c.session:h(wire.H_PLAYING) == 1
    ImGui.Text(ctx, string.format("%s   %.2f BPM   Live", playing and "Playing" or "Stopped", reaper.Master_GetTempo()))
    if c.session then
      ImGui.SameLine(ctx)
      ImGui.TextDisabled(ctx, string.format("   slot %d   block %.0f us   free pages %d", c.slot,
        c.session:h(wire.H_BLOCK_MAX_US), c.session:h(wire.H_POOL_FREE)))
    end
    if c.status ~= "ready" then
      ImGui.TextColored(ctx, COLOR.warn, STATUS_TEXT[c.status] or c.status)
    end
    if c.message then
      ImGui.TextColored(ctx, COLOR.warn, c.message)
      ImGui.SameLine(ctx)
      if ImGui.SmallButton(ctx, "Dismiss") then
        c.message = nil
      end
    end
    ImGui.Separator(ctx)
  end

  local function draw()
    if reaper.EnumProjects(-1) ~= proj then
      if coordinator then
        top_bar(coordinator)
      end
      ImGui.TextColored(ctx, COLOR.warn, "This rack's project is not the active tab.")
      return
    end
    if not coordinator then
      ImGui.Text(ctx, "This project has no Furrow rack.")
      if ImGui.Button(ctx, "New rack") then
        new_rack()
      end
      return
    end
    local c = coordinator
    top_bar(c)
    if c.session and c.session.snapshot then
      feel(c)
      pager(c)
      for _, lane in ipairs(c:lanes()) do
        lane_row(c, lane)
      end
      editor(c)
      drop_zone(c)
    end
    if ImGui.IsWindowFocused(ctx, ImGui.FocusedFlags_RootAndChildWindows) then
      if ImGui.IsKeyPressed(ctx, ImGui.Key_Space, false) then
        reaper.Main_OnCommand(40044, 0)
      elseif ImGui.IsKeyPressed(ctx, ImGui.Key_Z, true) and (ImGui.GetKeyMods(ctx) & ImGui.Mod_Ctrl) ~= 0 then
        -- ReaImGui keeps keystrokes for itself, so forward undo and redo to REAPER.
        local redo = (ImGui.GetKeyMods(ctx) & ImGui.Mod_Shift) ~= 0
        history = redo and REDO_COMMAND or UNDO_COMMAND
      end
    end
  end

  local function loop()
    -- A closed project takes its tracks with it, so nothing is left to drain or edit.
    if not reaper.ValidatePtr2(0, proj, "ReaProject*") then
      return
    end
    if coordinator then
      coordinator:tick()
    end
    -- Released here, not in draw(), so a hidden window or another project tab cannot
    -- leave a gesture open.
    if paint and not ImGui.IsMouseDown(ctx, paint.button) then
      coordinator:finish(paint.gesture)
      paint = nil
    end
    if shaping and not ImGui.IsMouseDown(ctx, ImGui.MouseButton_Left) then
      coordinator:finish(shaping.gesture)
      shaping = nil
    end
    if swing_edit and not ImGui.IsMouseDown(ctx, ImGui.MouseButton_Left) then
      coordinator:finish(swing_edit.gesture)
      swing_edit = nil
    end
    if history and not (coordinator and coordinator:busy()) then
      reaper.Main_OnCommand(history, 0)
      history = nil
    end
    if open then
      ImGui.SetNextWindowSize(ctx, 760, 360, ImGui.Cond_FirstUseEver)
      local visible
      visible, open = ImGui.Begin(ctx, "Furrow", true)
      if visible then
        draw()
        ImGui.End(ctx)
      end
      if not open then
        if paint then
          coordinator:finish(paint.gesture)
          paint = nil
        end
        if shaping then
          coordinator:finish(shaping.gesture)
          shaping = nil
        end
        if swing_edit then
          coordinator:finish(swing_edit.gesture)
          swing_edit = nil
        end
        closing_at = reaper.time_precise()
      end
    end
    -- A closed window keeps the coordinator running until its gestures settle, so a
    -- pending edit still gets its host changes and undo point.
    if not open and (not coordinator or not coordinator:busy()
        or reaper.time_precise() - closing_at > DRAIN_SECONDS) then
      return
    end
    reaper.defer(loop)
  end

  reaper.defer(loop)
end

return ui
