# Build bring-up helpers

Small scripts for reading large WebKit build logs while bringing up the port
(build with `tools/build.py build -k` to collect every failure in one pass).

| Script | Use |
| --- | --- |
| `failed_units.py <log>` | Failed translation units grouped by the kinds of error they hit |
| `unit_errors.py <log> <name>...` | First distinct errors, with locations, of the named units |
| `error_sites.py <log> <regex>...` | Source locations where errors matching each pattern occur |
| `compile_with.py <build dir> <file suffix> <clang-cl>` | Recompile one unit with another compiler (no PCH), to tell a compiler regression from a code problem |
