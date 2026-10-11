-- Test whether REAPER's native undo restores a step that the audio thread applied.
-- Run it in a project built by furrow_probe_layout.lua, with no other probe project open.
-- It changes steps, renames the folder track and adds tracks, then undoes and redoes them.
-- Results go to the console and to furrow-undo-results.txt next to the project.
--
-- Each gesture marks the engine's state as changed in one of three ways before its undo point.
-- none: no mark. REAPER 7.82 created no FX undo point and never restored the steps.
-- lua: Lua sets the engine's hidden State revision parameter after the acknowledgement.
-- engine: the engine bumps that parameter itself with sliderchange when it applies the command.

local GMEM = "FurrowProbeEngine"
local MAILBOX_VERSION = 1
local OP_TOGGLE = 1
local MIRROR = 100
local REVISION_PARAM = 1
local DELAY_BLOCKS = 20
local TIMEOUT = 3
local FOLDER = "Furrow Probe Folder"
local ENGINE = "Furrow Probe Engine"
local STATE_TRACKCFG = 1
local STATE_FX = 2
local STATE_ALL = -1

local lines = {}
local failures = 0

local function log(text)
  lines[#lines + 1] = text
  reaper.ShowConsoleMsg(text .. "\n")
end

local function check(label, ok, detail)
  if not ok then
    failures = failures + 1
  end
  log(string.format("%s %s%s", ok and "PASS" or "FAIL", label, detail and ("  " .. detail) or ""))
  return ok
end

local function read(index)
  return reaper.gmem_read(index)
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

local function settle(blocks)
  local target = read(1) + (blocks or 3)
  return wait_until(function()
    return read(1) >= target
  end)
end

local function step(index)
  return math.floor(read(MIRROR + index) + 0.5)
end

local function send_toggle(index, delay, method)
  local seq = math.max(read(2), read(6)) + 1
  reaper.gmem_write(3, OP_TOGGLE)
  reaper.gmem_write(4, index)
  reaper.gmem_write(5, delay)
  reaper.gmem_write(9, method == "engine" and 1 or 0)
  reaper.gmem_write(2, seq)
  return seq
end

local function acked(seq)
  return read(6) >= seq
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

-- Undo can replace track objects, so a test holds the GUID and looks the track up each time.
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

local function mark_changed(method)
  if method ~= "lua" then
    return
  end
  local engine = find_track(ENGINE)
  local revision = reaper.TrackFX_GetParam(engine, 0, REVISION_PARAM)
  reaper.TrackFX_SetParam(engine, 0, REVISION_PARAM, revision + 1)
end

local function last_undo()
  return reaper.Undo_CanUndo2(0) or ""
end

local function add_point(label, name, states)
  reaper.Undo_OnStateChangeEx2(0, name, states, -1)
  return check(label .. " undo point", last_undo() == name, "top of undo is '" .. last_undo() .. "'")
end

local function undo()
  reaper.Undo_DoUndo2(0)
  return settle()
end

local function redo()
  reaper.Undo_DoRedo2(0)
  return settle()
end

local function track_count()
  return reaper.CountTracks(0)
end

local function test_toggle(pass, method, index)
  local label = string.format("%s A-%s", pass, method)
  local before = step(index)
  local restores = read(8)
  local seq = send_toggle(index, 0, method)
  check(label .. " ack", wait_until(function() return acked(seq) end), "sequence " .. seq)
  settle()
  check(label .. " applied", step(index) == 1 - before, "step " .. step(index))
  mark_changed(method)
  if not add_point(label, "Furrow probe " .. label .. ": toggle step " .. index, STATE_FX) then
    return
  end
  undo()
  check(label .. " undo restores step", step(index) == before, "step " .. step(index))
  check(label .. " undo reached the engine", read(8) > restores, "restore count " .. read(8))
  redo()
  check(label .. " redo reapplies step", step(index) == 1 - before, "step " .. step(index))
  undo()
end

local function test_unrelated_edit(pass, method, index)
  local label = string.format("%s B-%s", pass, method)
  local folder = find_track(FOLDER)
  if not check(label .. " folder track found", folder ~= nil) then
    return
  end
  local guid = reaper.GetTrackGUID(folder)
  local renamed = FOLDER .. " " .. label
  local before = step(index)
  local seq = send_toggle(index, DELAY_BLOCKS, method)
  reaper.GetSetMediaTrackInfo_String(folder, "P_NAME", renamed, true)
  if not add_point(label .. " rename", "Furrow probe " .. label .. ": unrelated rename", STATE_TRACKCFG) then
    return
  end
  check(label .. " command still pending at the rename", not acked(seq), "ack " .. read(6) .. ", sequence " .. seq)
  check(label .. " ack", wait_until(function() return acked(seq) end))
  settle()
  mark_changed(method)
  if not add_point(label .. " toggle", "Furrow probe " .. label .. ": toggle step " .. index, STATE_FX) then
    undo()
    return
  end
  undo()
  check(label .. " first undo restores step", step(index) == before, "step " .. step(index))
  check(label .. " first undo keeps the rename", track_name_by_guid(guid) == renamed,
    "name '" .. track_name_by_guid(guid) .. "'")
  undo()
  check(label .. " second undo restores the name", track_name_by_guid(guid) == FOLDER,
    "name '" .. track_name_by_guid(guid) .. "'")
  check(label .. " second undo keeps the step restored", step(index) == before, "step " .. step(index))
  redo()
  redo()
  check(label .. " redo twice reapplies both", step(index) == 1 - before and track_name_by_guid(guid) == renamed,
    "step " .. step(index) .. ", name '" .. track_name_by_guid(guid) .. "'")
  undo()
  undo()
end

local function test_same_gesture(pass, method, index)
  local label = string.format("%s C-%s", pass, method)
  local track_label = "Furrow probe " .. label .. " track"
  local before = step(index)
  local tracks = track_count()
  local seq = send_toggle(index, DELAY_BLOCKS, method)
  reaper.InsertTrackAtIndex(tracks, true)
  reaper.GetSetMediaTrackInfo_String(reaper.GetTrack(0, tracks), "P_NAME", track_label, true)
  check(label .. " ack", wait_until(function() return acked(seq) end))
  settle()
  mark_changed(method)
  if not add_point(label, "Furrow probe " .. label .. ": toggle step " .. index .. " and add a track", STATE_ALL) then
    return
  end
  undo()
  check(label .. " one undo restores step and track",
    step(index) == before and track_count() == tracks and find_track(track_label) == nil,
    "step " .. step(index) .. ", tracks " .. track_count())
  redo()
  check(label .. " redo reapplies step and track",
    step(index) == 1 - before and find_track(track_label) ~= nil,
    "step " .. step(index) .. ", tracks " .. track_count())
  undo()
end

local function run_pass(pass, base)
  log("== " .. pass .. " transport")
  test_toggle(pass, "none", base)
  test_toggle(pass, "lua", base + 1)
  test_toggle(pass, "engine", base + 2)
  test_unrelated_edit(pass, "lua", base + 3)
  test_unrelated_edit(pass, "engine", base + 4)
  test_same_gesture(pass, "lua", base + 5)
  test_same_gesture(pass, "engine", base + 6)
end

local function output_path()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-undo-results.txt"
end

local function main()
  reaper.gmem_attach(GMEM)
  if not check("engine mailbox present", read(0) == MAILBOX_VERSION, "gmem[0] = " .. read(0)) then
    return
  end
  if not check("engine track found", find_track(ENGINE) ~= nil) then
    return
  end
  reaper.OnStopButton()
  local stopped_runs = settle(3)
  check("engine processes while stopped", stopped_runs)
  if stopped_runs then
    run_pass("stopped", 0)
  end
  reaper.OnPlayButton()
  check("engine processes while playing", settle(3))
  run_pass("playing", 16)
  reaper.OnStopButton()
end

local runner = coroutine.create(main)

local function tick()
  local ok, err = coroutine.resume(runner)
  if not ok then
    check("script ran without error", false, tostring(err))
  end
  if coroutine.status(runner) ~= "dead" then
    reaper.defer(tick)
    return
  end
  log(string.format("%d failures", failures))
  local path = output_path()
  local file = io.open(path, "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg(path .. "\n")
  end
end

tick()
