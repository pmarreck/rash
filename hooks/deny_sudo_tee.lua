-- Deny the parsed shape where an input redirection replaces the pipe feeding
-- `sudo -S tee`. Words are unexpanded; this never reads secret values.

local function has_input_redirect(redirects)
  local index = 1
  while redirects and redirects[index] do
    if redirects[index].is_input then
      return true
    end
    index = index + 1
  end
  return false
end

local function is_sudo_stdin_tee(command)
  local words = command.words
  return words and words[1] == "sudo" and words[2] == "-S" and words[3] == "tee"
end

rash.hook(function(command, run)
  if command.kind == "connection" and command.connector == "|" then
    local stage = command.right
    if stage and stage.kind == "simple" and is_sudo_stdin_tee(stage)
      and has_input_redirect(stage.redirects) then
      rash.deny("sudo -S tee with an input redirection replaces its pipeline input; secret input may be exposed")
      return
    end
  end
  return run()
end)
