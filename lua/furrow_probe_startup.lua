-- Measure whether SeqCore's release on its first block reaches the lane receivers.
-- Run it in a saved project built by furrow_probe_layout.lua. It bypasses the engine, takes it
-- offline while stopped and while a note is held at 30 BPM, then saves and reopens the project
-- without a prompt. Leave REAPER alone until the console prints the result count.
-- Results go to the console and to furrow-startup-results.txt next to the project.

local ENGINE_GMEM = "FurrowProbeEngine"
local RECEIVER_GMEM = "FurrowProbeReceiver"
local ENGINE = "Furrow Probe Engine"
local FOLDER = "Furrow Probe Folder"
local LANES = 4
local NOTES = { 36, 38, 42, 46 }
local HELD_SLOT = 2
local ALL_NOTES_OFF_CC = 123
local SERIALIZE_WRITES, STARTUP_RELEASES, HAS_STARTUP_RELEASE = 10, 11, 12
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
  return math.floor(reaper.gmem_read(index) + 0.5)
end

local function receiver_read(index)
  reaper.gmem_attach(RECEIVER_GMEM)
  return math.floor(reaper.gmem_read(index) + 0.5)
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
  local deadline = reaper.time_precise() + seconds
  while not condition() do
    if reaper.time_precise() > deadline then
      return false
    end
    coroutine.yield()
  end
  return true
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

-- The receivers keep event counts at gmem[10] to gmem[13] of their own segment, the same
-- indices as these engine counters, so each read names the segment it came from.
local function engine_counters()
  return string.format("segment version %d, heartbeat %d, writes %d, releases %d, flag %d", engine_read(0),
    engine_read(1), engine_read(SERIALIZE_WRITES), engine_read(STARTUP_RELEASES), engine_read(HAS_STARTUP_RELEASE))
end

local function heartbeat_moves()
  local first = engine_read(1)
  wait(0.3)
  return engine_read(1) > first
end

local function events(slot)
  local list = {}
  for i = 0, receiver_read(10 + slot) - 1 do
    local at = RECEIVER_EVENTS + slot * RECEIVER_STRIDE + i * 9
    list[#list + 1] = { kind = receiver_read(at + 8), data1 = receiver_read(at + 6) }
  end
  return list
end

local function marks()
  local mark = {}
  for slot = 0, LANES - 1 do
    mark[slot] = receiver_read(10 + slot)
  end
  return mark
end

-- The release sends a lane's note-off and then All Notes Off at offset 0 of one block. Playback
-- never sends All Notes Off, so that adjacent pair after the mark can only be the release.
local function releases_since(mark)
  local released = 0
  for slot = 0, LANES - 1 do
    local list = events(slot)
    for i = mark[slot] + 1, #list - 1 do
      local off, cc = list[i], list[i + 1]
      if off.kind == 0 and off.data1 == NOTES[slot + 1] and cc.kind == 2 and cc.data1 == ALL_NOTES_OFF_CC then
        released = released + 1
        break
      end
    end
  end
  return string.format("%d of %d lanes got the note-off then All Notes Off", released, LANES)
end

local function cycle(engine, label, toggle)
  local releases = engine_read(STARTUP_RELEASES)
  local writes = engine_read(SERIALIZE_WRITES)
  local mark = marks()
  result(label .. ", engine counters before", engine_counters())
  toggle(true)
  wait(0.5)
  result(label .. ", engine counters while down", engine_counters())
  result(label .. ", serialize writes while it went down", engine_read(SERIALIZE_WRITES) - writes)
  toggle(false)
  if not wait_until(function() return engine_read(STARTUP_RELEASES) > releases end, 1) then
    result(label .. ", startup release ran", false)
  else
    result(label .. ", startup release ran", true)
  end
  wait(0.3)
  result(label .. ", engine counters after", engine_counters())
  result(label .. ", receivers logged", releases_since(mark))
end

local function offline(engine)
  return function(down) reaper.TrackFX_SetOffline(engine, 0, down) end
end

local function main()
  local _, path = reaper.EnumProjects(-1, "")
  local engine = find_track(ENGINE)
  if not path or path == "" then
    fail("project saved before the run", "the active project tab has no file")
    return
  end
  if not engine or not find_track(FOLDER) then
    fail("probe layout found", "run furrow_probe_layout.lua in this project first")
    return
  end
  reaper.OnStopButton()
  if not heartbeat_moves() then
    fail("engine processes while stopped")
    return
  end
  if engine_read(HAS_STARTUP_RELEASE) ~= 1 then
    fail("engine has the startup release", "restart REAPER so it loads the new furrow_probe_engine.jsfx")
    return
  end
  receiver_write(5, 1)

  log("== bypass and enable while stopped")
  cycle(engine, "bypass", function(down) reaper.TrackFX_SetEnabled(engine, 0, not down) end)

  log("== offline and online while stopped")
  cycle(engine, "offline while stopped", offline(engine))

  log("== offline and online with a held note")
  local tempo = reaper.Master_GetTempo()
  reaper.SetCurrentBPM(0, 30, false)
  reaper.SetEditCurPos(0, false, false)
  reaper.OnPlayButton()
  local held = wait_until(function()
    local list = events(HELD_SLOT)
    return #list > 0 and list[#list].kind == 1
  end, 6)
  if held then
    cycle(engine, "offline with a held note", offline(engine))
  else
    fail("lane " .. (HELD_SLOT + 1) .. " held a note")
  end
  reaper.OnStopButton()
  reaper.SetCurrentBPM(0, tempo, false)
  wait(0.3)

  log("== save and reopen")
  local releases = engine_read(STARTUP_RELEASES)
  local mark = marks()
  reaper.Main_SaveProject(0, false)
  reaper.Main_openProject("noprompt:" .. path)
  local loaded = wait_until(function() return engine_read(STARTUP_RELEASES) > releases end, 3)
  result("reopen, startup release ran", loaded)
  wait(0.5)
  result("reopen, engine counters after", engine_counters())
  result("reopen, receivers logged", releases_since(mark))
  receiver_write(5, 0)
end

local function output_path()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-startup-results.txt"
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
