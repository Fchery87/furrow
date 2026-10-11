-- The probability roll. It is the same function as prob_roll in jsfx/furrow_seqcore.jsfx
-- and uses only double arithmetic below 2^53, so both give the same bits. The test vectors
-- in tests/test_seqcore.py pin the two together.
local prob = {}

local M = 67108859
local A = 40503817
local B = 60251237
local C = 9301841

local function reduce(x)
  local q = math.floor(x / M)
  local r = x - q * M
  if r < 0 then
    r = r + M
  elseif r >= M then
    r = r - M
  end
  return r
end

local function mix(h, x)
  h = reduce(h + x + 1)
  h = reduce(h * A)
  h = reduce(h * h + C)
  return reduce(h * B)
end

prob.MODULUS = M

-- seed and both keys are 0 to 16,777,215. n is the project step number.
function prob.roll(seed, pattern_key, lane_key, n)
  local h = mix(0, seed)
  h = mix(h, pattern_key)
  h = mix(h, lane_key)
  h = mix(h, reduce(n))
  return reduce(h * h + C)
end

-- True when a step with this probability percent plays.
function prob.plays(percent, seed, pattern_key, lane_key, n)
  if percent >= 100 then
    return true
  elseif percent <= 0 then
    return false
  end
  return prob.roll(seed, pattern_key, lane_key, n) * 100 < percent * M
end

-- The project step number for an absolute beat and a step length in beats.
function prob.step_number(beat, step_beats)
  return math.floor(beat / step_beats + 0.000001)
end

return prob
