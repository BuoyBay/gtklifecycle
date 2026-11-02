# Python policy for gtklifecycle

Python files are ignored by default. New production tooling belongs in Buoy or BuoyScript.

The following exact files remain temporary production compatibility exceptions:

| File | Required capability |
| --- | --- |
| `tools/build_gtklifecycle.py` | Compatibility compiler wrapper for assembled-workspace GTK guard builds and legacy cross-target options; normal source builds use cc directly. |

An exception may be removed after its native replacement passes the corresponding build or runtime checks.
Review additions file by file; directory-wide and wildcard Python exceptions are not allowed.
