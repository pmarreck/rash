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

local function snapshot(path)
  if not is_precious(path) then
    return
  end
  local ok, err = rash.snapshot_file(path)
  if ok then
    -- "already": this apply already holds the preimage; one warning only.
    if err ~= "already" then
      rash.warn("snapshotted precious clobber target: " .. tostring(path))
    end
  else
    rash.warn("could not snapshot precious path (" .. tostring(err) .. "): " .. tostring(path))
  end
end

-- Redirect middleware: rows carry a path only for safe shapes (literal or
-- plain $NAME). A denied apply never reaches here, so it never snapshots.
rash.on_redirect_policy(function(info)
  local index = 1
  while info.redirects[index] do
    local redirect = info.redirects[index]
    if redirect.path and redirect.will_clobber then
      snapshot(redirect.path)
    end
    index = index + 1
  end
end)

-- Fallback for every other shape ("$f", $(cmd), compound commands). Within
-- one apply, rash.snapshot_file takes each path once, so a path the policy
-- already snapshotted is not copied again.
rash.on_clobber(function(ctx)
  snapshot(ctx.path)
end)
