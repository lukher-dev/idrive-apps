# Changelog

## Unreleased

The HUD map's edge fade is gone, and its corners are rounded again.

## 0.10.18

Fork build with upstream's changes up to 0.10.12. On the HUD: the map can
show only near turns, from a set distance (1 km by default); it can be
zoomed; its size goes up to 135 %; its edges fade out instead of having
rounded corners; and it can be hidden behind BMW's speed readout. CarPlay:
the phone's arrival-time card on the cluster is a setting, and the phone's
turn card sits higher on the cluster.

## 0.10.12

The diagnostic journal is kept in memory and written to storage only around
failures and at shutdown, which greatly reduces writes to the head unit's
storage.

