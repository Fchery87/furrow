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
    if not c:processing() then
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
      if ImGui.MenuItem(ctx, "Delete lane", nil, false, c:processing()) then
        c:remove_lane(lane)
      end
      ImGui.EndPopup(ctx)
    end
    for step = 0, lane.length - 1 do
      ImGui.SameLine(ctx, 0, step == 0 and GAP * 3 or GAP)
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
    if not c:processing() or not ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_AllowWhenBlockedByActiveItem) then
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
    local def = fields.list[edit_field]
    ImGui.Dummy(ctx, GRID_OFFSET - GAP * 3, BAR_HEIGHT)
    for step = 0, lane.length - 1 do
      ImGui.SameLine(ctx, 0, step == 0 and GAP * 3 or GAP)
      ImGui.PushID(ctx, "bar" .. step)
      bar(c, lane, def, step)
      ImGui.PopID(ctx)
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
