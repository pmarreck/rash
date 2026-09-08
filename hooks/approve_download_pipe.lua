-- Opt-in: intercept download→shell pipelines (curl|bash and kin).
--
-- Factoring B: execute_pipeline seam buffers producer stdout, then this
-- handler reviews via rash.approve_bytes and runs the consumer with
-- rash.exec_with_stdin on the exact buffered bytes (never a second download).
--
-- Prior art: ~/dotfiles/bin/confirm (manual `| confirm |` middle).

rash.on_download_pipe(function(ctx)
  if not rash.approve_bytes(ctx.bytes, { kind = "download-to-shell" }) then
    rash.deny("download-to-shell script rejected (or no tty / auto-reject)")
    return
  end
  rash.exec_with_stdin(ctx.consumer_argv, ctx.bytes)
end)
