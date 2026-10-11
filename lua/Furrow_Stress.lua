-- @description Furrow: Stress test with 500 rapid edits
-- Sends 500 step edits to the first lane of the first rack as one gesture, then checks the
-- final steps, the result count and SeqCore's block timing. It overwrites that lane's first
-- 16 steps and makes one undo point, so run it in a scratch project. Start playback first
-- to measure timing under playback. The report goes to diag/stress.txt.
local dir = debug.getinfo(1, "S").source:match("^@?(.*[/\\])")
package.path = dir .. "?.lua;" .. package.path
local wire = require("furrow.wire")
local host = require("furrow.host")
local Coordinator = require("furrow.coordinator")

local EDITS = 500
local PER_FRAME = 50
local CATCH_UP_FRAMES = 40
local READY_SECONDS = 5

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
  local path = folder .. "stress.txt"
  local file = io.open(path, "w")
  if file then
    file:write(table.concat(lines, "\n"), "\n")
    file:close()
    reaper.ShowConsoleMsg("Furrow stress report written to " .. path .. "\n")
  else
    reaper.ShowConsoleMsg(table.concat(lines, "\n") .. "\n")
  end
end

local c = Coordinator.new(proj, racks[1])
local started = reaper.time_precise()
local edits, model, sent = {}, {}, 0
local gesture, slot, completed_before
local catch_up = 0
local sent_at, done_at

math.randomseed(500)
for i = 1, EDITS do
  edits[i] = { math.random(0, 15), math.random(0, 1) }
  model[edits[i][1]] = edits[i][2]
end

local function report()
  local session = c.session
  local budget = session:h(wire.H_BLOCK_SIZE) / session:h(wire.H_SRATE) * 1e6
  local mismatches = 0
  for step = 0, 15 do
    if c:step_value(slot, step, wire.F_ON) ~= (model[step] or 0) then
      mismatches = mismatches + 1
    end
  end
  local results = c.completed - completed_before
  out("Furrow stress report")
  out("REAPER %s", reaper.GetAppVersion())
  out("playing %d   tempo %.2f", session:h(wire.H_PLAYING), reaper.Master_GetTempo())
  out("edits sent %d   results received %d   message %s", sent, results, tostring(c.message))
  out("send time %.1f ms   settle time %.1f ms", (sent_at - started) * 1000, (done_at - sent_at) * 1000)
  out("steps checked 16   mismatches %d", mismatches)
  out("block time now %.0f us   block time max since engine start %.0f us   budget %.0f us",
    session:h(wire.H_BLOCK_US), session:h(wire.H_BLOCK_MAX_US), budget)
  out("overruns %d   pool free %d", session:h(wire.H_OVERRUNS), session:h(wire.H_POOL_FREE))
  out("undo next: %s", tostring(reaper.Undo_CanUndo2(proj)))
  local pass = mismatches == 0 and results == EDITS and c.message == nil and session:h(wire.H_OVERRUNS) == 0
  out("RESULT %s", pass and "PASS" or "FAIL")
  finish()
end

local function loop()
  c:tick()
  if c.status ~= "ready" and not gesture then
    if reaper.time_precise() - started > READY_SECONDS then
      out("RESULT FAIL   the rack never became ready, status %s, message %s", c.status, tostring(c.message))
      finish()
      return
    end
  elseif not gesture then
    local lanes = c:lanes()
    if #lanes == 0 then
      out("RESULT FAIL   the rack has no lane. Drop a sample into the rack window first.")
      finish()
      return
    end
    slot = lanes[1].slot
    completed_before = c.completed
    gesture = c:begin("stress edits")
  elseif sent < EDITS then
    for _ = 1, PER_FRAME do
      if sent >= EDITS then
        break
      end
      sent = sent + 1
      local e = edits[sent]
      c:add(gesture, { { wire.OP_SET_STEP, 0, slot, e[1], wire.F_ON, e[2] } })
    end
    if sent >= EDITS then
      c:finish(gesture)
      sent_at = reaper.time_precise()
      done_at = sent_at
    end
  elseif c:busy() then
    done_at = reaper.time_precise()
  else
    catch_up = catch_up + 1
    if catch_up >= CATCH_UP_FRAMES then
      report()
      return
    end
  end
  reaper.defer(loop)
end

reaper.defer(loop)
