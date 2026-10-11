-- Measure how ReaSamplOmatic5000 responds to the controls a Furrow lane sends.
-- Save the project first, then run this script. It writes a 1 kHz sine sample next to the
-- project, adds one track with a MIDI driver, RS5K and a meter, and measures RS5K's output.
-- The meter silences the track, so nothing plays through the speakers. Leave REAPER alone
-- until the console prints the result count.
-- Results go to the console and to furrow-rs5k-results.txt next to the project.

local GMEM = "FurrowProbeRS5K"
local TRACK = "Furrow RS5K Probe"
local SAMPLE_RATE = 44100
local SAMPLE_SECONDS = 3
local SINE_HZ = 1000
local NOTE = 60
local STATE_ALL = -1
local SILENCE = 0.001

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
  wait(0.03)
  local count = reaper.gmem_read(24)
  if count <= 0 then
    return { left = 0, right = 0, hz = 0 }
  end
  return {
    left = math.sqrt(reaper.gmem_read(21) / count),
    right = math.sqrt(reaper.gmem_read(22) / count),
    hz = reaper.gmem_read(23) / 2 / (count / SAMPLE_RATE),
  }
end

-- A voice still sounding from an earlier note adds to the next measurement at an unknown phase.
local function play(velocity)
  local before = measure(0.1)
  if before.left > SILENCE or before.right > SILENCE then
    fail("silent before the note-on", string.format("left %.6f, right %.6f", before.left, before.right))
  end
  send(0x90, NOTE, velocity)
  wait(0.1)
  local held = measure(0.4)
  send(0x80, NOTE, 0)
  wait(0.1)
  held.after_off = measure(0.25)
  wait(0.3)
  return held
end

local function db(ratio)
  if ratio <= 0 then
    return -math.huge
  end
  return 20 * math.log(ratio, 10)
end

local function reset_controllers()
  send(0xB0, 7, 100)
  send(0xB0, 10, 64)
  send(0xE0, 0, 64)
end

local function find_param(track, fx, pattern)
  for index = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, index, "")
    if name:lower():find(pattern) then
      return index, name
    end
  end
  return nil
end

local function set_config(track, fx, key, value)
  reaper.TrackFX_SetNamedConfigParm(track, fx, key, tostring(value))
  local ok, back = reaper.TrackFX_GetNamedConfigParm(track, fx, key)
  return ok and back or "(unreadable)"
end

local function controller_cases(label, track, fx, volume, pan, baseline)
  send(0xB0, 7, 0)
  local quiet = play(127)
  result(label .. " CC 7 = 0, level change dB", db(quiet.left / baseline.left))
  result(label .. " CC 7 = 0, Volume parameter reads", reaper.TrackFX_GetParamNormalized(track, fx, volume))
  send(0xB0, 7, 127)
  local loud = play(127)
  result(label .. " CC 7 = 127, level change dB", db(loud.left / baseline.left))
  send(0xB0, 7, 100)
  send(0xB0, 10, 0)
  local left = play(127)
  result(label .. " CC 10 = 0, right minus left dB", db(left.right / left.left))
  result(label .. " CC 10 = 0, Pan parameter reads", reaper.TrackFX_GetParamNormalized(track, fx, pan))
  send(0xB0, 10, 127)
  local right = play(127)
  result(label .. " CC 10 = 127, right minus left dB", db(right.right / right.left))
  reset_controllers()
end

local function link(track, fx, param, cc, active)
  local prefix = "param." .. param .. ".plink."
  local back = {}
  back[#back + 1] = "mod.active " .. set_config(track, fx, "param." .. param .. ".mod.active", active and 1 or 0)
  back[#back + 1] = "plink.active " .. set_config(track, fx, prefix .. "active", active and 1 or 0)
  if active then
    back[#back + 1] = "effect " .. set_config(track, fx, prefix .. "effect", -100)
    back[#back + 1] = "midi_bus " .. set_config(track, fx, prefix .. "midi_bus", 0)
    back[#back + 1] = "midi_chan " .. set_config(track, fx, prefix .. "midi_chan", 0)
    back[#back + 1] = "midi_msg " .. set_config(track, fx, prefix .. "midi_msg", 176)
    back[#back + 1] = "midi_msg2 " .. set_config(track, fx, prefix .. "midi_msg2", cc)
    back[#back + 1] = "scale " .. set_config(track, fx, prefix .. "scale", 1)
    back[#back + 1] = "offset " .. set_config(track, fx, prefix .. "offset", 0)
  end
  return table.concat(back, ", ")
end

local function learn(track, fx, param, cc, active)
  local prefix = "param." .. param .. ".learn."
  return "midi1 " .. set_config(track, fx, prefix .. "midi1", active and 176 or 0)
    .. ", midi2 " .. set_config(track, fx, prefix .. "midi2", active and cc or 0)
end

local function main()
  local dir = project_dir()
  if not dir or dir == "" then
    fail("project saved before the run")
    return
  end
  reaper.gmem_attach(GMEM)
  reaper.gmem_write(20, 0)
  local sample = dir .. package.config:sub(1, 1) .. "furrow-rs5k-sine-1k.wav"
  write_sine(sample)

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
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe: RS5K track", STATE_ALL, -1)
  local start = reaper.gmem_read(25)
  if not wait_until(function() return reaper.gmem_read(25) > start end, 2) then
    fail("meter processes while stopped")
    return
  end

  log("== RS5K parameters")
  local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
  result("RS5K name", fx_name)
  for index = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, index, "")
    local _, shown = reaper.TrackFX_GetFormattedParamValue(track, fx, index, "")
    log(string.format("PARAM %d %s = %.4f (%s)", index, name, reaper.TrackFX_GetParamNormalized(track, fx, index), shown))
  end
  local volume = find_param(track, fx, "^volume")
  local pan = find_param(track, fx, "^pan")
  local obey = find_param(track, fx, "note%-off")
  if not volume or not pan or not obey then
    fail("Volume, Pan and Obey note-offs parameters found")
    return
  end

  local min_gain = find_param(track, fx, "^gain for minimum velocity")
  if not min_gain then
    fail("Gain for minimum velocity parameter found")
    return
  end

  log("== note-off, default settings")
  reset_controllers()
  local unreleased = play(127)
  result("note-off, default settings, level change dB", db(unreleased.after_off.left / unreleased.left))
  reaper.TrackFX_SetParamNormalized(track, fx, obey, 1)
  send(0x80, NOTE, 0)
  wait(0.1)
  local stopped = measure(0.1)
  result("note-off after Obey note-offs is turned on, level", stopped.left)

  log("== Obey note-offs on")
  local baseline = play(127)
  result("baseline held level, left", baseline.left)
  result("baseline held level, right", baseline.right)
  result("baseline pitch Hz", baseline.hz)
  result("note-off with Obey note-offs, level change dB", db(baseline.after_off.left / baseline.left))
  for _, velocity in ipairs({ 64, 1 }) do
    result("velocity " .. velocity .. ", level change dB", db(play(velocity).left / baseline.left))
  end
  local _, default_gain = reaper.TrackFX_GetFormattedParamValue(track, fx, min_gain, "")
  reaper.TrackFX_SetParamNormalized(track, fx, min_gain, 0)
  local _, lowest_gain = reaper.TrackFX_GetFormattedParamValue(track, fx, min_gain, "")
  result("Gain for minimum velocity, default and lowest", default_gain .. " and " .. lowest_gain)
  for _, velocity in ipairs({ 64, 1 }) do
    result("lowest minimum-velocity gain, velocity " .. velocity .. ", level change dB", db(play(velocity).left / baseline.left))
  end
  reaper.TrackFX_SetParamNormalized(track, fx, min_gain, 0.5)

  log("== raw MIDI")
  controller_cases("raw", track, fx, volume, pan, baseline)
  send(0xE0, 127, 127)
  local up = play(127)
  result("pitch bend maximum, pitch change semitones", 12 * math.log(up.hz / baseline.hz, 2))
  send(0xE0, 0, 0)
  local down = play(127)
  result("pitch bend minimum, pitch change semitones", 12 * math.log(down.hz / baseline.hz, 2))
  reset_controllers()

  log("== parameter links, CC 7 to Volume and CC 10 to Pan")
  result("Volume link settings read back", link(track, fx, volume, 7, true))
  result("Pan link settings read back", link(track, fx, pan, 10, true))
  controller_cases("linked", track, fx, volume, pan, baseline)
  link(track, fx, volume, 7, false)
  link(track, fx, pan, 10, false)
  result("Volume parameter after the link is removed", reaper.TrackFX_GetParamNormalized(track, fx, volume))
  result("Pan parameter after the link is removed", reaper.TrackFX_GetParamNormalized(track, fx, pan))
  reaper.TrackFX_SetParamNormalized(track, fx, volume, 0.5)
  reaper.TrackFX_SetParamNormalized(track, fx, pan, 0.5)

  log("== MIDI learn, CC 7 to Volume and CC 10 to Pan")
  result("Volume learn settings read back", learn(track, fx, volume, 7, true))
  result("Pan learn settings read back", learn(track, fx, pan, 10, true))
  controller_cases("learned", track, fx, volume, pan, baseline)
  learn(track, fx, volume, 7, false)
  learn(track, fx, pan, 10, false)
end

local function output_path()
  local dir = project_dir()
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-rs5k-results.txt"
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
