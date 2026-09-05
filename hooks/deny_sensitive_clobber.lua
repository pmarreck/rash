-- Deny clobbering redirects whose unexpanded target word looks sensitive.
-- Lexical only: `$HOME/.ssh/id_rsa` may not match until a post-expansion stage exists.

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
}

local function word_is_sensitive(word)
  if not word then
    return false
  end
  local index = 1
  while patterns[index] do
    if string.find(word, patterns[index], 1, false) then
      return true
    end
    index = index + 1
  end
  return false
end

local function has_sensitive_clobber(redirects)
  local index = 1
  if not redirects then
    return false, nil
  end
  while redirects[index] do
    local redirect = redirects[index]
    if redirect.is_clobber and word_is_sensitive(redirect.word) then
      return true, redirect.word
    end
    index = index + 1
  end
  return false, nil
end

rash.hook(function(command, run)
  if command.kind == "simple" then
    local bad, word = has_sensitive_clobber(command.redirects)
    if bad then
      rash.deny("refusing clobber redirect onto a sensitive path word: " .. tostring(word))
      return
    end
  end
  return run()
end)
