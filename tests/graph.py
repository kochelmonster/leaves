#!/usr/bin/env python
# Uses graphviz to create a svg image of a graph specified by a yaml file
#
# python3 tests/graph.py tests/cmpfiles/*.yaml
import argparse
import subprocess
import sys
import yaml
from pathlib import Path

MAX_VAL_SIZE = 27
MAX_KEY_SIZE = 60


def display_key(key):
    if len(key) > MAX_KEY_SIZE:
        return key[:MAX_KEY_SIZE // 2] + "..." + key[-MAX_KEY_SIZE // 2:]
    return key


LNODE = ('"{id}" [fillcolor=darkolivegreen1 label="(({id})|({keysize}-{key})|'
         '({valuesize}-{value}))"]').replace("(", "{{").replace(")", "}}")

TNODE = '"{id}" [fillcolor=yellow label="(({id})|{compress}({slots}))"]'.replace(
    "(", "{{").replace(")", "}}")

BNODE = '"{id}" [fillcolor=lightblue label="(({id})|({count})|({sitems}))"]'.replace(
    "(", "{{").replace(")", "}}")


class Graph:
    def make_graph(self, nodes):
        self.nodes = {}

        lines = ["digraph G {", "layout=dot",
                 "node [shape=record style=filled]"]
        add = lines.append

        if not nodes:
            add('empty [shape=plaintext style="" label="Empty trie (null root)"]')

        # group nodes
        pages = {}
        for n in nodes:
            self.nodes[n["id"]] = n
            page = n.get("page", 0)
            pages.setdefault(page, []).append(n)

        for k, pnodes in pages.items():
            with_headers = "page" in pnodes[0]
            if with_headers:
                free = pnodes[0]["freespace"]
                size = pnodes[0]["size"]
                txn = pnodes[0]["txn"]
                add(f"subgraph cluster_Page{k} {{")
                add(f'label = "{k}\\nsize: {size} free: {free} txn: {txn}"')
            # add nodes
            for n in pnodes:
                type_ = n["type"]
                add(getattr(self, "handle_" + type_)(n))
            if with_headers:
                add("}")

        # add connections
        for n in nodes:
            type_ = n["type"]
            for i, c in enumerate(n.get("children", ())):
                add(f'"{n["id"]}":f{i} -> "{c}"')

        add("}")
        add("")

        # print("max page", max(map(int, pages.keys())))
        return "\n".join(lines)

    def handle_trie(self, node):
        compress = "{" + str(node["compressed"]["size"]) + \
            "-" + display_key(node["compressed"]["key"]) + "}|"

        children = []
        keys = filter(bool, node.get("branches", ""). split("]["))
        for k in keys:
            children.append(k.lstrip("[").rstrip("]"))

        slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(children))
        return TNODE.format(compress=compress, slots=slots, **node)

    def handle_leaf(self, node):
        if len(node["value"]) > MAX_VAL_SIZE:
            node["value"] = node["value"][:MAX_VAL_SIZE] + "..."
        return LNODE.format(**{**node, "key": display_key(node["key"])})

    def handle_burst(self, node):
        items = []
        for i, item in enumerate(node["items"]):
            items.append("{" + f"{i}|" +  item["key"] + "|" + item["value"][:10] + "}")
        items = "{" + "|".join(items) + "}"
        return BNODE.format(sitems=items, **node)


def main(paths, output_dir=None):
    sources = []
    for p in paths:
        src = Path(p)
        if src.is_dir():
            sources.extend(sorted(src.glob("*.yaml")))
        else:
            sources.append(src)
    if not sources:
        raise ValueError("No YAML dumps found")

    output_dir = Path(output_dir) if output_dir is not None else (
        Path(__file__).parent / "graphs"
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    for src in sources:
        dest = output_dir / (src.stem + ".svg")
        print("dest", src, dest)

        with open(src, "r") as f:
            nodes = yaml.load_all(f.read(), Loader=yaml.FullLoader)
            subprocess.run(
                ["dot", "-Tsvg", "-o", str(dest)],
                input=Graph().make_graph(list(filter(bool, nodes))),
                text=True,
                check=True,
            )

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+",
                        help="YAML dumps from _Dumper, or directories of dumps")
    parser.add_argument("--output-dir", type=Path,
                        help="SVG directory (default: tests/graphs)")
    args = parser.parse_args()
    sys.exit(main(args.paths, args.output_dir))
