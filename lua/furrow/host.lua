-- REAPER objects for a rack: the folder, the engine track, lane tracks with RS5K, MIDI
-- sends and the lane-to-track links. Every function here runs on REAPER's UI thread.
local wire = require("furrow.wire")
local capture = require("furrow.capture")
local commit = require("furrow.commit")

local host = {}

host.ENGINE_NAMES = { "JS:furrow_seqcore.jsfx", "JS:Furrow/furrow_seqcore.jsfx" }
host.RS5K = "ReaSamplOmatic5000"
host.LINKS_KEY = "P_EXT:Furrow"
host.CHANNELS_KEY = "P_EXT:FurrowChannels"
-- Product choice, not a measurement. Velocity 1 plays this far below velocity 127.
host.MIN_VELOCITY_DB = -30

function host.track_name(track)
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  return name
end

-- Return packed ImGui RGBA, or nil when the track uses REAPER's default colour.
function host.track_color(track)
  if not track then
    return nil
  end
  local native = reaper.GetTrackColor(track)
  if not native or native == 0 then
    return nil
  end
  local r, g, b = reaper.ColorFromNative(native)
  return (r << 24) | (g << 16) | (b << 8) | 0xFF
end

function host.track_by_guid(proj, guid)
  for i = 0, reaper.CountTracks(proj) - 1 do
    local track = reaper.GetTrack(proj, i)
    if reaper.GetTrackGUID(track) == guid then
      return track
    end
  end
  return nil
end

function host.engine_fx(track)
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, fx, "")
    if name:find("Furrow SeqCore", 1, true) then
      return fx
    end
  end
  return nil
end

-- Every SeqCore in proj, as { engine = track, fx = index, rack = parent folder or nil }.
function host.find_racks(proj)
  local racks = {}
  for i = 0, reaper.CountTracks(proj) - 1 do
    local track = reaper.GetTrack(proj, i)
    local fx = host.engine_fx(track)
    if fx then
      racks[#racks + 1] = { engine = track, fx = fx, rack = reaper.GetParentTrack(track) }
    end
  end
  return racks
end

-- The slots that some SeqCore in an open project reports. A slot outside this set has an
-- owner that no longer exists, because SeqCore has no destructor to release it.
function host.live_slots()
  local live = {}
  local p = 0
  while true do
    local proj = reaper.EnumProjects(p)
    if not proj then
      break
    end
    for i = 0, reaper.CountTracks(proj) - 1 do
      local track = reaper.GetTrack(proj, i)
      for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
        local _, name = reaper.TrackFX_GetFXName(track, fx, "")
        if name:find("Furrow SeqCore", 1, true) then
          local slot = math.floor(reaper.TrackFX_GetParam(track, fx, wire.PARAM_SLOT) + 0.5)
          if slot >= 0 then
            live[slot] = true
          end
        end
      end
    end
    p = p + 1
  end
  return live
end

-- Index of the last track inside the folder, or the folder itself when it is empty.
function host.last_child(folder)
  local index = math.floor(reaper.GetMediaTrackInfo_Value(folder, "IP_TRACKNUMBER")) - 1
  if reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH") < 1 then
    return index
  end
  local depth = 1
  local last = index
  for i = index + 1, reaper.CountTracks(0) - 1 do
    last = i
    depth = depth + reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH")
    if depth <= 0 then
      break
    end
  end
  return last
end

-- Inserts a track as the folder's last child and moves the folder's closing depth onto it.
function host.append_child(folder, name)
  local last = host.last_child(folder)
  local empty = last == math.floor(reaper.GetMediaTrackInfo_Value(folder, "IP_TRACKNUMBER")) - 1
  local last_track = reaper.GetTrack(0, last)
  local closing = empty and -1 or reaper.GetMediaTrackInfo_Value(last_track, "I_FOLDERDEPTH")
  if not empty then
    reaper.SetMediaTrackInfo_Value(last_track, "I_FOLDERDEPTH", 0)
  end
  reaper.InsertTrackAtIndex(last + 1, true)
  local track = reaper.GetTrack(0, last + 1)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  reaper.SetMediaTrackInfo_Value(track, "I_FOLDERDEPTH", closing)
  return track
end

-- Deletes a track and gives its closing folder depth to the track above it.
function host.delete_child(track)
  local depth = reaper.GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH")
  local index = math.floor(reaper.GetMediaTrackInfo_Value(track, "IP_TRACKNUMBER")) - 1
  if depth < 0 and index > 0 then
    local above = reaper.GetTrack(0, index - 1)
    reaper.SetMediaTrackInfo_Value(above, "I_FOLDERDEPTH", reaper.GetMediaTrackInfo_Value(above, "I_FOLDERDEPTH") + depth)
  end
  reaper.DeleteTrack(track)
end

function host.add_engine(track)
  for _, name in ipairs(host.ENGINE_NAMES) do
    local fx = reaper.TrackFX_AddByName(track, name, false, -1)
    if fx >= 0 then
      return fx
    end
  end
  return nil
end

-- Creates the Rack folder and its Engine child below the selected track or at the end.
-- Returns the rack, or nil and a message. The caller wraps it in one undo block.
function host.create_rack()
  local selected = reaper.GetSelectedTrack(0, 0)
  local index = selected and host.last_child(selected) + 1 or reaper.CountTracks(0)
  reaper.InsertTrackAtIndex(index, true)
  local folder = reaper.GetTrack(0, index)
  reaper.GetSetMediaTrackInfo_String(folder, "P_NAME", "Furrow Rack", true)
  reaper.SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 1)
  local engine = host.append_child(folder, "Furrow Engine")
  reaper.SetMediaTrackInfo_Value(engine, "B_MAINSEND", 0)
  local fx = host.add_engine(engine)
  if not fx then
    reaper.DeleteTrack(engine)
    reaper.DeleteTrack(folder)
    return nil, "REAPER could not load furrow_seqcore.jsfx. Copy it into the Effects folder."
  end
  return { engine = engine, fx = fx, rack = folder }
end

-- Lane links live on the rack track as "1" then one "lane <id> <track GUID>" line a lane.
function host.read_links(rack)
  local links = {}
  if not rack then
    return links
  end
  local _, text = reaper.GetSetMediaTrackInfo_String(rack, host.LINKS_KEY, "", false)
  for id, guid in (text or ""):gmatch("lane (%d+) (%S+)") do
    links[tonumber(id)] = guid
  end
  return links
end

function host.write_links(rack, links)
  local ids = {}
  for id in pairs(links) do
    ids[#ids + 1] = id
  end
  table.sort(ids)
  local lines = { "1" }
  for _, id in ipairs(ids) do
    lines[#lines + 1] = string.format("lane %d %s", id, links[id])
  end
  reaper.GetSetMediaTrackInfo_String(rack, host.LINKS_KEY, table.concat(lines, "\n"), true)
end

function host.read_lane_channels(rack)
  local channels = {}
  if not rack then
    return channels
  end
  local _, text = reaper.GetSetMediaTrackInfo_String(rack, host.CHANNELS_KEY, "", false)
  for id, channel in (text or ""):gmatch("lane (%d+) channel (%d+)") do
    channel = tonumber(channel)
    if channel >= 1 and channel <= 16 then
      channels[tonumber(id)] = channel
    end
  end
  return channels
end

function host.write_lane_channels(rack, channels)
  local ids = {}
  for id in pairs(channels) do
    ids[#ids + 1] = id
  end
  table.sort(ids)
  local lines = { "1" }
  for _, id in ipairs(ids) do
    local channel = math.floor(channels[id])
    if channel < 1 or channel > 16 then
      return false, "MIDI channel must be from 1 to 16"
    end
    lines[#lines + 1] = string.format("lane %d channel %d", id, channel)
  end
  reaper.GetSetMediaTrackInfo_String(rack, host.CHANNELS_KEY, table.concat(lines, "\n"), true)
  return true
end

-- A lane target must be a track in this project, cannot be its own engine, cannot share a
-- target/channel pair with another lane, and must not route back to the engine.
function host.validate_lane_target(proj, engine, target, channel, lane_id, links, channels)
  channel = math.floor(tonumber(channel) or 0)
  if not target then
    return false, "choose an instrument track"
  end
  if target == engine then
    return false, "the engine track cannot be a lane target"
  end
  if reaper.GetMediaTrackInfo_Value(target, "I_FOLDERDEPTH") > 0 then
    return false, "a folder track cannot be a lane target"
  end
  local _, managed_items = reaper.GetSetMediaTrackInfo_String(target, host.ITEMS_KEY, "", false)
  if managed_items and managed_items ~= "" then
    return false, "a managed item track cannot be a lane target"
  end
  if channel < 1 or channel > 16 then
    return false, "MIDI channel must be from 1 to 16"
  end
  local present = false
  for i = 0, reaper.CountTracks(proj) - 1 do
    if reaper.GetTrack(proj, i) == target then
      present = true
      break
    end
  end
  if not present then
    return false, "the target track is not in this project"
  end
  local guid = reaper.GetTrackGUID(target)
  for id, assigned_guid in pairs(links or {}) do
    if tonumber(id) ~= lane_id and assigned_guid == guid and ((channels or {})[tonumber(id)] or 1) == channel then
      return false, "that track and MIDI channel are already assigned to another lane"
    end
  end
  local visited, pending = {}, { target }
  while #pending > 0 do
    local track = table.remove(pending)
    if track == engine then
      return false, "the target would create a routing cycle to the engine"
    end
    if not visited[track] then
      visited[track] = true
      for i = 0, reaper.GetTrackNumSends(track, 0) - 1 do
        local next_track = reaper.GetTrackSendInfo_Value(track, 0, i, "P_DESTTRACK")
        if next_track then
          pending[#pending + 1] = next_track
        end
      end
    end
  end
  return true
end

local function find_param(track, fx, prefix)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if name:lower():sub(1, #prefix) == prefix then
      return i
    end
  end
  return nil
end

local function db_of(track, fx, param, normalized)
  local _, text = reaper.TrackFX_FormatParamValueNormalized(track, fx, param, normalized, "")
  if text:lower():find("inf", 1, true) then
    return -math.huge
  end
  return tonumber(text:match("[-+]?%d+%.?%d*")) or -math.huge
end

-- RS5K showed +0.0 dB at 0.5 and -inf at 0. Search that range for the floor's position.
local function min_velocity_position(track, fx, param, target)
  local lo, hi = 0, 0.5
  for _ = 1, 30 do
    local mid = (lo + hi) / 2
    if db_of(track, fx, param, mid) < target then
      lo = mid
    else
      hi = mid
    end
  end
  return hi
end

local function link(track, fx, param, cc)
  local prefix = "param." .. param .. ".plink."
  local keys = {
    { "param." .. param .. ".mod.active", 1 },
    { prefix .. "active", 1 },
    { prefix .. "effect", -100 },
    { prefix .. "midi_bus", 0 },
    { prefix .. "midi_chan", 0 },
    { prefix .. "midi_msg", 176 },
    { prefix .. "midi_msg2", cc },
    { prefix .. "scale", 1 },
    { prefix .. "offset", 0 },
  }
  for _, kv in ipairs(keys) do
    reaper.TrackFX_SetNamedConfigParm(track, fx, kv[1], tostring(kv[2]))
  end
end

-- Pattern names live on the rack track as "pattern <index> <name>" lines. A pattern with no
-- line still exists once the engine holds data for it, so the list always includes the ones
-- in use.
host.PATTERNS_KEY = "P_EXT:FurrowPatterns"
host.PATTERN_NAME_MAX = 40

function host.default_pattern_name(index)
  return "Pattern " .. (index + 1)
end

function host.clean_pattern_name(text)
  local name = (text or ""):gsub("[%c]", " "):gsub("^%s+", ""):gsub("%s+$", "")
  return name:sub(1, host.PATTERN_NAME_MAX)
end

function host.read_patterns(rack)
  local names = {}
  if not rack then
    return names
  end
  local _, text = reaper.GetSetMediaTrackInfo_String(rack, host.PATTERNS_KEY, "", false)
  for index, name in (text or ""):gmatch("pattern (%d+) ([^\n]*)") do
    names[tonumber(index)] = name
  end
  return names
end

function host.write_patterns(rack, names)
  local indexes = {}
  for index in pairs(names) do
    indexes[#indexes + 1] = index
  end
  table.sort(indexes)
  local lines = { "1" }
  for _, index in ipairs(indexes) do
    lines[#lines + 1] = string.format("pattern %d %s", index, names[index])
  end
  reaper.GetSetMediaTrackInfo_String(rack, host.PATTERNS_KEY, table.concat(lines, "\n"), true)
end

-- The quarter-note position of the next measure start at least margin seconds ahead of the
-- play cursor, from REAPER's time map. Falls back to four beats a measure.
function host.next_measure_beat(proj, margin)
  local qn = reaper.TimeMap2_timeToQN(proj, reaper.GetPlayPosition2Ex(proj) + margin)
  if reaper.TimeMap_QNToMeasures then
    local _, start_qn, end_qn = reaper.TimeMap_QNToMeasures(proj, qn)
    if end_qn and end_qn > qn then
      return end_qn
    end
  end
  return (math.floor(qn / 4) + 1) * 4
end

-- Configures RS5K by the instrument contract in the spec.
function host.configure_rs5k(track, fx, file)
  reaper.TrackFX_SetNamedConfigParm(track, fx, "FILE0", file)
  reaper.TrackFX_SetNamedConfigParm(track, fx, "DONE", "")
  local _, loaded = reaper.TrackFX_GetNamedConfigParm(track, fx, "FILE0")
  if loaded ~= file then
    return false, "RS5K did not load " .. file
  end
  local obey = find_param(track, fx, "obey note-offs")
  local min_gain = find_param(track, fx, "gain for minimum velocity")
  local volume = find_param(track, fx, "volume")
  local pan = find_param(track, fx, "pan")
  if not (obey and min_gain and volume and pan) then
    return false, "RS5K is missing an expected parameter"
  end
  reaper.TrackFX_SetParamNormalized(track, fx, obey, 1)
  reaper.TrackFX_SetParamNormalized(track, fx, min_gain, min_velocity_position(track, fx, min_gain, host.MIN_VELOCITY_DB))
  link(track, fx, volume, 7)
  link(track, fx, pan, 10)
  return true
end

-- Route slot n is JSFX bus n // 16 and channel n % 16. The send maps it to channel 1 on
-- bus 0 of the lane track. Field layout from the routing probe: low 5 bits source channel,
-- next 5 destination channel, one-based buses at bit 14 and bit 22.
function host.midi_flags(slot, dest_channel)
  return (slot % 16 + 1) | (dest_channel << 5) | ((slot // 16 + 1) << 14) | (1 << 22)
end

function host.add_send(engine, track, slot, channel)
  local send = reaper.CreateTrackSend(engine, track)
  if not send or send < 0 then
    return nil, "REAPER could not create the lane MIDI send"
  end
  reaper.SetTrackSendInfo_Value(engine, 0, send, "I_SRCCHAN", -1)
  reaper.SetTrackSendInfo_Value(engine, 0, send, "I_MIDIFLAGS", host.midi_flags(slot, channel or 1))
  return send
end

function host.remove_send(engine, track, slot, channel)
  local flags = host.midi_flags(slot, channel)
  for index = reaper.GetTrackNumSends(engine, 0) - 1, 0, -1 do
    if reaper.GetTrackSendInfo_Value(engine, 0, index, "P_DESTTRACK") == track
        and reaper.GetTrackSendInfo_Value(engine, 0, index, "I_MIDIFLAGS") == flags then
      reaper.RemoveTrackSend(engine, 0, index)
      return true
    end
  end
  return false
end

function host.lane_target_choices(proj, engine)
  local choices = {}
  for i = 0, reaper.CountTracks(proj) - 1 do
    local track = reaper.GetTrack(proj, i)
    local _, items_lane = reaper.GetSetMediaTrackInfo_String(track, host.ITEMS_KEY, "", false)
    if track ~= engine and reaper.GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH") <= 0 and items_lane == "" then
      choices[#choices + 1] = track
    end
  end
  return choices
end

function host.route_lane(engine, slot, old_track, old_channel, target, channel)
  if old_track == target and old_channel == channel then
    return true
  end
  local new_send = host.add_send(engine, target, slot, channel)
  if new_send == nil then return false, "REAPER could not create the lane MIDI send" end
  if old_track then
    host.remove_send(engine, old_track, slot, old_channel)
  end
  return true
end

-- Creates a sample lane's track, RS5K and send. Returns the track, or nil and a message
-- after removing whatever it created.
function host.create_sample_lane(rack, engine, file, slot)
  local name = file:match("([^/\\]+)$"):gsub("%.[^.]*$", "")
  local track = host.append_child(rack, name)
  local built, ok, err = pcall(function()
    local fx = reaper.TrackFX_AddByName(track, host.RS5K, false, -1)
    if fx < 0 then
      return false, "REAPER could not add RS5K"
    end
    local configured, message = host.configure_rs5k(track, fx, file)
    if configured then
      host.add_send(engine, track, slot)
    end
    return configured, message
  end)
  if not built or not ok then
    host.delete_child(track)
    return nil, built and err or ok
  end
  return track
end

-- Committed items. Each lane has its own "items" track inside the rack folder. The track holds
-- the item gate first, so the rack's mode decides whether its MIDI plays, and a MIDI send to
-- the lane's track. Its main send is off. P_EXT:FurrowItems names the lane it belongs to.
host.ITEMS_KEY = "P_EXT:FurrowItems"
host.ITEM_KEY = "P_EXT:Furrow"
host.GATE_NAMES = { "JS:furrow_itemgate.jsfx", "JS:Furrow/furrow_itemgate.jsfx" }
host.ITEM_NOTE_STEPS = 0.25

function host.find_items_track(rack, lane_id)
  local first = math.floor(reaper.GetMediaTrackInfo_Value(rack, "IP_TRACKNUMBER"))
  for i = first, host.last_child(rack) do
    local track = reaper.GetTrack(0, i)
    local _, text = reaper.GetSetMediaTrackInfo_String(track, host.ITEMS_KEY, "", false)
    if text == "lane " .. lane_id then
      return track
    end
  end
  return nil
end

-- Creates the items track for a lane. Returns the track, or nil and a message after removing
-- whatever it created.
function host.create_items_track(rack, lane_track, name, lane_id, rack_id)
  local track = host.append_child(rack, name .. " items")
  local ok, err = pcall(function()
    reaper.SetMediaTrackInfo_Value(track, "B_MAINSEND", 0)
    reaper.GetSetMediaTrackInfo_String(track, host.ITEMS_KEY, "lane " .. lane_id, true)
    local gate
    for _, gate_name in ipairs(host.GATE_NAMES) do
      gate = reaper.TrackFX_AddByName(track, gate_name, false, -1)
      if gate >= 0 then
        break
      end
    end
    if not gate or gate < 0 then
      error("REAPER could not load furrow_itemgate.jsfx. Copy it into the Effects folder.", 0)
    end
    reaper.TrackFX_SetParam(track, gate, 0, rack_id)
    local send = reaper.CreateTrackSend(track, lane_track)
    reaper.SetTrackSendInfo_Value(track, 0, send, "I_SRCCHAN", -1)
    reaper.SetTrackSendInfo_Value(track, 0, send, "I_MIDIFLAGS", 1 << 5)
  end)
  if not ok then
    host.delete_child(track)
    return nil, err
  end
  return track
end

-- Writes one MIDI item of `steps` sixteenths from quarter note `qn`, with the rendered events
-- and a versioned reference to the commit group. Returns the item.
function host.write_item(proj, track, qn, steps, events, meta)
  local start = reaper.TimeMap2_QNToTime(proj, qn)
  local stop = reaper.TimeMap2_QNToTime(proj, qn + steps * host.ITEM_NOTE_STEPS)
  local item = reaper.CreateNewMIDIItemInProj(track, start, stop, false)
  local take = reaper.GetActiveTake(item)
  for _, e in ipairs(events) do
    local ppq = reaper.MIDI_GetPPQPosFromProjQN(take, qn + e.beat)
    reaper.MIDI_InsertEvt(take, false, false, ppq, string.char(e.status, e.data1, e.data2))
  end
  reaper.MIDI_Sort(take)
  local fingerprint, fingerprint_error = host.item_fingerprint(item, take)
  if not fingerprint then
    error(fingerprint_error, 0)
  end
  reaper.GetSetMediaItemInfo_String(item, host.ITEM_KEY,
    string.format("1 group %d lane %d rack %d fp %s", meta.group, meta.lane, meta.rack, fingerprint), true)
  return item
end

local function item_properties(item)
  return {
    position = reaper.GetMediaItemInfo_Value(item, "D_POSITION"),
    length = reaper.GetMediaItemInfo_Value(item, "D_LENGTH"),
    startoffs = reaper.GetMediaItemInfo_Value(item, "D_STARTOFFS"),
    playrate = reaper.GetMediaItemInfo_Value(item, "D_PLAYRATE"),
    loopsrc = reaper.GetMediaItemInfo_Value(item, "B_LOOPSRC"),
    beatattach = reaper.GetMediaItemInfo_Value(item, "C_BEATATTACHMODE"),
  }
end

function host.item_fingerprint(item, take)
  local ok, raw = reaper.MIDI_GetAllEvts(take, "")
  if not ok then
    return nil, "REAPER could not read the item's MIDI events"
  end
  local normalized, err = capture.normalize_midi(raw)
  if not normalized then
    return nil, err
  end
  return commit.item_fingerprint(normalized, item_properties(item))
end

-- Reads the single selected MIDI item and classifies its source metadata. The returned item
-- pointer is only a preview reference; callers must rescan it before applying any changes.
function host.read_selected_capture(proj)
  if reaper.CountSelectedMediaItems(proj) ~= 1 then
    return nil, "select one MIDI item to capture"
  end
  local item = reaper.GetSelectedMediaItem(proj, 0)
  local take = item and reaper.GetActiveTake(item)
  if not take or not reaper.TakeIsMIDI(take) then
    return nil, "the selected item has no active MIDI take"
  end
  local _, metadata = reaper.GetSetMediaItemInfo_String(item, host.ITEM_KEY, "", false)
  local reference = capture.parse_reference(metadata)
  local source
  if reference then
    local _, value = reaper.GetProjExtState(proj, "FurrowCommits", tostring(reference.group))
    source = value
  end
  local fingerprint, fingerprint_error = host.item_fingerprint(item, take)
  if not fingerprint then
    return nil, fingerprint_error
  end
  local decision = capture.inspect(metadata, fingerprint, source)
  local ok, raw = reaper.MIDI_GetAllEvts(take, "")
  if not ok then
    return nil, "REAPER could not read the item's MIDI events"
  end
  local start = reaper.GetMediaItemInfo_Value(item, "D_POSITION")
  local stop = start + reaper.GetMediaItemInfo_Value(item, "D_LENGTH")
  local start_qn = reaper.TimeMap2_timeToQN(proj, start)
  local end_qn = reaper.TimeMap2_timeToQN(proj, stop)
  local notes, unsupported = capture.scan_midi(raw, function(ppq)
    return reaper.MIDI_GetProjQNFromPPQPos(take, ppq)
  end, end_qn)
  return {
    item = item, take = take, fingerprint = fingerprint, decision = decision, reference = reference, notes = notes,
    unsupported = unsupported, start_qn = start_qn, end_qn = end_qn,
  }
end

-- Removes an item from its track.
function host.delete_item(item)
  reaper.DeleteTrackMediaItem(reaper.GetMediaItem_Track(item), item)
end

-- Reads what capture needs from an item: its metadata text, its place, its take offset and
-- its events, with each event's beat counted from the item's start in quarter notes, the
-- same way write_item and commit.item_text use them.
function host.read_item(item)
  local take = reaper.GetActiveTake(item)
  local _, meta = reaper.GetSetMediaItemInfo_String(item, host.ITEM_KEY, "", false)
  local start_qn = reaper.MIDI_GetProjQNFromPPQPos(take, 0)
  local events = {}
  -- Reads every raw event until REAPER has no more. The note count leaves out note-offs.
  local index = 0
  while true do
    local ok, _, _, ppq, message = reaper.MIDI_GetEvt(take, index)
    if not ok then
      break
    end
    -- A two byte message, such as program change, has no third byte. It is marked so capture
    -- reads the item as edited rather than failing on a missing value.
    events[#events + 1] = {
      beat = reaper.MIDI_GetProjQNFromPPQPos(take, ppq) - start_qn,
      status = message:byte(1) or -1, data1 = message:byte(2) or -1, data2 = message:byte(3) or -1,
      supported = #message == 3,
    }
    index = index + 1
  end
  return {
    meta = meta,
    position = reaper.GetMediaItemInfo_Value(item, "D_POSITION"),
    length = reaper.GetMediaItemInfo_Value(item, "D_LENGTH"),
    offset = reaper.GetMediaItemTakeInfo_Value(take, "D_STARTOFFS"),
    events = events,
  }
end

return host
