# Driver Unit Tests

English | [日本語](README_ja.md)

These tests run driver C code in userspace. The headers in `stubs/` replace
Linux kernel and DRM APIs, so no Raspberry Pi, panel, kernel headers, or root
privileges are required. The tests do not build or load a kernel module.

## Requirements

* Linux or macOS
* A C11 compiler (GCC or Clang), Make, and POSIX thread development support
* The driver `src/` directory and the sources, `stubs/`, and `golden.txt` here

On Debian / Raspberry Pi OS, `build-essential` provides the basic build tools;
on macOS, use Xcode Command Line Tools. Python and Pillow are not required.
Select a compiler with an option such as `CC=clang`. Do not pass `-DNDEBUG`
in `CFLAGS`: these tests depend on assertions.

## Running the Tests

Run the following commands from the repository root:

```bash
make -C tests check
```

This builds and runs seven test programs. On native ARM64 hosts (`uname -m`
returns `aarch64` or `arm64`), it also runs the NEON rendering tests
automatically. Other architectures test the scalar C implementation.

Each successful test prints `all ... cases passed`; when all tests pass,
`make` exits with status `0`. Rendering mismatches report the case name and
expected/actual values. Other failures report diagnostics such as assertion
failures and return a nonzero status. Build failures also return a nonzero
status. Output order may vary with parallel execution.

| Source / build target | Coverage |
| --- | --- |
| `golden_render.c` / `golden_render` | Output hashes for 2/8/64-color conversion, quantization, NV12 conversion, right-panel partial updates, and monochrome ordered-dither density, cutoff monotonicity, inversion, and scalar/NEON agreement |
| `tx_pipeline.c` / `tx_pipeline` | Row generations, transmission order, progress across batches, retries and limits, pause/resume, and scheduling between panels |
| `spi_io.c` / `spi_io` | Controller transfer limits, tagged row batches, invalid batches, and panel commands |
| `perf_stats.c` / `perf_stats` | Disabling/resetting measurement, latency percentiles, and debugfs controls |
| `params_parse.c` / `params_parse` | Seconds-to-milliseconds parsing, invalid input, boundary values, and overflow |
| `fb_damage.c` / `fb_damage` | Panel selection from damage, expansion to complete rows, and regions for error diffusion and full regeneration |
| `registry.c` / `registry` | Concurrent device publication and color constraints using real POSIX threads and mutexes |

Individual tests can also be run. Specifying only a build target builds it
without executing it:

```bash
make -C tests tx_pipeline
./tests/.build/tx_pipeline

make -C tests golden_render
./tests/.build/golden_render --check tests/golden.txt
```

To build and run only the NEON variant:

```bash
make -C tests check-neon
```

On non-ARM64 hosts, this target prints an unsupported-host message and exits
successfully. That exit does not mean the NEON tests passed.

## Generated Files and Cleanup

Executables and proposed golden values are written to `tests/.build/`, which
is ignored by Git. Keep the test sources, `Makefile`, `stubs/`, and the
reference `golden.txt` in the public repository.

```bash
make -C tests clean
```

This removes generated executables and proposed golden values. It preserves
the `.build/` directory itself and the reference `tests/golden.txt`. Clean
and rebuild when changing the compiler or compiler flags.

You can override the output directory with `BUILD_DIR`, but must arrange Git
ignore rules for that location separately. Use the same `BUILD_DIR` when
cleaning.

## Updating golden.txt

`golden.txt` contains the expected output hash for each rendering case.
Normal `check` runs never change it. **Do not regenerate it merely to make a
failing test pass.** First determine whether a mismatch is a conversion bug
or an intentional behavior change. Update the reference only after reviewing
an intentional output change or new test cases and independently checking
the expected output. Matching hashes alone do not prove correct colors or
dithering.

Generate a candidate for review:

```bash
make -C tests golden
diff -u tests/golden.txt tests/.build/golden.txt
```

`make golden` writes `.build/golden.txt`; it does not overwrite the tracked
`golden.txt`. An exit status of `1` from `diff` means differences were found.
After checking the affected cases, reasons, and actual output, explicitly
accept the candidate:

```bash
cp tests/.build/golden.txt tests/golden.txt
make -C tests check
git diff -- tests/golden.txt
```

Review reference changes together with the corresponding code/test changes
and their rationale. Changes to paths supported by NEON must also be tested
on ARM64.

## Separate Hardware Validation

These tests do not validate real SPI transfers, GPIO/PWM, panel color
response, electrical wiring, or scheduling and DRM behavior in a real
kernel. Passing with `stubs/` is not equivalent to passing on hardware.

Kernel builds on Raspberry Pi, display and power control, re-enablement,
and DMA-BUF input still need separate checks. See the helper
[`scripts/drm_smoke.c`](../scripts/drm_smoke.c) and the
[main README](../README.md) for installation and performance measurement.
