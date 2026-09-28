-- Audit outermost commands. Words are unexpanded. Set RASH_AUDIT_LOG to a
-- file; otherwise lines go to stderr as "rash audit: ...".
-- This is a footgun log, not a security boundary.

local function walk(cmd)
  if not cmd then
    return
  end
  if cmd.kind == "simple" then
    local words = cmd.words
    local line = "simple"
    local i = 1
    while words and words[i] do
      line = line .. " " .. words[i]
      i = i + 1
    end
    rash.audit(line)
  elseif cmd.kind == "connection" then
    walk(cmd.left)
    walk(cmd.right)
  end
end

rash.use(function(cmd, nxt)
  walk(cmd)
  return nxt(cmd)
end)
