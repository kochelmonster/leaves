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
    cfg.env.INCLUDES_TEST = [os.path.abspath("include"),
                             os.path.abspath("src")]
    cfg.env.STLIBPATH_TEST = []
    if cfg.env.CXX_NAME == "gcc":
        # cfg.env.LINKFLAGS_TEST = ["-pthread"]
        # , "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_TEST = ["-std=c++17", "-Wall", "-g", "-march=corei7"]

    if cfg.options.gcov:
        cfg.env.CXXFLAGS.extend(["-fprofile-arcs", "-ftest-coverage"])
        print("++gcov", cfg.env.LINKFLAGS)
        cfg.env.LINKFLAGS.extend(["-lgcov", "--coverage"])
        print("--gcov", cfg.env.LINKFLAGS, cfg.env.CXXFLAGS)

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


def _build(bld):
    from os.path import join, abspath
    sources = "page.cpp storage.cpp node.cpp leaves.cpp base64.cpp trace.cpp"
    sources = [join("src", s) for s in sources.split()]

    bld.program(
        features="test",
        source=[join("tests", "test_trie.cpp")]+sources,
        use="TEST BOOST",
        target="test_trie")

    bld.program(
        # features="test",
        source=[join("tests", "test_bits.cpp")]+sources,
        use="TEST BOOST",
        target="test_bits")

    bld.program(
        # features="test",
        source=[join("tests", "test_memorydb.cpp")]+sources,
        use="TEST BOOST",
        target="test_memorydb")

    """
    only for generating test_trie tests
    """
    def generate_graph(task):
        command = abspath(join("build", "test_trie"))
        graph = abspath(join("tests", "graph.py"))
        task.exec_command(command + "|" + graph)
        task.exec_command("dot -Tsvg -O graph-0.dot")
        return task.exec_command("dot -Tsvg -O graph-1.dot")

    bld.add_post_fun(generate_graph)

    bld.program(
        source=[join("benchmarks", "sbench.cpp")]+sources,
        use="BENCH BOOST",
        target="sbench")


def build(bld):
    from os.path import join
    sources = "storage.cpp"
    sources = [join("src", s) for s in sources.split()]

    bld.program(
        features="test",
        source=[join("tests", "test_storage.cpp")]+sources,
        use="TEST BOOST",
        target="test_storage")

    bld.program(
        features="test",
        source=[join("tests", "test_node.cpp")]+sources,
        use="TEST BOOST",
        target="test_node")
