#! /usr/bin/env python
#@+leo-ver=5-thin
#@+node:michael.20141230111914.135: * @file wscript
#@@first
#@@language python
#@@tabwidth -4
top = "."
out = "build"
APPNAME = "base64bench"
VERSION = "0.0"
ALIGN = 4 # 4 is minimum align


def options(opt):
    opt.load('compiler_cxx boost waf_unit_test')
    opt.add_option('--gcov', action="store_true", default=False, dest='gcov')


def configure(cfg):
    import os
    
    cmpfiles_path = os.path.abspath(os.path.join("tests", "cmpfiles")) + os.sep
    
    cfg.check_waf_version(mini='1.8.5')
    cfg.load('compiler_cxx boost waf_unit_test')
        
    #cfg.check_boost(lib='system filesystem')
    cfg.env.DEFINES_TEST += ['DEBUG', 'TESTING', "ALIGN={}".format(ALIGN),
                             'CMPFILES="{}"'.format(cmpfiles_path)]
    cfg.env.DEFINES_BOOST_TEST += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_TEST = [os.path.abspath("include"), 
                             os.path.abspath("src")]
    cfg.env.STLIBPATH_TEST = []
    if cfg.env.CXX_NAME == "gcc":
        cfg.env.LINKFLAGS_TEST = ["-pthread"]
        #, "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_TEST = ["-std=c++11", "-Wall", "-g", "-march=corei7"]
    
    cfg.env.DEFINES_TEST += ["ALIGN={}".format(ALIGN),
                             'CMPFILES="{}"'.format(cmpfiles_path)]
    
    
    if cfg.options.gcov:
        cfg.env.CXXFLAGS_TEST.extend(["-fprofile-arcs", "-ftest-coverage", "-fPIC"])
        cfg.env.LINKFLAGS_TEST.extend(["-fprofile-arcs"])

    cfg.env.DEFINES_BENCH += ["ALIGN={}".format(ALIGN)]
    cfg.env.DEFINES_BOOST_BENCH += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_BENCH = [os.path.abspath("include"), 
                              os.path.abspath("src")]
    cfg.env.STLIBPATH_BENCH = []
    if cfg.env.CXX_NAME == "gcc":
        #cfg.env.LINKFLAGS_BENCH = ["-pthread"]
        #, "-lboost_filesystem",  "-lboost_system"]
        cfg.env.CXXFLAGS_BENCH = ["-std=c++11", "-Wall", "-march=corei7", "-g", "-O3"]
    
    

def build(bld):
    from os.path import join, abspath
    sources = "trie.cpp node.cpp leaves.cpp base64.cpp"
    sources = [join("src", s) for s in sources.split()]
    """
    bld.program(
        source=[join("tests", "simple.cpp")]+sources,
        use="TEST",
        target="simple")
    """
    
    bld.program(
        features="test",
        source=[join("tests", "test_trie.cpp")]+sources,
        use="TEST",
        target="test_trie")
    
    bld.program(
        features="test",
        source=[join("tests", "test_bits.cpp")]+sources,
        use="TEST",
        target="test_bits")

    bld.program(
        features="test",
        source=[join("tests", "test_memorydb.cpp")]+sources,
        use="TEST",
        target="test_memorydb")

    """
    only for generating test_trie tests

    def generate_graph(task):
        command = abspath(join("build", "test_trie"))
        graph = abspath(join("tests", "graph.py"))
        task.exec_command(command + "|" + graph)
        task.exec_command("dot -Tsvg -O graph-0.dot")
        return task.exec_command("dot -Tsvg -O graph-1.dot")
     
    bld.add_post_fun(generate_graph)
    """

    bld.program(
        source=[join("benchmarks", "sbench.cpp")]+sources,
        use="BENCH",
        target="sbench")
        
        
#@-leo
