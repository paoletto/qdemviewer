A library/demo application for provisioning and processing 
DEM EPSG3857 tiles in terrarium format as well as regular raster tiles.

The project consists of two main subprojects: *mapfetcher* is a map provisioning library,
while *demviewer* is a demo application using said library. Note that this application simply demostrates the usage of the *mapfetcher* library. It therefore lacks several features normally expected in a map renderer.

The project depends on Qt5.

More detailed documentation currently WIP.
Note: using *mapfetcher* to fetch aws terrarium DEM tiles with the *borders* option set produces *Heightmap*s objects sized 258x258, where the first and last sample is intended to be half-spaced.
See demviewer shaders for how to use it.

Update: this repository now includes also two additional subprojects: astcencoder is a library wrapping ARM astc encoder, to make it easier to build mapfetcher without it (although it's currently not completely disentangled).  The last subproject, cacheupdater, is intended to update the network or astc cache from one machine to another over TCP incrementally (currently only based on data timestamp).



https://github.com/paoletto/qdemviewer/assets/6912425/31946e81-c5c4-4b7c-bacc-3bddbc798a18

