# SRanipatch VRCFT Module

This is the VRCFT Receiver module for SRanipatch.

Over OSC, it receives the base SRanipal parameters and provides them to VRCFT in exactly the same manner that the original, SRanipal-SDK-consuming module did.

**Eye tracking is not supported as this module is targeted solely at the VIVE Facial Tracker.**


## Firewall

In order for VRCFT to receive network traffic for SRanipatch, a firewall rule will need to be created allowing inbound traffic for UDP port `9337`.

Run the following command in Windows PowerShell, as administrator:

```pwsh
New-NetFirewallRule -DisplayName "SRanipatch (UDP 9337)" -Direction Inbound -Protocol UDP -LocalPort 9337 -Action Allow -Profile Any
```

1. Right-click the Windows PowerShell entry in your start menu, then run as administrator.
2. Enter the command from above, then press enter.
3. You should not need to restart anything for this to take effect.


## Upstream

This is a fork of the [SRanipalTrackingModule](https://github.com/VRCFaceTracking/SRanipalTrackingModule) repository.

At the time this fork was made, no software license was included.
No license is granted by this fork and all rights for the source code remain with their respective rights holders.

The existence of this repository is not to be interpreted as granting permission to use, modify, or redistribute the upstream code.

Thank you to the original authors for their work!
