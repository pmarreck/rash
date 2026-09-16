-- Deny mv/cp/install when the destination is a symlink that is not a
-- directory (rename would replace the link). Symlink-to-dir is a container
-- and is allowed. Redirect `>` follows and is not this guard.
-- Override is rash.spawn from a parse-stage hook, not mv -f.

local guarded = {
  mv = true,
  cp = true,
  install = true,
  ginstall = true,
}

local function basename(path)
  if not path then
    return ""
  end
  return string.match(path, "[^/]+$") or path
end

local function destination(words)
  local i = 2
  local dest_from_t
  local operands = {}
  local n = 0
  local after_dd = false

  while words[i] do
    local w = words[i]
    if after_dd then
      n = n + 1
      operands[n] = w
    elseif w == "--" then
      after_dd = true
    elseif w == "-t" or w == "--target-directory" then
      i = i + 1
      dest_from_t = words[i]
    elseif string.sub(w, 1, 19) == "--target-directory=" then
      dest_from_t = string.sub(w, 20)
    elseif string.sub(w, 1, 1) == "-" and w ~= "-" then
      -- flag; -tDIR not handled
    else
      n = n + 1
      operands[n] = w
    end
    i = i + 1
  end
  if dest_from_t then
    return dest_from_t
  end
  if n < 2 then
    return nil
  end
  return operands[n]
end

local function guard(ctx)
  local words = ctx.words
  if not words then
    return
  end
  if not guarded[basename(words[1])] then
    return
  end
  local dest = destination(words)
  if not dest then
    return
  end
  if rash.is_symlink(dest) and not rash.is_directory(dest) then
    rash.deny("refusing to replace symlink with a regular file: " .. dest)
  end
end

rash.before(guard)
rash.on_exec(guard)
