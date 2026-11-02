# SRanipatch

## Language

**Installed package**: A SRanipatch package registered with SteamVR. Having the package's files on disk alone does not make it installed.

**Observation**: The dashboard's view of a tracking server session and the freshness of its camera image and tracking results.

**Server session**: One run of the tracking server. Tracking data from different sessions is distinct.

**Managed server**: A tracking server whose lifetime belongs to the current SteamVR session.

**Manual server**: A tracking server whose lifetime remains under the user's control, including while connected to SteamVR.

**Server availability**: Whether the tracking server is alive and available to the dashboard, independently of camera activity.

**Tracking freshness**: Whether displayed tracking data reflects recent camera activity, independently of server availability.

**Overlay**: The dashboard interface presented as a panel in SteamVR. Hiding the panel and closing the client are distinct actions.

**OSC destination**: The address and port selected by the tracking server to receive tracking parameters. The last destination remains visible while Steam Link is disconnected.
