-- Measure whether REAPER's delay compensation keeps a lane with plugin latency on time.
-- Save a new project first, then run this script. It builds a folder with the probe engine
-- and two lanes. Each lane turns note-ons into one-sample clicks, lane 1 on the left and
-- lane 2 on the right. Lane 2 also has a plugin that delays audio and reports that delay.
-- The script plays the pattern at 120 BPM once per latency. Leave REAPER alone until the
-- console prints the result count.
-- Results go to the console and to furrow-latency-results.txt next to the project.

local LATENCY_GMEM = "FurrowProbeLatency"
local RECEIVER_GMEM = "FurrowProbeReceiver"
local FOLDER = "Furrow Latency Folder"
local ENGINE = "Furrow Latency Engine"
local LANES = { "Furrow Latency Lane 1", "Furrow Latency Lane 2" }
local LATENCIES = { 0, 1000 }
local BPM = 120
local STEP_SECONDS = 0.25 * 60 / BPM
-- Steps that sound in the probe engine's first two lanes, matching probes/pattern-4x16.txt.
local STEPS = { { 0, 4, 8, 12 }, { 2, 6, 10, 14 } }
local PLAY_SECONDS = 2.5
local LEFT, RIGHT = 1000, 100000
local RECEIVER_EVENTS, RECEIVER_STRIDE = 2000000, 500000
local PDC_KEYS = { "pdc", "chain_pdc_reporting", "chain_pdc_actual" }

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

local function read(segment, index)
  reaper.gmem_attach(segment)
  return reaper.gmem_read(index)
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

local function insert(name, depth)
  local index = reaper.CountTracks(0)
  reaper.InsertTrackAtIndex(index, true)
  local track = reaper.GetTrack(0, index)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  reaper.SetMediaTrackInfo_Value(track, "I_FOLDERDEPTH", depth)
  return track
end

local function add_fx(track, name)
  local index = reaper.TrackFX_AddByName(track, name, false, -1)
  if index < 0 then
    fail(name .. " loads")
  end
  return index
end

local function build()
  local folder = insert(FOLDER, 1)
  local engine = insert(ENGINE, 0)
  local lanes = { insert(LANES[1], 0), insert(LANES[2], -1) }
  reaper.SetMediaTrackInfo_Value(engine, "B_MAINSEND", 0)
  local ok = add_fx(folder, "JS:furrow_probe_impulse_meter.jsfx") >= 0
  ok = add_fx(engine, "JS:furrow_probe_engine.jsfx") >= 0 and ok
  local latency
  for lane, track in ipairs(lanes) do
    local receiver = add_fx(track, "JS:furrow_probe_receiver.jsfx")
    local click = add_fx(track, "JS:furrow_probe_click.jsfx")
    ok = ok and receiver >= 0 and click >= 0
    if ok then
      reaper.TrackFX_SetParam(track, receiver, 0, lane - 1)
      reaper.TrackFX_SetParam(track, click, 0, lane - 1)
    end
    local send = reaper.CreateTrackSend(engine, track)
    reaper.SetTrackSendInfo_Value(engine, 0, send, "I_SRCCHAN", -1)
    -- One-based source and destination channel, and bus 1 for both, as in furrow_probe_layout.lua.
    reaper.SetTrackSendInfo_Value(engine, 0, send, "I_MIDIFLAGS", lane | (lane << 5) | (1 << 14) | (1 << 22))
  end
  latency = add_fx(lanes[2], "JS:furrow_probe_latency.jsfx")
  if not ok or latency < 0 then
    return nil
  end
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe: latency layout", -1, -1)
  return { folder = folder, engine = engine, lanes = lanes, latency = latency }
end

local function pdc_readout(track, fx)
  local parts = {}
  for _, key in ipairs(PDC_KEYS) do
    local ok, value = reaper.TrackFX_GetNamedConfigParm(track, fx, key)
    parts[#parts + 1] = key .. " " .. (ok and value or "(unreadable)")
  end
  return table.concat(parts, ", ")
end

-- Pairs each logged sample with the expected sample of its step, in order.
local function errors(samples, steps, srate)
  if #samples ~= #steps then
    return string.format("%d of %d events", #samples, #steps)
  end
  local low, high = math.huge, -math.huge
  for i, sample in ipairs(samples) do
    local error = sample - steps[i] * STEP_SECONDS * srate
    low, high = math.min(low, error), math.max(high, error)
  end
  return string.format("%d of %d events, error %.1f to %.1f samples", #samples, #steps, low, high)
end

local function clicks(base, count_index)
  local samples = {}
  for i = 0, math.floor(read(LATENCY_GMEM, count_index) + 0.5) - 1 do
    samples[#samples + 1] = read(LATENCY_GMEM, base + i)
  end
  return samples
end

local function note_ons(slot)
  local samples = {}
  for i = 0, math.floor(read(RECEIVER_GMEM, 10 + slot) + 0.5) - 1 do
    local at = RECEIVER_EVENTS + slot * RECEIVER_STRIDE + i * 9
    if math.floor(read(RECEIVER_GMEM, at + 8) + 0.5) == 1 then
      samples[#samples + 1] = read(RECEIVER_GMEM, at)
    end
  end
  return samples
end

local function play_case(layout, delay)
  local label = "latency " .. delay
  reaper.TrackFX_SetParam(layout.lanes[2], layout.latency, 0, delay)
  wait(0.5)
  result(label .. ", lane 2 delay reports", pdc_readout(layout.lanes[2], layout.latency))
  reaper.SetEditCurPos(0, false, false)
  reaper.OnPlayButton()
  if not wait_until(function() return reaper.GetPlayPosition() > PLAY_SECONDS end, PLAY_SECONDS + 3) then
    fail(label .. " played to " .. PLAY_SECONDS .. " s")
  end
  reaper.OnStopButton()
  wait(0.3)
  local srate = read(LATENCY_GMEM, 3)
  result(label .. ", lane 1 clicks at the folder", errors(clicks(LEFT, 1), STEPS[1], srate))
  result(label .. ", lane 2 clicks at the folder", errors(clicks(RIGHT, 2), STEPS[2], srate))
  result(label .. ", lane 1 note-ons at its receiver", errors(note_ons(0), STEPS[1], srate))
  result(label .. ", lane 2 note-ons at its receiver", errors(note_ons(1), STEPS[2], srate))
end

local function main()
  local _, path = reaper.EnumProjects(-1, "")
  if not path or path == "" then
    fail("project saved before the run")
    return
  end
  if find_track(FOLDER) then
    fail("no earlier latency layout in this project", "delete its tracks or use a new project")
    return
  end
  local layout = build()
  if not layout then
    return
  end
  local start = read(LATENCY_GMEM, 4)
  if not wait_until(function() return read(LATENCY_GMEM, 4) > start end, 2) then
    fail("impulse meter processes")
    return
  end
  local tempo = reaper.Master_GetTempo()
  reaper.SetCurrentBPM(0, BPM, false)
  result("sample rate", string.format("%d", read(LATENCY_GMEM, 3)))
  for _, delay in ipairs(LATENCIES) do
    play_case(layout, delay)
  end
  reaper.TrackFX_SetParam(layout.lanes[2], layout.latency, 0, 0)
  reaper.SetCurrentBPM(0, tempo, false)
end

local function output_path()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-latency-results.txt"
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
