# Manual Tests

These tests are not run by `make test`. They cover paths the automated SQL suite
cannot exercise deterministically.

## `repro_oom_mislabel.cpp`

Reproduces the defect fixed by the OOM-hardening change: when libxml2 fails to
allocate memory while parsing a **valid** document, `xmlCtxtReadMemory` returns
`NULL`, which the reader previously reported as `File X contains invalid XML`,
indistinguishable from genuinely malformed input.

The repro installs a failing allocator via `xmlMemSetup`, sweeps the
allocation-failure point across a valid parse, and shows that:

- many failure points turn a valid document into a `NULL` doc, and
- `xmlCtxtGetLastError()->code` is `XML_ERR_NO_MEMORY` for the allocation case
  but a structural code for genuinely malformed input,

so the two are distinguishable at parse time (which is what the fix relies on).

`xmlMemSetup` is a no-op on Apple's system libxml2 (macOS 15.4+), so the repro
must run on Linux. Under Docker:

```sh
docker run --rm -v "$PWD/test/manual":/src:ro catthehacker/ubuntu:act-latest bash -c \
  'g++ -std=c++17 -O2 /src/repro_oom_mislabel.cpp $(xml2-config --cflags --libs) -o /tmp/o && /tmp/o'
```

Exit code `0` and a line beginning `DEMONSTRATED:` indicate the defect (and its
distinguishability) were reproduced.
