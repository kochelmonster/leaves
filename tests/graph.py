#!/usr/bin/env python
# Uses graphviz to create a svg image of a graph specified by a yaml file
#
# python tests/graph.py tests/cmpfiles/*
import sys
import yaml
import sarge
from pathlib import Path

MAX_VAL_SIZE = 27

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

        # group nodes
        pages = {}
        for n in nodes:
            self.nodes[n["id"]] = n
            page = n.get("page", 0)
            pages.setdefault(page, []).append(n)

        for k, pnodes in pages.items():
            free = pnodes[0]["freespace"]
            size = pnodes[0]["size"]
            txn = pnodes[0]["txn"]
            add(f"subgraph cluster_Page{k} {{")
            add(f'label = "{k}\\nsize: {size} free: {free} txn: {txn}"')
            # add nodes
            for n in pnodes:
                type_ = n["type"]
                add(getattr(self, "handle_" + type_)(n))
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
            "-" + node["compressed"]["key"] + "}|"

        children = []
        keys = filter(bool, node.get("branches", ""). split("]["))
        for k in keys:
            children.append(k.lstrip("[").rstrip("]"))

        slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(children))
        return TNODE.format(compress=compress, slots=slots, **node)

    def handle_leaf(self, node):
        if len(node["value"]) > MAX_VAL_SIZE:
            node["value"] = node["value"][:MAX_VAL_SIZE] + "..."
        return LNODE.format(**node)

    def handle_burst(self, node):
        items = []
        for i, item in enumerate(node["items"]):
            items.append("{" + f"{i}|" +  item["key"] + "|" + item["value"][:10] + "}")
        items = "{" + "|".join(items) + "}"
        return BNODE.format(sitems=items, **node)


def main(paths):
    for p in paths:
        src = Path(p)
        dest = Path(__file__).parent / "graphs" / (src.stem + ".svg")
        print("dest", src, dest)

        with open(src, "r") as f:
            nodes = yaml.load_all(f.read(), Loader=yaml.FullLoader)
            # print(Graph().make_graph(list(filter(bool, nodes))))
            # return 0
            sarge.run(
                f"dot -Tsvg > {dest}",
                input=Graph().make_graph(list(filter(bool, nodes))),
            )

    return 0


if __name__ == "__main__":
    debug = [
        "cmpfiles/insert_compress_split_1_abddef.yaml"
    ]

    debug = [Path(__file__).parent / d for d in debug]
    sys.exit(main(sys.argv[1:] or debug))
