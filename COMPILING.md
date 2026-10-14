# Compiling

Sadly, there is extremely little information around creating native Xbox 360 programs. If you are interested in creating or modifying Xbox 360 programs, I would recommend taking a look at [ClementDreptin's ModdingResources](https://github.com/ClementDreptin/ModdingResources/). While it is mainly focussed on developing dashlaunch plugings for modding games, much of the information applies to creating homebrew as well.

## Requirements

* A computer running Windows 7 or higher
* Microsoft Visual Studio 2010
* The official Microsoft Xbox 360 XDK
* An Xbox capable of running homebrew (BadUpdate, JTAG or RGH) preferably with the XBDM plugin

1. Install Visual Studio 2010 and the official XDK
2. Clone this Git Repository
3. From within Visual Studio 2010, open *Omni360.sln*
4. In the solution explorer on the left you should see two projects: AtgFramework and XboxTLS. Right click each one and click Build, with XboxTLS **last** — it links against AtgFramework. (X-Store had seven projects here; the other five were deleted along with the features that needed them.)
5. Once everything has built successfully, navigate to the *Release* folder using file explorer. In this folder you should find *Omni360.xex*. Transfer this to your console and launch it. See `docs/Setup Guide.md` for what else needs to go alongside it.
6. To view the standard output of the Xbox, you will need to use a program on your PC called Xbox Watson. It is quite buggy, but it is what Microsoft provided developers with. To prevent some of the bugs, the program can be run in Windows 7 compatability mode.

## From the command line (Visual Studio 2022 with RXDK-360, or 2010)

`build.ps1` builds the whole solution - the ATG framework, then Omni360 - with the newest Visual Studio's MSBuild and the SDK in `XEDK`, and copies `Omni360.xex` and a `settings.txt` template into `dist\`:

```
.\build.ps1            # build
.\build.ps1 -Clean     # rebuild everything
.\build.ps1 -Tests     # and build and run tools\test_usenet on the PC
```

It passes `DeploymentType=None`, so nothing is copied to a dev kit - the step that otherwise fails with `xbecopy : error X1001: Could not connect to Xbox 360` on a machine without one.

## Issues

* HELP! It won't compile!
  * If you get an error about failing to find `uploadXEX.bat`, right click on the XboxTLS solution, go to Properties > configuration properties > Build Events > Post-Build Events, and remove the command line field.
  * Try to determine whether the error message is an error in the code, or an error in the build system. If you have not edited the code, something is likely incorrectly configured.
* The program crashed when running it on the Xbox
  * Use Xbox Watson to determine which part of the code likely caused the error
