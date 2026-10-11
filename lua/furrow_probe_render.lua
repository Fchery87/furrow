-- Render a timing case offline and capture the MIDI the lane receivers logged during the render.
-- Open a saved timing project built by furrow_probe_layout.lua, such as constant.RPP or ramp.RPP,
-- then run this script. It renders the first 3 seconds of the master mix next to the project with
-- the render dialog closing itself, then restores the project's render settings.
-- Results go to the console and to furrow-render-<project>-results.txt. The capture goes to
-- furrow-render-<project>-events.txt, the second argument to probes/diff_log.py.

local GMEM = "FurrowProbeReceiver"
local VERSION = 3
local SLOTS = 4
local BLOCK_LOG = 1000
local EVENT_LOG = 2000000
local EVENT_STRIDE = 500000
local ENGINE = "Furrow Probe Engine"
local RENDER_SECONDS = 3
local RENDER_WITH_LAST_SETTINGS_AUTO_CLOSE = 42230
local SETTINGS = { "RENDER_SETTINGS", "RENDER_BOUNDSFLAG", "RENDER_STARTPOS", "RENDER_ENDPOS" }
local STRINGS = { "RENDER_FILE", "RENDER_PATTERN" }

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

local function int(value)
  return math.floor(value + 0.5)
end

local function read(index)
  reaper.gmem_attach(GMEM)
  return reaper.gmem_read(index)
end

local function write(index, value)
  reaper.gmem_attach(GMEM)
  reaper.gmem_write(index, value)
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

local function project()
  local _, path = reaper.EnumProjects(-1, "")
  if not path or path == "" then
    return nil
  end
  local dir, file = path:match("^(.*)[/\\]([^/\\]*)$")
  return { path = path, dir = dir, name = file:gsub("%.[Rr][Pp][Pp]$", "") }
end

local function file_size(path)
  local file = io.open(path, "rb")
  if not file then
    return -1
  end
  local size = file:seek("end")
  file:close()
  return size
end

local function write_events(path)
  local events = {}
  for slot = 0, SLOTS - 1 do
    for index = 0, int(read(10 + slot)) - 1 do
      local at = EVENT_LOG + slot * EVENT_STRIDE + index * 9
      local row = {}
      for field = 0, 8 do
        row[field + 1] = int(read(at + field))
      end
      events[#events + 1] = row
    end
  end
  table.sort(events, function(a, b)
    for field = 1, 3 do
      if a[field] ~= b[field] then
        return a[field] < b[field]
      end
    end
    return false
  end)
  local out = { "# sample order lane src_bus dst_bus channel note velocity kind" }
  for _, row in ipairs(events) do
    out[#out + 1] = string.format("%d %d %d %d %d %d %d %d %s", row[1], row[2], row[3], row[4], row[5], row[6],
      row[7], row[8], ({ [0] = "off", [1] = "on", [2] = "cc" })[row[9]])
  end
  local file = assert(io.open(path, "w"))
  file:write(table.concat(out, "\n"), "\n")
  file:close()
  return #events
end

-- Block sizes come from the gaps between logged block starts.
local function block_sizes()
  local sizes = {}
  local seen = {}
  for index = 1, int(read(2)) - 1 do
    local size = int(read(BLOCK_LOG + index * 3)) - int(read(BLOCK_LOG + (index - 1) * 3))
    if not seen[size] then
      seen[size] = true
      sizes[#sizes + 1] = size
    end
  end
  table.sort(sizes)
  return #sizes > 0 and table.concat(sizes, ", ") or "none"
end

local function main(case)
  if not find_track(ENGINE) then
    fail("probe layout found", "open a timing project built by furrow_probe_layout.lua")
    return
  end
  if int(read(0)) ~= VERSION then
    fail("receiver gmem layout version " .. VERSION, "play the project once, then run this again")
    return
  end
  local saved, saved_strings = {}, {}
  for _, key in ipairs(SETTINGS) do
    saved[key] = reaper.GetSetProjectInfo(0, key, 0, false)
  end
  for _, key in ipairs(STRINGS) do
    _, saved_strings[key] = reaper.GetSetProjectInfo_String(0, key, "", false)
  end
  local pattern = string.format("furrow-render-%s-%d", case.name, math.floor(reaper.time_precise() * 1000))
  reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", RENDER_SECONDS, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", case.dir, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", pattern, true)

  -- The receivers clear their counts only when live playback starts, and log while stopped
  -- only when gmem[5] is 1, so both are set here in case a render does not count as playing.
  for slot = 0, SLOTS - 1 do
    write(10 + slot, 0)
  end
  write(2, 0)
  write(3, 0)
  write(5, 1)
  local start = reaper.time_precise()
  reaper.Main_OnCommand(RENDER_WITH_LAST_SETTINGS_AUTO_CLOSE, 0)
  result("render seconds", string.format("%.3f", reaper.time_precise() - start))
  write(5, 0)

  for _, key in ipairs(SETTINGS) do
    reaper.GetSetProjectInfo(0, key, saved[key], true)
  end
  for _, key in ipairs(STRINGS) do
    reaper.GetSetProjectInfo_String(0, key, saved_strings[key], true)
  end

  local rendered = nil
  local index = 0
  local name = reaper.EnumerateFiles(case.dir, index)
  while name do
    if name:sub(1, #pattern) == pattern then
      rendered = name
    end
    index = index + 1
    name = reaper.EnumerateFiles(case.dir, index)
  end
  if rendered then
    result("rendered file " .. rendered .. " bytes", file_size(case.dir .. package.config:sub(1, 1) .. rendered))
  else
    fail("the render wrote a file starting " .. pattern)
  end
  result("block tempo entries logged, which needs a playing state", int(read(2)))
  result("render block sizes", block_sizes())
  local counts = {}
  for slot = 0, SLOTS - 1 do
    counts[#counts + 1] = int(read(10 + slot))
  end
  result("receiver events per lane", table.concat(counts, ", "))
  local events = case.dir .. package.config:sub(1, 1) .. "furrow-render-" .. case.name .. "-events.txt"
  result("events written", write_events(events))
end

local case = project()
local runner = coroutine.create(function()
  if not case then
    fail("project saved before the run")
    return
  end
  main(case)
end)

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
  local dir = case and case.dir or reaper.GetResourcePath()
  local path = dir .. package.config:sub(1, 1) .. "furrow-render-" .. (case and case.name or "unsaved") .. "-results.txt"
  local file = io.open(path, "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg(path .. "\n")
  end
end

tick()
