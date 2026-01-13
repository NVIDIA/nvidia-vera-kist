# OpenBMC Template Application

The purpose of this application is to provide a template for writing new openbmc
applications. When using this template, the author should fill this section with
some small details about what the app is for, how it should be used, and how one
configures it into the build.

This template application supports: Meson builds Unit tests using gtest Opening
a basic application on DBus Static analysis with openbmc-build-scripts

## Usage

To use this application, take template-app_git.bb, and put it into an
appropriate meta layer (generally meta-<machine name>), and rename to your
application name in the form of

my-app-name_git.bb

Update the SRC_URI in the bb file to the latest commit from this repo.

Open $BBPATH/conf/local.conf and add

```bash
IMAGE_INSTALL:append = "my-app-name"
```

Then run bitbake, and your application will be included.
