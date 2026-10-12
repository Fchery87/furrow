-- @description Furrow: Item gate probe
-- Builds a "Furrow Items" track in the first rack the first time you run it, then switches the
-- rack between Live and Items mode each time after. The track holds the item gate and one MIDI
-- item with a note on every beat for the first lane. The item stays unmuted, so the gate alone
-- decides what you hear. Close the Furrow rack window first, because only one window can edit
-- a rack. Use a scratch project. The report goes to diag/gate.txt.
local dir = debug.getinfo(1, "S").source:match("^@?(.*[/\\])")
package.path = dir .. "?.lua;" .. package.path
local wire = require("furrow.wire")
local host = require("furrow.host")
local Coordinator = require("furrow.coordinator")

local ITEMS_NAME = "Furrow Items"
local GATE_NAMES = { "JS:furrow_itemgate.jsfx", "JS:Furrow/furrow_itemgate.jsfx" }
local ITEM_BARS = 2
local READY_SECONDS = 5
local SETTLE_FRAMES = 40

local proj = reaper.EnumProjects(-1)
local racks = host.find_racks(proj)
if #racks == 0 then
  reaper.ShowMessageBox("This project has no Furrow rack. Create one in the rack window first.", "Furrow", 0)
  return
end

local lines = {}
local function out(fmt, ...)
  lines[#lines + 1] = select("#", ...) > 0 and string.format(fmt, ...) or fmt
end

local function finish()
  local folder = dir .. "../diag/"
  reaper.RecursiveCreateDirectory(folder, 0)
  local file = io.open(folder .. "gate.txt", "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg("Furrow gate report written to " .. folder .. "gate.txt\n")
  else
    reaper.ShowConsoleMsg(table.concat(lines, "\n") .. "\n")
  end
end

local function find_items_track(rack)
  for i = host.last_child(rack.rack), reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
    if name == ITEMS_NAME then
      return track
    end
    if reaper.GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH") < 0 then
      break
    end
  end
  return nil
end

-- Adds the gate first in the track's chain and returns its index, or nil and a message.
local function add_gate(track, rack_id)
  for _, name in ipairs(GATE_NAMES) do
    local fx = reaper.TrackFX_AddByName(track, name, false, -1)
    if fx >= 0 then
      reaper.TrackFX_SetParam(track, fx, 0, rack_id)
      return fx
    end
  end
  return nil, "REAPER could not load furrow_itemgate.jsfx. Copy it into the Effects folder."
end

-- One MIDI item, ITEM_BARS bars long from the edit cursor, with a note on every beat.
local function add_item(track, note)
  local start = reaper.GetCursorPosition()
  local qn = reaper.TimeMap2_timeToQN(proj, start)
  local stop = reaper.TimeMap2_QNToTime(proj, qn + ITEM_BARS * 4)
  local item = reaper.CreateNewMIDIItemInProj(track, start, stop, false)
  local take = reaper.GetActiveTake(item)
  for beat = 0, ITEM_BARS * 4 - 1 do
    local from = reaper.MIDI_GetPPQPosFromProjQN(take, qn + beat)
    local to = reaper.MIDI_GetPPQPosFromProjQN(take, qn + beat + 0.5)
    reaper.MIDI_InsertNote(take, false, false, from, to, 0, note, 100, true)
  end
  reaper.MIDI_Sort(take)
  return item
end

local function build(c, rack, lanes)
  local lane_track = c:lane_track(lanes[1])
  if not lane_track then
    return nil, "the first lane has no track"
  end
  local track = host.append_child(rack.rack, ITEMS_NAME)
  reaper.SetMediaTrackInfo_Value(track, "B_MAINSEND", 0)
  local fx, err = add_gate(track, c.session.snapshot.rack.id)
  if not fx then
    host.delete_child(track)
    return nil, err
  end
  local send = reaper.CreateTrackSend(track, lane_track)
  reaper.SetTrackSendInfo_Value(track, 0, send, "I_SRCCHAN", -1)
  reaper.SetTrackSendInfo_Value(track, 0, send, "I_MIDIFLAGS", 1 << 5)
  add_item(track, lanes[1].note)
  return track
end

local c = Coordinator.new(proj, racks[1])
local started = reaper.time_precise()
local settle = 0
local phase = "wait"
local target_mode, built_now, items_track

local function loop()
  c:tick()
  if phase == "wait" then
    if c.status == "ready" and c.session.snapshot then
      local lanes = c:lanes()
      if #lanes == 0 then
        out("RESULT FAIL   the rack has no lane. Drop a sample into the rack window first.")
        finish()
        return
      end
      reaper.Undo_BeginBlock2(proj)
      items_track = find_items_track(racks[1])
      if not items_track then
        local err
        items_track, err = build(c, racks[1], lanes)
        if not items_track then
          reaper.Undo_EndBlock2(proj, "Furrow: item gate probe", -1)
          out("RESULT FAIL   %s", tostring(err))
          finish()
          return
        end
        built_now = true
      end
      reaper.Undo_EndBlock2(proj, "Furrow: item gate probe", -1)
      -- A new gate starts in Live. Every later run flips the mode.
      target_mode = built_now and 0 or (c.session.snapshot.rack.mode == 0 and 1 or 0)
      c:run("set playback mode", { { wire.OP_SET_RACK, wire.R_MODE, target_mode } })
      phase = "apply"
    elseif reaper.time_precise() - started > READY_SECONDS then
      out("RESULT FAIL   the rack never became ready, status %s, message %s", c.status, tostring(c.message))
      out("Close the Furrow rack window first, because only one window can edit a rack.")
      finish()
      return
    end
  elseif phase == "apply" then
    if not c:busy() then
      settle = settle + 1
      if settle >= SETTLE_FRAMES then
        local mode = c.session.snapshot.rack.mode
        local gate = c.session:h(wire.H_GATE)
        out("Furrow item gate probe")
        out("REAPER %s", reaper.GetAppVersion())
        out("%s", built_now and "built the Furrow Items track, its gate and one MIDI item" or "found the Furrow Items track")
        out("rack mode %s   gate word %s   message %s", mode == 0 and "Live" or "Items", tostring(gate), tostring(c.message))
        out("RESULT %s", ((mode == 1) == (gate == 1)) and c.message == nil and "PASS" or "FAIL")
        out("")
        out("Now play from the item's start with this window closed.")
        out("Live should sound only the sequencer. Items should sound only the item.")
        out("Mute and unmute the item, move it and delete it. Run this script again to switch mode.")
        finish()
        return
      end
    end
  end
  reaper.defer(loop)
end

reaper.defer(loop)
