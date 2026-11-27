# Watch a node

Keep an eye on a repeater, a friend's radio or a sensor without opening Contacts.

- **Start:** long-press a contact (or tap its map marker) and choose **Watch**. The same button reads **Unwatch** to stop. Up to 16 nodes.
- **Went quiet:** when nothing from a watched node has reached you for the set time, a pop-up says *"Hilltop went quiet: last heard 13 h ago"*.
- **Back:** the next time anything from it arrives, *"Hilltop is back on the air, quiet for 1 d 2 h"*.
- **How long is quiet:** Settings > Contact Management > Watched nodes, 1 to 72 hours (13 by default: repeaters flood an advert every 12 hours).

The pop-up chimes unless sound is off or Do Not Disturb is on. One that arrives while the screen is dark is shown when you next wake it.

"Heard" means any packet from the node reached you: its advert, a message, a path or a reply. The timing uses the device's uptime timer, so a clock that changes when it syncs can't set off a false alert. After a restart a node that was already quiet is marked quiet without a pop-up, so you only hear when it comes back.

Logic: `src/ui-touch/NodeWatch.h`, tested by `test/test_node_watch.cpp`.
