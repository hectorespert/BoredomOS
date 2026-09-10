"""Report the firmware's true RAM commitment, and fail while the numbers are legible.

`pio run` prints `.data + .noinit + .bss`, which leaves out the newlib heap, the main
stack and the vector table -- on this variant, another 9472 bytes. The figure it
prints is therefore not the commitment, and the headroom it implies is not the
headroom.

An actual overflow does already fail the link, because `fsp.ld` places `.heap` and
`.stack_dummy` at absolute addresses at the top of RAM and anything growing into them
overlaps. But it fails as a section-overlap message naming two addresses -- no size,
and not the object that did not fit -- and a post-build step never runs after a failed
link at all. So this check is not what detects the overflow. It exists to stop the
build *before* that point, while there is still a number to report.
"""

import re
import subprocess

Import("env")

MINIMUM_HEADROOM = int(env.GetProjectOption("custom_ram_min_headroom", 1024))

# Everything that occupies RAM at run time. `.data` is copied from flash into RAM at
# startup, so it costs both; the rest cost RAM only.
RAM_SECTIONS = (".data", ".noinit", ".bss", ".heap", ".stack_dummy", ".vector_table")


def _ram_length(env):
    """RAM_LENGTH from the variant's generated memory_regions.ld."""
    framework = env.PioPlatform().get_package_dir("framework-arduinorenesas-uno")
    board = env.BoardConfig().get("build.variant")
    path = "%s/variants/%s/memory_regions.ld" % (framework, board)
    with open(path) as handle:
        match = re.search(r"^RAM_LENGTH\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", handle.read(), re.M)
    if match is None:
        raise RuntimeError("no RAM_LENGTH in %s" % path)
    return int(match.group(1), 0)


def check_ram_budget(source, target, env):
    elf = str(target[0])
    size_tool = env.subst("$SIZETOOL")
    output = subprocess.check_output([size_tool, "-A", elf]).decode()

    sizes = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in RAM_SECTIONS:
            sizes[parts[0]] = int(parts[1])

    committed = sum(sizes.values())
    capacity = _ram_length(env)
    headroom = capacity - committed

    print("RAM budget:")
    for name in RAM_SECTIONS:
        if sizes.get(name):
            print("  %-14s %6d B" % (name, sizes[name]))
    print("  %-14s %6d B of %d (%.1f%%)" % ("committed", committed, capacity,
                                            100.0 * committed / capacity))
    print("  %-14s %6d B (minimum %d)" % ("headroom", headroom, MINIMUM_HEADROOM))

    if headroom < MINIMUM_HEADROOM:
        raise SystemExit(
            "RAM budget exceeded: %d bytes committed of %d, leaving %d bytes of "
            "headroom against a minimum of %d -- short by %d bytes."
            % (committed, capacity, headroom, MINIMUM_HEADROOM,
               MINIMUM_HEADROOM - headroom))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_ram_budget)
