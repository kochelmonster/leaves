#!/usr/bin/env python
import sys
import yaml


class GraphCreator(object):
    def __call__(self, state, no):
        lines = ["digraph G{} {{".format(no)]
        add = lines.append
        self.outer_connections = []

        for i, page in enumerate(state["state"]):
            add(" "*4+"subgraph cluster_{} {{".format(i))

            table = "<TR>"\
                    "<TD>id:</TD>"\
                    "<TD>{id}</TD>"\
                    "<TD>offset:</TD>"\
                    "<TD>{offset}</TD>"\
                    "</TR>"\
                    "<TR>"\
                    "<TD>nodes:</TD>"\
                    "<TD>{node_count}</TD>"\
                    "<TD></TD>"\
                    "<TD></TD>"\
                    "</TR>"\
                    "<TR>"\
                    "<TD>size:</TD>"\
                    "<TD>{size}</TD>"\
                    "<TD>free:</TD>"\
                    "<TD>{free_size}</TD></TR>".format(**page)
            add(" "*8+'label = <<TABLE border="0">{}</TABLE>>;'.format(table))

            connections = []
            for node in page["nodes"]:
                method = getattr(self, "add_"+node["type"])
                node, conns = method(page, node)
                add(" "*8+node)
                connections.extend(conns)

            for c in connections:
                add(" "*8+c)

            add(" "*4+"}")

        for c in self.outer_connections:
            add(" "*4+c)
        add("}")
        add("")

        return "\n".join(lines)

    def _add_trie_kind(self, color, page, node):
        if node["data"]:
            connections = [p.split(">") for p in node["data"].split("|")]
            appartments = "|".join("<f{}> {}".format(index, index)
                                   for index, _ in connections)
        else:
            connections = []
            appartments = ""

        attribs = [
            "shape=record",
            "style=filled",
            "fillcolor={}",
            'label="{}"'
        ]
        label = "{{{}({})-{}|{{{}}}}}".format(
            node["ptr"], page["id"], node["size"], appartments)
        attribs = (",".join(attribs)).format(color, label)
        node_id = "node{}_{}".format(page["id"], node["ptr"])
        node = "{} [{}];".format(node_id, attribs)

        tmpl = node_id+":f{{}} -> node{}_{{}};".format(page["id"])
        connections = [tmpl.format(index, nid)
                       for index, nid in connections if int(nid)]
        return node, connections

    def add_bittrie(self, page, node):
        return self._add_trie_kind("Azure", page, node)

    def add_trie(self, page, node):
        return self._add_trie_kind("Moccasin", page, node)

    def add_compressed(self, page, node):
        attribs = [
            "shape=record",
            "style=filled",
            "fillcolor=yellow",
            'label="{{{}({})-{}|{{{}}}}}"'
        ]
        attribs = (",".join(attribs)).format(
            node["ptr"], page["id"], node["size"],
            str(node["data"]).replace("|", "+"))
        node_id = "node{}_{}".format(page["id"], node["ptr"])
        conn = [node_id+" -> node{}_{};".format(page["id"], node["child"])]
        node = "{} [{}];".format(node_id, attribs)
        return node, conn

    def add_leaf(self, page, node):
        attribs = [
            "shape=box",
            "style=filled",
            "fillcolor=Gold",
            'label=<{}>'
        ]
        node["page"] = page["id"]
        node["data"] = node["data"][:10]
        table = '<TABLE border="0">'\
                "<TR>"\
                "<TD>{ptr}({page})-{size}</TD>"\
                "</TR>"\
                "<TR>"\
                "<TD>{data}</TD>"\
                "</TR>"\
                "</TABLE>".format(**node)

        attribs = (",".join(attribs)).format(table)
        node = "node{}_{} [{}];".format(page["id"], node["ptr"], attribs)
        return node, []

    def add_link(self, page, node):
        attribs = [
            "shape=diamond",
            "style=filled",
            "fillcolor=orange",
            'label="{}({})"'
        ]
        attribs = (",".join(attribs)).format(node["ptr"], page["id"])
        node_id = "node{}_{}".format(page["id"], node["ptr"])
        conn = [node_id+" -> node{}_{};".format(node["page"], 0)]
        node = "{} [{}];".format(node_id, attribs)
        self.outer_connections.extend(conn)
        return node, []


def main(instream):
    all_data = []
    while True:
        data = instream.read()
        if not data:
            break
        all_data.append(data)

    obj = yaml.load_all("".join(all_data), Loader=yaml.FullLoader)
    i = 0
    for state in obj:
        if state is None:
            continue
        if "state" not in state:
            continue

        with open("graph-{}.dot".format(i), "w") as f:
            f.write(GraphCreator()(state, i))

        i += 1


if __name__ == "__main__":
    main(sys.stdin)
