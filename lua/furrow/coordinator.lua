-- Owns one rack's command producer. A gesture sends its engine commands, waits for every
-- acknowledgement, then applies host changes, sets SeqCore's State revision parameter and
-- creates one undo point, all in one deferred callback. That is the order the milestone 1
-- undo probe proved on REAPER 7.82.
--
-- A gesture can outlive its coordinator, because the window closes or the script stops.
-- Each open gesture is journaled in the project's extension data, which undo does not
-- touch. A new coordinator reads the journal when its first snapshot arrives, asks the
-- snapshot whether the engine applied a lane change, and finishes or removes it.
local wire = require("furrow.wire")
local Session = require("furrow.session")
local host = require("furrow.host")
local tools = require("furrow.tools")

local Coordinator = {}
Coordinator.__index = Coordinator

local UNDO_ALL = -1
local UNDO_FX = 2
local STALL_SECONDS = 0.5
local OTHER_COORDINATOR_SECONDS = 1.0
local JOURNAL_SECTION = "Furrow"
local SWITCH_MARGIN_SECONDS = 0.15
local TOOL_CHUNK = 200
local CLONE_WINDOW_STEPS = 32
local TOOL_TIMEOUT_SECONDS = 2
local TOOL_SETTLE_SECONDS = 5

local mem = {
  read = function(i) return reaper.gmem_read(i) end,
  write = function(i, v) reaper.gmem_write(i, v) end,
}

function Coordinator.new(proj, rack)
  local self = setmetatable({}, Coordinator)
  self.proj = proj
  self.engine_guid = reaper.GetTrackGUID(rack.engine)
  self.rack_guid = rack.rack and reaper.GetTrackGUID(rack.rack)
  self.gestures = {}
  self.overlay = {}
  self.reserved = {}
  self.acked_revision = 0
  self.status = "connecting"
  self.message = nil
  self.recovered = false
  self.page = 0
  self.pattern = nil
  self.engine_pattern = nil
  self.tool = nil
  self.completed = 0
  self.journaled = nil
  return self
end

function Coordinator:journal_key()
  return "journal " .. self.engine_guid
end

-- Writes the lines of every open gesture, or nothing when none is open.
function Coordinator:journal_sync()
  local seen, lines = {}, {}
  for _, gesture in ipairs(self.gestures) do
    for _, line in ipairs(gesture.journal or {}) do
      if not seen[line] then
        seen[line] = true
        lines[#lines + 1] = line
      end
    end
  end
  local text = table.concat(lines, "\n")
  if text ~= (self.journaled or "") then
    reaper.SetProjExtState(self.proj, JOURNAL_SECTION, self:journal_key(), text)
    self.journaled = text
  end
end

function Coordinator:journal_read()
  local lines = {}
  local _, text = reaper.GetProjExtState(self.proj, JOURNAL_SECTION, self:journal_key())
  for line in (text or ""):gmatch("[^\n]+") do
    lines[#lines + 1] = line
  end
  return lines
end

function Coordinator:engine()
  local track = host.track_by_guid(self.proj, self.engine_guid)
  return track, track and host.engine_fx(track)
end

function Coordinator:rack_track()
  return self.rack_guid and host.track_by_guid(self.proj, self.rack_guid)
end

-- Frees slots that no SeqCore in any open project reports, then attaches.
function Coordinator:connect()
  reaper.gmem_attach(wire.GMEM)
  local track, fx = self:engine()
  if not fx then
    return false, "the engine track or SeqCore is gone"
  end
  local slot = math.floor(reaper.TrackFX_GetParam(track, fx, wire.PARAM_SLOT) + 0.5)
  if slot < 0 then
    return false, "SeqCore has not claimed a slot yet"
  end
  local live = host.live_slots()
  for s = 0, wire.SLOTS - 1 do
    if not live[s] and mem.read(wire.DIR_OWNER + s) ~= 0 then
      mem.write(wire.DIR_OWNER + s, 0)
    end
  end
  local session = Session.new(mem, slot)
  local beat = session:h(wire.H_COORD_BEAT)
  if session:h(wire.H_SESSION) ~= 0 and reaper.time_precise() - beat < OTHER_COORDINATOR_SECONDS
    and beat <= reaper.time_precise() then
    return false, "another Furrow window already edits this rack"
  end
  local ok, err = session:attach(math.random(1, 16777215))
  if not ok then
    return false, err
  end
  self.session = session
  self.slot = slot
  self.heartbeat = session:h(wire.H_HEARTBEAT)
  self.heartbeat_at = reaper.time_precise()
  session:request(self:window())
  self.status = "waiting"
  return true
end

-- The grid starts on the engine's pattern and follows it when it changes by itself, as it
-- does after an undo. A pattern chosen in the window shows at once, before the engine's
-- queued switch lands.
function Coordinator:follow_pattern(snap)
  local engine = snap.rack.pattern
  if self.pattern == nil or (self.engine_pattern ~= nil and engine ~= self.engine_pattern) then
    self:set_view(engine)
  end
  self.engine_pattern = engine
end

function Coordinator:set_view(pattern)
  if pattern == self.pattern then
    return
  end
  self.pattern = pattern
  self.overlay = {}
  if self.session then
    self.session:request(self:window())
  end
end

-- The snapshot window follows the page the grid shows, 16 steps of every lane.
function Coordinator:window()
  return { pattern = self.pattern or 0, l0 = 0, nl = wire.LANES, s0 = self.page * wire.PAGE_STEPS, ns = wire.PAGE_STEPS }
end

function Coordinator:set_page(page)
  page = math.max(0, math.min(page, wire.MAX_STEPS // wire.PAGE_STEPS - 1))
  if page == self.page then
    return
  end
  self.page = page
  if self.session then
    self.session:request(self:window())
  end
end

-- True once the snapshot holds the page the grid shows. Until then a step reads as zero
-- whatever it holds, so step edits wait.
function Coordinator:page_ready()
  local snap = self.session and self.session.snapshot
  return snap ~= nil and snap.s0 == self.page * wire.PAGE_STEPS
end

function Coordinator:set_length(slot, length)
  length = math.max(1, math.min(wire.MAX_STEPS, math.floor(length)))
  self:run("set length", { { wire.OP_SET_LENGTH, self.pattern or 0, slot, length } })
end

function Coordinator:processing()
  return self.status == "ready"
end

-- Starts a gesture. opts.after(c) applies host changes after the engine acknowledged and
-- returns true or false and a message. opts.undo_states defaults to FX state only.
-- opts.rollback lists commands that undo the engine side if the host side fails.
function Coordinator:begin(name, opts)
  opts = opts or {}
  local gesture = {
    name = name,
    after = opts.after,
    rollback = opts.rollback,
    undo_states = opts.undo_states or UNDO_FX,
    queue = {},
    txns = {},
    waiting = 0,
    applied = 0,
    failed = nil,
    open = true,
    journal = opts.journal or { "edit" },
    silent = opts.silent,
    abort_on_failure = opts.abort_on_failure,
    on_abort = opts.on_abort,
  }
  if opts.host_only then
    gesture.applied = 1
  end
  if self.session.snapshot and self.session.snapshot.rack.id == 0 and not self.rack_id_sent then
    gesture.queue[#gesture.queue + 1] = { commands = { { wire.OP_SET_RACK, wire.R_ID, math.random(1, 16777215) } } }
    self.rack_id_sent = true
  end
  self.gestures[#self.gestures + 1] = gesture
  self:journal_sync()
  return gesture
end

-- Adds one transaction to an open gesture. overlay lists { slot, step, field, value }
-- shown until a snapshot includes the change.
function Coordinator:add(gesture, commands, overlay)
  gesture.queue[#gesture.queue + 1] = { commands = commands, overlay = overlay, pattern = self.pattern or 0 }
  self:flush()
end

function Coordinator:finish(gesture)
  gesture.open = false
end

function Coordinator:run(name, commands, opts)
  local gesture = self:begin(name, opts)
  self:add(gesture, commands)
  self:finish(gesture)
  return gesture
end

-- Sends queued transactions in order until the ring is full.
function Coordinator:flush()
  for _, gesture in ipairs(self.gestures) do
    while #gesture.queue > 0 do
      local item = gesture.queue[1]
      local txn = self.session:send(item.commands, gesture)
      if not txn then
        return
      end
      table.remove(gesture.queue, 1)
      gesture.txns[txn] = item
      gesture.waiting = gesture.waiting + 1
      for _, o in ipairs(item.overlay or {}) do
        self.overlay[#self.overlay + 1] =
          { txn = txn, pattern = item.pattern, slot = o[1], step = o[2], field = o[3], value = o[4] }
      end
    end
  end
end

function Coordinator:drop_overlay(txn, before_revision)
  local kept = {}
  for _, o in ipairs(self.overlay) do
    local done = o.txn == txn or (before_revision and o.revision and o.revision <= before_revision)
    if not done then
      kept[#kept + 1] = o
    end
  end
  self.overlay = kept
end

function Coordinator:mark_revision()
  local track, fx = self:engine()
  if not fx then
    return false
  end
  local revision = reaper.TrackFX_GetParam(track, fx, wire.PARAM_REVISION)
  reaper.TrackFX_SetParam(track, fx, wire.PARAM_REVISION, (revision + 1) % 16777216)
  return true
end

-- Closes every finished gesture whose transactions all have results.
function Coordinator:settle()
  -- Host changes use the active project, so they wait until this rack's tab is active.
  if reaper.EnumProjects(-1) ~= self.proj then
    return
  end
  local kept = {}
  for _, gesture in ipairs(self.gestures) do
    local done = not gesture.open and #gesture.queue == 0 and gesture.waiting == 0
    if done then
      self:close(gesture)
    else
      kept[#kept + 1] = gesture
    end
  end
  self.gestures = kept
  self:journal_sync()
end

function Coordinator:close(gesture)
  if gesture.applied == 0 then
    if gesture.failed then
      self.message = gesture.name .. " failed: " .. gesture.failed
      if gesture.abort_on_failure and gesture.on_abort then
        gesture.on_abort(self)
      end
    end
    return
  end
  if gesture.failed and gesture.abort_on_failure then
    -- Part of the gesture failed, and a half-applied result is worse than none.
    if gesture.rollback then
      self.session:send(gesture.rollback, { name = "rollback" })
    end
    if gesture.on_abort then
      gesture.on_abort(self)
    end
    self.message = gesture.name .. " failed: " .. gesture.failed
    return
  end
  local ok, err = true, nil
  if gesture.after then
    local ran, first, second = pcall(gesture.after, self)
    if ran then
      ok, err = first, second
    else
      ok, err = false, first
    end
  end
  if not ok then
    self.message = gesture.name .. " failed: " .. tostring(err)
    if gesture.rollback then
      self.session:send(gesture.rollback, { name = "rollback" })
    end
    if gesture.on_abort then
      gesture.on_abort(self)
    end
    return
  end
  self:mark_revision()
  if gesture.silent then
    return
  end
  reaper.Undo_OnStateChangeEx2(self.proj, "Furrow: " .. gesture.name, gesture.undo_states, -1)
  if gesture.failed then
    self.message = gesture.name .. " partly failed: " .. gesture.failed
  end
end

local CODE_TEXT = {
  [wire.ACK_STALE] = "the rack changed first (undo, reload or restart)",
  [wire.ACK_INVALID] = "the engine rejected the edit",
  [wire.ACK_CAPACITY] = "the rack is out of pattern storage",
  [wire.ACK_ABORTED] = "the edit was interrupted",
  [wire.ACK_LOAD_ERROR] = "the rack's saved state could not be read",
}

function Coordinator:handle(results)
  for _, r in ipairs(results) do
    local gesture = r.tag
    if type(gesture) == "table" and gesture.txns then
      self.completed = self.completed + 1
      gesture.waiting = gesture.waiting - 1
      if r.code == wire.ACK_OK then
        gesture.applied = gesture.applied + 1
        self.acked_revision = math.max(self.acked_revision, r.revision or 0)
        for _, o in ipairs(self.overlay) do
          if o.txn == r.txn then
            o.revision = r.revision
          end
        end
      else
        gesture.failed = CODE_TEXT[r.code] or ("code " .. tostring(r.code))
        self:drop_overlay(r.txn)
      end
    end
  end
end

-- One deferred tick: results, snapshot, liveness, then settled gestures.
function Coordinator:tick()
  if not self.session then
    local ok, err = self:connect()
    if not ok then
      self.status = "offline"
      self.message = err
      return
    end
  end
  local session = self.session
  session:set_h(wire.H_COORD_BEAT, reaper.time_precise())
  local results, note = session:poll()
  self:handle(results)
  if note == "restarted" then
    self.session = nil
    self.tool = nil
    self.overlay = {}
    self.reserved = {}
    self.status = "connecting"
    self.message = "the engine restarted; reconnecting"
    return
  end
  if note == "replaced" then
    self.overlay = {}
    self.reserved = {}
  end
  local snap = session:read_snapshot()
  if snap then
    self:follow_pattern(snap)
    self:drop_overlay(nil, snap.revision)
    for slot in pairs(self.reserved) do
      if snap.lanes[slot] then
        self.reserved[slot] = nil
      end
    end
  end
  local beat = session:h(wire.H_HEARTBEAT)
  local now = reaper.time_precise()
  if beat ~= self.heartbeat then
    self.heartbeat = beat
    self.heartbeat_at = now
  end
  if now - self.heartbeat_at > STALL_SECONDS then
    self.status = "stalled"
  elseif session.snapshot then
    self.status = session.snapshot.rack.load_error ~= 0 and "load_error" or "ready"
  end
  if self.status == "ready" and not self.recovered then
    self:recover()
  end
  self:flush()
  self:settle()
  self:step_tool()
end

local TOOL_LABEL = {
  invert = "invert steps",
  clear = "clear steps",
}

local function tool_label(kind, arg)
  if kind == "fill" then
    return "fill every " .. arg
  elseif kind == "rotate" then
    return arg < 0 and "rotate left" or "rotate right"
  end
  return TOOL_LABEL[kind]
end

-- Starts a lane tool: "fill" with arg = every N steps, "rotate" with arg = -1 or 1, "invert"
-- or "clear". The tool waits until pending edits land, reads the lane's whole length through
-- a one-lane side window and sends the result as one gesture.
function Coordinator:lane_tool(slot, kind, arg)
  if self.tool then
    self.message = "another lane tool is still running"
    return
  end
  if not (self.session and self.session.snapshot and self.session.snapshot.lanes[slot]) then
    self.message = "the rack is not connected yet"
    return
  end
  self.tool = { slot = slot, kind = kind, arg = arg, stage = "settling", created = reaper.time_precise() }
end

-- The side window a tool reads next. A lane tool reads its lane. A clone reads every lane,
-- 32 steps at a time, which is the most a 2,048 cell window holds.
function Coordinator:tool_window(tool)
  if tool.kind == "clone" then
    local first = tool.block * CLONE_WINDOW_STEPS
    return { pattern = tool.source, l0 = 0, nl = wire.LANES, s0 = first,
             ns = math.min(CLONE_WINDOW_STEPS, tool.longest - first) }
  end
  return { pattern = self.pattern or 0, l0 = tool.slot, nl = 1, s0 = 0, ns = tool.length }
end

function Coordinator:end_tool()
  self.tool = nil
  if self.session then
    self.session:request(self:window())
  end
end

function Coordinator:step_tool()
  local tool = self.tool
  if not tool then
    return
  end
  local session = self.session
  local lane = tool.slot and session.snapshot and session.snapshot.lanes[tool.slot]
  local now = reaper.time_precise()
  if tool.slot and not lane then
    self:end_tool()
  elseif tool.stage == "settling" then
    local stale = session.snapshot and session.snapshot.revision < self.acked_revision
    if self:busy() or #self.overlay > 0 or stale or self.status ~= "ready" then
      if now - tool.created > TOOL_SETTLE_SECONDS then
        self.message = "the lane tool gave up waiting for pending edits"
        self:end_tool()
      end
      return
    end
    if tool.kind == "clone" then
      tool.lanes, tool.longest = self:clone_lanes()
    end
    tool.length = lane and lane.length
    tool.since = now
    tool.stage = "waiting"
    session:request(self:tool_window(tool), true)
  elseif session.side_snapshot then
    local side = session.side_snapshot
    if tool.kind == "clone" then
      if not self:collect_clone(tool, side) then
        self:end_tool()
      elseif tool.block * CLONE_WINDOW_STEPS < tool.longest then
        tool.since = now
        session:request(self:tool_window(tool), true)
      else
        self:end_tool()
        self:run_clone(tool)
      end
      return
    end
    self:end_tool()
    self:run_tool(tool, side)
  elseif now - tool.since > TOOL_TIMEOUT_SECONDS then
    self.message = "the lane tool timed out"
    self:end_tool()
  end
end

function Coordinator:run_tool(tool, snap)
  local rows = snap.steps[tool.slot] or {}
  for step = 0, tool.length - 1 do
    if not rows[step] then
      self.message = "the lane tool could not read the lane"
      return
    end
  end
  local edits
  if tool.kind == "fill" then
    edits = tools.fill(tool.arg, tool.length, rows)
  elseif tool.kind == "rotate" then
    edits = tools.rotate(tool.arg, tool.length, rows)
  elseif tool.kind == "invert" then
    edits = tools.invert(tool.length, rows)
  else
    edits = tools.clear(tool.length, rows)
  end
  if #edits == 0 then
    return
  end
  local gesture = self:begin(tool_label(tool.kind, tool.arg))
  for first = 1, #edits, TOOL_CHUNK do
    local commands, overlay = {}, {}
    for i = first, math.min(first + TOOL_CHUNK - 1, #edits) do
      local e = edits[i]
      commands[#commands + 1] = { wire.OP_SET_STEP, self.pattern or 0, tool.slot, e[1], e[2], e[3] }
      overlay[#overlay + 1] = { tool.slot, e[1], e[2], e[3] }
    end
    self:add(gesture, commands, overlay)
  end
  self:finish(gesture)
end

-- The patterns the window lists: every named one, plus the one the engine plays and the one
-- the grid shows, each with a default name when it has none. Returns { index, name } in order.
function Coordinator:patterns()
  local names = host.read_patterns(self:rack_track())
  names[0] = names[0] or host.default_pattern_name(0)
  for _, index in ipairs({ self.engine_pattern, self.pattern }) do
    if index and not names[index] then
      names[index] = host.default_pattern_name(index)
    end
  end
  local list = {}
  for index, name in pairs(names) do
    list[#list + 1] = { index = index, name = name }
  end
  table.sort(list, function(a, b) return a.index < b.index end)
  return list
end

-- Shows a pattern now and queues the engine's switch for the next measure boundary while
-- playing, or applies it at the cursor when stopped.
function Coordinator:queue_commands(pattern)
  local ticks = 0
  if self.session:h(wire.H_PLAYING) == 1 then
    ticks = math.floor(host.next_measure_beat(self.proj, SWITCH_MARGIN_SECONDS) * 1024 + 0.5)
  end
  return { { wire.OP_QUEUE_PATTERN, pattern, ticks // 16777216, ticks % 16777216 } }
end

function Coordinator:select_pattern(pattern)
  if not self:processing() or pattern == self.pattern and self.engine_pattern == pattern then
    return
  end
  self:run("switch pattern", self:queue_commands(pattern))
  self:set_view(pattern)
end

function Coordinator:new_pattern()
  if not self:processing() then
    return
  end
  local taken = {}
  for _, entry in ipairs(self:patterns()) do
    taken[entry.index] = true
  end
  local index = 0
  while taken[index] do
    index = index + 1
  end
  if index >= wire.PATTERNS then
    self.message = "the rack already has " .. wire.PATTERNS .. " patterns"
    return
  end
  local gesture = self:begin("new pattern", {
    undo_states = UNDO_ALL,
    after = function(c)
      local rack = c:rack_track()
      if not rack then
        return false, "the rack tracks are gone"
      end
      local list = host.read_patterns(rack)
      for _, entry in ipairs(c:patterns()) do
        list[entry.index] = list[entry.index] or entry.name
      end
      list[index] = host.default_pattern_name(index)
      host.write_patterns(rack, list)
      return true
    end,
  })
  self:add(gesture, self:queue_commands(index))
  self:finish(gesture)
  self:set_view(index)
end

function Coordinator:rename_pattern(pattern, text)
  local name = host.clean_pattern_name(text)
  if name == "" or not self:processing() then
    return
  end
  local gesture = self:begin("rename pattern", {
    host_only = true,
    undo_states = UNDO_ALL,
    after = function(c)
      local rack = c:rack_track()
      if not rack then
        return false, "the rack tracks are gone"
      end
      local list = host.read_patterns(rack)
      for _, entry in ipairs(c:patterns()) do
        list[entry.index] = list[entry.index] or entry.name
      end
      list[pattern] = name
      host.write_patterns(rack, list)
      return true
    end,
  })
  self:finish(gesture)
end

function Coordinator:clear_pattern(pattern)
  if self:processing() then
    self:run("clear pattern", { { wire.OP_CLEAR_PATTERN, pattern } })
  end
end

-- Starts a copy of the shown pattern into a free one, then switches to the copy.
function Coordinator:clone_pattern()
  if not self:processing() then
    return
  end
  if self.tool then
    self.message = "another lane tool is still running"
    return
  end
  local lanes, longest = self:clone_lanes()
  self.tool = {
    kind = "clone", stage = "settling", created = reaper.time_precise(), source = self.pattern or 0,
    lanes = lanes, longest = longest, block = 0, edits = {},
  }
end

-- The lanes and the longest length now in the snapshot. Read again once pending edits settle.
function Coordinator:clone_lanes()
  local lanes, longest = {}, 1
  for _, lane in pairs(self.session.snapshot.lanes) do
    lanes[#lanes + 1] = lane
    longest = math.max(longest, lane.length)
  end
  return lanes, longest
end

-- Adds the non-default values of one side window to the clone. False when a row is missing.
function Coordinator:collect_clone(tool, snap)
  for _, lane in ipairs(tool.lanes) do
    local rows = snap.steps[lane.slot] or {}
    for step = snap.s0, math.min(snap.s0 + snap.ns, lane.length) - 1 do
      local row = rows[step]
      if not row then
        self.message = "the clone could not read the pattern"
        return false
      end
      for field = 0, wire.STEP_FIELDS - 1 do
        if row[field] ~= wire.STEP_DEFAULTS[field] then
          tool.edits[#tool.edits + 1] = { lane.slot, step, field, row[field] }
        end
      end
    end
  end
  tool.block = tool.block + 1
  return true
end

function Coordinator:run_clone(tool)
  local taken = {}
  for _, entry in ipairs(self:patterns()) do
    taken[entry.index] = true
  end
  local target = 0
  while taken[target] do
    target = target + 1
  end
  if target >= wire.PATTERNS then
    self.message = "the rack already has " .. wire.PATTERNS .. " patterns"
    return
  end
  local source_name = host.default_pattern_name(tool.source)
  for _, entry in ipairs(self:patterns()) do
    if entry.index == tool.source then
      source_name = entry.name
    end
  end
  local commands = {}
  for _, lane in ipairs(tool.lanes) do
    if lane.length ~= wire.DEFAULT_LENGTH then
      commands[#commands + 1] = { wire.OP_SET_LENGTH, target, lane.slot, lane.length }
    end
  end
  for _, e in ipairs(tool.edits) do
    commands[#commands + 1] = { wire.OP_SET_STEP, target, e[1], e[2], e[3], e[4] }
  end
  local gesture = self:begin("clone pattern", {
    undo_states = UNDO_ALL,
    abort_on_failure = true,
    rollback = { { wire.OP_CLEAR_PATTERN, target }, { wire.OP_QUEUE_PATTERN, tool.source, 0, 0 } },
    on_abort = function(c) c:set_view(tool.source) end,
    after = function(c)
      local rack = c:rack_track()
      if not rack then
        return false, "the rack tracks are gone"
      end
      local list = host.read_patterns(rack)
      for _, entry in ipairs(c:patterns()) do
        list[entry.index] = list[entry.index] or entry.name
      end
      list[target] = host.clean_pattern_name(source_name .. " copy")
      host.write_patterns(rack, list)
      return true
    end,
  })
  for first = 1, #commands, TOOL_CHUNK do
    local chunk = {}
    for i = first, math.min(first + TOOL_CHUNK - 1, #commands) do
      chunk[#chunk + 1] = commands[i]
    end
    self:add(gesture, chunk)
  end
  self:add(gesture, self:queue_commands(target))
  self:finish(gesture)
  self:set_view(target)
end

-- Finishes what a lost coordinator left half done. A lane the journal names is removed
-- from the engine if the engine applied it, and its track and link go too. That is a
-- rollback for an add and a roll forward for a delete. An "edit" line means engine edits
-- may have no undo point yet, so recovery adds one.
function Coordinator:recover()
  self.recovered = true
  local lines = self:journal_read()
  if #lines == 0 then
    return
  end
  local ids, edit = {}, false
  for _, line in ipairs(lines) do
    local id = line:match("^lane (%d+)$")
    if id then
      ids[#ids + 1] = tonumber(id)
    elseif line == "edit" then
      edit = true
    end
  end
  local commands = {}
  for _, id in ipairs(ids) do
    for _, lane in pairs(self.session.snapshot.lanes) do
      if lane.id == id then
        commands[#commands + 1] = { wire.OP_REMOVE_LANE, lane.slot }
      end
    end
  end
  local function drop_hosts(c)
    local rack = c:rack_track()
    if rack then
      local links = host.read_links(rack)
      for _, id in ipairs(ids) do
        local track = links[id] and host.track_by_guid(c.proj, links[id])
        if track then
          host.delete_child(track)
        end
        links[id] = nil
      end
      host.write_links(rack, links)
    end
    return true
  end
  local gesture = self:begin(edit and "recover edits" or "recover", {
    journal = lines,
    silent = not edit,
    undo_states = UNDO_ALL,
    after = drop_hosts,
  })
  if #commands > 0 then
    self:add(gesture, commands)
  else
    gesture.applied = 1
  end
  self:finish(gesture)
end

function Coordinator:busy()
  return #self.gestures > 0
end

-- The value shown for a step: the newest pending edit, else the snapshot.
function Coordinator:step_value(slot, step, field)
  for i = #self.overlay, 1, -1 do
    local o = self.overlay[i]
    if o.pattern == (self.pattern or 0) and o.slot == slot and o.step == step and o.field == field then
      return o.value
    end
  end
  local snap = self.session and self.session.snapshot
  local row = snap and snap.steps[slot] and snap.steps[slot][step]
  return row and row[field] or 0
end

function Coordinator:free_slot()
  local snap = self.session.snapshot
  for s = 0, wire.LANES - 1 do
    if not snap.lanes[s] and not self.reserved[s] then
      return s
    end
  end
  return nil
end

function Coordinator:lanes()
  local list = {}
  local snap = self.session and self.session.snapshot
  if not snap then
    return list
  end
  for _, lane in pairs(snap.lanes) do
    list[#list + 1] = lane
  end
  table.sort(list, function(a, b) return a.row < b.row or (a.row == b.row and a.slot < b.slot) end)
  return list
end

-- Adds a sample lane: engine lane first, then the track, RS5K, send and link after the
-- acknowledgement, in one undo point.
function Coordinator:add_sample(file)
  local snap = self.session and self.session.snapshot
  if not snap then
    self.message = "the rack is not connected yet"
    return
  end
  local slot = self:free_slot()
  if not slot then
    self.message = "the rack already has " .. wire.LANES .. " lanes"
    return
  end
  self.reserved[slot] = true
  local id = math.random(1, 16777215)
  local row = 0
  for _, lane in pairs(snap.lanes) do
    row = math.max(row, lane.row + 1)
  end
  for _, g in ipairs(self.gestures) do
    row = row + (g.adds or 0)
  end
  local gesture = self:begin("add lane", {
    journal = { "lane " .. id },
    undo_states = UNDO_ALL,
    rollback = { { wire.OP_REMOVE_LANE, slot } },
    after = function(c)
      local rack = c:rack_track()
      local engine = c:engine()
      if not rack or not engine then
        return false, "the rack tracks are gone"
      end
      local track, err = host.create_sample_lane(rack, engine, file, slot)
      if not track then
        return false, err
      end
      local links = host.read_links(rack)
      links[id] = reaper.GetTrackGUID(track)
      host.write_links(rack, links)
      return true
    end,
  })
  gesture.adds = 1
  self:add(gesture, { { wire.OP_ADD_LANE, slot, id, math.random(0, 16777215), 60, math.min(row, 63) } })
  self:finish(gesture)
end

-- Sets one step field inside an open gesture. Returns false when the step already shows
-- that value, so a drag sends one edit per change.
function Coordinator:set_field(gesture, slot, step, field, value)
  if self:step_value(slot, step, field) == value then
    return false
  end
  self:add(gesture, { { wire.OP_SET_STEP, self.pattern or 0, slot, step, field, value } }, { { slot, step, field, value } })
  return true
end

-- Removes the engine lane, then its track if Furrow linked one, in one undo point.
function Coordinator:remove_lane(lane)
  self:run("delete lane", { { wire.OP_REMOVE_LANE, lane.slot } }, {
    journal = { "lane " .. lane.id },
    undo_states = UNDO_ALL,
    after = function(c)
      local rack = c:rack_track()
      if rack then
        local links = host.read_links(rack)
        local track = links[lane.id] and host.track_by_guid(c.proj, links[lane.id])
        if track then
          host.delete_child(track)
        end
        links[lane.id] = nil
        host.write_links(rack, links)
      end
      return true
    end,
  })
end

function Coordinator:lane_track(lane)
  local rack = self:rack_track()
  local guid = rack and host.read_links(rack)[lane.id]
  return guid and host.track_by_guid(self.proj, guid)
end

function Coordinator:audition(slot)
  self.session:immediate(wire.OP_AUDITION, slot, 100)
end

return Coordinator
