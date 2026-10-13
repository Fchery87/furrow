-- @description Furrow: Start the REAPER bridge
-- Lets a local tool read this project and run requests against REAPER. It polls the folder
-- ~/.furrow-bridge/in for request files, runs each one, and writes the result to
-- ~/.furrow-bridge/out. Anything that can write to that folder can run code in REAPER with
-- your permissions, so the folder must be private to you (mode 700). The bridge server
-- creates it that way. Start this script on purpose, in a scratch project. It stops when you
-- run it again, or when a shutdown request arrives.
--
-- It will not start when the project has unsaved changes, unless the file
-- ~/.furrow-bridge/allow_dirty exists.
local dir = debug.getinfo(1, "S").source:match("^@?(.*[/\\])")
package.path = dir .. "?.lua;" .. package.path
local core = require("core")

local home = os.getenv("HOME")
if not home or home == "" then
  reaper.ShowMessageBox("The bridge needs the HOME environment variable.", "Furrow bridge", 0)
  return
end
local spool = home .. "/.furrow-bridge"

-- A second run stops the first, through a flag in extended state.
if reaper.GetExtState("FurrowBridge", "running") == "1" then
  reaper.SetExtState("FurrowBridge", "stop", "1", false)
  reaper.ShowConsoleMsg("Furrow bridge: asked the running bridge to stop.\n")
  return
end

if not reaper.EnumerateFiles(spool .. "/in", 0) and not io.open(spool .. "/in/.keep", "rb") then
  reaper.ShowMessageBox("Start the bridge server first. It creates " .. spool .. " with private permissions.",
    "Furrow bridge", 0)
  return
end

local allowed_dirty = io.open(spool .. "/allow_dirty", "rb")
if allowed_dirty then
  allowed_dirty:close()
elseif reaper.IsProjectDirty(0) ~= 0 then
  reaper.ShowMessageBox("This project has unsaved changes. Save it, or create " .. spool .. "/allow_dirty first.",
    "Furrow bridge", 0)
  return
end

reaper.SetExtState("FurrowBridge", "running", "1", false)
reaper.SetExtState("FurrowBridge", "stop", "0", false)
local ctx = {}

local function finish()
  reaper.SetExtState("FurrowBridge", "running", "0", false)
  reaper.ShowConsoleMsg("Furrow bridge stopped.\n")
end

reaper.atexit(finish)
reaper.ShowConsoleMsg("Furrow bridge running on " .. spool .. "\n")

local function loop()
  if reaper.GetExtState("FurrowBridge", "stop") == "1" or ctx.stopping then
    reaper.SetExtState("FurrowBridge", "running", "0", false)
    return
  end
  local ok, err = pcall(core.poll_once, reaper, spool, ctx, 4)
  if not ok then
    reaper.ShowConsoleMsg("Furrow bridge error: " .. tostring(err) .. "\n")
  end
  reaper.defer(loop)
end

reaper.defer(loop)
