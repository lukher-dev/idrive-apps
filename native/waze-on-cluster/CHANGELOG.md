# Changelog

## Unreleased

## 0.10.20

The HUD map can fade to black towards the edges of the HUD (the vignette,
levels 1 to 5, 2 by default, 0 for none). The fade belongs to the HUD, so
moving, resizing or zooming the map does not move it. It is saved per
driver profile.

CarPlay: the phone's turn cards on the cluster are a setting, on by
default. Turned off, CarPlay asks the phone for its map-only cluster page,
which leaves out the turn card and still shows the arrival-time card when
that is on. This follows from how iOS draws that page and is not yet
confirmed on the car.

## 0.10.19

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

