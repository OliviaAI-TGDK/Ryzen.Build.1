-- delta_link_ffi.lua
-- LuaJIT FFI wrapper for libdeltaffi.so
--
-- Termux:
--   local dl = require("delta_link_ffi")
--   local m = dl.manifest("/data/data/com.termux/files/home/test.bin", 4096, true)
--   print(dl.json(m))

local ffi = require("ffi")
local bit = require("bit")

ffi.cdef[[
typedef unsigned char uint8_t;
typedef unsigned long size_t;
typedef unsigned long long uint64_t;

uint64_t dl_fnv1a64(const uint8_t *data, size_t len);
void dl_reverse_canonical(const uint8_t *src, size_t len, uint8_t *dst);
void dl_inter_reversed(const uint8_t *src, size_t len, uint8_t *dst);
void dl_xor_delta(const uint8_t *a, size_t a_len,
                  const uint8_t *b, size_t b_len,
                  uint8_t *out);
size_t dl_block_hashes(const uint8_t *data, size_t len,
                       size_t block_size,
                       uint64_t *out, size_t out_cap);
size_t dl_pack_rle(const uint8_t *src, size_t len,
                   uint8_t *out, size_t out_cap);
size_t dl_unpack_rle(const uint8_t *src, size_t len,
                     uint8_t *out, size_t out_cap);
]]

local ok, C = pcall(ffi.load, "./libdeltaffi.so")
if not ok then
  C = ffi.load("libdeltaffi.so")
end

local M = {}

local function hex64_u64(u64)
  local v = tonumber(u64)
  if v then
    return string.format("%016x", v)
  end
  local hi = tonumber(bit.rshift(u64, 32))
  local lo = tonumber(bit.band(u64, 0xffffffff))
  return string.format("%08x%08x", hi, lo)
end

local function read_file(path)
  local f, e = io.open(path, "rb")
  if not f then return nil, e end
  local d = f:read("*a")
  f:close()
  return d
end

local function write_file(path, data)
  local f, e = io.open(path, "wb")
  if not f then return nil, e end
  f:write(data)
  f:close()
  return true
end

local function as_u8_buf(s)
  local n = #s
  local buf = ffi.new("uint8_t[?]", n)
  ffi.copy(buf, s, n)
  return buf, n
end

local function from_u8_buf(buf, n)
  return ffi.string(buf, n)
end

local function reverse_bytes_lua(data)
  return data:reverse()
end

local function json_escape(s)
  return s
    :gsub("\\", "\\\\")
    :gsub('"', '\\"')
    :gsub("\b", "\\b")
    :gsub("\f", "\\f")
    :gsub("\n", "\\n")
    :gsub("\r", "\\r")
    :gsub("\t", "\\t")
    :gsub("[%z\1-\31]", function(c)
      return string.format("\\u%04x", c:byte())
    end)
end

local function is_array(t)
  local n = 0
  for k, _ in pairs(t) do
    if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then return false end
    if k > n then n = k end
  end
  for i = 1, n do
    if t[i] == nil then return false end
  end
  return true
end

local function encode_json(v, seen)
  seen = seen or {}
  local tv = type(v)

  if tv == "nil" then
    return "null"
  elseif tv == "boolean" then
    return v and "true" or "false"
  elseif tv == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    return tostring(v)
  elseif tv == "string" then
    return '"' .. json_escape(v) .. '"'
  elseif tv == "table" then
    if seen[v] then error("cyclic table in json encode") end
    seen[v] = true

    local out = {}
    if is_array(v) then
      for i = 1, #v do out[i] = encode_json(v[i], seen) end
      seen[v] = nil
      return "[" .. table.concat(out, ",") .. "]"
    else
      local idx = 1
      for k, val in pairs(v) do
        out[idx] = encode_json(tostring(k), seen) .. ":" .. encode_json(val, seen)
        idx = idx + 1
      end
      table.sort(out)
      seen[v] = nil
      return "{" .. table.concat(out, ",") .. "}"
    end
  else
    return encode_json(tostring(v), seen)
  end
end

function M.fnv1a64(data)
  local buf, n = as_u8_buf(data)
  return "0x" .. hex64_u64(C.dl_fnv1a64(buf, n))
end

function M.canonical(data)
  return data
end

function M.reverse_canonical(data)
  local buf, n = as_u8_buf(data)
  local out = ffi.new("uint8_t[?]", n)
  C.dl_reverse_canonical(buf, n, out)
  return from_u8_buf(out, n)
end

function M.inter_reversed(data)
  local buf, n = as_u8_buf(data)
  local out = ffi.new("uint8_t[?]", n)
  C.dl_inter_reversed(buf, n, out)
  return from_u8_buf(out, n)
end

function M.xor_delta(a, b)
  local abuf, an = as_u8_buf(a)
  local bbuf, bn = as_u8_buf(b)
  local outn = math.max(an, bn)
  local out = ffi.new("uint8_t[?]", outn)
  C.dl_xor_delta(abuf, an, bbuf, bn, out)
  return from_u8_buf(out, outn)
end

function M.block_hashes(data, block_size)
  block_size = block_size or 4096
  local buf, n = as_u8_buf(data)
  local cap = math.max(1, math.ceil(n / block_size))
  local out = ffi.new("uint64_t[?]", cap)
  local count = tonumber(C.dl_block_hashes(buf, n, block_size, out, cap))
  local hashes = {}
  for i = 0, count - 1 do
    hashes[#hashes + 1] = "0x" .. hex64_u64(out[i])
  end
  return hashes
end

function M.block_root(data, block_size)
  local hashes = M.block_hashes(data, block_size or 4096)
  return "0x" .. hex64_u64(C.dl_fnv1a64(hashes and table.concat(hashes, "|") or "", #(table.concat(hashes, "|"))))
end

function M.pack(data)
  local buf, n = as_u8_buf(data)
  local need = tonumber(C.dl_pack_rle(buf, n, nil, 0))
  local out = ffi.new("uint8_t[?]", need)
  local wrote = tonumber(C.dl_pack_rle(buf, n, out, need))
  return from_u8_buf(out, wrote)
end

function M.unpack(data)
  local buf, n = as_u8_buf(data)
  local need = tonumber(C.dl_unpack_rle(buf, n, nil, 0))
  if need == 0 and #data > 0 then
    return nil, "corrupt packed stream"
  end
  local out = ffi.new("uint8_t[?]", need)
  local wrote = tonumber(C.dl_unpack_rle(buf, n, out, need))
  if wrote == 0 and #data > 0 then
    return nil, "corrupt packed stream"
  end
  return from_u8_buf(out, wrote)
end

function M.manifest_from_data(data, block_size, emit_blocks)
  block_size = block_size or 4096

  local canonical = data
  local reverse = M.reverse_canonical(data)
  local inter = M.inter_reversed(data)

  local d_can = M.xor_delta(data, canonical)
  local d_rev = M.xor_delta(data, reverse)
  local d_int = M.xor_delta(data, inter)

  local source_blocks = M.block_hashes(data, block_size)
  local canonical_blocks = M.block_hashes(canonical, block_size)
  local reverse_blocks = M.block_hashes(reverse, block_size)
  local inter_blocks = M.block_hashes(inter, block_size)

  local m = {
    schema = "tgdk-delta-link/v1",
    format_family = "posix-delta-link-ffi",
    block_size = block_size,
    link_modes = { "canonical", "reverse_canonical", "inter_reversed" },

    lengths = {
      source = #data,
      canonical = #canonical,
      reverse_canonical = #reverse,
      inter_reversed = #inter
    },

    hashes = {
      source = M.fnv1a64(data),
      canonical = M.fnv1a64(canonical),
      reverse_canonical = M.fnv1a64(reverse),
      inter_reversed = M.fnv1a64(inter)
    },

    delta_link = {
      canonical_delta_hash = M.fnv1a64(d_can),
      reverse_delta_hash = M.fnv1a64(d_rev),
      inter_reversed_delta_hash = M.fnv1a64(d_int)
    },

    block_hash_roots = {
      source = M.fnv1a64(table.concat(source_blocks, "|")),
      canonical = M.fnv1a64(table.concat(canonical_blocks, "|")),
      reverse_canonical = M.fnv1a64(table.concat(reverse_blocks, "|")),
      inter_reversed = M.fnv1a64(table.concat(inter_blocks, "|"))
    }
  }

  if emit_blocks then
    m.block_hashes = {
      source = source_blocks,
      canonical = canonical_blocks,
      reverse_canonical = reverse_blocks,
      inter_reversed = inter_blocks
    }
  end

  return m
end

function M.manifest(path, block_size, emit_blocks)
  local data, err = read_file(path)
  if not data then return nil, err end
  local m = M.manifest_from_data(data, block_size, emit_blocks)
  m.path = path
  return m
end

function M.read_file(path)
  return read_file(path)
end

function M.write_file(path, data)
  return write_file(path, data)
end

function M.json(v)
  return encode_json(v)
end

return M
