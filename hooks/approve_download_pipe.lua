-- Opt-in: intercept download→shell pipelines (curl|bash and kin).
--
-- v1 (this file): fail closed — deny the shape so the consumer never runs.
-- Interactive review + exact-byte exec needs C ports (see INSTALLER_APPROVAL.md)
-- and Peter's UX choice; do not pretend deny-only is full approval.
--
-- Prior art: ~/dotfiles/bin/confirm (manual `| confirm |` middle).

local producers = {
  curl = true,
  wget = true,
}

local consumers = {
  bash = true,
  sh = true,
  rash = true,
}

local function basename(word)
  if not word then
    return nil
  end
  local i = 1
  local last = 0
  while true do
    local next_slash = string.find(word, "/", i, true)
    if not next_slash then
      break
    end
    last = next_slash
    i = next_slash + 1
  end
  if last == 0 then
    return word
  end
  return string.sub(word, last + 1)
end

local function is_producer(command)
  if not command or command.kind ~= "simple" or not command.words then
    return false
  end
  return producers[basename(command.words[1])] == true
end

local function is_consumer(command)
  if not command or command.kind ~= "simple" or not command.words then
    return false
  end
  return consumers[basename(command.words[1])] == true
end

local function is_download_to_shell(command)
  if not command or command.kind ~= "connection" or command.connector ~= "|" then
    return false
  end
  return is_producer(command.left) and is_consumer(command.right)
end

rash.hook(function(command, run)
  if is_download_to_shell(command) then
    rash.deny("download-to-shell pipeline intercepted; review/approval exec not enabled in this hook build — refusing rather than streaming into a shell")
    return
  end
  return run()
end)
