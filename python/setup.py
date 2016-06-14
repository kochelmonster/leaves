#! /usr/bin/env python
#!/usr/bin/env python
import os
from setuptools import setup, find_packages
from Cython.Distutils import build_ext, Extension

"""
with open('README.rst') as file_readme:
    readme = file_readme.read()

with open('CHANGES.rst') as file_changes:
    changes = file_changes.read()
"""

join = os.path.join
LEAVES_FILES = ["node.cpp", "trie.cpp", "leaves.cpp", "base64.cpp"]
LEAVES_FILES = [join("..", "src", f) for f in LEAVES_FILES]

class LarchExtension(Extension):
    def __init__(self):
        Extension.__init__(self, "larch.leaves", [])
        self.language="c++"
        self.include_dirs=[join("..", "include")]
        self.define_macros=[("ALIGN", 4), ("DEBUG", None)]
        self.sources=[join("larch", "leaves.pyx")] + LEAVES_FILES
    
    def make_flags(self, compiler):
        if compiler == "unix":
            self.extra_compile_args=["-std=c++11", "-O3", "-mssse3"]
            

extension = LarchExtension()

class my_build_ext(build_ext):
    def build_extensions(self):
        c = self.compiler.compiler_type
        for e in self.extensions:
            e.make_flags(c)
        build_ext.build_extensions(self)

setup(
    name="larch-leaves",
    version="1.0",
    description="larch-leaves for Python",
    #long_description = readme + "\n\n" + changes,
    author='kochelmonster',
        
    #author_email='kmike84@gmail.com',
    #url='https://github.com/kmike/hat-trie/',

    packages=find_packages(),
    namespace_packages=['larch'],
    zip_safe=False,
    ext_modules = [ extension ],

    classifiers=[
        'Development Status :: 5 - Production/Stable',
        'Intended Audience :: Developers',
        'License :: Other/Proprietary License',
        'Programming Language :: Cython',
        'Programming Language :: Python',
        'Programming Language :: Python :: 2',
        'Programming Language :: Python :: 2.6',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.2',
        'Programming Language :: Python :: 3.3',
        'Programming Language :: Python :: 3.4',
        'Programming Language :: Python :: Implementation :: CPython',
        'Topic :: Software Development :: Libraries :: Python Modules',
    ],
    cmdclass={"build_ext": my_build_ext}
)
