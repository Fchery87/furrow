-- A small JSON encoder and decoder for the bridge. It handles what the protocol sends:
-- objects, arrays, strings, numbers, booleans and null. Not a general library.
local json = {}

local ESCAPES = { ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }

local function escape(text)
  return (text:gsub('[%c"\\]', function(c)
    return ESCAPES[c] or string.format("\\u%04x", c:byte())
  end))
end

local function is_array(value)
  local count = 0
  for key in pairs(value) do
    if type(key) ~= "number" or key < 1 or key ~= math.floor(key) then
      return false
    end
    count = count + 1
  end
  for i = 1, count do
    if value[i] == nil then
      return false
    end
  end
  return true
end

-- Encodes value. Functions and userdata become their tostring text. Depth past 12 becomes
-- the text "<deep>", so a cyclic table cannot run forever.
function json.encode(value, depth)
  depth = depth or 0
  local kind = type(value)
  if value == nil then
    return "null"
  elseif kind == "boolean" then
    return tostring(value)
  elseif kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then
      return "null"
    end
    if value == math.floor(value) and math.abs(value) < 2 ^ 53 then
      return string.format("%d", value)
    end
    return string.format("%.17g", value)
  elseif kind == "string" then
    return '"' .. escape(value) .. '"'
  elseif kind == "table" then
    if depth > 12 then
      return '"<deep>"'
    end
    local parts = {}
    if next(value) == nil or is_array(value) then
      for i = 1, #value do
        parts[i] = json.encode(value[i], depth + 1)
      end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for key in pairs(value) do
      keys[#keys + 1] = tostring(key)
    end
    table.sort(keys)
    for _, key in ipairs(keys) do
      local item = value[key]
      if item == nil then
        item = value[tonumber(key)]
      end
      parts[#parts + 1] = '"' .. escape(key) .. '":' .. json.encode(item, depth + 1)
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return '"' .. escape(tostring(value)) .. '"'
end

local function skip(text, i)
  return text:find("%S", i) or #text + 1
end

local parse_value

local function parse_string(text, i)
  local out = {}
  i = i + 1
  while true do
    local c = text:sub(i, i)
    if c == "" then
      error("unterminated string")
    elseif c == '"' then
      return table.concat(out), i + 1
    elseif c == "\\" then
      local e = text:sub(i + 1, i + 1)
      local map = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
      if e == "u" then
        local code = tonumber(text:sub(i + 2, i + 5), 16)
        if not code then
          error("bad unicode escape")
        end
        out[#out + 1] = utf8.char(code)
        i = i + 6
      elseif map[e] then
        out[#out + 1] = map[e]
        i = i + 2
      else
        error("bad escape")
      end
    else
      out[#out + 1] = c
      i = i + 1
    end
  end
end

function parse_value(text, i, depth)
  if depth > 32 then
    error("nested too deeply")
  end
  i = skip(text, i)
  local c = text:sub(i, i)
  if c == "{" then
    local obj = {}
    i = skip(text, i + 1)
    if text:sub(i, i) == "}" then
      return obj, i + 1
    end
    while true do
      i = skip(text, i)
      if text:sub(i, i) ~= '"' then
        error("expected a key")
      end
      local key
      key, i = parse_string(text, i)
      i = skip(text, i)
      if text:sub(i, i) ~= ":" then
        error("expected a colon")
      end
      obj[key], i = parse_value(text, i + 1, depth + 1)
      i = skip(text, i)
      local d = text:sub(i, i)
      i = i + 1
      if d == "}" then
        return obj, i
      elseif d ~= "," then
        error("expected a comma or brace")
      end
    end
  elseif c == "[" then
    local arr = {}
    i = skip(text, i + 1)
    if text:sub(i, i) == "]" then
      return arr, i + 1
    end
    while true do
      arr[#arr + 1], i = parse_value(text, i, depth + 1)
      i = skip(text, i)
      local d = text:sub(i, i)
      i = i + 1
      if d == "]" then
        return arr, i
      elseif d ~= "," then
        error("expected a comma or bracket")
      end
    end
  elseif c == '"' then
    return parse_string(text, i)
  elseif text:sub(i, i + 3) == "true" then
    return true, i + 4
  elseif text:sub(i, i + 4) == "false" then
    return false, i + 5
  elseif text:sub(i, i + 3) == "null" then
    return nil, i + 4
  end
  local number = text:match("^-?%d+%.?%d*[eE]?[+-]?%d*", i)
  if not number or number == "" then
    error("unexpected character at " .. i)
  end
  return tonumber(number), i + #number
end

-- Returns the decoded value, or nil and a message.
function json.decode(text)
  local ok, value, stop = pcall(function()
    local v, i = parse_value(text, 1, 0)
    if skip(text, i) <= #text then
      error("trailing text")
    end
    return v, i
  end)
  if not ok then
    return nil, value
  end
  return value
end

return json
