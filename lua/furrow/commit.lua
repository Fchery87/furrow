-- Renders a pattern snapshot into the MIDI events SeqCore would play for it, so a commit can
-- write them into items. It follows the engine's rules for one lane at a time: the logical
-- step grid starts at the origin, swing delays odd steps, shift delays after swing, a gate
-- split into ratchets repeats, a later playing step cancels waiting ratchets, and a new hit
-- releases the held note first. Beats are relative to the origin. The probability roll uses
-- the absolute project beat, so the origin has to be the item's real project position.
local wire = require("furrow.wire")
local prob = require("furrow.prob")

local commit = {}

local STEP_BEATS = 0.25
local EPS = 1e-9
local BEND_CENTER = 8192

commit.NOTE_OFF = 0x80
commit.NOTE_ON = 0x90
commit.CC = 0xB0
commit.BEND = 0xE0

local function volume_cc(v)
  return math.floor(v * 127 / 10000 + 0.5)
end

local function pan_cc(v)
  if v < 0 then
    return math.floor(64 + v * 64 / 10000 + 0.5)
  end
  return math.floor(64 + v * 63 / 10000 + 0.5)
end

local function render_lane(lane, o)
  local events = {}
  local held, rel_time, pending
  local last_bend, last_a, last_b = 0, 64, 64

  local function emit(t, status, d1, d2)
    events[#events + 1] = { beat = t, status = status, data1 = d1, data2 = d2 }
  end

  local function release(t)
    if held then
      emit(t, commit.NOTE_OFF, held.note, held.release)
      held = nil
    end
    rel_time = nil
  end

  local function flush_releases(limit)
    if rel_time and rel_time <= limit + EPS then
      release(rel_time)
    end
  end

  local function send_hit(t, p)
    release(t)
    emit(t, commit.CC, 7, volume_cc(lane.volume))
    local pan = pan_cc(lane.pan) + p.row[wire.F_PAN]
    emit(t, commit.CC, 10, math.max(0, math.min(127, pan)))
    local cents = p.row[wire.F_FINE]
    if cents ~= last_bend and lane.bend > 0 then
      local bend = math.max(0, math.min(16383, math.floor(BEND_CENTER + cents * 8192 / (lane.bend * 100) + 0.5)))
      emit(t, commit.BEND, bend % 128, bend // 128)
      last_bend = cents
    end
    local a = p.row[wire.F_MOD_A]
    if a ~= last_a then
      emit(t, commit.CC, lane.mod_a, a)
      last_a = a
    end
    local b = p.row[wire.F_MOD_B]
    if b ~= last_b then
      emit(t, commit.CC, lane.mod_b, b)
      last_b = b
    end
    emit(t, commit.NOTE_ON, p.note, p.row[wire.F_VEL])
    held = { note = p.note, release = p.row[wire.F_REL] }
    rel_time = t + p.d
  end

  local function flush_pending(limit)
    while pending and pending.t < limit - EPS do
      flush_releases(pending.t)
      local at = pending.t
      send_hit(at, pending)
      if pending.n > 1 then
        pending.t = at + pending.d
        pending.n = pending.n - 1
      else
        pending = nil
      end
    end
  end

  local length = lane.length
  for k = 0, o.steps - 1 do
    local hit = k * STEP_BEATS
    flush_pending(hit)
    flush_releases(hit)
    local row = lane.rows[k % length]
    if row and row[wire.F_ON] == 1 then
      local plays = prob.plays(row[wire.F_PROB], o.seed, o.pattern_key, lane.prob_key,
        prob.step_number(o.origin + hit, STEP_BEATS))
      local note = lane.note + row[wire.F_PITCH]
      if plays and note >= 0 and note <= 127 then
        local t = hit
        if k % 2 == 1 then
          t = t + o.swing / 10000 * lane.swing / 10000 * STEP_BEATS / 3
        end
        t = t + row[wire.F_SHIFT] / 10000 * STEP_BEATS
        local stop = t + row[wire.F_GATE] / 10000 * STEP_BEATS
        if o.truncate == 1 then
          stop = math.min(stop, hit + STEP_BEATS)
        end
        if stop > t then
          local n = row[wire.F_RATCHET]
          pending = { t = t, n = n, d = (stop - t) / n, note = note, row = row }
        end
      end
    end
  end

  -- Hits that start at or after the end are left out. A note still held is released at the end.
  local finish = o.steps * STEP_BEATS
  flush_pending(finish)
  pending = nil
  if held then
    release(math.min(rel_time, finish))
  end
  if last_bend ~= 0 then
    emit(finish, commit.BEND, BEND_CENTER % 128, BEND_CENTER // 128)
  end
  if last_a ~= 64 then
    emit(finish, commit.CC, lane.mod_a, 64)
  end
  if last_b ~= 64 then
    emit(finish, commit.CC, lane.mod_b, 64)
  end
  return events
end

-- o = { origin, steps, seed, swing, truncate, pattern_key, lanes }. Each lane carries slot,
-- note, volume, pan, bend, mod_a, mod_b, swing, prob_key, length and rows[0 .. length - 1],
-- where a row is the 16 step fields. Returns { [slot] = events } with events in time order.
function commit.render(o)
  local out = {}
  for _, lane in ipairs(o.lanes) do
    out[lane.slot] = render_lane(lane, o)
  end
  return out
end

-- The source snapshot as printable text, one line a record. Only fields that differ from the
-- step defaults are written, so a decoded step starts from the defaults. Lane ids are the
-- stable ids, and each lane carries every step of its length.
local LANE_KEYS = { "slot", "note", "volume", "pan", "bend", "mod_a", "mod_b", "swing", "prob_key", "length" }

function commit.encode(o)
  local lines = {
    "furrow-commit 1",
    string.format("rack seed %d swing %d truncate %d pattern %d steps %d origin %.17g",
      o.seed, o.swing, o.truncate, o.pattern_key, o.steps, o.origin),
  }
  for _, lane in ipairs(o.lanes) do
    local parts = { "lane", string.format("%d", lane.id) }
    for _, key in ipairs(LANE_KEYS) do
      parts[#parts + 1] = key .. "=" .. string.format("%d", lane[key])
    end
    lines[#lines + 1] = table.concat(parts, " ")
    for pos = 0, lane.length - 1 do
      local row = lane.rows[pos]
      local fields = {}
      for field = 0, wire.STEP_FIELDS - 1 do
        if row[field] ~= wire.STEP_DEFAULTS[field] then
          fields[#fields + 1] = field .. "=" .. string.format("%d", row[field])
        end
      end
      if #fields > 0 then
        lines[#lines + 1] = string.format("step %d %d %s", lane.id, pos, table.concat(fields, " "))
      end
    end
  end
  return table.concat(lines, "\n") .. "\n"
end

-- Reads encode's text back. Returns nil and a message when the text is not format 1.
function commit.decode(text)
  local first = text:match("^[^\n]*")
  if first ~= "furrow-commit 1" then
    return nil, "not a Furrow commit snapshot"
  end
  local o = { lanes = {} }
  local by_id = {}
  for line in text:gmatch("[^\n]+") do
    local kind = line:match("^(%S+)")
    if kind == "rack" then
      o.seed, o.swing, o.truncate, o.pattern_key, o.steps, o.origin = line:match(
        "^rack seed (%-?%d+) swing (%-?%d+) truncate (%-?%d+) pattern (%-?%d+) steps (%-?%d+) origin (%S+)$")
      if not o.origin then
        return nil, "bad rack line"
      end
      o.seed, o.swing, o.truncate = tonumber(o.seed), tonumber(o.swing), tonumber(o.truncate)
      o.pattern_key, o.steps, o.origin = tonumber(o.pattern_key), tonumber(o.steps), tonumber(o.origin)
    elseif kind == "lane" then
      local lane = { id = tonumber(line:match("^lane (%-?%d+)")), rows = {} }
      for key, value in line:gmatch("(%a[%a_]*)=(%-?%d+)") do
        lane[key] = tonumber(value)
      end
      for _, key in ipairs(LANE_KEYS) do
        if not lane[key] then
          return nil, "lane line misses " .. key
        end
      end
      for pos = 0, lane.length - 1 do
        local row = {}
        for field = 0, wire.STEP_FIELDS - 1 do
          row[field] = wire.STEP_DEFAULTS[field]
        end
        lane.rows[pos] = row
      end
      o.lanes[#o.lanes + 1] = lane
      by_id[lane.id] = lane
    elseif kind == "step" then
      local id, pos = line:match("^step (%-?%d+) (%d+)")
      local lane = by_id[tonumber(id)]
      if not lane or not lane.rows[tonumber(pos)] then
        return nil, "step line for an unknown lane or position"
      end
      for field, value in line:gmatch("(%d+)=(%-?%d+)") do
        lane.rows[tonumber(pos)][tonumber(field)] = tonumber(value)
      end
    end
  end
  if not o.steps then
    return nil, "no rack line"
  end
  return o
end

-- A 64 bit fingerprint of text as 16 hex digits, from two FNV-1a runs with different bases.
function commit.fingerprint(text)
  local a, b = 0x811C9DC5, 0x1B873593
  for i = 1, #text do
    local byte = text:byte(i)
    a = ((a ~ byte) * 16777619) & 0xFFFFFFFF
    b = ((b ~ byte) * 0x01000193 + 0x9E3779B1) & 0xFFFFFFFF
  end
  return string.format("%08x%08x", a, b)
end

-- The text an item's fingerprint covers: its place on the timeline, its take offset, and
-- its events one a line.
function commit.item_text(position, length, offset, events)
  local lines = { string.format("%.9f %.9f %.9f", position, length, offset) }
  for _, e in ipairs(events) do
    lines[#lines + 1] = string.format("%d %d %d %.9f", e.status, e.data1, e.data2, e.beat)
  end
  return table.concat(lines, "\n")
end

return commit
