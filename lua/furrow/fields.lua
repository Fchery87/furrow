-- The step fields the step editor can draw as bars, with the range each bar maps onto.
-- SeqCore accepts wider ranges. These are the useful ones for dragging.
local wire = require("furrow.wire")

local fields = {}

local function whole(v)
  return string.format("%d", v)
end

local function signed(v)
  return string.format("%+d", v)
end

local function percent(v)
  return string.format("%d%%", math.floor(v / 100 + 0.5))
end

local function times(v)
  return string.format("%dx", v)
end

fields.list = {
  { name = "Velocity", field = wire.F_VEL, min = 1, max = 127, text = whole },
  { name = "Gate", field = wire.F_GATE, min = 500, max = 10000, text = percent },
  { name = "Pitch", field = wire.F_PITCH, min = -24, max = 24, centered = true, text = signed },
  { name = "Shift", field = wire.F_SHIFT, min = 0, max = 9900, text = percent },
  { name = "Ratchet", field = wire.F_RATCHET, min = 1, max = 8, text = times },
  { name = "Probability", field = wire.F_PROB, min = 0, max = 100, text = function(v) return string.format("%d%%", v) end },
  { name = "Release", field = wire.F_REL, min = 0, max = 127, text = whole },
  { name = "Fine", field = wire.F_FINE, min = -100, max = 100, centered = true,
    text = function(v) return string.format("%+d ct", v) end },
  { name = "Pan", field = wire.F_PAN, min = -64, max = 63, centered = true, text = signed },
  { name = "Mod A", field = wire.F_MOD_A, min = 0, max = 127, text = whole },
  { name = "Mod B", field = wire.F_MOD_B, min = 0, max = 127, text = whole },
}

-- Where a value sits in the bar, from 0 at the bottom to 1 at the top.
function fields.fraction(def, value)
  local f = (value - def.min) / (def.max - def.min)
  return math.max(0, math.min(1, f))
end

-- The value for a fraction of the bar's height, rounded to a whole number.
function fields.value_at(def, fraction)
  local f = math.max(0, math.min(1, fraction))
  return math.floor(def.min + f * (def.max - def.min) + 0.5)
end

return fields
