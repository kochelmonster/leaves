#! /usr/bin/env python
top = "."
out = "build"
VERSION = "0.0"


def options(opt):
    opt.load('compiler_cxx cxx waf_unit_test')
    opt.add_option(
        '--gcov', action="store_true", default=False, dest='gcov', help='Add gconv support')


def configure(cfg):
    import os

    cmpfiles_path = os.path.abspath(os.path.join("tests", "cmpfiles")) + os.sep

    cfg.check_waf_version(mini='1.8.5')
    cfg.load("compiler_cxx waf_unit_test")

    cfg.env.CXXFLAGS = []
    cfg.env.LINKFLAGS = []

    cfg.env.DEFINES_TEST += ['DEBUG', 'TESTING', 'CMPFILES="{}"'.format(cmpfiles_path)]

    cfg.env.INCLUDES = [os.path.abspath("include"), os.path.abspath("src")]
    cfg.env.STLIBPATH_TEST = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_TEST = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_TEST = ["-std=c++17", "-Wall", "-g"]

    if cfg.options.gcov:
        cfg.env.CXXFLAGS.extend(["-fprofile-arcs", "-ftest-coverage"])
        cfg.env.LINKFLAGS.extend(["-lgcov", "--coverage"])

    cfg.env.DEFINES_BENCH += ['DEBUG']  # debug
    cfg.env.DEFINES_BOOST_BENCH += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_BENCH = [os.path.abspath("include"),
                              os.path.abspath("src")]
    cfg.env.STLIBPATH_BENCH = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_BENCH = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_BENCH = ["-std=c++17", "-Wall", "-Wformat=0",
                                  "-g", "-O0"]

    cfg.env.DEFINES_LIB = ["NDEBUG"]
    cfg.env.CXXFLAGS_LIB = ["-std=c++17", "-Wall", "-O3"]


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
        source=test("test_node.cpp")+source("storage.cpp trace.cpp node.cpp trie.cpp"),
        use="TEST BOOST",
        target="test_node")

    bld.program(
        features="test",
        source=test("test_db.cpp")+source("storage.cpp trace.cpp node.cpp trie.cpp leaves.cpp"),
        use="TEST BOOST",
        target="test_db")

    bld.stlib(
        source=source("storage.cpp trace.cpp node.cpp trie.cpp leaves.cpp"),
        use="LIB",
        target="leaves")

    from waflib.Tools import waf_unit_test
    bld.add_post_fun(waf_unit_test.summary)

    print("-----------------------------")
    print("missing test!  Value::advance")
