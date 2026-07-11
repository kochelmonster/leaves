#!/bin/bash

set -e

perf script -i perf.data.t1024000 > t1024000.perf
/home/michael/src/FlameGraph/stackcollapse-perf.pl t1024000.perf > t1024000.folded
/home/michael/src/FlameGraph/flamegraph.pl t1024000.folded > t1024000.svg

perf script -i perf.data.t2048000 > t2048000.perf
/home/michael/src/FlameGraph/stackcollapse-perf.pl t2048000.perf > t2048000.folded
/home/michael/src/FlameGraph/flamegraph.pl t2048000.folded > t2048000.svg
