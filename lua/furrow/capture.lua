-- Pure item-capture decisions. REAPER reads item data; this module decides whether the
-- source snapshot is safe to restore and validates conversion previews.
local commit = require("furrow.commit")

local capture = {}
local wire = require("furrow.wire")

local STEP_BEATS = 0.25
local EPS = 1e-9

local function convert_only(reason)
  return { kind = "convert", can_restore = false, can_convert = true, reason = reason }
end

function capture.parse_reference(text)
  local version, group, lane, rack, fingerprint = (text or ""):match(
    "^(%d+) group (%d+) lane (%d+) rack (%d+) fp ([%da-fA-F]+)$")
  version, group, lane, rack = tonumber(version), tonumber(group), tonumber(lane), tonumber(rack)
  if version ~= 1 or not group or group < 1 or group > 16777215
      or not lane or lane < 0 or not rack or rack < 1 or rack > 16777215
      or not fingerprint or #fingerprint ~= 16 then
    return nil, "missing or invalid Furrow item metadata"
  end
  return { version = version, group = group, lane = lane, rack = rack, fingerprint = fingerprint:lower() }
end

-- current_fingerprint covers the current MIDI events and relevant item/take properties.
-- snapshot_text is the group-level project extstate value.
function capture.inspect(metadata, current_fingerprint, snapshot_text)
  local reference, message = capture.parse_reference(metadata)
  if not reference then
    return convert_only(message)
  end
  local snapshot, decode_error = commit.decode(snapshot_text or "")
  if not snapshot then
    return convert_only("the source snapshot is unavailable: " .. tostring(decode_error))
  end
  local found = false
  for _, lane in ipairs(snapshot.lanes) do
    if lane.id == reference.lane then
      found = true
      if not snapshot.rack_id or snapshot.rack_id ~= reference.rack then
        return convert_only("the source snapshot does not identify this rack")
      end
      if not lane.target_guid then
        return convert_only("the source snapshot does not identify the lane destination")
      end
      break
    end
  end
  if not found then
    return convert_only("the item lane is not present in its source snapshot")
  end
  if type(current_fingerprint) ~= "string" or #current_fingerprint ~= 16 then
    return convert_only("the current item fingerprint is invalid")
  end
  if reference.fingerprint == current_fingerprint:lower() then
    return { kind = "restore", can_restore = true, can_convert = false, reference = reference, snapshot = snapshot }
  end
  return { kind = "choice", can_restore = true, can_convert = true, reference = reference, snapshot = snapshot,
    reason = "the committed MIDI or item properties changed" }
end

local function read_blob(blob)
  local offset, normalized = 1, {}
  while offset <= #blob do
    local ok, delta, flags, message, next_offset = pcall(string.unpack, "i4Bs4", blob, offset)
    if not ok or next_offset <= offset then
      return nil, nil, "the MIDI event stream is malformed"
    end
    normalized[#normalized + 1] = string.pack("i4Bs4", delta, flags & ~1, message)
    offset = next_offset
  end
  return normalized, table.concat(normalized)
end

-- Clears REAPER's event-selected flag while preserving MIDI data and all other event flags.
-- Selection in the MIDI editor does not change the content of a committed item.
function capture.normalize_midi(blob)
  local _, normalized, err = read_blob(blob or "")
  return normalized, err
end

local function event_name(status)
  local kind = status & 0xF0
  if kind == 0xA0 then return "polyphonic pressure" end
  if kind == 0xB0 then return "control change" end
  if kind == 0xC0 then return "program change" end
  if kind == 0xD0 then return "channel pressure" end
  if kind == 0xE0 then return "pitch bend" end
  return "unsupported MIDI event"
end

-- Returns note records, unsupported event names, and normalized raw MIDI bytes.
-- ppq_to_qn converts absolute take PPQ to effective project quarter notes.
function capture.scan_midi(blob, ppq_to_qn, item_end_qn)
  local packed, normalized, err = read_blob(blob or "")
  if not packed then
    return {}, { err }, ""
  end
  local notes, unsupported, pending = {}, {}, {}
  local ppq = 0
  local function reject(name)
    unsupported[#unsupported + 1] = name
  end
  for event_index, event in ipairs(packed) do
    local delta, flags, message = string.unpack("i4Bs4", event)
    ppq = ppq + delta
    if (flags & 2) ~= 0 then
      reject("muted MIDI event")
    end
    if #message > 0 then
      local status = message:byte(1)
      local kind, channel = status & 0xF0, (status & 0x0F) + 1
      if kind == 0x90 and #message >= 3 and message:byte(3) > 0 then
        local pitch, velocity = message:byte(2), message:byte(3)
        local key = channel .. ":" .. pitch
        pending[key] = pending[key] or {}
        pending[key][#pending[key] + 1] = {
          channel = channel, pitch = pitch, velocity = velocity,
          start_qn = ppq_to_qn(ppq), start_ppq = ppq,
        }
      elseif (kind == 0x80 or kind == 0x90) and #message >= 3 then
        local pitch, release = message:byte(2), message:byte(3)
        local key = channel .. ":" .. pitch
        local queue = pending[key]
        local note = queue and table.remove(queue, 1)
        if not note then
          reject("unmatched note-off")
        else
          note.end_qn = ppq_to_qn(ppq)
          note.end_ppq = ppq
          note.release = release
          if item_end_qn and note.end_qn > item_end_qn + EPS then
            reject("a note ends after the item")
          end
          notes[#notes + 1] = note
        end
      elseif kind == 0xB0 and #message >= 3 and message:byte(2) == 123 and message:byte(3) == 0
          and event_index == #packed then
        -- REAPER appends this neutral cleanup event when reading MIDI takes. It carries no
        -- additional notes or controller state, so it should not block note-only conversion.
      elseif status < 0xF0 then
        reject(event_name(status))
      else
        reject("system or sysex event")
      end
    end
  end
  for _, queue in pairs(pending) do
    for _ in ipairs(queue) do
      reject("unmatched note-on")
    end
  end
  table.sort(notes, function(a, b)
    return a.start_qn < b.start_qn or (a.start_qn == b.start_qn and a.start_ppq < b.start_ppq)
  end)
  return notes, unsupported, normalized
end

-- notes use effective project quarter-note positions. mapping maps MIDI channels to
-- { slot = lane slot, note = lane base pitch }. Unsupported events are listed by type.
function capture.preview(notes, item_start_qn, mapping, unsupported)
  local reasons = {}
  local unsupported_names = {}
  for _, name in ipairs(unsupported or {}) do
    unsupported_names[#unsupported_names + 1] = tostring(name)
  end
  if #unsupported_names > 0 then
    table.sort(unsupported_names)
    reasons[#reasons + 1] = "unsupported MIDI events: " .. table.concat(unsupported_names, ", ")
  end

  local lanes, occupied, lane_notes = {}, {}, {}
  local ordered = {}
  for _, note in ipairs(notes or {}) do
    ordered[#ordered + 1] = note
  end
  table.sort(ordered, function(a, b) return a.start_qn < b.start_qn end)

  for _, note in ipairs(ordered) do
    local target = mapping and mapping[note.channel]
    if not target then
      reasons[#reasons + 1] = string.format("MIDI channel %d has no lane mapping", note.channel)
    elseif note.start_qn < item_start_qn - EPS then
      reasons[#reasons + 1] = "a note starts before the item"
    elseif note.end_qn <= note.start_qn then
      reasons[#reasons + 1] = "a note has no positive duration"
    else
      local relative = (note.start_qn - item_start_qn) / STEP_BEATS
      local step = math.floor(relative + EPS)
      if step >= wire.MAX_STEPS then
        reasons[#reasons + 1] = "a note extends beyond 512 steps"
      else
        local shift = math.floor((relative - step) * 10000 + 0.5)
        local gate = math.floor(((note.end_qn - note.start_qn) / STEP_BEATS) * 10000 + 0.5)
        local pitch = note.pitch - target.note
        if shift > 9900 then
          reasons[#reasons + 1] = "a note shift exceeds 0.99"
        end
        if gate < 500 or gate > 10000 then
          reasons[#reasons + 1] = "a note gate is outside the supported range"
        end
        if note.pitch < 0 or note.pitch > 127 or pitch < -24 or pitch > 24 then
          reasons[#reasons + 1] = "a note pitch is outside the lane range"
        end
        if note.velocity < 1 or note.velocity > 127 or note.release < 0 or note.release > 127 then
          reasons[#reasons + 1] = "a note velocity is outside the MIDI range"
        end

        local slot = target.slot
        local key = tostring(slot) .. ":" .. tostring(step)
        if occupied[key] then
          reasons[#reasons + 1] = "multiple notes map to one lane step"
        end
        occupied[key] = true
        lane_notes[slot] = lane_notes[slot] or {}
        for _, earlier in ipairs(lane_notes[slot]) do
          if note.start_qn < earlier.end_qn - EPS and earlier.start_qn < note.end_qn - EPS then
            reasons[#reasons + 1] = "notes overlap on one lane"
            break
          end
        end
        lane_notes[slot][#lane_notes[slot] + 1] = note

        lanes[slot] = lanes[slot] or { slot = slot, note = target.note, length = 1, rows = {} }
        local lane = lanes[slot]
        lane.length = math.max(lane.length, step + 1)
        local row = lane.rows[step]
        if not row then
          row = {}
          for field = 0, wire.STEP_FIELDS - 1 do
            row[field] = wire.STEP_DEFAULTS[field]
          end
          lane.rows[step] = row
        end
        row[wire.F_ON], row[wire.F_VEL], row[wire.F_REL] = 1, note.velocity, note.release
        row[wire.F_PITCH], row[wire.F_SHIFT], row[wire.F_GATE] = pitch, shift, gate
      end
    end
  end

  if #reasons > 0 then
    return { accepted = false, reasons = reasons, lanes = lanes }
  end
  return { accepted = true, reasons = {}, lanes = lanes }
end

return capture
