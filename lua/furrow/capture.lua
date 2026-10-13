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
