"""Exercise offline network logging, queue overflow, and bounded SD log rotation."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetdiagTest(unittest.TestCase):
    def test_offline_log(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc/machine").mkdir(parents=True)
            (tmp / "ogc/machine/processor.h").write_text(
                "#include <stdint.h>\ntypedef uint32_t u32; typedef uint64_t u64;\n"
                "#define _CPU_ISR_Disable(x) ((x)=0)\n"
                "#define _CPU_ISR_Restore(x) ((void)(x))\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <stdint.h>\nextern uint64_t fake_time;\n"
                "static inline uint64_t gettime(void) { return fake_time; }\n"
                "#define ticks_to_millisecs(x) (x)\n")
            harness = tmp / "check.c"
            harness.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "netdiag.h"
uint64_t fake_time = 100;
static char data[20000];
static void read_log(const char *path) {
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(data, 1, sizeof(data)-1, f);
    data[n] = 0;
    fclose(f);
}
int main(void) {
    // Failed SD opens retain early startup events.
    netdiag_event("Boot test");
    netdiag_flush(true);
    assert(mkdir("sd:", 0700) == 0);
    fake_time += 25;
    netdiag_event("Attempt status=%d", -110);
    netdiag_flush(true);
    read_log("sd:/hbc-network.log");
    assert(strstr(data, "+0 ms Boot test"));
    assert(strstr(data, "+25 ms Attempt status=-110"));
    // An overflowing queue reports loss and remains usable after draining.
    for (int i=0; i<70; ++i) netdiag_event("event %d", i);
    fake_time += 1000;
    netdiag_flush(false);
    read_log("sd:/hbc-network.log");
    assert(strstr(data, "event 63\n"));
    assert(!strstr(data, "event 64\n"));
    assert(strstr(data, "Dropped 6 diagnostic events"));
    // Relative times and throttle: a normal flush waits one second.
    netdiag_event("throttled");
    netdiag_flush(false);
    read_log("sd:/hbc-network.log");
    assert(!strstr(data, "throttled"));
    fake_time += 1000;
    netdiag_flush(false);
    read_log("sd:/hbc-network.log");
    assert(strstr(data, "throttled"));
    // Rotation keeps the previous file and starts a fresh current one.
    FILE *f = fopen("sd:/hbc-network.log", "a");
    assert(f);
    for (int i=0; i<1024*1024; ++i) fputc('x', f);
    fclose(f);
    netdiag_event("after rotation");
    netdiag_flush(true);
    read_log("sd:/hbc-network.log");
    assert(strstr(data, "after rotation"));
    assert(!strstr(data, "Boot test"));
    read_log("sd:/hbc-network.old.log");
    assert(strstr(data, "Boot test"));
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netdiag.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], cwd=tmp, check=True)


if __name__ == "__main__":
    unittest.main()
