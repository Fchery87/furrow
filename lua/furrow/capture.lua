-- Capture from an item: decides whether an item is an unedited Furrow commit, and restores
-- the source snapshot of an unedited one. Pure functions, so the host reads the item and
-- the stored text, and this module decides what they mean.
local commit = require("furrow.commit")

local capture = {}

-- commit.fingerprint always writes 16 hex digits, so any other length is damage.
local META = "^1 group (%d+) lane (%d+) rack (%d+) fp (" .. string.rep("%x", 16) .. ")$"

-- The metadata a commit writes on each item, or nil when it is missing or malformed.
function capture.parse_meta(text)
  if type(text) ~= "string" then
    return nil
  end
  local group, lane, rack, fp = text:match(META)
  if not fp then
    return nil
  end
  return { group = tonumber(group), lane = tonumber(lane), rack = tonumber(rack), fp = fp }
end

-- "unedited" when the item still matches what the commit wrote, "edited" when its position,
-- length, take offset or events changed. Only "unedited" may restore the source silently.
function capture.check(meta, position, length, offset, events)
  -- A message the commit never writes, such as program change, is an edit by definition.
  for _, e in ipairs(events) do
    if e.supported == false then
      return "edited"
    end
  end
  local now = commit.fingerprint(commit.item_text(position, length, offset, events))
  if now == meta.fp then
    return "unedited"
  end
  return "edited"
end

-- The engine commands that rebuild a source snapshot as pattern `target` of the rack. Each
-- source lane needs a rack lane with the same id, otherwise nothing is planned and the
-- missing ids come back. Only steps and lengths move. Lane settings belong to the rack's
-- lanes, so any that differ are reported with capture.setting_differences, not applied.
function capture.restore_commands(source, lanes, target)
  local wire = require("furrow.wire")
  local by_id = {}
  for _, lane in ipairs(lanes) do
    by_id[lane.id] = lane
  end
  local missing, commands = {}, {}
  for _, src in ipairs(source.lanes) do
    if not by_id[src.id] then
      missing[#missing + 1] = src.id
    end
  end
  if #missing > 0 then
    return nil, missing
  end
  for _, src in ipairs(source.lanes) do
    local dst = by_id[src.id]
    -- A new pattern starts every lane at the default length, so compare with that.
    if src.length ~= wire.DEFAULT_LENGTH then
      commands[#commands + 1] = { wire.OP_SET_LENGTH, target, dst.slot, src.length }
    end
    for step = 0, src.length - 1 do
      local row = src.rows[step] or wire.STEP_DEFAULTS
      for field = 0, wire.STEP_FIELDS - 1 do
        if row[field] ~= wire.STEP_DEFAULTS[field] then
          commands[#commands + 1] = { wire.OP_SET_STEP, target, dst.slot, step, field, row[field] }
        end
      end
    end
  end
  return commands
end

-- The lane settings that differ between a source snapshot and the rack's lanes, by lane id.
-- They are reported, never applied, because they are rack settings and restore must not
-- change lanes that other patterns use.
function capture.setting_differences(source, lanes)
  local by_id = {}
  for _, lane in ipairs(lanes) do
    by_id[lane.id] = lane
  end
  local found = {}
  local keys = { "note", "volume", "pan", "bend", "mod_a", "mod_b", "swing", "prob_key" }
  for _, src in ipairs(source.lanes) do
    local dst = by_id[src.id]
    if dst then
      for _, key in ipairs(keys) do
        if src[key] ~= dst[key] then
          found[#found + 1] = string.format("lane %d %s", src.id, key)
        end
      end
    end
  end
  return found
end

-- The source snapshot of one commit group, or nil and a message when it is not valid.
function capture.restore(stored)
  if type(stored) ~= "string" or stored == "" then
    return nil, "the commit's source snapshot is missing"
  end
  local source, err = commit.decode(stored)
  if not source then
    return nil, err or "the commit's source snapshot is not valid"
  end
  return source
end

return capture
