#!/usr/bin/env python


import confu
parser = confu.standard_parser("pthreadpool configuration script")


def main(args):
    options = parser.parse_args(args)
    build = confu.Build.from_options(options)

    build.export_cpath("include", ["pthreadpool.h"])

    with build.options(source_dir="src", extra_include_dirs="src", deps=build.deps.fxdiv):
        if build.target.is_emscripten:
            source = "threadpool-shim.c"
        else:
            source = "threadpool-pthreads.c"
        build.static_library("pthreadpool", build.cc(source))

    with build.options(source_dir="test", deps=[build, build.deps.googletest]):
        build.unittest("pthreadpool-test", build.cxx("pthreadpool.cc"))

    with build.options(source_dir="bench", deps=[build, build.deps.googlebenchmark]):
        build.benchmark("latency-bench", build.cxx("latency.cc"))
        build.benchmark("throughput-bench", build.cxx("throughput.cc"))

    return build


if __name__ == "__main__":
    import sys
    main(sys.argv[1:]).generate()
