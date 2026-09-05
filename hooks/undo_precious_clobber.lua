-- Opt-in example: snapshot precious paths before a truncating clobber so
-- rash.undo_last() can restore them. Not loaded by default — install into
-- RASH_HOOK_DIR (root-owned for enforcing) when you want this policy.
--
-- Coverage is shell redirects only (`>`, `>|`, `&>`). External writers
-- (editors, dd, language tools) need COW / a workspace envelope.

local patterns = {
  "%.ssh/",
  "id_rsa",
  "id_ed25519",
  "id_ecdsa",
  "id_dsa",
  "%.aws/credentials",
  "%.gnupg/",
  "%.netrc",
  "%.pgpass",
  "%.env$",
  "%.env%.",
}

local function is_precious(path)
  if not path then
    return false
  end
  local index = 1
  while patterns[index] do
    if string.find(path, patterns[index], 1, false) then
      return true
    end
    index = index + 1
  end
  return false
end

rash.on_clobber(function(ctx)
  if not is_precious(ctx.path) then
    return
  end
  local ok, err = rash.snapshot_file(ctx.path)
  if ok then
    rash.warn("snapshotted precious clobber target: " .. tostring(ctx.path))
  else
    rash.warn("could not snapshot precious path (" .. tostring(err) .. "): " .. tostring(ctx.path))
  end
end)
