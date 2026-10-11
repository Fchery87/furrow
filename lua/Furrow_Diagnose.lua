-- @description Furrow: Write diagnostic report
-- Writes the project's tracks, FX, sends, RS5K settings, SeqCore state and undo history to
-- diag/report.txt in the repo. Run it once, with the rack open, and share the file.
local dir = debug.getinfo(1, "S").source:match("^@?(.*[/\\])")
package.path = dir .. "?.lua;" .. package.path
local wire = require("furrow.wire")

local lines = {}
local function out(fmt, ...)
  lines[#lines + 1] = select("#", ...) > 0 and string.format(fmt, ...) or fmt
end

local function guarded(label, fn)
  local ok, err = pcall(fn)
  if not ok then
    out("ERROR in %s: %s", label, tostring(err))
  end
end

out("Furrow diagnostic report")
out("REAPER %s", reaper.GetAppVersion())
out("play state %d (1 playing, 2 paused, 4 recording)", reaper.GetPlayState())
out("tempo %.3f", reaper.Master_GetTempo())
out("undo next: %s", tostring(reaper.Undo_CanUndo2(0)))
out("redo next: %s", tostring(reaper.Undo_CanRedo2(0)))
out("")

local function formatted(track, fx, param)
  local ok, text = reaper.TrackFX_GetFormattedParamValue(track, fx, param, "")
  return ok and text or "?"
end

local function dump_fx(track)
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, fx, "")
    out("  fx %d  %s  enabled=%s offline=%s", fx, name, tostring(reaper.TrackFX_GetEnabled(track, fx)),
      tostring(reaper.TrackFX_GetOffline(track, fx)))
    if name:find("Furrow SeqCore", 1, true) then
      local rev = reaper.TrackFX_GetParam(track, fx, wire.PARAM_REVISION)
      local slot = reaper.TrackFX_GetParam(track, fx, wire.PARAM_SLOT)
      out("    revision param %s   slot param %s", tostring(rev), tostring(slot))
    elseif name:find("ReaSamplOmatic5000", 1, true) then
      local _, file = reaper.TrackFX_GetNamedConfigParm(track, fx, "FILE0")
      out("    FILE0 %s", tostring(file))
      for p = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
        local _, pname = reaper.TrackFX_GetParamName(track, fx, p, "")
        local value = reaper.TrackFX_GetParamNormalized(track, fx, p)
        local _, active = reaper.TrackFX_GetNamedConfigParm(track, fx, "param." .. p .. ".plink.active")
        local _, cc = reaper.TrackFX_GetNamedConfigParm(track, fx, "param." .. p .. ".plink.midi_msg2")
        out("    param %2d %-32s norm %.4f  %-14s plink.active=%s cc=%s", p, pname, value,
          formatted(track, fx, p), tostring(active), tostring(cc))
      end
    end
  end
end

local function dump_sends(track)
  for i = 0, reaper.GetTrackNumSends(track, 0) - 1 do
    local _, dest = reaper.GetTrackSendName(track, i, "")
    out("  send %d to %s  srcchan %d  midiflags %d  mute %d  vol %.3f", i, dest,
      reaper.GetTrackSendInfo_Value(track, 0, i, "I_SRCCHAN"),
      reaper.GetTrackSendInfo_Value(track, 0, i, "I_MIDIFLAGS"),
      reaper.GetTrackSendInfo_Value(track, 0, i, "B_MUTE"),
      reaper.GetTrackSendInfo_Value(track, 0, i, "D_VOL"))
  end
end

out("tracks")
guarded("tracks", function()
  for i = 0, reaper.CountTracks(0) - 1 do
    local track = reaper.GetTrack(0, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
    out("track %d  %s  depth %d  mute %d  mainsend %d  vol %.3f  nchan %d", i, name,
      reaper.GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"), reaper.GetMediaTrackInfo_Value(track, "B_MUTE"),
      reaper.GetMediaTrackInfo_Value(track, "B_MAINSEND"), reaper.GetMediaTrackInfo_Value(track, "D_VOL"),
      reaper.GetMediaTrackInfo_Value(track, "I_NCHAN"))
    local _, ext = reaper.GetSetMediaTrackInfo_String(track, "P_EXT:Furrow", "", false)
    if ext ~= "" then
      out("  P_EXT:Furrow  %s", (ext:gsub("\n", " | ")))
    end
    dump_fx(track)
    dump_sends(track)
  end
end)

out("")
out("gmem")
guarded("gmem", function()
  reaper.gmem_attach(wire.GMEM)
  out("directory magic %s wire %s", tostring(reaper.gmem_read(wire.DIR_MAGIC)), tostring(reaper.gmem_read(wire.DIR_WIRE)))
  for s = 0, wire.SLOTS - 1 do
    local owner = reaper.gmem_read(wire.DIR_OWNER + s)
    if owner ~= 0 then
      local base = wire.SLOT_BASE + s * wire.SLOT_SIZE
      local function h(i) return reaper.gmem_read(base + i) end
      out("slot %d owner %s magic %s wire %s heartbeat %s playing %s beat %.3f lanes %s pool_free %s",
        s, tostring(owner), tostring(h(wire.H_MAGIC)), tostring(h(wire.H_WIRE)), tostring(h(wire.H_HEARTBEAT)),
        tostring(h(wire.H_PLAYING)), h(wire.H_BEAT), tostring(h(wire.H_LANE_COUNT)), tostring(h(wire.H_POOL_FREE)))
      out("  revision %s generation %s block_us %s block_max_us %s overruns %s load_error %s",
        tostring(h(wire.H_REVISION)), tostring(h(wire.H_GENERATION)), tostring(h(wire.H_BLOCK_US)),
        tostring(h(wire.H_BLOCK_MAX_US)), tostring(h(wire.H_OVERRUNS)), tostring(h(wire.H_LOAD_ERROR)))
    end
  end
end)

local folder = dir .. "../diag/"
reaper.RecursiveCreateDirectory(folder, 0)
local path = folder .. "report.txt"
local file = io.open(path, "w")
if file then
  file:write(table.concat(lines, "\n"), "\n")
  file:close()
  reaper.ShowConsoleMsg("Furrow report written to " .. path .. "\n")
else
  reaper.ShowConsoleMsg(table.concat(lines, "\n") .. "\n")
end
