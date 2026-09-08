# Stage conf/offsets_windows.conf next to the exe, but NEVER over the top of one
# that is already there.
#
# The running app owns that file: it re-fetches offsets whenever Roblox updates
# and writes the result beside its own exe (offsets_fetch.cpp). It is the only
# conf anything reads at runtime.
#
# A plain `cmake -E copy` therefore undid the fetch on every rebuild. The failure
# is quiet and expensive: the build reports success, the app comes up, and the
# only symptom is a refusal to run with a version mismatch -- or worse, on a
# build where the check does not fire, offsets that are subtly wrong for the
# client. Debugging that costs far more than the copy ever saved.
#
# THERE IS NO REPO CONF TO STAGE FROM, and that is deliberate. The old
# conf/offsets_windows.conf came from a Linux-only generator left over from the
# original port, so the seed could not be regenerated here -- only rot. A stale
# seed is worse than none: either the version check rejects it (a confusing
# first run) or, on a build where that check does not fire, it is silently wrong
# for the attached client. Both the seed and the generator are gone.
#
# So a missing source is a normal state, not an error. The app fetches what it
# needs on first attach.

if(EXISTS "${DST}")
    message(STATUS "conf: keeping staged offsets (runtime fetch owns them)")
elseif(EXISTS "${SRC}")
    configure_file("${SRC}" "${DST}" COPYONLY)
    message(STATUS "conf: staged the repo copy (none was present)")
else()
    message(STATUS "conf: none staged and no repo copy -- the app fetches on "
                   "first attach")
endif()
