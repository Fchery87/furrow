-- Measure how engine edits, undo and note cleanup behave across host lifecycle events.
-- Run it in a project built by furrow_probe_layout.lua, with no other project tab open.
-- It edits steps, renames and adds tracks, bypasses the engine, takes it offline, opens and
-- closes a project tab, sets the tempo to 30 BPM and plays. Leave REAPER alone until the
-- console prints the result count. Do not save the project afterwards.
-- Results go to the console and to furrow-lifecycle-results.txt next to the project.

local ENGINE_GMEM = "FurrowProbeEngine"
local RECEIVER_GMEM = "FurrowProbeReceiver"
local OP_TOGGLE = 1
local MIRROR = 100
local REVISION_PARAM = 1
local DELAY_BLOCKS = 20
local TIMEOUT = 2
local FOLDER = "Furrow Probe Folder"
local ENGINE = "Furrow Probe Engine"
local STATE_TRACKCFG = 1
local STATE_FX = 2
local STATE_ALL = -1
local NEW_PROJECT_TAB = 40859
local CLOSE_PROJECT_TAB = 40860
local ALL_NOTES_OFF = 40345
local RECEIVER_EVENTS = 2000000
local RECEIVER_STRIDE = 500000

local lines = {}
local failures = 0

local function log(text)
  lines[#lines + 1] = text
  reaper.ShowConsoleMsg(text .. "\n")
end

local function fail(label, detail)
  failures = failures + 1
  log("FAIL " .. label .. (detail and ("  " .. detail) or ""))
end

local function result(label, value)
  log("RESULT " .. label .. " = " .. tostring(value))
end

local function engine_read(index)
  reaper.gmem_attach(ENGINE_GMEM)
  return reaper.gmem_read(index)
end

local function engine_write(index, value)
  reaper.gmem_attach(ENGINE_GMEM)
  reaper.gmem_write(index, value)
end

local function receiver_read(index)
  reaper.gmem_attach(RECEIVER_GMEM)
  return reaper.gmem_read(index)
end

local function receiver_write(index, value)
  reaper.gmem_attach(RECEIVER_GMEM)
  reaper.gmem_write(index, value)
end

local function wait(seconds)
  local deadline = reaper.time_precise() + seconds
  while reaper.time_precise() < deadline do
    coroutine.yield()
  end
end

local function wait_until(condition, seconds)
  local deadline = reaper.time_precise() + (seconds or TIMEOUT)
  while not condition() do
    if reaper.time_precise() > deadline then
      return false
    end
    coroutine.yield()
  end
  return true
end

local function heartbeat_moves()
  local first = engine_read(1)
  wait(0.3)
  return engine_read(1) > first
end

local function step(index)
  return math.floor(engine_read(MIRROR + index) + 0.5)
end

local function send_toggle(index, delay)
  local seq = math.max(engine_read(2), engine_read(6)) + 1
  engine_write(3, OP_TOGGLE)
  engine_write(4, index)
  engine_write(5, delay)
  engine_write(9, 0)
  engine_write(2, seq)
  return seq
end

local function acked(seq)
  return engine_read(6) >= seq
end

local function find_track(name)
  for index = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, index)
    local _, track_name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
    if track_name == name then
      return track
    end
  end
  return nil
end

local function track_name_by_guid(guid)
  for index = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, index)
    if reaper.GetTrackGUID(track) == guid then
      local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
      return name
    end
  end
  return "(missing)"
end

local function mark_changed()
  local engine = find_track(ENGINE)
  local revision = reaper.TrackFX_GetParam(engine, 0, REVISION_PARAM)
  reaper.TrackFX_SetParam(engine, 0, REVISION_PARAM, revision + 1)
end

local function undo()
  reaper.Undo_DoUndo2(0)
  wait(0.2)
end

local function add_track(label)
  local index = reaper.CountTracks(0)
  reaper.InsertTrackAtIndex(index, true)
  reaper.GetSetMediaTrackInfo_String(reaper.GetTrack(0, index), "P_NAME", label, true)
end

-- L1. A user edit with its own undo point lands while a gesture waits for the engine.
local function test_interleaved(order, index)
  local label = "L1 " .. order
  local folder = find_track(FOLDER)
  local guid = reaper.GetTrackGUID(folder)
  local track_label = "Furrow probe " .. label .. " track"
  local renamed = FOLDER .. " " .. label
  local before = step(index)
  if order == "host first" then
    add_track(track_label)
  end
  local seq = send_toggle(index, DELAY_BLOCKS)
  reaper.GetSetMediaTrackInfo_String(folder, "P_NAME", renamed, true)
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe " .. label .. ": user rename", STATE_TRACKCFG, -1)
  if not wait_until(function() return acked(seq) end) then
    fail(label .. " ack")
    return
  end
  if order == "engine first" then
    add_track(track_label)
  end
  mark_changed()
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe " .. label .. ": gesture", STATE_ALL, -1)
  undo()
  result(label .. " first undo restores the step", step(index) == before)
  result(label .. " first undo removes the gesture's track", find_track(track_label) == nil)
  result(label .. " first undo keeps the user rename", track_name_by_guid(guid) == renamed)
  undo()
  result(label .. " second undo restores the name", track_name_by_guid(guid) == FOLDER)
  result(label .. " second undo leaves no gesture track", find_track(track_label) == nil)
end

-- L2. The engine is bypassed while a command is pending.
local function test_bypass_pending(index)
  local engine = find_track(ENGINE)
  local before = step(index)
  reaper.TrackFX_SetEnabled(engine, 0, false)
  wait(0.2)
  result("L2 heartbeat moves while bypassed", heartbeat_moves())
  local seq = send_toggle(index, 0)
  result("L2 command acknowledged while bypassed", wait_until(function() return acked(seq) end, 1))
  reaper.TrackFX_SetEnabled(engine, 0, true)
  result("L2 command acknowledged after re-enable", wait_until(function() return acked(seq) end))
  wait(0.2)
  result("L2 step toggled once after re-enable", step(index) == 1 - before)
end

-- L3. The engine goes offline and back online after a marked and an unmarked edit.
local function test_offline(index, marked)
  local label = "L3 " .. (marked and "marked" or "unmarked")
  local engine = find_track(ENGINE)
  local before = step(index)
  local seq = send_toggle(index, 0)
  if not wait_until(function() return acked(seq) end) then
    fail(label .. " ack")
    return
  end
  if marked then
    mark_changed()
    reaper.Undo_OnStateChangeEx2(0, "Furrow probe " .. label .. ": toggle step " .. index, STATE_FX, -1)
  end
  reaper.TrackFX_SetOffline(engine, 0, true)
  wait(0.3)
  result(label .. " heartbeat moves while offline", heartbeat_moves())
  reaper.TrackFX_SetOffline(engine, 0, false)
  result(label .. " heartbeat moves after online", wait_until(function() return heartbeat_moves() end))
  wait(0.2)
  result(label .. " edit survives offline and online", step(index) == 1 - before)
  if marked then
    undo()
  end
end

-- L4. A project tab switch while a command is pending.
local function test_tab_switch(index)
  local first_project = reaper.EnumProjects(-1, "")
  local before = step(index)
  local seq = send_toggle(index, DELAY_BLOCKS)
  reaper.Main_OnCommand(NEW_PROJECT_TAB, 0)
  wait(1)
  result("L4 command acknowledged while the engine's tab is in the background", acked(seq))
  reaper.Main_OnCommand(CLOSE_PROJECT_TAB, 0)
  reaper.SelectProjectInstance(first_project)
  result("L4 command acknowledged after returning", wait_until(function() return acked(seq) end))
  wait(0.2)
  result("L4 step toggled once", step(index) == 1 - before)
end

local function lane_events(slot)
  local count = math.floor(receiver_read(10 + slot) + 0.5)
  local events = {}
  for i = 0, count - 1 do
    local at = RECEIVER_EVENTS + slot * RECEIVER_STRIDE + i * 9
    events[#events + 1] = { kind = math.floor(receiver_read(at + 8) + 0.5), note = receiver_read(at + 6) }
  end
  return events
end

local function last_kind(slot)
  local events = lane_events(slot)
  return #events > 0 and events[#events].kind or -1
end

local function wait_for_held_note(slot)
  return wait_until(function() return last_kind(slot) == 1 end, 6)
end

local function cleanup_after(slot, from)
  local events = lane_events(slot)
  local off, cc = false, false
  for i = from + 1, #events do
    off = off or events[i].kind == 0
    cc = cc or events[i].kind == 2
  end
  return off, cc
end

-- L5. A held note when the engine is bypassed, taken offline, or the transport stops.
local function test_cleanup(action)
  local label = "L5 " .. action
  local engine = find_track(ENGINE)
  local slot = 2
  reaper.Main_OnCommand(ALL_NOTES_OFF, 0)
  reaper.SetEditCurPos(0, false, false)
  reaper.OnPlayButton()
  if not wait_for_held_note(slot) then
    reaper.OnStopButton()
    fail(label .. " saw a held note")
    return
  end
  local mark = #lane_events(slot)
  if action == "bypass" then
    reaper.TrackFX_SetEnabled(engine, 0, false)
  elseif action == "offline" then
    reaper.TrackFX_SetOffline(engine, 0, true)
  else
    reaper.OnStopButton()
  end
  wait(0.5)
  local off, cc = cleanup_after(slot, mark)
  result(label .. " note-off reached the receiver", off)
  result(label .. " controller reached the receiver", cc)
  local resumed = #lane_events(slot)
  if action == "bypass" then
    reaper.TrackFX_SetEnabled(engine, 0, true)
  elseif action == "offline" then
    reaper.TrackFX_SetOffline(engine, 0, false)
  end
  wait(0.5)
  if action ~= "stop" then
    local late_off = cleanup_after(slot, resumed)
    result(label .. " note-off after the engine returned", late_off)
  end
  reaper.OnStopButton()
  reaper.Main_OnCommand(ALL_NOTES_OFF, 0)
  wait(0.3)
end

local function main()
  if not find_track(ENGINE) or not find_track(FOLDER) then
    fail("probe layout found")
    return
  end
  if engine_read(0) ~= 1 then
    fail("engine mailbox present", "gmem[0] = " .. engine_read(0))
    return
  end
  reaper.OnStopButton()
  if not heartbeat_moves() then
    fail("engine processes while stopped")
    return
  end
  log("== L1 user edit during a gesture")
  test_interleaved("host first", 0)
  test_interleaved("engine first", 1)
  log("== L2 bypass with a pending command")
  test_bypass_pending(2)
  log("== L3 offline and online")
  test_offline(3, true)
  test_offline(4, false)
  log("== L4 project tab switch with a pending command")
  test_tab_switch(5)
  log("== L5 note cleanup")
  local tempo = reaper.Master_GetTempo()
  reaper.SetCurrentBPM(0, 30, false)
  receiver_write(5, 1)
  test_cleanup("bypass")
  test_cleanup("offline")
  test_cleanup("stop")
  receiver_write(5, 0)
  reaper.SetCurrentBPM(0, tempo, false)
end

local function output_path()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-lifecycle-results.txt"
end

local runner = coroutine.create(main)

local function tick()
  local ok, err = coroutine.resume(runner)
  if not ok then
    fail("script ran without error", tostring(err))
  end
  if coroutine.status(runner) ~= "dead" then
    reaper.defer(tick)
    return
  end
  local count = 0
  for _, line in ipairs(lines) do
    if line:sub(1, 6) == "RESULT" then
      count = count + 1
    end
  end
  log(string.format("%d results, %d failures", count, failures))
  local file = io.open(output_path(), "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg(output_path() .. "\n")
  end
end

tick()
