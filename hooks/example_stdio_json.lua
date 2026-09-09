-- Opt-in example: with RASH_CAPTURE_FD=N, write minimal JSON {rc,out,err} to FD N.
-- Text-oriented; for binary use printable-binary encoding in a custom hook.

local function json_escape(s)
  if not s then return "" end
  s = string.gsub(s, '\\', '\\\\')
  s = string.gsub(s, '"', '\\"')
  s = string.gsub(s, '\n', '\\n')
  s = string.gsub(s, '\r', '\\r')
  s = string.gsub(s, '\t', '\\t')
  return s
end

rash.on_stdio_bundle(function(ctx)
  return '{"rc":' .. tostring(ctx.status)
    .. ',"out":"' .. json_escape(ctx.stdout)
    .. '","err":"' .. json_escape(ctx.stderr) .. '"}'
end)
