-- lsp_span: LSP data-plane helpers — semantic-token decode and cached-span
-- clipping (editor-agnostic; attrmap + position fn are injected).
-- Decode turns the flat int token array (5 ints per token: deltaLine,
-- deltaStartChar, length, tokenType, tokenModifiers) into byte-offset
-- spans in document order; clip binary-searches that sorted cache so
-- render only folds the visible slice.
local lsp_span = {}

-- Decode a semanticTokens/full response into {offset, length, attr} spans.
-- Tokens are relative-encoded: deltaLine accumulates; deltaStartChar is
-- absolute when deltaLine > 0, else relative to the previous token.
--- @param tokens integer[]
--- @param legend table  {tokenTypes = string[]}
--- @param attrmap table  tokenType name -> attr table (unknown: skip)
--- @param posfn fun(line: integer, unit: integer): integer  UTF-16 pos -> byte offset
--- @return table
function lsp_span.decode(tokens, legend, attrmap, posfn)
  local types = legend and legend.tokenTypes or {}
  local line, unit = 0, 0
  local out = {}
  local i = 1
  while i + 4 <= #tokens do
    local dline, dunit = tokens[i], tokens[i + 1]
    local len, ttype = tokens[i + 2], tokens[i + 3]
    i = i + 5
    line = line + dline
    if dline == 0 then unit = unit + dunit else unit = dunit end
    local attr = attrmap[types[ttype + 1]]
    if attr and len > 0 then
      local s = posfn(line, unit)
      local e = posfn(line, unit + len)
      if e > s then
        out[#out + 1] = { offset = s, length = e - s, attr = attr }
      end
    end
  end
  return out
end

-- Slice spans (sorted by offset) to those touching [start, endoff).
--- @param spans table sorted array of {offset, length, attr}
--- @param start integer
--- @param endoff integer  (exclusive)
--- @return table
function lsp_span.clip(spans, start, endoff)
  local n = #spans
  if n == 0 or endoff <= start then return {} end
  local lo, hi = 1, n
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if spans[mid].offset + spans[mid].length <= start then
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  local from = lo
  hi = n
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if spans[mid].offset < endoff then
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  local out = {}
  for i = from, hi do out[#out + 1] = spans[i] end
  return out
end

return lsp_span
