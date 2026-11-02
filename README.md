# SRanipatch

Unofficial SRanipal runtime for the **VIVE Facial Tracker**, with a Receiver for VRCFaceTracking. Supports Windows PCVR and Steam Frame. Eye tracking is not supported.

## Choose your setup

Install Receiver on your Windows PC and Runtime on the device running SteamVR.

| Headset | Receiver on Windows PC | Runtime | Windows Firewall rule |
| --- | --- | --- | --- |
| PCVR headset | `SRanipatch.VRCFT.win-x64.zip` | `SRanipatch-Runtime.win-x64.zip` | Not required for local tracking |
| Steam Frame | `SRanipatch.VRCFT.win-x64.zip` | `SRanipatch-Runtime.linux-arm64.tar.gz` on Frame | Required on your PC |

Get packages from the [latest release](https://github.com/SteffanDonal/SRanipatch/releases/latest).

## Setup

Before installing Runtime, install and run SteamVR at least once on the device that will run it: your PC for PCVR, or Frame for Steam Frame.

### 1. Install Receiver on your Windows PC

1. Install the current Steam version of [VRCFaceTracking](https://docs.vrcft.io/docs/intro/getting-started).
2. Download `SRanipatch.VRCFT.win-x64.zip` from the latest release. Leave it zipped.
3. In VRCFaceTracking, open **Module Registry**, click **+** (**Install from .zip**), and select the Receiver ZIP.

### 2. Install Runtime for your headset

#### PCVR headset

1. Download `SRanipatch-Runtime.win-x64.zip` and extract it into a folder you will keep.
2. Run `install.cmd` from that folder to register Runtime with SteamVR. Accept the SteamVR restart offer if prompted.
3. Connect your VIVE Facial Tracker to the PC and start SteamVR and VRCFaceTracking.

Keep the extracted folder: SteamVR runs Runtime from this location.

#### Steam Frame

On **Steam Frame**, open a terminal and run:

```sh
curl -fsSL https://raw.githubusercontent.com/SteffanDonal/SRanipatch/main/runtime/scripts/get.sh | bash
```

The installer downloads the latest stable Linux Runtime, checks its checksum, and registers it with SteamVR. If USB Video (UVC) support is missing, it offers to set up [steam-frame-uvc](https://github.com/mavishtheblue/steam-frame-uvc). Follow its prompts; setup can require sudo. Restarting SteamVR temporarily makes the Frame display go black.

Connect the VIVE Facial Tracker to Frame. Connect Frame to your Windows PC using Steam Link; Runtime discovers that PC automatically. Run VRCFaceTracking on the PC and complete the firewall step below.

For manual installation, extract `SRanipatch-Runtime.linux-arm64.tar.gz` on Frame into a folder you will keep and run `./install` from that folder. See [Runtime instructions](runtime/README.md#install-on-frame) for installation options.

### 3. Steam Frame: allow tracking through Windows Firewall

Run this on your **Windows PC**, not on Frame. Receiver needs inbound **UDP port 9337** to receive tracking from Frame.

1. Find **Windows PowerShell** in Start, right-click it, and choose **Run as administrator**.
2. Run:

```powershell
New-NetFirewallRule -DisplayName "SRanipatch (UDP 9337)" -Direction Inbound -Protocol UDP -LocalPort 9337 -Action Allow -Profile Any
```

The rule takes effect immediately.

## Update Runtime

- **Windows:** run `update.cmd` in your Runtime folder.
- **Steam Frame:** run `./update` in your Runtime folder (default `~/.local/share/sranipatch`), or rerun the bootstrap command above.

The updater preserves settings and SteamVR registration. It briefly restarts running tracking applications and prompts for a SteamVR restart if the driver changes.

## More information

- [Runtime commands, configuration, and building](runtime/README.md)
- [Receiver and building](receiver/README.md)
- [Creating a release from build artifacts](docs/releases.md)
