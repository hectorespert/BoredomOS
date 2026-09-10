// PlatformIO refuses to build a test suite whose directory contributes no source
// file: builder/tools/piobuild.py counts what it compiled from the test directory
// and exits before it ever looks at src/, so test_build_src = yes is not enough on
// its own. This translation unit exists only to satisfy that count.
//
// The suite itself is the Python in this directory, run on the host by run.py.
// What gets flashed is src/, unchanged -- the firmware the checks then interrogate.
