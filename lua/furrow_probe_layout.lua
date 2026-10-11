-- Create the Furrow routing probe once.
-- A second run finds the named tracks and stops. Remove them before running again.
-- This script uses REAPER's own send API. It does not use SWS.

local NAME = {
  folder = "Furrow Probe Folder",
  engine = "Furrow Probe Engine",
}
local LANE_COUNT = 4

local function track_name(track)
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  return name
end

local function find_named(name)
  local index = 0
  local count = reaper.CountTracks(0)
  while index < count do
    local track = reaper.GetTrack(0, index)
    if track_name(track) == name then
      return track
    end
    index = index + 1
  end
  return nil
end

local function already_present()
  if find_named(NAME.folder) or find_named(NAME.engine) then
    return true
  end
  local lane = 0
  while lane < LANE_COUNT do
    if find_named(string.format("Furrow Probe Lane %d", lane + 1)) then
      return true
    end
    lane = lane + 1
  end
  return false
end

if already_present() then
  reaper.ShowMessageBox(
    "The Furrow probe layout is already in this project. Remove it before creating another.",
    "Furrow probe",
    0
  )
  return
end

reaper.Undo_BeginBlock()
reaper.PreventUIRefresh(1)

local start = reaper.CountTracks(0)

local function insert(index, name)
  reaper.InsertTrackAtIndex(index, true)
  local track = reaper.GetTrack(0, index)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  return track
end

local missing = nil

local function add_fx(track, name)
  local index = reaper.TrackFX_AddByName(track, name, false, -1)
  if index < 0 then
    missing = missing or name
  end
  return index
end

local folder = insert(start, NAME.folder)
reaper.SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 1)

local engine = insert(start + 1, NAME.engine)
reaper.SetMediaTrackInfo_Value(engine, "B_MAINSEND", 0)
reaper.SetMediaTrackInfo_Value(engine, "I_NCHAN", 8)
add_fx(engine, "JS:furrow_probe_engine.jsfx")

local lane = 0
while lane < LANE_COUNT do
  local track = insert(start + 2 + lane, string.format("Furrow Probe Lane %d", lane + 1))
  reaper.SetMediaTrackInfo_Value(track, "I_NCHAN", 2)
  local receiver = add_fx(track, "JS:furrow_probe_receiver.jsfx")
  if receiver >= 0 then
    -- Parameter 0 is the receiver slot. Each slot writes its own gmem event region.
    reaper.TrackFX_SetParam(track, receiver, 0, lane)
  end
  local send = reaper.CreateTrackSend(engine, track)
  -- Audio source -1 means this send carries no audio. The folder receives lane audio.
  reaper.SetTrackSendInfo_Value(engine, 0, send, "I_SRCCHAN", -1)
  -- Low 5 bits are the one-based source channel filter. 0 would pass every lane to every receiver.
  -- Next 5 bits are the one-based destination channel. Lane 1 keeps zero-based channel 0.
  local flags = (1 + lane) | ((1 + lane) << 5)
  reaper.SetTrackSendInfo_Value(engine, 0, send, "I_MIDIFLAGS", flags)
  lane = lane + 1
end

local last = reaper.GetTrack(0, start + 1 + LANE_COUNT)
reaper.SetMediaTrackInfo_Value(last, "I_FOLDERDEPTH", -1)

if missing then
  local index = start + 1 + LANE_COUNT
  while index >= start do
    reaper.DeleteTrack(reaper.GetTrack(0, index))
    index = index - 1
  end
end

reaper.PreventUIRefresh(-1)
reaper.TrackList_AdjustWindows(false)
reaper.Undo_EndBlock("Create Furrow probe layout", -1)

if missing then
  reaper.ShowMessageBox(
    "REAPER could not load " .. missing .. ". Copy both files from jsfx/ into the Effects directory and run this again.",
    "Furrow probe",
    0
  )
end
