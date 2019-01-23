# Wine 1.8 stable branch for Mesa's gallium nine statetracker
[![Build Status](https://travis-ci.org/iXit/wine.svg?branch=wine-stable-1.8)](https://travis-ci.org/iXit/wine)

**This branch is intended for package maintainers.**

Only bugfixes and stable feature are cherry-picked to this branch.
Once stable it is taged *wine-nine-1.8-x*.

As the Wine API is stable, the build d3d9-nine.dll.so works with all
Wine 1.8.x releases.

More details on the branches can be found in the [Wiki](https://github.com/iXit/wine/wiki/Branch-description).

## How to build


    $ ./configure --with-d3d9-nine
    $ make include
    $ make __builddeps__
    $ make d3d9-nine.dll.so -C dlls/d3d9-nine
    $ make d3d9-nine.dll.fake -C dlls/d3d9-nine
    $ make programs/ninewinecfg

## How to use

Run the config tool to create a symlink from d3d9.dll to d3d9-nine.dll.so.
On multiarch enabled wine it creates two symlinks in the current WINEPREFIX!

    $ wine ninewinecfg

Run a D3D9 enabled application using wine.

If no longer required you can deactive it by using the *ninewinecfg* tool.

## Packaging

An example how to create a wine addon package can be found in the [Wiki](https://github.com/iXit/wine/wiki/Creating-wine-addon-packages).


