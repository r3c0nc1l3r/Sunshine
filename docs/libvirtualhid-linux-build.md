# Linux libvirtualhid build copy

On Linux (excluding Android), configuration copies the pinned libvirtualhid
submodule into the build directory and applies the checked-in Linux key mapping
patch there. The copy retains the dependency's relative directories, including
any initialized nested submodules; initialize the nested dependencies needed by
that build. Other platforms use the original submodule directly.

The submodule must be at commit `53e1a949fc0784af716b782ddfa6c647cafd1f05`
and clean, including untracked and ignored files. Configuration requires Git
and the `patch` utility. It checks the patch digest, verifies its applicability
without fuzz, and records source and patched-copy digests. Subsequent
configurations reuse the copy only while all digests still match. A failed or
interrupted preparation, or a changed source or copy, requires a fresh build
directory; no source files or existing build copies are deleted or overwritten.
