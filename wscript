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
ALIGN = 4 # minimum align

def options(opt):
    opt.load('compiler_cxx boost')
    #ctx.add_option('--debug', default=false, dest='flags', type='flag')


def configure(cfg):
    import os
    cfg.check_waf_version(mini='1.8.5')
    cfg.load('compiler_cxx boost')
    #cfg.check_boost(lib='system filesystem')
    cfg.env.DEFINES_TEST += ['DEBUG', 'TESTING', "ALIGN={}".format(ALIGN)]
    cfg.env.DEFINES_BOOST_TEST += ['BOOST_ALL_NO_LIB']
    cfg.env.INCLUDES_TEST = [os.path.abspath("include"), 
                             os.path.abspath("src")]
    cfg.env.CXXFLAGS_TEST = ["-std=c++11", "-Wall", "-g"]
    cfg.env.STLIBPATH_TEST = []
    cfg.env.LINKFLAGS_TEST = ["-pthread"]#, "-lboost_filesystem",  "-lboost_system"]
    
    

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
        source=[join("tests", "test_trie.cpp")]+sources,
        use="TEST",
        target="test_trie")
    
    bld.program(
        source=[join("tests", "test_bits.cpp")]+sources,
        use="TEST",
        target="test_bits")


#@-leo
