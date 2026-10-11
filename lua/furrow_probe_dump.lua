-- Write the Furrow probe capture from gmem to text files.
-- Run it after one playback pass. Each new pass from stopped clears the previous capture.
-- The events file is the second argument to probes/diff_log.py.

local GMEM = "FurrowProbeReceiver"
local VERSION = 2
local SLOTS = 4
local BLOCK_LOG = 1000
local SAMPLE_LOG = 200000
local EVENT_LOG = 2000000
local EVENT_STRIDE = 500000

local function int(value)
  return math.floor(value + 0.5)
end

local function output_dir()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if dir and dir ~= "" then
    return dir
  end
  return reaper.GetResourcePath()
end

local function write(path, lines)
  local file = assert(io.open(path, "w"))
  file:write(table.concat(lines, "\n"), "\n")
  file:close()
end

reaper.gmem_attach(GMEM)
local read = reaper.gmem_read

if read(0) ~= VERSION then
  reaper.ShowMessageBox(
    string.format("gmem %s holds layout version %s, not %d. Play the probe once with the current receiver.", GMEM, read(0), VERSION),
    "Furrow probe",
    0
  )
  return
end

local events = {}
for slot = 0, SLOTS - 1 do
  local count = int(read(10 + slot))
  for index = 0, count - 1 do
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

local event_lines = { "# sample order lane src_bus dst_bus channel note velocity kind" }
for _, row in ipairs(events) do
  event_lines[#event_lines + 1] = string.format(
    "%d %d %d %d %d %d %d %d %s",
    row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8], row[9] == 1 and "on" or "off"
  )
end

local block_lines = { "# project_sample tempo_in_block" }
for index = 0, int(read(2)) - 1 do
  local at = BLOCK_LOG + index * 2
  block_lines[#block_lines + 1] = string.format("%d %.10f", int(read(at)), read(at + 1))
end

local sample_lines = { "# project_sample tempo_in_sample" }
local first = int(read(4))
for index = 0, int(read(3)) - 1 do
  sample_lines[#sample_lines + 1] = string.format("%d %.10f", first + index, read(SAMPLE_LOG + index))
end

local dir = output_dir()
local sep = package.config:sub(1, 1)
local paths = {
  events = dir .. sep .. "furrow-capture-events.txt",
  block = dir .. sep .. "furrow-capture-tempo-block.txt",
  sample = dir .. sep .. "furrow-capture-tempo-sample.txt",
}
write(paths.events, event_lines)
write(paths.block, block_lines)
write(paths.sample, sample_lines)

reaper.ShowConsoleMsg(string.format(
  "Furrow probe: %d events, %d block tempos, %d sample tempos\n%s\n%s\n%s\n",
  #events, #block_lines - 1, #sample_lines - 1, paths.events, paths.block, paths.sample
))
