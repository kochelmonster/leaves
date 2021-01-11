#! /usr/bin/env python
import os
import subprocess
from os.path import join
from waflib.Tools import waf_unit_test

top = "."
out = "build"
VERSION = "0.0"


def options(opt):
    opt.load('compiler_cxx cxx waf_unit_test')
    opt.add_option(
        '--enable-gcov', action="store_true", default=False, dest='gcov', help='Add gconv support')


def configure(cfg):
    cmpfiles_path = os.path.abspath(os.path.join("tests", "burstcmp")) + os.sep
    trie_cmp_path = os.path.abspath(os.path.join("tests", "triecmp")) + os.sep

    cfg.check_waf_version(mini='1.8.5')
    cfg.load("compiler_cxx waf_unit_test")

    cfg.env.CXXFLAGS = []
    cfg.env.LINKFLAGS = []

    cfg.env.DEFINES_TEST += ['DEBUG', 'TESTING']
    cfg.env.DEFINES_TRIE = ["PURE_TRIE", 'CMPFILES="{}"'.format(trie_cmp_path)]
    cfg.env.DEFINES_BURST = ['CMPFILES="{}"'.format(cmpfiles_path), "SPLIT_COUNT=4"]

    cfg.env.INCLUDES = [os.path.abspath("include"), os.path.abspath("src")]
    cfg.env.STLIBPATH_TEST = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_TEST = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_TEST = ["-std=c++17", "-Wall", "-g"]

    if cfg.options.gcov:
        cfg.env['GCOV_ENABLED'] = True
        cfg.env.CXXFLAGS.extend(["-fprofile-arcs", "-ftest-coverage"])
        cfg.env.LINKFLAGS.extend(["-lgcov", "--coverage"])

    cfg.env.DEFINES_LIB = ["NDEBUG"]
    cfg.env.CXXFLAGS_LIB = ["-std=c++17", "-Wall", "-O3"]


def join_source(names):
    return [join("src", s) for s in names.split()]


def join_test(name):
    return [join("tests", name)]


def build(bld):
    core = "storage.cpp trace.cpp node.cpp trie.cpp table.cpp"

    bld.program(
        features="test",
        source=join_test("test_storage.cpp")+join_source("storage.cpp"),
        use="TEST BOOST",
        target="test_storage")

    bld.program(
        features="test",
        source=join_test("test_trie.cpp")+join_source(core)[:-1],
        use="TEST BOOST TRIE",
        target="test_trie")

    bld.program(
        features="test",
        source=join_test("test_burst.cpp")+join_source(core),
        use="TEST BOOST BURST",
        target="test_burst")

    bld.program(
        features="test",
        source=join_test("test_db.cpp")+join_source(core + " leaves.cpp"),
        use="TEST BOOST",
        target="test_db")

    bld.stlib(
        source=join_source(core + " leaves.cpp"),
        use="LIB",
        target="leaves")

    #bld.add_post_fun(lcov_zerocounters)
    bld.add_post_fun(waf_unit_test.summary)
    ##bld.add_post_fun(lcov_report)

    print("-----------------------------")
    print("missing test!  Value::advance")


def check_lcov():
    try:
        subprocess.call(["lcov", "--help"], stdout=subprocess.DEVNULL)
    except OSError as e:
        if e.errno == os.errno.ENOENT:
            raise WafError("Error: lcov program not found")
        else:
            raise


def lzero(bld):
    check_lcov()

    os.chdir(out)
    lcov_clear_command = "lcov -d . --zerocounters"
    if subprocess.Popen(lcov_clear_command, shell=True).wait():
        raise SystemExit(1)
    os.chdir("..")


def lreport(bld):
    check_lcov()

    os.chdir(out)
    try:
        lcov_report_dir = 'lcov-report'
        create_dir_command = "rm -rf " + lcov_report_dir
        create_dir_command += " && mkdir " + lcov_report_dir + ";"

        if subprocess.Popen(create_dir_command, shell=True).wait():
            raise SystemExit(1)

        info_file = os.path.join(lcov_report_dir, 'report.info')
        lcov_command = "lcov -c -d . -o " + info_file
        lcov_command += " -b " + os.getcwd()
        if subprocess.Popen(lcov_command, shell=True).wait():
            raise SystemExit(1)

        genhtml_command = "genhtml -o " + lcov_report_dir
        genhtml_command += " " + info_file
        if subprocess.Popen(genhtml_command, shell=True).wait():
            raise SystemExit(1)
    finally:
        os.chdir("..")
