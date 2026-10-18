-- @description Furrow: Open rack
-- Opens the Furrow rack window for the selected rack, or offers to create one.
local dir = debug.getinfo(1, "S").source:match("^@?(.*[/\\])")
package.path = dir .. "?.lua;" .. package.path

if not reaper.ImGui_GetBuiltinPath then
  reaper.ShowMessageBox(
    "Furrow needs ReaImGui 0.9 or newer. Install it from Extensions > ReaPack > Browse packages, then restart REAPER.",
    "Furrow", 0)
  return
end

package.path = reaper.ImGui_GetBuiltinPath() .. "/?.lua;" .. package.path
local ok, ImGui = pcall(function()
  return require("imgui")("0.9")
end)
if not ok or not ImGui then
  reaper.ShowMessageBox(
    "Furrow could not load ReaImGui API 0.9. Install or update ReaImGui from Extensions > ReaPack > Browse packages, then restart REAPER.",
    "Furrow", 0)
  return
end
require("furrow.ui").run(ImGui)
