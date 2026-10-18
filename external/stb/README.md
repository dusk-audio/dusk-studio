# stb_image

stb_image 2.30 from nothings/stb, commit
013ac3beddff3dbffafd5177e7972067cd2b5083 (2024-05-31).

Source: https://github.com/nothings/stb/blob/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h

Upstream SHA-256: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
The original was verified byte-for-byte against that upstream blob and checked
for newer `stb_image.h` commits on 2026-10-16.

Dusk's JPEG decoder patch propagates marker-processing failures and requires
every frame component to have a complete initial scan before rendering or
progressive refinement. A missing restart marker succeeds only after the final
MCU; an earlier end fails. This prevents incomplete frames from exposing
uninitialized component samples or coefficients. The artwork tests cover baseline
and progressive JPEGs with missing components, premature scan ends, truncated
data, invalid markers, filled start markers, embedded metadata end markers and
valid trailing bytes.
Patched SHA-256: `2df44809cf130045787eaf99bf7e2f06a613039027d79ebdbc3f56852b320bed`.

Used only for bounded PNG/JPEG decoding of SFZ artwork. DAF's separate embedded
copy is not used by this decoder. Distributed under the MIT alternative printed
at the end of the header; the notice is also reproduced in LICENSES.txt.
