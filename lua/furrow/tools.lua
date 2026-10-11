-- Lane tools as pure functions. Each returns the step edits that turn the lane's current
-- steps into the tool's result, as { step, field, value }, and nothing when no step changes.
-- rows[step] holds the 16 field values of that step, for steps 0 to length - 1.
local wire = require("furrow.wire")

local tools = {}

local function changed_fields(edits, step, now, want)
  for field = 0, wire.STEP_FIELDS - 1 do
    if now[field] ~= want[field] then
      edits[#edits + 1] = { step, field, want[field] }
    end
  end
end

local function with_on(row, on)
  local copy = {}
  for field = 0, wire.STEP_FIELDS - 1 do
    copy[field] = row[field]
  end
  copy[wire.F_ON] = on
  return copy
end

-- Turns on every every-th step from step 0 and turns the others off.
function tools.fill(every, length, rows)
  local edits = {}
  for step = 0, length - 1 do
    changed_fields(edits, step, rows[step], with_on(rows[step], step % every == 0 and 1 or 0))
  end
  return edits
end

function tools.invert(length, rows)
  local edits = {}
  for step = 0, length - 1 do
    changed_fields(edits, step, rows[step], with_on(rows[step], 1 - rows[step][wire.F_ON]))
  end
  return edits
end

-- Sets every field of every step to its default.
function tools.clear(length, rows)
  local edits = {}
  for step = 0, length - 1 do
    changed_fields(edits, step, rows[step], wire.STEP_DEFAULTS)
  end
  return edits
end

-- Moves every step, with all its fields, by amount places, wrapping at the lane length.
-- Positive moves later in the pattern.
function tools.rotate(amount, length, rows)
  local edits = {}
  for step = 0, length - 1 do
    changed_fields(edits, step, rows[step], rows[(step - amount) % length])
  end
  return edits
end

return tools
