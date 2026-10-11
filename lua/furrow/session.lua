-- One coordinator's view of one SeqCore slot. It writes commands, reads acknowledgements
-- and snapshots, and notices when the engine restarts or its state is replaced.
-- It takes the memory functions as arguments, so tests can run it without REAPER.
local wire = require("furrow.wire")

local Session = {}
Session.__index = Session

local function round(x)
  return math.floor(x + 0.5)
end

-- mem has read(index) and write(index, value) over the named gmem Furrow.
function Session.new(mem, slot)
  local self = setmetatable({}, Session)
  self.mem = mem
  self.slot = slot
  self.base = wire.SLOT_BASE + slot * wire.SLOT_SIZE
  self.pending = {}
  self.pending_count = 0
  self.snapshot = nil
  self.window = { pattern = 0, l0 = 0, nl = 0, s0 = 0, ns = 0 }
  return self
end

function Session:h(field)
  return self.mem.read(self.base + field)
end

function Session:set_h(field, value)
  self.mem.write(self.base + field, value)
end

-- True while the slot holds a live wire-1 engine with the token this session attached to.
function Session:valid()
  return self.mem.read(wire.DIR_OWNER + self.slot) == self.token and self:h(wire.H_TOKEN) == self.token
    and self:h(wire.H_MAGIC) == wire.MAGIC and self:h(wire.H_WIRE) == wire.WIRE_VERSION
end

-- Takes the command producer. Commands from an earlier session stay in the ring and the
-- engine rejects them by session id, so nothing here rewinds the engine's counters.
function Session:attach(id)
  self.token = self:h(wire.H_TOKEN)
  if self.token == 0 or self:h(wire.H_MAGIC) ~= wire.MAGIC or self:h(wire.H_WIRE) ~= wire.WIRE_VERSION then
    return false, "slot " .. self.slot .. " holds no wire " .. wire.WIRE_VERSION .. " engine"
  end
  self.id = id
  self.generation = self:h(wire.H_GENERATION)
  self.seq = 0
  self.txn = 0
  self.prod = self:h(wire.H_CMD_PROD)
  self:set_h(wire.H_SESSION, id)
  self.ack_cons = self:h(wire.H_ACK_PROD)
  self:set_h(wire.H_ACK_CONS, self.ack_cons)
  self:free_banks()
  self.last_snap = -1
  self.snapshot = nil
  self:request(self.window)
  return true
end

-- A published bank from an earlier session or generation would block the engine's next
-- publication, so it is freed unread.
function Session:free_banks()
  for bank = 0, 1 do
    local at = self.base + wire.BANK0 + bank * wire.BANK_STRIDE
    if self.mem.read(at + wire.B_STATE) == wire.BANK_PUBLISHED then
      self.mem.write(at + wire.B_STATE, wire.BANK_FREE)
    end
  end
end

function Session:free_entries()
  return wire.RING_SIZE - (self.prod - self:h(wire.H_CMD_CONS))
end

function Session:write_entry(txn, op, p)
  self.seq = self.seq + 1
  local at = self.base + wire.RING + (self.prod % wire.RING_SIZE) * wire.ENTRY
  local values = { self.id, self.generation, self.seq, txn, op, p[1] or 0, p[2] or 0, p[3] or 0, p[4] or 0, p[5] or 0 }
  for i = 1, wire.ENTRY do
    self.mem.write(at + i - 1, values[i])
  end
  self.prod = self.prod + 1
end

-- Publishes the commands as one transaction. Each command is { op, p1, ..., p5 }.
-- Returns the transaction id, or nil and "full" when the ring lacks room. Nothing is
-- written then, so a full ring never drops part of a transaction.
function Session:send(commands, tag)
  if self:free_entries() < #commands + 1 then
    return nil, "full"
  end
  self.txn = self.txn + 1
  local txn = self.txn
  for _, command in ipairs(commands) do
    self:write_entry(txn, command[1], { command[2], command[3], command[4], command[5], command[6] })
  end
  self:write_entry(txn, wire.OP_COMMIT, { #commands })
  self:set_h(wire.H_CMD_PROD, self.prod)
  self.pending[txn] = { tag = tag, generation = self.generation }
  self.pending_count = self.pending_count + 1
  return txn
end

-- Audition and panic change no state, so they carry transaction 0 and get no result.
function Session:immediate(op, a, b)
  if self:free_entries() < 1 then
    return false
  end
  self:write_entry(0, op, { a, b })
  self:set_h(wire.H_CMD_PROD, self.prod)
  return true
end

-- Returns the results that arrived since the last poll, oldest first. A result is
-- { txn, code, revision, generation, tag }. A restarted engine or a replaced state ends
-- every pending transaction with ACK_STALE, since the engine will not answer for it.
function Session:poll()
  local results = {}
  if not self:valid() then
    self:fail_pending(results)
    return results, "restarted"
  end
  local prod = self:h(wire.H_ACK_PROD)
  while self.ack_cons < prod do
    local at = self.base + wire.ACK + (self.ack_cons % wire.RING_SIZE) * wire.ACK_ENTRY
    local txn = round(self.mem.read(at))
    local record = self.pending[txn]
    if record then
      local result = {
        txn = txn,
        revision = round(self.mem.read(at + 1)),
        code = round(self.mem.read(at + 2)),
        generation = round(self.mem.read(at + 3)),
        tag = record.tag,
      }
      if result.generation ~= self.generation then
        result.code = wire.ACK_STALE
      end
      results[#results + 1] = result
      self.pending[txn] = nil
      self.pending_count = self.pending_count - 1
    end
    self.ack_cons = self.ack_cons + 1
  end
  self:set_h(wire.H_ACK_CONS, self.ack_cons)
  local generation = self:h(wire.H_GENERATION)
  if generation ~= self.generation then
    self.generation = generation
    self:fail_pending(results)
    self:free_banks()
    self:request(self.window)
    return results, "replaced"
  end
  return results
end

function Session:fail_pending(results)
  local txns = {}
  for txn in pairs(self.pending) do
    txns[#txns + 1] = txn
  end
  table.sort(txns)
  for _, txn in ipairs(txns) do
    results[#results + 1] = { txn = txn, code = wire.ACK_STALE, tag = self.pending[txn].tag }
  end
  self.pending = {}
  self.pending_count = 0
end

-- Asks for a snapshot of window = { pattern, l0, nl, s0, ns } in route slots and steps.
function Session:request(window)
  self.window = window
  self:set_h(wire.H_SNAP_PATTERN, window.pattern)
  self:set_h(wire.H_SNAP_L0, window.l0)
  self:set_h(wire.H_SNAP_NL, window.nl)
  self:set_h(wire.H_SNAP_S0, window.s0)
  self:set_h(wire.H_SNAP_NS, window.ns)
  self:set_h(wire.H_SNAP_REQ, self:h(wire.H_SNAP_REQ) + 1)
end

local function decode_bank(mem, at)
  local function r(i)
    return round(mem.read(at + i))
  end
  local snap = {
    seq = r(wire.B_SEQ),
    revision = r(wire.B_REVISION),
    generation = r(wire.B_GENERATION),
    pattern = r(wire.B_PATTERN),
    l0 = r(wire.B_L0),
    nl = r(wire.B_NL),
    s0 = r(wire.B_S0),
    ns = r(wire.B_NS),
    rack = {
      id = r(wire.B_RACK + wire.R_ID),
      seed = r(wire.B_RACK + wire.R_SEED),
      pattern = r(wire.B_RACK + wire.R_PATTERN),
      mode = r(wire.B_RACK + wire.R_MODE),
      swing = r(wire.B_RACK + wire.R_SWING),
      truncate = r(wire.B_RACK + wire.R_TRUNCATE),
      lane_count = r(wire.B_RACK + 6),
      pool_free = r(wire.B_RACK + 7),
      value_count = r(wire.B_RACK + 8),
      load_error = r(wire.B_RACK + 9),
    },
    lanes = {},
    steps = {},
  }
  for s = 0, wire.LANES - 1 do
    local lane = wire.B_LANES + s * wire.B_LANE_STRIDE
    if r(lane + wire.L_ACTIVE) == 1 then
      snap.lanes[s] = {
        slot = s,
        id = r(lane + wire.L_ID),
        prob_key = r(lane + wire.L_PROB_KEY),
        row = r(lane + wire.L_ROW),
        note = r(lane + wire.L_NOTE),
        mute = r(lane + wire.L_MUTE) == 1,
        solo = r(lane + wire.L_SOLO) == 1,
        volume = r(lane + wire.L_VOLUME),
        pan = r(lane + wire.L_PAN),
        swing = r(lane + wire.L_SWING),
        length = r(lane + wire.LANE_LENGTH),
      }
    end
  end
  for cell = 0, snap.nl * snap.ns - 1 do
    local s = snap.l0 + cell // snap.ns
    local step = snap.s0 + cell % snap.ns
    local values = {}
    local base = wire.B_STEPS + cell * wire.STEP_FIELDS
    for f = 0, wire.STEP_FIELDS - 1 do
      values[f] = r(base + f)
    end
    snap.steps[s] = snap.steps[s] or {}
    snap.steps[s][step] = values
  end
  return snap
end

-- Reads the newest published bank and returns the snapshot, or nil when nothing new
-- arrived. It frees only banks it saw published before decoding, so a bank SeqCore
-- publishes during the decode waits for the next call. A bank from an older generation
-- is freed unread.
function Session:read_snapshot()
  local best, best_seq = nil, self.last_snap
  local seen = {}
  for bank = 0, 1 do
    local at = self.base + wire.BANK0 + bank * wire.BANK_STRIDE
    if self.mem.read(at + wire.B_STATE) == wire.BANK_PUBLISHED then
      seen[#seen + 1] = at
      local seq = round(self.mem.read(at + wire.B_SEQ))
      if seq > best_seq and round(self.mem.read(at + wire.B_GENERATION)) == self.generation then
        best, best_seq = at, seq
      end
    end
  end
  local snap = best and decode_bank(self.mem, best)
  for _, at in ipairs(seen) do
    self.mem.write(at + wire.B_STATE, wire.BANK_FREE)
  end
  if snap then
    self.last_snap = snap.seq
    self.snapshot = snap
  end
  return snap
end

function Session:playhead(s)
  return round(self.mem.read(self.base + wire.PLAYHEAD + s))
end

return Session
