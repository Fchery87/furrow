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
local FIELD_ROW_BREAK = 7
local GRID_OFFSET = 2 * CELL + NAME_WIDTH + 5 * GAP
local DRAIN_SECONDS = 3
local NOTICE_SECONDS = 5
local NOTICE_FADE = 0.4
-- The quietest step keeps this share of the full colour, so it still reads as on.
local VELOCITY_FLOOR = 0.4
local PLAYHEAD_REACH = 2
local UNDO_COMMAND = 40029
local REDO_COMMAND = 40030

local COLOR = {
  off = 0x3A3F47FF,
  off_alt = 0x2E3239FF,
  on = 0xE8A33DFF,
  on_alt = 0xD08A2AFF,
  on_text = 0x1E1E1EFF,
  selected = 0x4F5B6EFF,
  hover = 0xFFFFFF55,
  press = 0x00000040,
  notice = 0x9FB7D0FF,
  playhead = 0xFFFFFFFF,
  missing = 0xE0605AFF,
  muted = 0x8A8F98FF,
  bar_off = 0x555B64FF,
  warn = 0xE8C35AFF,
  drop = 0x2A3A4AFF,
  playhead_wash = 0xFFFFFF30,
  selected_row = 0xFFFFFF0D,
}

-- A colour with its red, green and blue scaled by f and its alpha kept.
function ui.shade(color, f)
  local r = math.floor(((color >> 24) & 0xFF) * f + 0.5)
  local g = math.floor(((color >> 16) & 0xFF) * f + 0.5)
  local b = math.floor(((color >> 8) & 0xFF) * f + 0.5)
  return (r << 24) | (g << 16) | (b << 8) | (color & 0xFF)
end

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

local AUDIO_FILTER = "Audio files\0*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg\0All files\0*.*\0\0"

-- Asks for audio files and returns their paths, empty when cancelled. The js_ReaScriptAPI
-- dialog picks several at once. REAPER's own dialog picks one.
function ui.browse_files()
  if reaper.JS_Dialog_BrowseForOpenFiles then
    local ok, names = reaper.JS_Dialog_BrowseForOpenFiles("Add audio files to Furrow", "", "", AUDIO_FILTER, true)
    if ok ~= 1 or not names or names == "" then
      return {}
    end
    -- One file comes back as its full path. Several come back as the folder, then each name,
    -- separated by zero bytes.
    local parts = {}
    for part in names:gmatch("[^\0]+") do
      parts[#parts + 1] = part
    end
    if #parts == 1 then
      return parts
    end
    local folder = parts[1]:gsub("[/\\]$", "")
    local sep = folder:find("\\", 1, true) and "\\" or "/"
    local files = {}
    for i = 2, #parts do
      files[#files + 1] = folder .. sep .. parts[i]
    end
    return files
  end
  local ok, file = reaper.GetUserFileNameForRead("", "Add an audio file to Furrow", "")
  if ok and file and file ~= "" then
    return { file }
  end
  return {}
end

function ui.run(ImGui)
  math.randomseed(math.floor(reaper.time_precise() * 1000) % 2147483647)
  local ctx = ImGui.CreateContext("Furrow")
  local proj = reaper.EnumProjects(-1)
  local coordinator = nil
  local paint = nil
  local shaping = nil
  local swing_edit = nil
  local length_edit = nil
  local length_shown = nil
  local length_typed = nil
  local rename_typed = nil
  local swing_shown = nil
  local lane_shown = {}
  local edit_slot = nil
  local edit_field = 1
  local open = true
  local closing_at = nil
  local browse_requested = false
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

  -- Explains the item just drawn when the mouse rests on it. The first tooltip waits a
  -- moment, so sweeping across the window does not flash them. ImGui shares that delay, so
  -- the next one opens at once.
  local function tooltip(text)
    if ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_DelayNormal) then
      ImGui.SetTooltip(ctx, text)
    end
  end

  -- The space before a step cell or bar. A wider gap marks each group of 16, so long rows
  -- read in bars.
  local function step_gap(step, first)
    if step == first or step % wire.PAGE_STEPS == 0 then
      return GAP * 3
    end
    return GAP
  end

  local function step_cell(c, lane, step)
    local value = c:step_value(lane.slot, step, wire.F_ON)
    ImGui.InvisibleButton(ctx, "##s" .. step, CELL, CELL, ImGui.ButtonFlags_MouseButtonLeft | ImGui.ButtonFlags_MouseButtonRight)
    local x1, y1 = ImGui.GetItemRectMin(ctx)
    local alt = (step // 4) % 2 == 1
    local color
    if value == 1 then
      -- Louder steps are brighter, so the row shows its dynamics without the editor.
      local velocity = c:step_value(lane.slot, step, wire.F_VEL) or 100
      color = ui.shade(alt and COLOR.on_alt or COLOR.on, VELOCITY_FLOOR + (1 - VELOCITY_FLOOR) * velocity / 127)
    else
      color = alt and COLOR.off_alt or COLOR.off
    end
    local draw = ImGui.GetWindowDrawList(ctx)
    ImGui.DrawList_AddRectFilled(draw, x1, y1, x1 + CELL, y1 + CELL, color, 3)
    local playing = c.session:h(wire.H_PLAYING) == 1
    if playing and c.session:playhead(lane.slot) == step then
      -- The wash reaches into the row gaps, so lanes on the same step read as one column.
      ImGui.DrawList_AddRectFilled(draw, x1, y1 - PLAYHEAD_REACH, x1 + CELL, y1 + CELL + PLAYHEAD_REACH,
        COLOR.playhead_wash)
      ImGui.DrawList_AddRect(draw, x1, y1, x1 + CELL, y1 + CELL, COLOR.playhead, 3, 0, 2)
    end
    if not c:processing() or not c:page_ready() then
      return
    end
    local hovered = ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_AllowWhenBlockedByActiveItem)
    if ImGui.IsItemActive(ctx) then
      ImGui.DrawList_AddRectFilled(draw, x1, y1, x1 + CELL, y1 + CELL, COLOR.press, 3)
    elseif hovered and not paint then
      ImGui.DrawList_AddRect(draw, x1, y1, x1 + CELL, y1 + CELL, COLOR.hover, 3, 0, 1)
    end
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
          cells = {},
          gesture = c:begin(target == 1 and "paint steps" or "erase steps"),
        }
      end
    end
    -- The sweep after the grid decides which cells the drag crossed.
    if paint then
      paint.cells[#paint.cells + 1] = { x1, y1, x1 + CELL, y1 + CELL, lane.slot, step, value }
    end
  end

  -- The points from the last mouse position to this one, every half cell, so a fast drag
  -- still lands at least once in every cell it crossed.
  local function drag_path(drag)
    local mx, my = ImGui.GetMousePos(ctx)
    if not mx then
      return {}
    end
    local lx, ly = drag.x or mx, drag.y or my
    drag.x, drag.y = mx, my
    local n = math.max(1, math.ceil(math.max(math.abs(mx - lx), math.abs(my - ly)) / (CELL / 2)))
    local points = {}
    for i = 0, n do
      points[#points + 1] = { lx + (mx - lx) * i / n, ly + (my - ly) * i / n }
    end
    return points, math.min(lx, mx), math.max(lx, mx), math.min(ly, my), math.max(ly, my)
  end

  -- Paints every cell the mouse crossed since the last frame.
  local function sweep_paint(c)
    if not paint or not ImGui.IsMouseDown(ctx, paint.button) then
      return
    end
    local points, left, right, top, bottom = drag_path(paint)
    for _, cell in ipairs(paint.cells) do
      if cell[3] >= left and cell[1] <= right and cell[4] >= top and cell[2] <= bottom then
        for _, p in ipairs(points) do
          if p[1] >= cell[1] and p[1] < cell[3] and p[2] >= cell[2] and p[2] < cell[4] then
            local slot, step, value = cell[5], cell[6], cell[7]
            local key = slot * wire.MAX_STEPS + step
            if not paint.done[key] then
              paint.done[key] = true
              if value ~= paint.value then
                c:add(paint.gesture, { { wire.OP_SET_STEP, c.pattern or 0, slot, step, wire.F_ON, paint.value } },
                  { { slot, step, wire.F_ON, paint.value } })
              end
            end
            break
          end
        end
      end
    end
    paint.cells = {}
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
    local selected = lane.slot == edit_slot
    -- A faint band behind the whole row ties it to the step editor below.
    local rx, ry = ImGui.GetCursorScreenPos(ctx)
    if selected and rx then
      local _, span = c:view()
      local first = c.page * span
      local count = math.max(0, math.min(lane.length, first + span) - first)
      local width = GRID_OFFSET + count * (CELL + GAP) + (count // wire.PAGE_STEPS) * 2 * GAP
      ImGui.DrawList_AddRectFilled(ImGui.GetWindowDrawList(ctx), rx - GAP, ry - 1, rx + width, ry + CELL + 1,
        COLOR.selected_row, 4)
    end
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
    if selected then
      ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.selected)
    end
    -- A click selects the lane for editing. Hearing it is deliberate, so selecting during
    -- playback stays quiet.
    local clicked = ImGui.Button(ctx, name .. "##name", NAME_WIDTH, CELL)
    local alt = ((ImGui.GetKeyMods(ctx) or 0) & ImGui.Mod_Alt) ~= 0
    local hear = (clicked and alt)
      or (ImGui.IsItemHovered(ctx) and ImGui.IsMouseDoubleClicked(ctx, ImGui.MouseButton_Left))
    if (clicked or hear) and c:processing() then
      edit_slot = lane.slot
      if hear then
        c:audition(lane.slot)
      end
    end
    if selected then
      ImGui.PopStyleColor(ctx, 1)
    end
    if not track or lane.mute then
      ImGui.PopStyleColor(ctx, 1)
    end
    tooltip("Click to edit its steps. Double-click or Alt-click to hear it. Right-click for more.")
    if ImGui.BeginPopupContextItem(ctx, "lane menu") then
      local ready = c:processing() and not c.tool
      if ImGui.MenuItem(ctx, "Hear sample", nil, false, c:processing()) then
        c:audition(lane.slot)
      end
      ImGui.Separator(ctx)
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
    local _, span = c:view()
    local first = c.page * span
    local last = math.min(lane.length, first + span) - 1
    if first > last then
      ImGui.SameLine(ctx, 0, GAP * 3)
      ImGui.TextDisabled(ctx, "this lane has " .. lane.length .. " steps")
    end
    for step = first, last do
      ImGui.SameLine(ctx, 0, step_gap(step, first))
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
    if c.session:h(wire.H_PLAYING) == 1 and c.session:playhead(lane.slot) == step then
      ImGui.DrawList_AddRectFilled(draw, x1, y1, x1 + CELL, y1 + BAR_HEIGHT, COLOR.playhead_wash, 3)
    end
    if not c:processing() or not c:page_ready() then
      return
    end
    -- The sweep after the bars sets every bar the drag crossed, from the mouse height.
    if shaping then
      shaping.bars[#shaping.bars + 1] = { x1, x1 + CELL, y1, lane.slot, step }
    end
    if not ImGui.IsItemHovered(ctx, ImGui.HoveredFlags_AllowWhenBlockedByActiveItem) then
      return
    end
    if not shaping then
      ImGui.DrawList_AddRect(draw, x1, y1, x1 + CELL, y1 + BAR_HEIGHT, COLOR.hover, 3, 0, 1)
    end
    local _, my = ImGui.GetMousePos(ctx)
    local target = fields.value_at(def, 1 - (my - y1) / BAR_HEIGHT)
    if ImGui.IsMouseClicked(ctx, ImGui.MouseButton_Left) and not shaping and not paint then
      shaping = { gesture = c:begin("edit " .. def.name:lower()), field = def.field, def = def,
        bars = { { x1, x1 + CELL, y1, lane.slot, step } } }
    end
    ImGui.SetTooltip(ctx, def.text(shaping and target or value))
  end

  -- Sets every bar the mouse crossed since the last frame. A bar takes the height the path
  -- had over it, so dragging above or below the bars still sets the top or bottom value.
  local function sweep_bars(c)
    if not shaping or not ImGui.IsMouseDown(ctx, ImGui.MouseButton_Left) then
      return
    end
    local points, left, right = drag_path(shaping)
    for _, b in ipairs(shaping.bars) do
      if b[2] >= left and b[1] <= right then
        local over = nil
        for _, p in ipairs(points) do
          if p[1] >= b[1] and p[1] < b[2] then
            over = p
          end
        end
        if over then
          local target = fields.value_at(shaping.def, 1 - (over[2] - b[3]) / BAR_HEIGHT)
          c:set_field(shaping.gesture, b[4], b[5], shaping.field, target)
        end
      end
    end
    shaping.bars = {}
  end

  -- Settings that belong to the selected lane. A lane swing drag shares the swing gesture.
  local LANE_STEPPERS = {
    { key = "bend", label = "Bend range", field = wire.L_BEND, low = 1, high = 48, unit = " st" },
    { key = "mod_a", label = "Mod A CC", field = wire.L_MOD_A, low = 0, high = 127, unit = "" },
    { key = "mod_b", label = "Mod B CC", field = wire.L_MOD_B, low = 0, high = 127, unit = "" },
  }

  -- A slider for one lane field. The shown value holds briefly after a change so the slider
  -- does not jump back before the snapshot catches up.
  local function lane_slider(c, lane, spec)
    local now = reaper.time_precise()
    local value = lane[spec.key] // spec.scale
    local held = lane_shown[spec.key]
    if held and held.slot == lane.slot and now < held.until_time then
      value = held.value
    end
    ImGui.SetNextItemWidth(ctx, 140)
    local changed, new = ImGui.SliderInt(ctx, spec.label, value, spec.low, spec.high, spec.format)
    if changed and c:processing() then
      swing_edit = swing_edit or { gesture = c:begin("set " .. spec.name) }
      lane_shown[spec.key] = { slot = lane.slot, value = new, until_time = now + 0.5 }
      c:add(swing_edit.gesture, { { wire.OP_SET_LANE, lane.slot, spec.field, new * spec.scale } })
    end
  end

  local LANE_SLIDERS = {
    { key = "swing", name = "lane swing", label = "Lane swing", field = wire.L_SWING, scale = 100, low = 0, high = 100, format = "%d%%" },
    { key = "volume", name = "lane volume", label = "Volume", field = wire.L_VOLUME, scale = 100, low = 0, high = 100, format = "%d%%" },
    { key = "pan", name = "lane pan", label = "Pan##lane", field = wire.L_PAN, scale = 100, low = -100, high = 100, format = "%d" },
  }

  local function lane_settings(c, lane)
    for index, spec in ipairs(LANE_SLIDERS) do
      if index > 1 then
        ImGui.SameLine(ctx, 0, GAP * 4)
      end
      lane_slider(c, lane, spec)
    end
    for index, stepper in ipairs(LANE_STEPPERS) do
      local current = lane[stepper.key]
      if index > 1 then
        ImGui.SameLine(ctx, 0, GAP * 4)
      end
      ImGui.Text(ctx, stepper.label .. " " .. current .. stepper.unit)
      for _, change in ipairs({ -1, 1 }) do
        ImGui.SameLine(ctx, 0, GAP)
        local label = (change < 0 and "-" or "+") .. "##" .. stepper.key
        if ImGui.SmallButton(ctx, label) and c:processing() then
          local target = math.max(stepper.low, math.min(stepper.high, current + change))
          if target ~= current then
            c:run("set " .. stepper.label:lower(), { { wire.OP_SET_LANE, lane.slot, stepper.field, target } })
          end
        end
      end
    end
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
    ImGui.Dummy(ctx, 0, GAP * 2)
    ImGui.Separator(ctx)
    ImGui.Text(ctx, track and host.track_name(track) or ("Lane " .. (lane.slot + 1)))

    local now = reaper.time_precise()
    local shown = lane.length
    if length_shown and length_shown.slot == lane.slot and now < length_shown.until_time then
      shown = length_shown.value
    end
    local function set_length(target)
      target = math.max(1, math.min(wire.MAX_STEPS, target))
      length_typed = nil
      if target ~= shown then
        length_shown = { slot = lane.slot, value = target, until_time = now + 0.5 }
        c:set_length(lane.slot, target)
        shown = target
      end
    end
    -- A held button repeats. The whole hold is one gesture, so it is one undo point.
    local function length_button(change)
      ImGui.PushButtonRepeat(ctx, true)
      local pressed = ImGui.SmallButton(ctx, string.format("%+d##length%d", change, change))
      ImGui.PopButtonRepeat(ctx)
      if not pressed or not c:processing() then
        return
      end
      local target = math.max(1, math.min(wire.MAX_STEPS, shown + change))
      if target == shown then
        return
      end
      length_typed = nil
      length_edit = length_edit or { gesture = c:begin("set length") }
      length_shown = { slot = lane.slot, value = target, until_time = now + 0.5 }
      c:add(length_edit.gesture, { { wire.OP_SET_LENGTH, c.pattern or 0, lane.slot, target } })
      shown = target
    end
    ImGui.SameLine(ctx, 0, GAP * 6)
    ImGui.TextDisabled(ctx, "Length")
    ImGui.SameLine(ctx, 0, GAP * 2)
    length_button(-1)
    ImGui.SameLine(ctx, 0, GAP * 2)
    ImGui.Text(ctx, tostring(shown))
    ImGui.SameLine(ctx, 0, GAP * 2)
    length_button(1)

    -- The everyday fields are buttons. The rest wait in a list, so the row stays short.
    for index, def in ipairs(fields.list) do
      if index < FIELD_ROW_BREAK then
        if index > 1 then
          ImGui.SameLine(ctx, 0, GAP)
        end
        local active = index == edit_field
        if active then
          ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.on)
          ImGui.PushStyleColor(ctx, ImGui.Col_Text, COLOR.on_text)
        end
        if ImGui.SmallButton(ctx, def.name) then
          edit_field = index
        end
        if active then
          ImGui.PopStyleColor(ctx, 2)
        end
      end
    end
    ImGui.SameLine(ctx, 0, GAP * 3)
    local more = edit_field >= FIELD_ROW_BREAK and fields.list[edit_field].name or "More"
    ImGui.SetNextItemWidth(ctx, 110)
    if ImGui.BeginCombo(ctx, "##morefields", more) then
      for index = FIELD_ROW_BREAK, #fields.list do
        if ImGui.Selectable(ctx, fields.list[index].name .. "##field" .. index, index == edit_field) then
          edit_field = index
        end
      end
      ImGui.EndCombo(ctx)
    end

    local def = fields.list[edit_field]
    ImGui.Dummy(ctx, GRID_OFFSET - GAP * 3, BAR_HEIGHT)
    local _, span = c:view()
    local first = c.page * span
    for step = first, math.min(lane.length, first + span) - 1 do
      ImGui.SameLine(ctx, 0, step_gap(step, first))
      ImGui.PushID(ctx, "bar" .. step)
      bar(c, lane, def, step)
      ImGui.PopID(ctx)
    end
    sweep_bars(c)

    -- Settings a lane rarely changes stay folded away until asked for.
    if ImGui.CollapsingHeader(ctx, "Lane settings##lanesettings") then
      ImGui.TextDisabled(ctx, "Length")
      for _, change in ipairs(LENGTH_CHANGES) do
        if change ~= -1 and change ~= 1 then
          ImGui.SameLine(ctx, 0, GAP * 2)
          length_button(change)
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
      lane_settings(c, lane)
    end
  end

  -- The pattern picker. A chosen pattern shows at once. While playing the engine switches on
  -- the next measure, and while stopped it switches at the cursor.
  local function patterns_row(c)
    local list = c:patterns()
    local current = c.pattern or 0
    local name = ""
    for _, entry in ipairs(list) do
      if entry.index == current then
        name = entry.name
      end
    end
    ImGui.SetNextItemWidth(ctx, 180)
    if ImGui.BeginCombo(ctx, "##pattern", name) then
      for _, entry in ipairs(list) do
        if ImGui.Selectable(ctx, entry.name .. "##pattern" .. entry.index, entry.index == current) then
          c:select_pattern(entry.index)
        end
      end
      ImGui.EndCombo(ctx)
    end
    -- New, clone, clear and rename are occasional, so they live in the pattern menu.
    ImGui.SameLine(ctx, 0, GAP)
    if ImGui.Button(ctx, "...##patternmenu") then
      ImGui.OpenPopup(ctx, "pattern menu")
    end
    tooltip("New, clone, clear or rename the pattern")
    if ImGui.BeginPopup(ctx, "pattern menu") then
      if ImGui.MenuItem(ctx, "New pattern##pattern") then
        c:new_pattern()
      end
      if ImGui.MenuItem(ctx, "Clone pattern##pattern") then
        c:clone_pattern()
      end
      if ImGui.MenuItem(ctx, "Clear pattern##pattern") then
        c:clear_pattern(current)
      end
      ImGui.Separator(ctx)
      if rename_typed and rename_typed.pattern ~= current then
        rename_typed = nil
      end
      ImGui.SetNextItemWidth(ctx, 160)
      local edited, text = ImGui.InputText(ctx, "##patternname", rename_typed and rename_typed.text or name)
      if edited then
        rename_typed = { pattern = current, text = text }
      end
      ImGui.SameLine(ctx, 0, GAP)
      if ImGui.SmallButton(ctx, "Rename##pattern") and rename_typed then
        c:rename_pattern(current, rename_typed.text)
        rename_typed = nil
        ImGui.CloseCurrentPopup(ctx)
      end
      ImGui.EndPopup(ctx)
    end
    local queued = c.session:h(wire.H_PENDING_PATTERN)
    if queued >= 0 and queued ~= current then
      for _, entry in ipairs(list) do
        if entry.index == queued then
          ImGui.SameLine(ctx, 0, GAP * 3)
          ImGui.TextDisabled(ctx, "next: " .. entry.name)
        end
      end
    end
  end

  -- Global feel controls, shown in the Groove popup. A swing drag is one gesture and one
  -- undo point.
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
    local cut, cut_value = ImGui.Checkbox(ctx, "Cut swung notes", snap.rack.truncate == 1)
    if cut and c:processing() then
      c:run("set swing cut", { { wire.OP_SET_RACK, wire.R_TRUNCATE, cut_value and 1 or 0 } })
    end
    ImGui.Separator(ctx)
    if ImGui.Button(ctx, "Reroll chances") and c:processing() then
      c:run("reroll chances", { { wire.OP_SET_RACK, wire.R_SEED, math.random(0, wire.SEED_MAX) } })
    end
    ImGui.SameLine(ctx, 0, GAP * 2)
    ImGui.TextDisabled(ctx, "seed " .. snap.rack.seed)
  end

  -- A two-button switch whose lit half shows what is sounding.
  local function segment(label, lit, hint)
    if lit then
      ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.on)
      ImGui.PushStyleColor(ctx, ImGui.Col_Text, COLOR.on_text)
    end
    local clicked = ImGui.Button(ctx, label)
    if lit then
      ImGui.PopStyleColor(ctx, 2)
    end
    tooltip(hint)
    return clicked and not lit
  end

  -- What plays. Live is SeqCore's pattern. Items is the committed MIDI items, which sound
  -- only while the rack is in Items mode. Commit renders the shown pattern at the edit cursor.
  local function playback(c)
    local items = c.session.snapshot.rack.mode == 1
    if segment("Live##mode", not items, "Play the pattern live from SeqCore") then
      c:set_mode(0)
    end
    ImGui.SameLine(ctx, 0, 0)
    if segment("Items##mode", items, "Play the committed MIDI items instead") then
      c:set_mode(1)
    end
    ImGui.SameLine(ctx, 0, GAP * 2)
    if ImGui.Button(ctx, "Commit##commit") then
      c:commit_pattern()
    end
    tooltip("Render the shown pattern to MIDI items at the edit cursor")
  end

  local function groove(c)
    if ImGui.Button(ctx, "Groove##groove") then
      ImGui.OpenPopup(ctx, "groove")
    end
    tooltip("Swing and chance settings for the whole rack")
    if ImGui.BeginPopup(ctx, "groove") then
      feel(c)
      ImGui.EndPopup(ctx)
    end
  end

  -- The transport readout. Engine diagnostics wait in its tooltip.
  local function transport(c)
    local playing = c.session:h(wire.H_PLAYING) == 1
    ImGui.TextDisabled(ctx, string.format("%s  %.0f BPM", playing and "Playing" or "Stopped", reaper.Master_GetTempo()))
    tooltip(string.format("slot %d   block %.0f us   free pages %d", c.slot,
      c.session:h(wire.H_BLOCK_MAX_US), c.session:h(wire.H_POOL_FREE)))
  end

  -- One row: the pattern and its menu, what plays, the groove popup and the transport.
  local function header(c)
    patterns_row(c)
    ImGui.SameLine(ctx, 0, GAP * 6)
    playback(c)
    ImGui.SameLine(ctx, 0, GAP * 6)
    groove(c)
    ImGui.SameLine(ctx, 0, GAP * 6)
    transport(c)
    ImGui.Dummy(ctx, 0, GAP)
  end

  -- One button for each 16 steps of the longest lane. Every lane shows the chosen page.
  -- Pages appear only when the lanes in use are too long to fit one snapshot window.
  local function pager(c)
    local _, span, pages = c:view()
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
      local from = page * span + 1
      if ImGui.SmallButton(ctx, string.format("%d-%d##page%d", from, math.min(from + span - 1, wire.MAX_STEPS), page)) then
        c:set_page(page)
      end
      if active then
        ImGui.PopStyleColor(ctx, 1)
      end
    end
  end

  -- Large while the rack is empty, and a slim strip once it has lanes. A click opens the
  -- file browser after the frame ends, because the dialog blocks until it closes.
  local function drop_zone(c, empty)
    local w = ImGui.GetContentRegionAvail(ctx)
    ImGui.Dummy(ctx, 0, GAP * 2)
    ImGui.PushStyleColor(ctx, ImGui.Col_Button, COLOR.drop)
    local label = empty and "Drop audio files here, or click to browse##addlanes"
      or "+ Add audio files: click to browse, or drop them here##addlanes"
    if ImGui.Button(ctx, label, w, empty and 96 or CELL) and c:processing() then
      browse_requested = true
    end
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

  -- Shows only what needs attention: a connection problem or a message to dismiss.
  local function status_line(c)
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
    -- A confirmation fades on its own and holds while the mouse rests on it. A problem
    -- above it stays until dismissed, and hides the confirmation meanwhile.
    local notice = c.notice
    if notice and not c.message then
      local now = reaper.time_precise()
      local age = now - notice.at
      if age >= NOTICE_SECONDS then
        c.notice = nil
      else
        ImGui.PushStyleVar(ctx, ImGui.StyleVar_Alpha, math.min(1, (NOTICE_SECONDS - age) / NOTICE_FADE))
        ImGui.TextColored(ctx, COLOR.notice, notice.text)
        ImGui.PopStyleVar(ctx)
        if ImGui.IsItemHovered(ctx) then
          notice.at = math.max(notice.at, now - (NOTICE_SECONDS - NOTICE_FADE))
        end
      end
    end
  end

  local function draw()
    if reaper.EnumProjects(-1) ~= proj then
      if coordinator then
        status_line(coordinator)
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
    if c.session and c.session.snapshot then
      header(c)
      status_line(c)
      ImGui.Separator(ctx)
      ImGui.Dummy(ctx, 0, GAP)
      pager(c)
      local lanes = c:lanes()
      for _, lane in ipairs(lanes) do
        lane_row(c, lane)
      end
      sweep_paint(c)
      editor(c)
      drop_zone(c, #lanes == 0)
    else
      status_line(c)
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
    if length_edit and not ImGui.IsMouseDown(ctx, ImGui.MouseButton_Left) then
      coordinator:finish(length_edit.gesture)
      length_edit = nil
    end
    if history and not (coordinator and coordinator:busy()) then
      reaper.Main_OnCommand(history, 0)
      history = nil
    end
    if open then
      ImGui.SetNextWindowSize(ctx, 760, 360, ImGui.Cond_FirstUseEver)
      local visible
      visible, open = ImGui.Begin(ctx, "Furrow", true, ImGui.WindowFlags_HorizontalScrollbar)
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
        if length_edit then
          coordinator:finish(length_edit.gesture)
          length_edit = nil
        end
        closing_at = reaper.time_precise()
      end
    end
    if browse_requested then
      browse_requested = false
      for _, file in ipairs(ui.browse_files()) do
        if coordinator and coordinator:processing() then
          coordinator:add_sample(file)
        end
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
