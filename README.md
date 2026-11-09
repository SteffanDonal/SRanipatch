# SRanipatch

Unofficial SRanipal runtime for the **VIVE Facial Tracker**, bypassing the need to use the SRanipal SDK.

The face tracking model runs _locally_, and only the inferred parameters are streamed over OSC. No video streaming involved!

Eye tracking is not supported.

## AI Usage

<table>
  <tr>
    <td style="text-align:center">
      <img src=".github/ai-w.svg" alt="AI:W" width="auto" height="48">
    </td>
    <td>
      AI tools have read/write access to code.<br>
      All code is reviewed and tested by a software developer.
    </td>
  </tr>
</table>

Read more at [0rwx.com](https://0rwx.com/)

## Supported Configurations

| Name                 | Headset          | Tracker Connection | Runtime Platform |
| -------------------- | ---------------- | ------------------ | ---------------- |
| Standalone Inference | Steam Frame      | Steam Frame        | Linux ARM64      |
| Local Tethered       | Any PCVR Headset | Your PC            | Windows x64      |

## Mounting Hardware

Need a way to mount the VIVE Facial Tracker to your Steam Frame?

A 3d-printable solution is [available here](https://www.printables.com/model/1902859-bonn-mounts-for-steam-frame-babble-vft-vive-facial).

## Setup

### Install the VRCFT module on your Windows PC

1. Install the current Steam version of [VRCFaceTracking](https://docs.vrcft.io/docs/intro/getting-started).
2. Download `SRanipatch.VRCFT.win-x64.zip` from the [latest release](https://github.com/SteffanDonal/SRanipatch/releases/latest). Leave it zipped.
3. In VRCFaceTracking, open **Module Registry**, click **+** (**Install from .zip**), and select the downloaded ZIP.

- If you have a Steam Frame, and the VFT is connected to it, continue to the [next step](#standalone-inference-setup).
- If your VFT is connected directly to your Windows PC, continue to [local tethered setup](#local-tethered-setup).

### Standalone Inference Setup

On **Steam Frame**, open a terminal and run:

```sh
curl -fsSL https://raw.githubusercontent.com/SteffanDonal/SRanipatch/main/runtime/scripts/get.sh | bash
```

The installer:

1. Downloads & validates the latest release
2. Checks for USB Video (UVC) support
   - If not available, it will offer to install [steam-frame-uvc](https://github.com/mavishtheblue/steam-frame-uvc) for you. Follow the prompts, **it requires `sudo`**.
3. Registers SRanipatch with SteamVR and restarts it. Your headset's display will temporarily go black.

Once installed, SRanipatch will automatically start working whenever you are streaming PCVR _and_ your VIVE Facial Tracker is connected.

#### Windows Firewall Rule

The VRCFT module listens to inbound **UDP port 9337** to receive tracking from Steam Frame.

1. In Windows, find **Windows PowerShell** in Start, right-click it, and choose **Run as administrator**.
2. Run:

```powershell
New-NetFirewallRule -DisplayName "SRanipatch (UDP 9337)" -Direction Inbound -Protocol UDP -LocalPort 9337 -Action Allow -Profile Any
```

The rule takes effect immediately, you will not need to restart anything.

### Local Tethered Setup

1. Download `SRanipatch-Runtime.win-x64.zip` and extract it into a folder you will keep.
2. Run `install.cmd` from that folder to register SRanipatch with SteamVR. Accept the SteamVR restart offer if prompted.
3. Connect your VIVE Facial Tracker to the PC, start SteamVR and VRCFaceTracking.

SRanipatch will automatically start working whenever SteamVR is running and your VIVE Facial Tracker is connected.

## Updating

- **Standalone Inference:** run `./update` in your SRanipatch folder (default `~/.local/share/sranipatch`), or rerun the [setup command](#standalone-inference-setup).
- **Local Tethered:** run `update.cmd` in your SRanipatch folder.

## More Reading

If you wish to manually install SRanipatch, uninstall it, or debug your setup, you may learn more about that in the [SRanipatch runtime README](runtime/README.md).

# Support

Raise an issue if you are encountering issues.

Runtime logs are stored here:

- Steam Frame: `~/.local/state/sranipatch/logs`
- Windows: `%LOCALAPPDATA%\sranipatch\logs`
