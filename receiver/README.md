# SRanipatch VRCFT Module

Receives SRanipal mouth tracking parameters over OSC for VRCFaceTracking. Eye tracking is not supported.

## Install

Follow the [setup instructions](../README.md#setup).

## Build

Requires the .NET 10 SDK and the current Steam version of VRCFaceTracking. From the repository root:

```powershell
dotnet publish receiver/SRanipatch/SRanipatch.csproj -c Release
```

Writes `output/SRanipatch.VRCFT.win-x64.zip`. If VRCFaceTracking is installed elsewhere, add `-p:VrcftDirectory="C:\path\to\VRCFaceTracking"`.

## Upstream

This is a fork of the [SRanipalTrackingModule](https://github.com/VRCFaceTracking/SRanipalTrackingModule) repository.

At the time this fork was made, no software license was included. No license is granted by this fork; rights to the source remain with their respective holders.
