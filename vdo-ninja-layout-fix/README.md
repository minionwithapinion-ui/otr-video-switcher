# OTR VDO.Ninja OBS layout-preservation fix

Target: upstream `steveseguin/ninja-obs-plugin` v1.1.65 / OBS 32.2.x.

This patch changes only the auto-inbound path:

- excludes the OBS/host screen-share return stream aliases (`<stream>_ss` and `<stream>S`, including hashed forms) from guest-camera auto-inbound;
- a blank Auto-Inbound Target Scene no longer falls back to whatever OBS scene is currently active;
- the default auto-inbound layout is `None`, so OBS scene-specific positions and sizes are preserved;
- intentionally auto-created guests use the official `VDO.Ninja Source` kind instead of a raw `Browser Source`.

This preserves the production rule used by the OTR controller: VDO.Ninja owns the connection; OBS owns the layout. Screen sharing can remain available to guests without being automatically inserted into the broadcast.

## Apply

```powershell
git clone https://github.com/steveseguin/ninja-obs-plugin.git
git -C ninja-obs-plugin checkout a6a73755da86adb8ae782117c66a3cb169eb1d3f
python vdo-ninja-layout-fix/apply_vdo_layout_fix.py ninja-obs-plugin
```

Then build for OBS 32.2.x using the upstream `BUILDING.md` instructions.
