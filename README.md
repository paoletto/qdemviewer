A library/demo application for provisioning and processing 
DEM EPSG3857 tiles in terrarium format as well as regular raster tiles.

The project consists of two subprojects: *mapfetcher* is a map provisioning library,
while *demviewer* is a demo application using said library. Note that this application simply demostrates the usage of the *mapfetcher* library. It therefore lacks several features normally expected in a map renderer.

The project depends on Qt5.

More detailed documentation currently WIP.
Note: using *mapfetcher* to fetch aws terrarium DEM tiles with the *borders* option set produces *Heightmap*s objects sized 258x258, where the first and last sample is intended to be half-spaced.
See demviewer shaders for how to use it.






https://github.com/paoletto/qdemviewer/assets/6912425/16ccf90d-75ff-4d60-8332-35835e94240f

