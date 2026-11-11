-- The bridge's request handling. A request is a JSON object with an id and an op. The
-- bridge.lua script polls a folder and calls poll_once. Everything here takes the REAPER API
-- as an argument, so tests can run it against a stub.
local json = require("json")

local core = {}

core.MAX_RESULT_BYTES = 400000
core.RENDER_ACTION = 42230 -- File: Render project, using the most recent render settings, auto-close render dialog

local function track_name(reaper, track)
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  return name
end

local function ext(reaper, track, key)
  local _, text = reaper.GetSetMediaTrackInfo_String(track, key, "", false)
  return text
end

local function item_info(reaper, item)
  local take = reaper.GetActiveTake(item)
  local notes, events = 0, 0
  if take and reaper.TakeIsMIDI(take) then
    local _, n, c = reaper.MIDI_CountEvts(take)
    notes, events = n, c
  end
  local _, meta = reaper.GetSetMediaItemInfo_String(item, "P_EXT:Furrow", "", false)
  return {
    position = reaper.GetMediaItemInfo_Value(item, "D_POSITION"),
    length = reaper.GetMediaItemInfo_Value(item, "D_LENGTH"),
    muted = reaper.GetMediaItemInfo_Value(item, "B_MUTE") == 1,
    notes = notes,
    cc_events = events,
    furrow = meta,
  }
end

function core.state(reaper)
  local tracks = {}
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local fx = {}
    for f = 0, reaper.TrackFX_GetCount(track) - 1 do
      local _, name = reaper.TrackFX_GetFXName(track, f, "")
      fx[#fx + 1] = { name = name, enabled = reaper.TrackFX_GetEnabled(track, f) }
    end
    local sends = {}
    for s = 0, reaper.GetTrackNumSends(track, 0) - 1 do
      local _, dest = reaper.GetTrackSendName(track, s, "")
      sends[#sends + 1] = { to = dest, midi_flags = reaper.GetTrackSendInfo_Value(track, 0, s, "I_MIDIFLAGS") }
    end
    local items = {}
    for n = 0, reaper.CountTrackMediaItems(track) - 1 do
      items[#items + 1] = item_info(reaper, reaper.GetTrackMediaItem(track, n))
    end
    tracks[#tracks + 1] = {
      index = i, name = track_name(reaper, track), depth = reaper.GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"),
      muted = reaper.GetMediaTrackInfo_Value(track, "B_MUTE") == 1, fx = fx, sends = sends, items = items,
      furrow_links = ext(reaper, track, "P_EXT:Furrow"), furrow_items = ext(reaper, track, "P_EXT:FurrowItems"),
    }
  end
  -- REAPER returns the path alone. The test stub returns a flag first.
  local first, second = reaper.GetProjectPath("")
  local path = type(first) == "string" and first or second
  return {
    reaper = reaper.GetAppVersion(),
    play_state = reaper.GetPlayState(),
    play_position = reaper.GetPlayPosition(),
    cursor = reaper.GetCursorPosition(),
    tempo = reaper.Master_GetTempo(),
    undo_next = reaper.Undo_CanUndo2(0),
    redo_next = reaper.Undo_CanRedo2(0),
    dirty = reaper.IsProjectDirty(0) ~= 0,
    project_path = path,
    tracks = tracks,
  }
end

-- Runs Lua source with the REAPER API in scope and returns what it returns.
function core.eval(reaper, code)
  local env = setmetatable({ reaper = reaper }, { __index = _G })
  local chunk, err = load(code, "=bridge", "t", env)
  if not chunk then
    error("compile error: " .. tostring(err), 0)
  end
  local results = table.pack(chunk())
  if results.n <= 1 then
    return results[1]
  end
  local list = {}
  for i = 1, results.n do
    list[i] = results[i]
  end
  return list
end

local TRANSPORT = { play = "OnPlayButton", stop = "OnStopButton", pause = "OnPauseButton" }

function core.transport(reaper, req)
  if req.position ~= nil then
    reaper.SetEditCurPos(req.position, true, false)
  end
  local call = TRANSPORT[req.action]
  if call then
    reaper[call]()
  elseif req.action ~= nil and req.action ~= "seek" then
    error("unknown transport action " .. tostring(req.action), 0)
  end
  return { play_state = reaper.GetPlayState(), cursor = reaper.GetCursorPosition() }
end

function core.action(reaper, req)
  local id = req.command
  if type(id) == "string" then
    id = reaper.NamedCommandLookup(id)
  end
  if type(id) ~= "number" or id == 0 then
    error("unknown command", 0)
  end
  reaper.Main_OnCommand(id, 0)
  return { command = id }
end

-- Lists a folder's files. REAPER caches directory listings, so a forced reread comes first.
-- The end of the list is nil or an empty string.
local function list_files(reaper, folder)
  reaper.EnumerateFiles(folder, -1)
  local names = {}
  local i = 0
  while true do
    local file = reaper.EnumerateFiles(folder, i)
    if not file or file == "" then
      break
    end
    names[#names + 1] = file
    i = i + 1
  end
  return names
end

local function is_render_of(file, name)
  return file:sub(1, #name + 1) == name .. "." and file:sub(#name + 2):match("^%w+$") ~= nil
end

local RENDER_KEYS = { "RENDER_SETTINGS", "RENDER_BOUNDSFLAG", "RENDER_STARTPOS", "RENDER_ENDPOS", "RENDER_SRATE", "RENDER_CHANNELS" }

-- Renders a time range of the master mix into folder and returns the new file's path. The
-- project's own render settings come back afterwards.
function core.render(reaper, req, folder)
  local name = tostring(req.name or "render"):gsub("[^%w_-]", "_")
  local saved = {}
  for _, key in ipairs(RENDER_KEYS) do
    saved[key] = reaper.GetSetProjectInfo(0, key, 0, false)
  end
  local _, saved_file = reaper.GetSetProjectInfo_String(0, "RENDER_FILE", "", false)
  local _, saved_pattern = reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "", false)
  local function restore()
    for key, value in pairs(saved) do
      reaper.GetSetProjectInfo(0, key, value, true)
    end
    reaper.GetSetProjectInfo_String(0, "RENDER_FILE", saved_file, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", saved_pattern, true)
  end
  -- Earlier renders of the same name go first, so the file found afterwards is this one.
  for _, file in ipairs(list_files(reaper, folder)) do
    if is_render_of(file, name) then
      os.remove(folder .. "/" .. file)
    end
  end
  local ok, err = pcall(function()
    reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 1, true)
    reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", req.start or 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", req.stop, true)
    reaper.GetSetProjectInfo(0, "RENDER_SRATE", req.srate or 44100, true)
    reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_FILE", folder, true)
    reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", name, true)
    reaper.Main_OnCommand(core.RENDER_ACTION, 0)
  end)
  restore()
  if not ok then
    error(err, 0)
  end
  local found
  for _, file in ipairs(list_files(reaper, folder)) do
    if is_render_of(file, name) then
      found = file
    end
  end
  if not found then
    error("the render produced no file in " .. folder, 0)
  end
  return { path = folder .. "/" .. found }
end

local OPS = {}

function OPS.ping(reaper)
  return { reaper = reaper.GetAppVersion(), time = reaper.time_precise() }
end

function OPS.state(reaper)
  return core.state(reaper)
end

function OPS.eval(reaper, req)
  if type(req.code) ~= "string" then
    error("eval needs a code string", 0)
  end
  return core.eval(reaper, req.code)
end

function OPS.transport(reaper, req)
  return core.transport(reaper, req)
end

function OPS.action(reaper, req)
  return core.action(reaper, req)
end

function OPS.render(reaper, req, ctx)
  if type(req.stop) ~= "number" then
    error("render needs a stop time in seconds", 0)
  end
  return core.render(reaper, req, ctx.render_folder)
end

function OPS.shutdown(_, _, ctx)
  ctx.stopping = true
  return { stopping = true }
end

-- Handles one decoded request and returns the response table.
function core.handle(reaper, req, ctx)
  local op = OPS[req.op]
  if not op then
    return { id = req.id, ok = false, error = "unknown op " .. tostring(req.op) }
  end
  local ok, result = pcall(op, reaper, req, ctx)
  if not ok then
    return { id = req.id, ok = false, error = tostring(result) }
  end
  local text = json.encode(result)
  if #text > core.MAX_RESULT_BYTES then
    return { id = req.id, ok = false, error = "result is " .. #text .. " bytes, past the " .. core.MAX_RESULT_BYTES .. " limit" }
  end
  return { id = req.id, ok = true, result = result }
end

local function read_all(path)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local text = file:read("a")
  file:close()
  return text
end

-- Looks in spool/in for request files, handles up to limit of them and writes spool/out/<id>.res.
-- Returns how many it handled. A file name must be letters, digits, underscore or dash plus .req.
function core.poll_once(reaper, spool, ctx, limit)
  ctx.render_folder = ctx.render_folder or (spool .. "/render")
  local handled = 0
  local names = list_files(reaper, spool .. "/in")
  for _, name in ipairs(names) do
    if handled >= (limit or 4) then
      break
    end
    local id = name:match("^([%w_-]+)%.req$")
    if id then
      local path = spool .. "/in/" .. name
      local text = read_all(path)
      os.remove(path)
      local req, err = json.decode(text or "")
      local response
      if type(req) ~= "table" then
        response = { id = id, ok = false, error = "bad request: " .. tostring(err) }
      else
        req.id = id
        response = core.handle(reaper, req, ctx)
      end
      local out = spool .. "/out/" .. id .. ".res"
      local file = io.open(out .. ".tmp", "wb")
      if file then
        file:write(json.encode(response))
        file:close()
        os.rename(out .. ".tmp", out)
      end
      handled = handled + 1
    end
  end
  return handled
end

return core
