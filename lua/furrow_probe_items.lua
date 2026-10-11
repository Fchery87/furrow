-- Measure whether item and take extension data keep a Furrow source snapshot.
-- Save the project first, then run this script. It adds one track of MIDI items, writes
-- payloads up to 8 MiB, copies them the ways a user does, saves, clears the payloads in
-- memory and reopens the project without saving, so every reload reading comes from disk.
-- Leave REAPER alone until the console prints the result count.
-- Results go to the console and to furrow-items-results.txt next to the project.

local TRACK = "Furrow Item Probe"
local KEY = "P_EXT:Furrow"
local TAG = "furrow-probe"
local STATE_ITEMS = 4
local UNSELECT_ALL_ITEMS = 40289
local DUPLICATE_ITEMS = 41295
local COPY_ITEMS = 40698
local PASTE_ITEMS = 42398
local ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

-- A full V1 pattern is 64 lanes x 512 steps x 16 values. At four hex digits a value that is 2 MiB.
local PAYLOADS = {
  { name = "1 KiB", size = 1024, seed = 1 },
  { name = "64 KiB", size = 65536, seed = 2 },
  { name = "2 MiB", size = 2097152, seed = 3 },
  { name = "8 MiB", size = 8388608, seed = 4 },
}
local TAKE_PAYLOAD = { name = "64 KiB take", size = 65536, seed = 5 }
local SPECIALS = {
  { name = "quotes and brackets", value = "a b\"c'd`e<f>g" },
  { name = "tab, pipe and backslash", value = "a\tb|c\\d" },
  { name = "UTF-8", value = "caf\u{e9} \u{65e5}\u{672c}" },
  { name = "newline", value = "line1\nline2" },
  { name = "chunk syntax", value = "x\n>\n<ITEM\ny" },
}

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

local function payload(spec)
  local blocks = {}
  local x = spec.seed
  local remaining = spec.size
  while remaining > 0 do
    local count = math.min(remaining, 4096)
    local chars = {}
    for i = 1, count do
      x = (x * 1103515245 + 12345) % 2147483648
      local index = (x >> 16) % 64 + 1
      chars[i] = ALPHABET:sub(index, index)
    end
    blocks[#blocks + 1] = table.concat(chars)
    remaining = remaining - count
    if #blocks % 64 == 0 then
      coroutine.yield()
    end
  end
  return table.concat(blocks)
end

local function project_path()
  local _, path = reaper.EnumProjects(-1, "")
  return path
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

local function item_ext(item)
  local _, value = reaper.GetSetMediaItemInfo_String(item, KEY, "", false)
  return value
end

local function take_ext(item)
  local _, value = reaper.GetSetMediaItemTakeInfo_String(reaper.GetActiveTake(item), KEY, "", false)
  return value
end

local function label(item, kind)
  reaper.GetSetMediaItemTakeInfo_String(reaper.GetActiveTake(item), "P_NAME", TAG .. "|" .. kind, true)
end

local function kind_of(item)
  local take = reaper.GetActiveTake(item)
  if not take then
    return nil
  end
  local _, name = reaper.GetSetMediaItemTakeInfo_String(take, "P_NAME", "", false)
  local prefix = TAG .. "|"
  return name:sub(1, #prefix) == prefix and name:sub(#prefix + 1) or nil
end

local function find_item(track, kind)
  for index = 0, reaper.CountTrackMediaItems(track) - 1 do
    local item = reaper.GetTrackMediaItem(track, index)
    if kind_of(item) == kind then
      return item
    end
  end
  return nil
end

local function new_item(track, position)
  local item = reaper.CreateNewMIDIItemInProj(track, position, position + 1)
  if not item then
    fail("MIDI item created at " .. position)
  end
  return item
end

local function compare(name, got, want)
  local exact = got == want
  local detail = string.format("length %d of %d, exact %s", #got, #want, tostring(exact))
  if not exact and #got > 0 and #want > 0 then
    local first = 1
    while first <= math.min(#got, #want) and got:byte(first) == want:byte(first) do
      first = first + 1
    end
    detail = detail .. ", first difference at byte " .. first
  end
  result(name, detail)
  return exact
end

local function guid(item)
  local _, value = reaper.GetSetMediaItemInfo_String(item, "GUID", "", false)
  return value
end

-- Selecting the source and the track makes REAPER's own copy actions act on that item alone.
local function copy_by_action(track, source, position, actions)
  local before = reaper.CountTrackMediaItems(track)
  reaper.Main_OnCommand(UNSELECT_ALL_ITEMS, 0)
  reaper.SetMediaItemSelected(source, true)
  reaper.SetOnlyTrackSelected(track)
  reaper.SetEditCurPos(position, false, false)
  for _, action in ipairs(actions) do
    reaper.Main_OnCommand(action, 0)
  end
  local copy = reaper.GetSelectedMediaItem(0, 0)
  if reaper.CountTrackMediaItems(track) ~= before + 1 or not copy or guid(copy) == guid(source) then
    return nil
  end
  return copy
end

local function copy_by_chunk(track, source, position)
  local _, chunk = reaper.GetItemStateChunk(source, "", false)
  chunk = chunk:gsub("\n%s*I?GUID [^\n]*", "")
  local copy = reaper.AddMediaItemToTrack(track)
  reaper.SetItemStateChunk(copy, chunk, false)
  reaper.SetMediaItemInfo_Value(copy, "D_POSITION", position)
  return copy, #chunk
end

local function first_run(track, expected)
  log("== write and read back")
  for index, spec in ipairs(PAYLOADS) do
    expected[spec.name] = payload(spec)
    local item = new_item(track, 2 * (index - 1))
    if not item then
      return false
    end
    label(item, spec.name .. "|source")
    local start = reaper.time_precise()
    reaper.GetSetMediaItemInfo_String(item, KEY, expected[spec.name], true)
    local wrote = reaper.time_precise() - start
    start = reaper.time_precise()
    local back = item_ext(item)
    local read = reaper.time_precise() - start
    compare(spec.name .. " item, written", back, expected[spec.name])
    result(spec.name .. " item, write and read seconds", string.format("%.3f and %.3f", wrote, read))
    if spec.name == "64 KiB" then
      expected[TAKE_PAYLOAD.name] = payload(TAKE_PAYLOAD)
      reaper.GetSetMediaItemTakeInfo_String(reaper.GetActiveTake(item), KEY, expected[TAKE_PAYLOAD.name], true)
      compare(TAKE_PAYLOAD.name .. ", written", take_ext(item), expected[TAKE_PAYLOAD.name])
    end
  end

  log("== copies of the 2 MiB item")
  local source = find_item(track, "2 MiB|source")
  local _, chunk = reaper.GetItemStateChunk(source, "", false)
  result("2 MiB item chunk holds the payload", string.format("chunk length %d, contains it %s", #chunk,
    tostring(chunk:find(expected["2 MiB"], 1, true) ~= nil)))
  local copies = {
    { kind = "duplicate", item = copy_by_action(track, source, 10, { DUPLICATE_ITEMS }) },
    { kind = "copy and paste", item = copy_by_action(track, source, 12, { COPY_ITEMS, PASTE_ITEMS }) },
    { kind = "state chunk copy", item = (copy_by_chunk(track, source, 14)) },
  }
  local split = copy_by_chunk(track, source, 16)
  label(split, "2 MiB|split left")
  local right = reaper.SplitMediaItem(split, 16.5)
  copies[#copies + 1] = { kind = "split left", item = split }
  copies[#copies + 1] = { kind = "split right", item = right }
  for _, copy in ipairs(copies) do
    if copy.item then
      label(copy.item, "2 MiB|" .. copy.kind)
      compare("2 MiB " .. copy.kind .. ", written", item_ext(copy.item), expected["2 MiB"])
    else
      fail("2 MiB " .. copy.kind .. " made one new item on the probe track")
    end
  end

  log("== undo of an extension change")
  local small = find_item(track, "1 KiB|source")
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe: item payloads", STATE_ITEMS, -1)
  reaper.GetSetMediaItemInfo_String(small, KEY, "replaced", true)
  reaper.Undo_OnStateChangeEx2(0, "Furrow probe: replace a payload", STATE_ITEMS, -1)
  local top = reaper.Undo_CanUndo2(0)
  if top == "Furrow probe: replace a payload" then
    reaper.Undo_DoUndo2(0)
    -- Undo can rebuild items and tracks, so earlier handles may no longer be valid.
    track = find_track()
    compare("1 KiB item after undo of a replacement", item_ext(find_item(track, "1 KiB|source")), expected["1 KiB"])
  else
    fail("the replacement is the top undo point", tostring(top))
  end

  log("== special characters through a state chunk")
  for index, special in ipairs(SPECIALS) do
    local item = new_item(track, 30 + 2 * index)
    reaper.GetSetMediaItemInfo_String(item, KEY, special.value, true)
    compare(special.name .. ", written", item_ext(item), special.value)
    local copy = copy_by_chunk(track, item, 31 + 2 * index)
    compare(special.name .. ", state chunk copy", item_ext(copy), special.value)
    reaper.DeleteTrackMediaItem(track, item)
    reaper.DeleteTrackMediaItem(track, copy)
  end
  return track
end

local function check_reload(track, expected)
  local seen = {}
  for index = 0, reaper.CountTrackMediaItems(track) - 1 do
    local item = reaper.GetTrackMediaItem(track, index)
    local kind = kind_of(item)
    if kind then
      local name, how = kind:match("^(.-)|(.*)$")
      seen[#seen + 1] = kind
      compare(name .. " " .. how .. ", reloaded", item_ext(item), expected[name])
      if name == "64 KiB" and how == "source" then
        compare(TAKE_PAYLOAD.name .. ", reloaded", take_ext(item), expected[TAKE_PAYLOAD.name])
      end
    end
  end
  result("items reloaded", tostring(#seen))
end

local function flush()
  local _, project = reaper.EnumProjects(-1, "")
  local dir = project and project:match("^(.*)[/\\][^/\\]*$")
  if not dir or dir == "" then
    dir = reaper.GetResourcePath()
  end
  local path = dir .. package.config:sub(1, 1) .. "furrow-items-results.txt"
  local file = io.open(path, "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
  end
  return path
end

local function main()
  local path = project_path()
  if not path or path == "" then
    fail("project saved before the run")
    return
  end
  if find_track() then
    fail("no earlier probe track in this project", "delete the " .. TRACK .. " track or use a new project")
    return
  end
  reaper.InsertTrackAtIndex(reaper.CountTracks(0), true)
  local track = reaper.GetTrack(0, reaper.CountTracks(0) - 1)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", TRACK, true)
  local expected = {}
  track = first_run(track, expected)
  if not track then
    return
  end

  log("== save, clear in memory and reopen")
  local before = file_size(path)
  local start = reaper.time_precise()
  reaper.Main_SaveProject(0, false)
  result("save seconds", reaper.time_precise() - start)
  result("project file bytes, before and after the save", string.format("%d and %d", before, file_size(path)))
  for index = 0, reaper.CountTrackMediaItems(track) - 1 do
    local item = reaper.GetTrackMediaItem(track, index)
    reaper.GetSetMediaItemInfo_String(item, KEY, "", true)
    reaper.GetSetMediaItemTakeInfo_String(reaper.GetActiveTake(item), KEY, "", true)
  end
  flush()
  start = reaper.time_precise()
  reaper.Main_openProject("noprompt:" .. path)
  result("reopen seconds", reaper.time_precise() - start)
  local reopened = find_track()
  if not reopened then
    fail("the probe track is in the reopened project")
    return
  end
  check_reload(reopened, expected)
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
  reaper.ShowConsoleMsg(flush() .. "\n")
end

tick()
