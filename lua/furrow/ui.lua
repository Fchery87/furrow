-- The rack window. Draws the grid from the coordinator's view and turns mouse input into
-- gestures. It never schedules notes.
local wire = require("furrow.wire")
local host = require("furrow.host")
local Coordinator = require("furrow.coordinator")

local ui = {}

local CELL = 22
local GAP = 3
local NAME_WIDTH = 150
local DRAIN_SECONDS = 3

local COLOR = {
  off = 0x3A3F47FF,
  off_alt = 0x2E3239FF,
  on = 0xE8A33DFF,
  on_alt = 0xD08A2AFF,
  playhead = 0xFFFFFFFF,
  missing = 0xE0605AFF,
  muted = 0x8A8F98FF,
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
  local open = true
  local closing_at = nil

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
    if not coordinator then
      ImGui.Text(ctx, "This project has no Furrow rack.")
      if ImGui.Button(ctx, "New rack") then
        new_rack()
      end
      return
    end
    local c = coordinator
    top_bar(c)
    if reaper.EnumProjects(-1) ~= proj then
      ImGui.TextColored(ctx, COLOR.warn, "This rack's project is not the active tab.")
      return
    end
    if c.session and c.session.snapshot then
      for _, lane in ipairs(c:lanes()) do
        lane_row(c, lane)
      end
      drop_zone(c)
    end
    if paint and not ImGui.IsMouseDown(ctx, paint.button) then
      c:finish(paint.gesture)
      paint = nil
    end
    if ImGui.IsWindowFocused(ctx, ImGui.FocusedFlags_RootAndChildWindows)
        and ImGui.IsKeyPressed(ctx, ImGui.Key_Space, false) then
      reaper.Main_OnCommand(40044, 0)
    end
  end

  local function loop()
    if coordinator then
      coordinator:tick()
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
