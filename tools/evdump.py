"""Decode event scripts (.bfevfl) and list what they change.

    python evdump.py <flow.bfevfl> [--all]     state-changing actions and queries with their parameters
"""
import sys
import evfl

# actions that only change what is seen or heard: left out unless --all
VISUAL = ("PlayAS", "Camera", "Fade", "LookAt", "DoF", "Lod", "Sound", "Emotion", "Shading", "Foliage",
          "Palette", "ModelBind", "Offstage", "Warp", "Wait", "Comment", "Participate", "Turn", "Voice",
          "Graphics", "Effect", "Rumble", "Light", "ScreenShot", "Blur")


def _value(holder, attribute):
    """holder.attribute.v, or None where either is missing (evfl wraps references in objects with .v)."""
    return getattr(getattr(holder, attribute, None), "v", None)


def events(path):
    """(flowchart, event name, event kind, actor, action or query, parameters, sub-flow) for each event."""
    flow = evfl.EventFlow()
    flow.read(open(path, "rb").read())
    flowchart = flow.flowchart
    for event in flowchart.events:
        data = event.data
        kind = type(data).__name__
        actor = _value(data, "actor")
        action = _value(data, "actor_action") or _value(data, "actor_query")
        params = dict(data.params.data) if getattr(data, "params", None) else {}
        sub_flow = getattr(data, "res_flowchart_name", "")
        if getattr(data, "entry_point_name", ""):
            sub_flow += "/" + data.entry_point_name
        yield (flowchart.name, event.name, kind, (str(actor.identifier) if actor else ""), (action.v if action else ""),
               params, sub_flow)


def main():
    show_all = "--all" in sys.argv
    for path in [arg for arg in sys.argv[1:] if not arg.startswith("--")]:
        for flowchart, name, kind, actor, action, params, sub_flow in events(path):
            if not show_all and kind == "ActionEvent" and any(word in action for word in VISUAL):
                continue
            if kind in ("ForkEvent", "JoinEvent"):
                continue
            param_list = ", ".join(f"{key}={value}" for key, value in params.items())
            # kind[:-5]: without "Event"
            print(f"{flowchart}:{name:<10} {kind[:-5]:<8} {actor}.{action}({param_list}) {sub_flow}".rstrip())


if __name__ == "__main__":
    main()
