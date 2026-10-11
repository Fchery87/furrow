-- Measure whether RS5K stops a held note on All Notes Off or All Sound Off, and whether
-- named gmem keeps its values once nothing is attached.
-- Save a new project first, then run this script. It writes a 1 kHz sine next to the project
-- and adds one track with a MIDI driver, RS5K and a meter. The meter silences the track.
-- Leave REAPER alone until the console prints the result count.
-- Results go to the console and to furrow-cleanup-results.txt next to the project.

local GMEM = "FurrowProbeRS5K"
local TRACK = "Furrow Cleanup Probe"
local OTHER_PROBE = "Furrow RS5K Probe"
local SAMPLE_RATE = 44100
local SAMPLE_SECONDS = 3
local SINE_HZ = 1000
local NOTE = 60
local SILENCE = 0.001
local CONTROLLERS = { { name = "All Notes Off", cc = 123 }, { name = "All Sound Off", cc = 120 } }
local MARK_INDEX = 4000
local MARK = 4242.5

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
  if type(value) == "number" then
    value = string.format("%.4f", value)
  end
  log("RESULT " .. label .. " = " .. tostring(value))
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

local function project_dir()
  local _, project = reaper.EnumProjects(-1, "")
  return project and project:match("^(.*)[/\\][^/\\]*$") or nil
end

local function write_sine(path)
  local frames = SAMPLE_RATE * SAMPLE_SECONDS
  local samples = {}
  for i = 0, frames - 1 do
    samples[#samples + 1] = string.pack("<i2", math.floor(16383 * math.sin(2 * math.pi * SINE_HZ * i / SAMPLE_RATE) + 0.5))
  end
  local data = table.concat(samples)
  local header = "RIFF" .. string.pack("<I4", 36 + #data) .. "WAVE"
    .. "fmt " .. string.pack("<I4I2I2I4I4I2I2", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16)
    .. "data" .. string.pack("<I4", #data)
  local file = assert(io.open(path, "wb"))
  file:write(header, data)
  file:close()
end

local function send(status, data1, data2)
  local seq = reaper.gmem_read(2) + 1
  reaper.gmem_write(3, status)
  reaper.gmem_write(4, data1)
  reaper.gmem_write(5, data2)
  reaper.gmem_write(2, seq)
  if not wait_until(function() return reaper.gmem_read(6) >= seq end, 1) then
    fail("driver sent message " .. status .. " " .. data1 .. " " .. data2)
  end
  wait(0.03)
end

local function measure(seconds)
  reaper.gmem_write(20, 1)
  wait(seconds)
  reaper.gmem_write(20, 0)
  if not wait_until(function() return reaper.gmem_read(26) == 0 end, 1) then
    fail("meter stopped measuring")
  end
  local count = reaper.gmem_read(24)
  if count <= 0 then
    return { left = 0, right = 0, hz = 0 }
  end
  return {
    left = math.sqrt(reaper.gmem_read(21) / count),
    right = math.sqrt(reaper.gmem_read(22) / count),
    hz = reaper.gmem_read(23) / 2 / (count / reaper.gmem_read(27)),
  }
end

local function db(ratio)
  if ratio <= 0 then
    return -math.huge
  end
  return 20 * math.log(ratio, 10)
end

local function find_param(track, fx, pattern)
  for index = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, index, "")
    if name:lower():find(pattern) then
      return index
    end
  end
  return nil
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

-- Writes a mark, attaches another segment so this script no longer holds the first, and reads it back.
local function lifetime(label, name, pause)
  reaper.gmem_attach(name)
  reaper.gmem_write(MARK_INDEX, MARK)
  reaper.gmem_attach("FurrowProbeLifetimeElsewhere")
  wait(pause)
  reaper.gmem_attach(name)
  result(label .. ", value read back", reaper.gmem_read(MARK_INDEX))
  reaper.gmem_write(MARK_INDEX, 0)
end

-- Stops any voice still sounding, then restores Obey note-offs.
local function silence(track, fx, obey, obeyed)
  reaper.TrackFX_SetParamNormalized(track, fx, obey, 1)
  send(0x80, NOTE, 0)
  wait(0.1)
  reaper.TrackFX_SetParamNormalized(track, fx, obey, obeyed and 1 or 0)
  wait(0.1)
end

local function controller_case(track, fx, obey, obeyed, controller)
  local label = string.format("%s, Obey note-offs %s", controller.name, obeyed and "on" or "off")
  reaper.TrackFX_SetParamNormalized(track, fx, obey, obeyed and 1 or 0)
  local before = measure(0.1)
  if before.left > SILENCE then
    fail(label .. " silent before the note-on", string.format("%.6f", before.left))
  end
  send(0x90, NOTE, 127)
  wait(0.1)
  local held = measure(0.3)
  send(0xB0, controller.cc, 0)
  wait(0.1)
  local after = measure(0.25)
  result(label .. ", level change dB", db(after.left / held.left))
  silence(track, fx, obey, obeyed)
end

local function main()
  local dir = project_dir()
  if not dir or dir == "" then
    fail("project saved before the run")
    return
  end
  if find_track(TRACK) or find_track(OTHER_PROBE) then
    fail("no other probe track shares this run's gmem", "use a new project")
    return
  end
  local sample = dir .. package.config:sub(1, 1) .. "furrow-rs5k-sine-1k.wav"
  write_sine(sample)

  log("== gmem lifetime")
  local unique = string.format("FurrowProbeLifetime%d", math.floor(reaper.time_precise() * 1000))
  lifetime("segment nothing else attaches, read at once", unique .. "A", 0)
  lifetime("segment nothing else attaches, read after 0.5 s", unique .. "B", 0.5)

  reaper.gmem_attach(GMEM)
  reaper.gmem_write(20, 0)
  reaper.InsertTrackAtIndex(reaper.CountTracks(0), true)
  local track = reaper.GetTrack(0, reaper.CountTracks(0) - 1)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", TRACK, true)
  local driver = reaper.TrackFX_AddByName(track, "JS:furrow_probe_driver.jsfx", false, -1)
  local fx = reaper.TrackFX_AddByName(track, "ReaSamplOmatic5000", false, -1)
  local meter = reaper.TrackFX_AddByName(track, "JS:furrow_probe_meter.jsfx", false, -1)
  if driver < 0 or fx < 0 or meter < 0 then
    fail("driver, RS5K and meter load", string.format("indices %d %d %d", driver, fx, meter))
    return
  end
  reaper.TrackFX_SetNamedConfigParm(track, fx, "FILE0", sample)
  reaper.TrackFX_SetNamedConfigParm(track, fx, "DONE", "")
  local start = reaper.gmem_read(25)
  if not wait_until(function() return reaper.gmem_read(25) > start end, 2) then
    fail("meter processes while stopped")
    return
  end
  lifetime("segment a running JSFX attaches, read after 0.5 s", GMEM, 0.5)
  reaper.gmem_attach(GMEM)

  local obey = find_param(track, fx, "note%-off")
  if not obey then
    fail("Obey note-offs parameter found")
    return
  end
  log("== All Notes Off and All Sound Off")
  for _, obeyed in ipairs({ true, false }) do
    for _, controller in ipairs(CONTROLLERS) do
      controller_case(track, fx, obey, obeyed, controller)
    end
  end
end

local function output_path()
  local dir = project_dir()
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-cleanup-results.txt"
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
