-- Measure named gmem scope and size, and local memory, for the Furrow memory-scope proof.
-- Run it from a saved or empty project. It adds three tracks to this project, opens a new
-- project tab with one more, and switches tabs while it runs. Leave REAPER alone until the
-- console prints the result count. Close the new tab without saving afterwards.
-- Results go to the console and to furrow-memory-results.txt next to the first project.

local GMEM = "FurrowProbeMemory"
local FX = "JS:furrow_probe_memory.jsfx"
local SLOT, NONCE, HEARTBEAT, TOKEN, PERSISTENT, MEMTOP, MARKER, LOCAL_WRITE = 0, 1, 2, 3, 4, 5, 6, 7
local REPORT = 100
local MARKERS = { 1048575, 2097151, 4194303, 8388607, 8388608, 16777215, 33554431 }
local NEW_PROJECT_TAB = 40859

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

local function wait(seconds)
  local deadline = reaper.time_precise() + seconds
  while reaper.time_precise() < deadline do
    coroutine.yield()
  end
end

local function param(instance, index)
  local value = reaper.TrackFX_GetParam(instance.track, instance.fx, index)
  return math.floor(value + 0.5)
end

local function add_instance(project, name, slot)
  local index = reaper.CountTracks(project)
  reaper.InsertTrackAtIndex(index, true)
  local track = reaper.GetTrack(project, index)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  local fx = reaper.TrackFX_AddByName(track, FX, false, -1)
  if fx < 0 then
    fail(name .. " loads " .. FX)
    return nil
  end
  reaper.TrackFX_SetParam(track, fx, SLOT, slot)
  return { name = name, project = project, track = track, fx = fx, slot = slot }
end

local function duplicate(project, source, name, slot)
  local index = reaper.CountTracks(project)
  reaper.InsertTrackAtIndex(index, true)
  local track = reaper.GetTrack(project, index)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  reaper.TrackFX_CopyToTrack(source.track, source.fx, track, 0, false)
  reaper.TrackFX_SetParam(track, 0, SLOT, slot)
  return { name = name, project = project, track = track, fx = 0, slot = slot }
end

local function processing(instance)
  local first = param(instance, HEARTBEAT)
  wait(0.3)
  return param(instance, HEARTBEAT) > first
end

local function report(instance, nonce)
  local at = REPORT + 4 * instance.slot
  local token = param(instance, TOKEN)
  result(instance.name .. " runs", processing(instance))
  result(instance.name .. " sees the Lua nonce", param(instance, NONCE) == nonce)
  result(instance.name .. " gmem report visible to Lua", reaper.gmem_read(at) == token and token ~= 0)
  result(instance.name .. " gmem report saw the Lua nonce", reaper.gmem_read(at + 3) == nonce)
  return token
end

local function main()
  math.randomseed(math.floor(reaper.time_precise() * 1000))
  reaper.gmem_attach(GMEM)
  reaper.gmem_write(1, 0)
  reaper.gmem_write(2, 0)
  local first_project = reaper.EnumProjects(-1, "")

  log("== one project")
  local a1 = add_instance(first_project, "Furrow Memory A1", 0)
  local a2 = add_instance(first_project, "Furrow Memory A2", 1)
  if not a1 or not a2 then
    return
  end
  wait(0.5)
  local a3 = duplicate(first_project, a1, "Furrow Memory A3 copy of A1", 2)
  wait(0.5)
  local nonce = math.random(1, 16000000)
  reaper.gmem_write(1, nonce)
  wait(0.5)
  local a1_token = report(a1, nonce)
  local a2_token = report(a2, nonce)
  local a3_token = report(a3, nonce)
  result("A1 and A2 runtime tokens differ", a1_token ~= a2_token)
  result("A3 copy runtime token differs from A1", a3_token ~= a1_token)
  result("A3 copy persistent id equals A1", param(a3, PERSISTENT) == param(a1, PERSISTENT))

  log("== second project tab")
  reaper.Main_OnCommand(NEW_PROJECT_TAB, 0)
  local second_project = reaper.EnumProjects(-1, "")
  if second_project == first_project then
    fail("a new project tab opened")
    return
  end
  local b1 = add_instance(second_project, "Furrow Memory B1", 3)
  if not b1 then
    reaper.SelectProjectInstance(first_project)
    return
  end
  wait(0.5)
  local tab_nonce = math.random(1, 16000000)
  reaper.gmem_write(1, tab_nonce)
  wait(0.5)
  local b1_token = report(b1, tab_nonce)
  result("B1 runtime token differs from A1", b1_token ~= a1_token)

  log("== first tab active, second tab in the background")
  reaper.SelectProjectInstance(first_project)
  local background_nonce = math.random(1, 16000000)
  reaper.gmem_write(1, background_nonce)
  wait(0.5)
  report(a1, background_nonce)
  report(b1, background_nonce)

  log("== named gmem size")
  for _, index in ipairs(MARKERS) do
    reaper.gmem_write(index, index + 0.25)
  end
  for _, index in ipairs(MARKERS) do
    result(string.format("Lua reads back its own write at %d", index), reaper.gmem_read(index) == index + 0.25)
  end
  wait(0.5)
  result("A1 highest Lua marker index it read back", param(a1, MARKER))
  reaper.gmem_write(2, 2)
  wait(0.5)
  reaper.gmem_write(2, 0)
  for _, index in ipairs(MARKERS) do
    result(string.format("Lua reads an instance write at %d", index), reaper.gmem_read(index) == index + 0.5)
  end

  log("== local memory")
  result("A1 local memory top", param(a1, MEMTOP))
  result("A1 local write at 8388607 read back", param(a1, LOCAL_WRITE) == 1)
end

local function output_path()
  local _, project = reaper.EnumProjects(0, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-memory-results.txt"
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
  local path = output_path()
  local file = io.open(path, "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg(path .. "\n")
  end
end

tick()
