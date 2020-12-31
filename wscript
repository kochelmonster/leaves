#! /usr/bin/env python
top = "."
out = "build"
APPNAME = "base64bench"
VERSION = "0.0"
ALIGN = 16  # 4 is minimum align
PAGE_SIZE = 1024*8


def options(opt):
    opt.load('compiler_cxx cxx waf_unit_test')
    opt.add_option('--gcov', action="store_true", default=False, dest='gcov')


def configure(cfg):
    import os

    cmpfiles_path = os.path.abspath(os.path.join("tests", "cmpfiles")) + os.sep

    cfg.check_waf_version(mini='1.8.5')
    cfg.load("compiler_cxx waf_unit_test")

    cfg.env.CXXFLAGS = []
    cfg.env.LINKFLAGS = []

    cfg.env.DEFINES_TEST += ['DEBUG', 'TESTING', "ALIGN={}".format(ALIGN),
                             'CMPFILES="{}"'.format(cmpfiles_path)]

    cfg.env.DEFINES_BOOST_TEST += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_TEST = [os.path.abspath("include"), os.path.abspath("src")]
    cfg.env.STLIBPATH_TEST = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_TEST = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_TEST = ["-std=c++17", "-Wall", "-g", "-march=corei7"]

    if cfg.options.gcov:
        cfg.env.CXXFLAGS.extend(["-fprofile-arcs", "-ftest-coverage"])
        cfg.env.LINKFLAGS.extend(["-lgcov", "--coverage"])

    cfg.env.DEFINES_BENCH += ["ALIGN={}".format(ALIGN),
                              "PAGE_SIZE={}".format(PAGE_SIZE),
                              'DEBUG']  # debug
    cfg.env.DEFINES_BOOST_BENCH += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_BENCH = [os.path.abspath("include"),
                              os.path.abspath("src")]
    cfg.env.STLIBPATH_BENCH = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_BENCH = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_BENCH = ["-std=c++17", "-Wall", "-Wformat=0",
                                  "-march=corei7", "-g", "-O0"]


def source(names):
    from os.path import join
    return [join("src", s) for s in names.split()]


def test(name):
    from os.path import join
    return [join("tests", name)]


def build(bld):
    bld.program(
        features="test",
        source=test("test_storage.cpp")+source("storage.cpp"),
        use="TEST BOOST",
        target="test_storage")

    bld.program(
        features="test",
        source=test("test_node.cpp")+source("storage.cpp trace.cpp node.cpp"),
        use="TEST BOOST",
        target="test_node")

    bld.program(
        features="test",
        source=test("test_db.cpp")+source("storage.cpp trace.cpp node.cpp leaves.cpp"),
        use="TEST BOOST",
        target="test_db")
