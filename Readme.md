# Wine development for Mesa's gallium nine statetracker
[![Build Status](https://travis-ci.org/iXit/wine.svg?branch=master)](https://travis-ci.org/iXit/wine)

The patched wine allows to run any D3D9 applications with nearly
no CPU overhead, which provides a smother gaming experience and
increased FPS.
To achieve a low CPU overhead a new library, called d3d9-nine.dll,
talks directly to the graphics driver.

You must "activate" that library to be used instead of the wine
default (OpenGL backed) d3d9.dll.

**Note:** This library only work with **Mesa** and **Gallium drivers**.

More details in the [Wiki](https://github.com/iXit/wine/wiki).


## How to build


    $ ./configure --with-d3d9-nine
    $ make

## How to use

Run the config tool to create a symlink from d3d9.dll to d3d9-nine.dll.so.
On multiarch enable wine it creates to symlinks !

    $ wine ninewinecfg

Run a D3D9 enabled application using wine.

If no longer required you can deactive it by using the *ninewinecfg* tool.

## Branches

### master

The master branch contains experimental features and is updated
from time to time.

### stable branches

Stables branches are for package maintainers.
Only fixes are pushed and (if stable) tagged.

The wine 3.0 branch is tagged: *wine-nine-3.0-1*

More details in the [Wiki](https://github.com/iXit/wine/wiki/Branch-description)


