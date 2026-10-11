-- Test whether REAPER's native undo restores a step that the audio thread applied.
-- Run it in a project built by furrow_probe_layout.lua, with no other probe project open.
-- It changes steps, renames the folder track and adds tracks, then undoes and redoes them.
-- Results go to the console and to furrow-undo-results.txt next to the project.

local GMEM = "FurrowProbeEngine"
local MAILBOX_VERSION = 1
local OP_TOGGLE = 1
local MIRROR = 100
local DELAY_BLOCKS = 20
local TIMEOUT = 3
local FOLDER = "Furrow Probe Folder"
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

local function send_toggle(index, delay)
  local seq = math.max(read(2), read(6)) + 1
  reaper.gmem_write(3, OP_TOGGLE)
  reaper.gmem_write(4, index)
  reaper.gmem_write(5, delay)
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

local function last_undo()
  return reaper.Undo_CanUndo2(0) or ""
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

local function test_toggle(pass, index)
  local name = string.format("Furrow probe %s A: toggle step %d", pass, index)
  local before = step(index)
  local restores = read(8)
  local seq = send_toggle(index, 0)
  check(pass .. " A ack", wait_until(function() return acked(seq) end), "sequence " .. seq)
  settle()
  check(pass .. " A applied", step(index) == 1 - before, "step " .. step(index))
  reaper.Undo_OnStateChangeEx2(0, name, STATE_FX, -1)
  check(pass .. " A undo point", last_undo() == name, "top of undo is '" .. last_undo() .. "'")
  undo()
  check(pass .. " A undo restores step", step(index) == before, "step " .. step(index))
  check(pass .. " A undo reached the engine", read(8) > restores, "restore count " .. read(8))
  redo()
  check(pass .. " A redo reapplies step", step(index) == 1 - before, "step " .. step(index))
end

local function test_unrelated_edit(pass, index)
  local folder = find_track(FOLDER)
  if not check(pass .. " B folder track found", folder ~= nil) then
    return
  end
  local guid = reaper.GetTrackGUID(folder)
  local renamed = FOLDER .. " " .. pass .. " B"
  local before = step(index)
  local seq = send_toggle(index, DELAY_BLOCKS)
  reaper.GetSetMediaTrackInfo_String(folder, "P_NAME", renamed, true)
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe " .. pass .. " B: unrelated rename", STATE_TRACKCFG, -1)
  check(pass .. " B command still pending at the rename", not acked(seq), "ack " .. read(6) .. ", sequence " .. seq)
  check(pass .. " B ack", wait_until(function() return acked(seq) end))
  settle()
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe " .. pass .. " B: toggle step " .. index, STATE_FX, -1)
  undo()
  local name_after_one = track_name_by_guid(guid)
  check(pass .. " B first undo restores step", step(index) == before, "step " .. step(index))
  check(pass .. " B first undo keeps the rename", name_after_one == renamed, "name '" .. name_after_one .. "'")
  undo()
  local name_after_two = track_name_by_guid(guid)
  check(pass .. " B second undo restores the name", name_after_two == FOLDER, "name '" .. name_after_two .. "'")
  check(pass .. " B second undo keeps the step restored", step(index) == before, "step " .. step(index))
  redo()
  redo()
  local name_after_redo = track_name_by_guid(guid)
  check(pass .. " B redo twice reapplies both", step(index) == 1 - before and name_after_redo == renamed,
    "step " .. step(index) .. ", name '" .. name_after_redo .. "'")
  undo()
  undo()
end

local function add_track(label)
  reaper.InsertTrackAtIndex(track_count(), true)
  reaper.GetSetMediaTrackInfo_String(reaper.GetTrack(0, track_count() - 1), "P_NAME", label, true)
end

local function test_same_gesture(pass, index, spanning_block)
  local test = spanning_block and "C2" or "C1"
  local label = string.format("Furrow probe %s %s track", pass, test)
  local name = string.format("Furrow probe %s %s: toggle step %d and add a track", pass, test, index)
  local before = step(index)
  local tracks = track_count()
  if spanning_block then
    reaper.Undo_BeginBlock2(0)
  end
  local seq = send_toggle(index, DELAY_BLOCKS)
  add_track(label)
  check(pass .. " " .. test .. " ack", wait_until(function() return acked(seq) end))
  settle()
  if spanning_block then
    reaper.Undo_EndBlock2(0, name, STATE_ALL)
  else
    reaper.Undo_OnStateChangeEx2(0, name, STATE_ALL, -1)
  end
  check(pass .. " " .. test .. " undo point", last_undo() == name, "top of undo is '" .. last_undo() .. "'")
  undo()
  check(pass .. " " .. test .. " one undo restores step and track",
    step(index) == before and track_count() == tracks and find_track(label) == nil,
    "step " .. step(index) .. ", tracks " .. track_count())
  redo()
  check(pass .. " " .. test .. " redo reapplies step and track",
    step(index) == 1 - before and find_track(label) ~= nil,
    "step " .. step(index) .. ", tracks " .. track_count())
  undo()
end

local function run_pass(pass, base)
  log("== " .. pass .. " transport")
  test_toggle(pass, base)
  test_unrelated_edit(pass, base + 1)
  test_same_gesture(pass, base + 2, false)
  test_same_gesture(pass, base + 3, true)
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
