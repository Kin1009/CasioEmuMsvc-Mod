-- Data-only script plugin for iPadOS.
-- No native code, no dlopen, no file/OS access: only the permission-gated
-- `host` table provided by ScriptRuntime. This demo only READS architecture
-- status; no control command is issued, so it cannot disturb the emulator.
local frames = 0

function on_frame(dt)
    frames = frames + 1
    if frames % 600 ~= 0 then
        return
    end
    local status = host.arch_status()
    if not status then
        host.log("arch status is unavailable (no emulator bound)")
        return
    end
    host.log(string.format("arch: model=%s hw_id=%d running=%s paused=%s cycles=%d",
        status.model, status.hw_id, tostring(status.running),
        tostring(status.paused), status.cycles))
end
