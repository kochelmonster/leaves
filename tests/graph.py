#!/usr/bin/env python
# python tests/graph.py tests/cmpfiles/*
import sys
import yaml
import sarge
from pathlib import Path

MAX_VAL_SIZE = 27

LNODE = ('"{id}" [fillcolor=darkolivegreen1 label="(({id})|({keysize}-{key})|'
         '({valuesize}-{value}))"]').replace("(", "{{").replace(")", "}}")

BNODE = '"{id}" [fillcolor={color} label="(({id})|{compress}({slots}))"]'.replace(
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
            page = n["block"]
            pages.setdefault(page, []).append(n)

        for k, pnodes in pages.items():
            size = pnodes[0]["size"]
            free = pnodes[0]["freespace"]
            space = pnodes[0]["space"]
            lsize = pnodes[0]["leaf_size"]
            lfree = pnodes[0]["leaf_free"]
            lspace = pnodes[0]["leaf_space"]
            add(f"subgraph cluster_Page{k} {{")
            add(f'label = "{k}\\nsize: {size}|free: {free}|space: {space}\\nlsize: {lsize}|lfree: {lfree}|lspace: {lspace}"')
            # add nodes
            for n in pnodes:
                type_ = n["type"]
                add(getattr(self, "handle_" + type_)(n))
            add("}")

        # add connections
        for n in nodes:
            type_ = n["type"]
            start = 0
            null_link = n.get("nulllink")
            if null_link:
                add(f'"{n["id"]}":f0 -> "{null_link}"')
                start = 1
            for i, c in enumerate(n.get("children", ()), start=start):
                add(f'"{n["id"]}":f{i} -> "{c}"')

        add("}")
        add("")

        print("max page", max(map(int, pages.keys())))
        return "\n".join(lines)

    def handle_branch(self, node):
        branch = node.get("branch")
        if branch == "array":
            color = "yellow"
        elif branch == "trie":
            color = "lightblue"
        else:
            color = "pink"

        if node.get("compressed"):
            compress = "{" + str(node["compressed"]["size"]) + \
                "-" + node["compressed"]["key"] + "}|"
        else:
            compress = ""

        children = []
        null_link = node.get("nulllink")
        if null_link:
            children.append("")

        keys = filter(bool, node.get("key", "").split("]["))
        for k in keys:
            children.append(k.lstrip("[").rstrip("]"))

        slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(children))
        return BNODE.format(color=color, compress=compress, slots=slots, **node)

    def handle_leaf(self, node):
        if len(node["value"]) > MAX_VAL_SIZE:
            node["value"] = node["value"][:MAX_VAL_SIZE] + "..."
        return LNODE.format(**node)


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
