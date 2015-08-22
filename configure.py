#!/usr/bin/env python

import os
import sys
import glob
import argparse
import ninja_syntax


class Configuration:

    def __init__(self, options, ninja_build_file=os.path.join(os.path.dirname(os.path.abspath(__file__)), "build.ninja")):
        self.output = open(ninja_build_file, "w")
        self.writer = ninja_syntax.Writer(self.output)
        self.source_dir = None
        self.build_dir = None
        self.artifact_dir = None
        self.prefix_dir = options.prefix
        self.include_dirs = []
        self.object_ext = ".bc"

        # Variables
        self.writer.variable("nacl_sdk_dir", options.nacl_sdk)
        self._set_pnacl_vars()
        self.writer.variable("cflags", "-std=gnu11")
        self.writer.variable("cxxflags", "-std=gnu++11")
        self.writer.variable("optflags", "-O3")

        # Rules
        self.writer.rule("cc", "$pnacl_cc -o $out -c $in -MMD -MF $out.d $optflags $cflags $includes",
            deps="gcc", depfile="$out.d",
            description="CC[PNaCl] $descpath")
        self.writer.rule("cxx", "$pnacl_cxx -o $out -c $in -MMD -MF $out.d $optflags $cxxflags $includes",
            deps="gcc", depfile="$out.d",
            description="CXX[PNaCl] $descpath")
        self.writer.rule("ccld", "$pnacl_cc -o $out $in $libs $libdirs $ldflags",
            description="CCLD[PNaCl] $descpath")
        self.writer.rule("cxxld", "$pnacl_cxx -o $out $in $libs $libdirs $ldflags",
            description="CXXLD[PNaCl] $descpath")
        self.writer.rule("ar", "$pnacl_ar rcs $out $in",
            description="AR[PNaCl] $descpath")
        self.writer.rule("finalize", "$pnacl_finalize $finflags -o $out $in",
            description="FINALIZE[PNaCl] $descpath")
        self.writer.rule("translate", "$pnacl_translate -arch $arch -o $out $in",
            description="TRANSLATE[PNaCl] $descpath")
        self.writer.rule("run", "$pnacl_sel_ldr $in",
            description="RUN[PNaCl] $descpath", pool="console")
        self.writer.rule("install", "install -m $mode $in $out",
            description="INSTALL $out")


    def _set_pnacl_vars(self):
        if sys.platform == "win32":
            self.writer.variable("pnacl_toolchain_dir", "$nacl_sdk_dir/toolchain/win_pnacl")
            self.writer.variable("pnacl_cc", "$pnacl_toolchain_dir/bin/pnacl-clang.bat")
            self.writer.variable("pnacl_cxx", "$pnacl_toolchain_dir/bin/pnacl-clang++.bat")
            self.writer.variable("pnacl_ar", "$pnacl_toolchain_dir/bin/pnacl-ar.bat")
            self.writer.variable("pnacl_finalize", "$pnacl_toolchain_dir/bin/pnacl-finalize.bat")
            self.writer.variable("pnacl_translate", "$pnacl_toolchain_dir/bin/pnacl-translate.bat")
        elif sys.platform == "linux2" or sys.platform == "darwin":
            if sys.platform == "linux2":
                self.writer.variable("pnacl_toolchain_dir", "$nacl_sdk_dir/toolchain/linux_pnacl")
            else:
                self.writer.variable("pnacl_toolchain_dir", "$nacl_sdk_dir/toolchain/mac_pnacl")
            self.writer.variable("pnacl_cc", "$pnacl_toolchain_dir/bin/pnacl-clang")
            self.writer.variable("pnacl_cxx", "$pnacl_toolchain_dir/bin/pnacl-clang++")
            self.writer.variable("pnacl_ar", "$pnacl_toolchain_dir/bin/pnacl-ar")
            self.writer.variable("pnacl_finalize", "$pnacl_toolchain_dir/bin/pnacl-finalize")
            self.writer.variable("pnacl_translate", "$pnacl_toolchain_dir/bin/pnacl-translate")
        else:
            raise OSError("Unsupported platform: " + sys.platform)
        self.writer.variable("pnacl_sel_ldr", "$nacl_sdk_dir/tools/sel_ldr.py")


    def _compile(self, rule, source_file, object_file):
        if not os.path.isabs(source_file):
            source_file = os.path.join(self.source_dir, source_file)
        if object_file is None:
            object_file = os.path.join(self.build_dir, os.path.relpath(source_file, self.source_dir)) + self.object_ext
        variables = {
            "descpath": os.path.relpath(source_file, self.source_dir)
        }
        if self.include_dirs:
            variables["includes"] = " ".join(["-I" + i for i in self.include_dirs])
        self.writer.build(object_file, rule, source_file, variables=variables)
        return object_file


    def cc(self, source_file, object_file=None):
        return self._compile("cc", source_file, object_file)


    def cxx(self, source_file, object_file=None):
        return self._compile("cxx", source_file, object_file)


    def _link(self, rule, object_files, binary_file, binary_dir, lib_dirs, libs):
        if not os.path.isabs(binary_file):
            binary_file = os.path.join(binary_dir, binary_file)
        variables = {
            "descpath": os.path.relpath(binary_file, binary_dir)
        }
        if lib_dirs:
            variables["libdirs"] = " ".join(["-L" + l for l in lib_dirs])
        if libs:
            variables["libs"] = " ".join(["-l" + l for l in libs])
        self.writer.build(binary_file, rule, object_files, variables=variables)
        return binary_file


    def ccld(self, object_files, binary_file, lib_dirs=[], libs=[]):
        return self._link("ccld", object_files, binary_file, self.build_dir, lib_dirs, libs)


    def cxxld(self, object_files, binary_file, lib_dirs=[], libs=[]):
        return self._link("cxxld", object_files, binary_file, self.build_dir, lib_dirs, libs)


    def ar(self, object_files, archive_file):
        if not os.path.isabs(archive_file):
            archive_file = os.path.join(self.artifact_dir, archive_file)
        variables = {
            "descpath": os.path.relpath(archive_file, self.artifact_dir)
        }
        self.writer.build(archive_file, "ar", object_files, variables=variables)
        return archive_file

    def finalize(self, binary_file, executable_file):
        if not os.path.isabs(binary_file):
            binary_file = os.path.join(self.build_dir, binary_file)
        if not os.path.isabs(executable_file):
            executable_file = os.path.join(self.artifact_dir, executable_file)
        variables = {
            "descpath": os.path.relpath(executable_file, self.artifact_dir)
        }
        self.writer.build(executable_file, "finalize", binary_file, variables=variables)
        return executable_file

    def translate(self, portable_file, native_file):
        if not os.path.isabs(portable_file):
            portable_file = os.path.join(self.artifact_dir, portable_file)
        if not os.path.isabs(native_file):
            native_file = os.path.join(self.artifact_dir, native_file)
        variables = {
            "descpath": os.path.relpath(portable_file, self.artifact_dir),
            "arch": "x86_64"
        }
        self.writer.build(native_file, "translate", portable_file, variables=variables)
        return native_file

    def run(self, executable_file, target):
        variables = {
            "descpath": os.path.relpath(executable_file, self.artifact_dir)
        }
        self.writer.build(target, "run", executable_file, variables=variables)

    def install(self, source_file, destination_file, mode=0o644):
        if not os.path.isabs(destination_file):
            destination_file = os.path.join(self.prefix_dir, destination_file)
        variables = {
            "mode": "0%03o" % mode
        }
        self.writer.build(destination_file, "install", source_file, variables=variables)
        return destination_file


parser = argparse.ArgumentParser(description="PThreadPool configuration script")
parser.add_argument("--with-nacl-sdk", dest="nacl_sdk", default=os.getenv("NACL_SDK_ROOT"),
    help="Native Client (Pepper) SDK to use")
parser.add_argument("--prefix", dest="prefix", default="/usr/local")


def main():
    options = parser.parse_args()

    config = Configuration(options)

    root_dir = os.path.dirname(os.path.abspath(__file__))

    config.source_dir = os.path.join(root_dir, "src")
    config.build_dir = os.path.join(root_dir, "build")
    config.artifact_dir = os.path.join(root_dir, "artifacts")
    config.include_dirs = [os.path.join("$nacl_sdk_dir", "include"), os.path.join(root_dir, "include"), os.path.join(root_dir, "src")]

    pthreadpool_object = config.cc("pthreadpool.c")
    pthreadpool_library = config.ar([pthreadpool_object], "libpthreadpool.a")

    config.source_dir = os.path.join(root_dir, "test")
    config.build_dir = os.path.join(root_dir, "build", "test")
    pthreadpool_test_object = config.cxx("pthreadpool.cc")
    pthreadpool_test_binary = config.cxxld([pthreadpool_object, pthreadpool_test_object], "pthreadpool.bc", libs=["gtest"], lib_dirs=[os.path.join("$nacl_sdk_dir", "lib", "pnacl", "Release")])
    pthreadpool_test_binary = config.finalize(pthreadpool_test_binary, "pthreadpool.pexe")
    pthreadpool_test_binary = config.translate(pthreadpool_test_binary, "pthreadpool.nexe")
    config.run(pthreadpool_test_binary, "test")

    config.writer.default([pthreadpool_library, pthreadpool_test_binary])

    config.writer.build("install", "phony", [
        config.install(os.path.join(root_dir, "include", "pthreadpool.h"), "include/pthreadpool.h"),
        config.install(os.path.join(pthreadpool_object), "lib/libpthreadpool.a")])


if __name__ == "__main__":
    sys.exit(main())
