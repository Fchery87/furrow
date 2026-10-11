-- Test whether start positions and seeds survive @serialize bit for bit.
-- Save the project first, then run this script. The first run adds the state probe track,
-- round-trips its state through a track chunk, and saves the project. Reopen the project
-- from File > Recent projects and run this script again to check the values loaded from disk.
-- Results are appended to furrow-state-results.txt next to the project.

local GMEM = "FurrowProbeState"
local VERSION = 1
local TRACK = "Furrow State Probe"
local FX = "JS:furrow_probe_state.jsfx"
local AWAITING_RELOAD = 5
local TIMEOUT = 3

-- The JSFX builds the same values with the same arithmetic.
local VALUES = {
  { "zero", 0.0 },
  { "negative zero, stored as zero", -0.0 },
  { "negative origin -16.25", -16.25 },
  { "fractional origin 1/3", 1 / 3 },
  { "large position 288000 + 1/3", 288000 + 1 / 3 },
  { "negative large -(2^30) - 1/7", -(2 ^ 30) - 1 / 7 },
  { "2^52 + 2", 2 ^ 52 + 2 },
  { "small 2^-30", 2 ^ -30 },
}
local SEEDS = { 0, 16777215, 16777217 }

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

local function wait_until(condition)
  local deadline = reaper.time_precise() + TIMEOUT
  while not condition() do
    if reaper.time_precise() > deadline then
      return false
    end
    coroutine.yield()
  end
  return true
end

local function expected_words(value)
  local w0, w1, w2, w3 = string.unpack("<I2I2I2I2", string.pack("<d", value == 0 and 0.0 or value))
  return { w0, w1, w2, w3 }
end

local function find_track()
  for index = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, index)
    local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
    if name == TRACK then
      return track
    end
  end
  return nil
end

local function check_values(phase)
  for i, entry in ipairs(VALUES) do
    local name, value = entry[1], entry[2]
    local base = 1000 + 4 * (i - 1)
    local stored = {}
    local want = expected_words(value)
    local same = true
    for w = 1, 4 do
      stored[w] = math.floor(read(base + w - 1) + 0.5)
      same = same and stored[w] == want[w]
    end
    check(string.format("%s %s words", phase, name), same,
      string.format("stored %d %d %d %d, IEEE %d %d %d %d", stored[1], stored[2], stored[3], stored[4],
        want[1], want[2], want[3], want[4]))
    local decoded = read(100 + i - 1)
    check(string.format("%s %s decodes exactly", phase, name), decoded == value and read(200 + i - 1) == 1,
      string.format("%.17g", decoded))
  end
  local raw = read(300)
  log(string.format("RESULT %s raw 1/3 through file_var = %.17g, exact %s", phase, raw, tostring(read(301) == 1)))
  for k, seed in ipairs(SEEDS) do
    local got = read(400 + k - 1)
    if seed <= 16777215 then
      check(string.format("%s seed %d", phase, seed), got == seed and read(410 + k - 1) == 1, string.format("%.17g", got))
    else
      log(string.format("RESULT %s seed %d, above the spec's bound = %.17g", phase, seed, got))
    end
  end
end

local function first_run()
  log("== first run, state chunk round trip")
  reaper.InsertTrackAtIndex(reaper.CountTracks(0), true)
  local track = reaper.GetTrack(0, reaper.CountTracks(0) - 1)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", TRACK, true)
  if not check("state probe loads", reaper.TrackFX_AddByName(track, FX, false, -1) >= 0) then
    return
  end
  local start = read(2)
  check("state probe runs", wait_until(function() return read(2) > start end))
  local reads = read(1)
  local _, chunk = reaper.GetTrackStateChunk(track, "", false)
  reaper.SetTrackStateChunk(track, chunk, false)
  check("chunk round trip read the state",
    wait_until(function() return read(1) > reads and read(4) == read(3) end), "reads " .. read(1))
  check_values("chunk")
  reaper.gmem_write(AWAITING_RELOAD, read(3))
  reaper.Main_SaveProject(0, false)
  log("Saved. Reopen this project from File > Recent projects, then run this script again.")
end

local function second_run()
  log("== second run, project reload")
  local start = read(2)
  check("state probe runs", wait_until(function() return read(2) > start end))
  check("the project was reopened", read(3) ~= read(AWAITING_RELOAD),
    string.format("token %d, first run left %d", read(3), read(AWAITING_RELOAD)))
  check("this instance read its state when the project loaded", read(4) == read(3) and read(1) >= 1,
    string.format("reads %d", read(1)))
  check_values("reload")
end

local function main()
  reaper.gmem_attach(GMEM)
  local _, path = reaper.EnumProjects(-1, "")
  if not check("project saved before the run", path ~= nil and path ~= "") then
    return
  end
  if find_track() then
    if check("state probe version", read(0) == VERSION, "gmem[0] = " .. read(0)) then
      second_run()
    end
  else
    first_run()
  end
end

local function output_path()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  return dir .. package.config:sub(1, 1) .. "furrow-state-results.txt"
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
  local file = io.open(output_path(), "a")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg(output_path() .. "\n")
  end
end

tick()
