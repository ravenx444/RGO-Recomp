include_guard(GLOBAL)

# RGO: write a configure-time generated file only when its content changes.
# file(WRITE) always gives the file a new timestamp, so every reconfigure made
# MSBuild recompile the patched RT64 sources even when nothing had changed.
function(jfg_write_if_changed path content)
    # Compare hashes: file(READ) drops carriage returns, file(SHA256) does not.
    if(EXISTS "${path}")
        file(SHA256 "${path}" existing)
        string(SHA256 wanted "${content}")
        if(existing STREQUAL wanted)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()
